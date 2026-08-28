/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * bke::pbvh::Tree drawing.
 * Embeds GPU meshes inside of bke::pbvh::Tree nodes, used by mesh sculpt mode.
 */

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_map.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subdiv_eval.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_batch.hh"
#include "GPU_vertex_buffer.hh"

#include "DRW_engine.hh"
#include "DRW_pbvh.hh"
#include "DRW_render.hh"

#include "PRF_profile.hh"

#include "draw_pbvh_allocator.hh"
#include "draw_sculpt.hh"

#include "attribute_convert.hh"
#include "bmesh.hh"

namespace blender {

template<> struct DefaultHash<draw::pbvh::AttributeRequest> {
  uint64_t operator()(const draw::pbvh::AttributeRequest &value) const
  {
    using namespace draw::pbvh;
    if (const CustomRequest *request_type = std::get_if<CustomRequest>(&value)) {
      return get_default_hash(*request_type);
    }
    const GenericRequest &attr = std::get<GenericRequest>(value);
    return get_default_hash(attr);
  }
};

namespace draw::pbvh {

uint64_t ViewportRequest::hash() const
{
  return get_default_hash(attributes, use_coarse_grids);
}

/**
 * Because many sculpt mode operations skip tagging dependency graph for reevaluation for
 * performance reasons, the relevant data must be retrieved directly from the original mesh rather
 * than the evaluated copy.
 */
struct OrigMeshData {
  StringRef active_color;
  StringRef default_color;
  VectorSet<StringRef> uv_map_names;
  StringRef active_uv_map;
  StringRef default_uv_map;
  int face_set_default;
  int face_set_seed;
  bke::AttributeAccessor attributes;
  OrigMeshData(const Mesh &mesh)
      : active_color(mesh.active_color_attribute),
        default_color(mesh.default_color_attribute),
        active_uv_map(mesh.active_uv_map_name()),
        default_uv_map(mesh.default_uv_map_name()),
        face_set_default(mesh.face_sets_color_default),
        face_set_seed(mesh.face_sets_color_seed),
        attributes(mesh.attributes())
  {
    uv_map_names.add_multiple(mesh.uv_map_names().as_span().cast<StringRef>());
  }
};

/**
 * Stores the data necessary to draw the PBVH geometry. A separate `*Impl` class is used to hide
 * implementation details from the public header.
 */
class DrawCacheImpl : public DrawCache {
 public:
  struct AttributeData {
    /**
     * \brief A vertex buffer containing data for all BVH nodes.
     */
    gpu::VertBufPtr vbo;

    /**
     * A separate "dirty" bit per node. We track the dirty value separately from deleting the VBO
     * for a node in order to avoid recreating batches with new VBOs. It's also a necessary
     * addition to the flags stored in the PBVH which are cleared after it's used for drawing
     * (those aren't sufficient when multiple viewports are drawing with the same PBVH but request
     * different sets of attributes).
     */
    BitVector<> dirty_nodes;
    /**
     * Mark attribute values dirty for specific nodes. The next time the attribute is requested,
     * the values will be extracted again.
     */
    void tag_dirty(const IndexMask &node_mask);
    void ensure_nodes_capacity(int64_t nodes_len);
    void ensure_buffer_capacity(const GPUVertFormat &format, uint64_t vertex_len);
    void update(const NodeInfo &node_info);
  };

 private:
  /**
   * \brief Used to determine whether to use indexed VBO layouts for multires grids.
   *
   * Bit for each node. `false` means that the node contains smooth shaded vertex data (elements
   * are shared between faces). `true` means that the node is "flat shaded" (elements are stored
   * per face corner).
   */
  BitVector<> use_flat_layout_;
  /**
   * \brief The material index for each node.
   *
   * Is used during drawing to place the node in the correct material bucket.
   */
  Array<int> material_indices_;

  /**
   * \brief Shared index buffer with space bookkeeping and the per-node subranges.
   */
  struct IBOFamily {
    RangeAllocator space;
    /**
     * \brief Limit for the index buffer space.
     *
     * When the allocated space grows past this, the underlying buffer is reallocated and batches
     * need rebuilding.
     */
    uint64_t space_limit = 0;
    /** \brief Index buffer containing all indices. */
    gpu::IndexBufPtr ibo;
    /** \brief Per node a sub-range of ibo. */
    Vector<gpu::IndexBufPtr> ibos;

    /**
     * \brief Ensure the shared index buffer is large enough to hold the assigned space.
     *
     * When this requires growing (or creating) the buffer, existing data is copied over and
     * existing sub-ranges are recreated.
     */
    void ensure_capacity();
  };
  IBOFamily ibo_lines_;
  IBOFamily ibo_lines_coarse_;
  IBOFamily ibo_tris_;
  IBOFamily ibo_tris_coarse_;

  /** \brief Info per node where it is located in the buffers. */
  Vector<NodeInfo> node_infos_;
  RangeAllocator vbo_space_;

  /** Limit of the vertex buffer space, used to detect when it was reallocated. */
  uint64_t vbo_space_limit_ = 0;

  /**
   * GPU data and per-node dirty status for all requested attributes.
   * \note Currently we do not remove "stale" attributes that haven't been requested in a while.
   */
  Map<AttributeRequest, AttributeData> attribute_vbos_;

  /**
   * \brief Cached shared batch for a given setup.
   *
   * The batch points at the shared attribute VBOs and the full index buffer. The `dirty` flag
   * is set by `ensure_node_infos` when any of those buffers need to be reallocated, and the
   * batch is rebuilt lazily on the next access.
   */
  struct BatchCache {
    gpu::Batch *shared = nullptr;
    bool dirty = false;
  };

  /** Batches for drawing wireframe geometry. */
  BatchCache lines_batches_;
  /** Batches for drawing coarse "fast navigate" wireframe geometry. */
  BatchCache lines_batches_coarse_;
  /**
   * Batches for drawing triangles, stored separately for each combination of attributes and
   * coarse-ness. Different viewports might request different sets of attributes, and we don't want
   * to recreate the batches on every redraw.
   */
  Map<ViewportRequest, BatchCache> tris_batches_;

  /**
   * Which nodes (might) have a different number of visible faces.
   *
   * \note Theoretically the dirty tag is redundant with checking for a different number of visible
   * triangles in the PBVH node on every redraw. We could do that too, but it's simpler overall to
   * just tag the node whenever there is such a topology change, and for now there is no real
   * downside.
   */
  BitVector<> dirty_topology_;

 public:
  ~DrawCacheImpl() override;

  void tag_positions_changed(const IndexMask &node_mask) override;
  void tag_visibility_changed(const IndexMask &node_mask) override;
  void tag_topology_changed(const IndexMask &node_mask) override;
  void tag_face_sets_changed(const IndexMask &node_mask) override;
  void tag_masks_changed(const IndexMask &node_mask) override;
  void tag_attribute_changed(const IndexMask &node_mask, StringRef attribute_name) override;

  SculptBatch ensure_tris_batch(const Object &object,
                                const ViewportRequest &request,
                                const IndexMask &leaf_nodes) override;

  SculptBatch ensure_lines_batch(const Object &object,
                                 const ViewportRequest &request,
                                 const IndexMask &leaf_nodes) override;

  Span<int> ensure_material_indices(const Object &object) override;
  Span<NodeInfo> ensure_node_infos(const Object &object, const IndexMask &all_leaf_nodes) override;
  Span<NodeInfo> get_node_infos() const
  {
    return node_infos_;
  }
  uint64_t get_vert_len() const
  {
    return vbo_space_.size();
  }

 private:
  /**
   * Free all GPU data for nodes with a changed visible triangle count. The next time the data is
   * requested it will be rebuilt.
   */
  void free_nodes_with_changed_topology(const bke::pbvh::Tree &pbvh);

  BitSpan ensure_use_flat_layout(const Object &object, const OrigMeshData &orig_mesh_data);

  AttributeData &ensure_attribute_data(const Object &object,
                                       const OrigMeshData &orig_mesh_data,
                                       const AttributeRequest &attr,
                                       const IndexMask &node_mask);

  Span<gpu::IndexBufPtr> ensure_tri_indices(const Object &object,
                                            const OrigMeshData &orig_mesh_data,
                                            const IndexMask &node_mask,
                                            bool coarse);

  Span<gpu::IndexBufPtr> ensure_lines_indices(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const IndexMask &node_mask,
                                              bool coarse);

  /**
   * Return (and rebuild if necessary) the shared batch for a given cache entry.
   *
   * The shared batch uses the full shared index buffer and all attribute vertex buffers.
   * It is rebuilt when the `dirty` flag is set (buffers were reallocated).
   */
  void ensure_batch(BatchCache &cache,
                    GPUPrimType prim_type,
                    gpu::IndexBuf *ibo,
                    Span<gpu::VertBuf *> vbos);
};

void DrawCacheImpl::AttributeData::tag_dirty(const IndexMask &node_mask)
{
  this->dirty_nodes.resize(std::max(this->dirty_nodes.size(), node_mask.min_array_size()), false);
  node_mask.set_bits(this->dirty_nodes);
}

void DrawCacheImpl::tag_positions_changed(const IndexMask &node_mask)
{
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Position)) {
    data->tag_dirty(node_mask);
  }
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Normal)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_visibility_changed(const IndexMask &node_mask)
{
  dirty_topology_.resize(std::max(dirty_topology_.size(), node_mask.min_array_size()), false);
  node_mask.set_bits(dirty_topology_);
  const int64_t size = std::max(node_infos_.size(), node_mask.min_array_size());
  node_infos_.resize(size, NodeInfo{});
  node_mask.foreach_index([&](const int i) { node_infos_[i].is_dirty = true; });
}

void DrawCacheImpl::tag_topology_changed(const IndexMask &node_mask)
{
  /** Currently the only times where topology changes are for BMesh dynamic topology, where tagging
   * a visibility update deletes all the GPU data anyway. */
  this->tag_visibility_changed(node_mask);
}

void DrawCacheImpl::tag_face_sets_changed(const IndexMask &node_mask)
{
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::FaceSet)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_masks_changed(const IndexMask &node_mask)
{
  if (DrawCacheImpl::AttributeData *data = attribute_vbos_.lookup_ptr(CustomRequest::Mask)) {
    data->tag_dirty(node_mask);
  }
}

void DrawCacheImpl::tag_attribute_changed(const IndexMask &node_mask, StringRef attribute_name)
{
  for (const auto &[data_request, data] : attribute_vbos_.items()) {
    if (const GenericRequest *request = std::get_if<GenericRequest>(&data_request)) {
      if (*request == attribute_name) {
        data.tag_dirty(node_mask);
      }
    }
  }
}

DrawCache &ensure_draw_data(std::unique_ptr<bke::pbvh::DrawCache> &ptr)
{
  if (!ptr) {
    ptr = std::make_unique<DrawCacheImpl>();
  }
  return dynamic_cast<DrawCache &>(*ptr);
}

BLI_NOINLINE static void free_ibos(const MutableSpan<gpu::IndexBufPtr> ibos,
                                   const IndexMask &node_mask)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_intersection(node_mask, ibos.index_range(), memory);
  mask.foreach_index([&](const int i) { ibos[i].reset(); });
}

/**
 * Upload a node's index data to the shared IBO and return its subrange. Zero-length ranges (a
 * leaf with no visible geometry) are skipped.
 */
static void update_ibos_subrange(gpu::IndexBuf &ibo_shared,
                                 const NodeInfo::BufferRange &range,
                                 const uint32_t *host_data)
{
  if (range.len == 0) {
    return;
  }
  GPU_indexbuf_update_sub(&ibo_shared,
                          uint(range.offset) * sizeof(uint32_t),
                          uint(range.len) * sizeof(uint32_t),
                          host_data);
}

static const GPUVertFormat &position_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  return format;
}

static const GPUVertFormat &normal_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "nor", gpu::VertAttrType::SNORM_16_16_16_16);
  return format;
}

static const GPUVertFormat &mask_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute("msk",
                                                                    gpu::VertAttrType::SFLOAT_32);
  return format;
}

static const GPUVertFormat &face_set_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "fset", gpu::VertAttrType::UNORM_8_8_8_8);
  return format;
}

static GPUVertFormat attribute_format(const OrigMeshData &orig_mesh_data,
                                      const StringRef name,
                                      const bke::AttrType data_type)
{
  GPUVertFormat format = init_format_for_attribute(data_type, "data");

  bool is_render, is_active;
  const char *prefix = "a";

  if (CD_TYPE_AS_MASK(*bke::attr_type_to_custom_data_type(data_type))) {
    prefix = "c";
    is_active = orig_mesh_data.active_color == name;
    is_render = orig_mesh_data.default_color == name;
  }
  if (data_type == bke::AttrType::Float2) {
    prefix = "u";
    is_active = orig_mesh_data.active_uv_map == name;
    is_render = orig_mesh_data.default_uv_map == name;
  }

  DRW_cdlayer_attr_aliases_add(&format, prefix, data_type, name, is_render, is_active);
  return format;
}

inline short4 normal_float_to_short(const float3 &value)
{
  short3 result;
  normal_float_to_short_v3(result, value);
  return short4(result.x, result.y, result.z, 0);
}

template<typename T>
void extract_data_vert_mesh(const OffsetIndices<int> faces,
                            const Span<int> corner_verts,
                            const Span<T> attribute,
                            const Span<int> face_indices,
                            gpu::VertBuf &vbo,
                            const NodeInfo &node_info)
{
  PRF_scope(ProfileCategory::Draw);
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data() + node_info.vbo.offset;
  VBOType *data_start = data;
  for (const int face : face_indices) {
    for (const int vert : corner_verts.slice(faces[face])) {
      *data = Converter::convert(attribute[vert]);
      data++;
    }
  }
  UNUSED_VARS_NDEBUG(data_start);
  BLI_assert_msg(data == data_start + node_info.vbo.len,
                 "Node VBO data extraction did not fill the expected amount");
}

template<typename T>
void extract_data_face_mesh(const OffsetIndices<int> faces,
                            const Span<T> attribute,
                            const Span<int> face_indices,
                            gpu::VertBuf &vbo,
                            const NodeInfo &node_info)
{
  PRF_scope(ProfileCategory::Draw);
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  VBOType *data = vbo.data<VBOType>().data() + node_info.vbo.offset;
  VBOType *data_start = data;
  for (const int face : face_indices) {
    const int face_size = faces[face].size();
    std::fill_n(data, face_size, Converter::convert(attribute[face]));
    data += face_size;
  }
  UNUSED_VARS_NDEBUG(data_start);
  BLI_assert_msg(data == data_start + node_info.vbo.len,
                 "Node VBO data extraction did not fill the expected amount");
}

template<typename T>
void extract_data_corner_mesh(const OffsetIndices<int> faces,
                              const Span<T> attribute,
                              const Span<int> face_indices,
                              gpu::VertBuf &vbo,
                              const NodeInfo &node_info)
{
  PRF_scope(ProfileCategory::Draw);
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  VBOType *data = vbo.data<VBOType>().data() + node_info.vbo.offset;
  VBOType *data_start = data;
  for (const int face : face_indices) {
    for (const int corner : faces[face]) {
      *data = Converter::convert(attribute[corner]);
      data++;
    }
  }
  UNUSED_VARS_NDEBUG(data_start);
  BLI_assert_msg(data == data_start + node_info.vbo.len,
                 "Node VBO data extraction did not fill the expected amount");
}

template<typename T> const T &bmesh_cd_vert_get(const BMVert &vert, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(vert.head.data, offset));
}

template<typename T> const T &bmesh_cd_loop_get(const BMLoop &loop, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(loop.head.data, offset));
}

template<typename T> const T &bmesh_cd_face_get(const BMFace &face, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(face.head.data, offset));
}

template<typename T>
void extract_data_vert_bmesh(const Set<BMFace *, 0> &faces,
                             const int cd_offset,
                             gpu::VertBuf &vbo,
                             const NodeInfo &node_info)
{
  PRF_scope(ProfileCategory::Draw);
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data() + node_info.vbo.offset;
  VBOType *data_start = data;

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    const BMLoop *l = face->l_first;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->prev->v, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->v, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->next->v, cd_offset));
    data++;
  }
  UNUSED_VARS_NDEBUG(data_start);
  BLI_assert_msg(data == data_start + node_info.vbo.len,
                 "Node VBO data extraction did not fill the expected amount");
}

template<typename T>
void extract_data_face_bmesh(const Set<BMFace *, 0> &faces,
                             const int cd_offset,
                             gpu::VertBuf &vbo,
                             const NodeInfo &node_info)
{
  PRF_scope(ProfileCategory::Draw);
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data() + node_info.vbo.offset;
  VBOType *data_start = data;

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    std::fill_n(data, 3, Converter::convert(bmesh_cd_face_get<T>(*face, cd_offset)));
    data += 3;
  }
  UNUSED_VARS_NDEBUG(data_start);
  BLI_assert_msg(data == data_start + node_info.vbo.len,
                 "Node VBO data extraction did not fill the expected amount");
}

template<typename T>
void extract_data_corner_bmesh(const Set<BMFace *, 0> &faces,
                               const int cd_offset,
                               gpu::VertBuf &vbo,
                               const NodeInfo &node_info)
{
  PRF_scope(ProfileCategory::Draw);
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data() + node_info.vbo.offset;
  VBOType *data_start = data;

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    const BMLoop *l = face->l_first;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l->prev, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l->next, cd_offset));
    data++;
  }
  UNUSED_VARS_NDEBUG(data_start);
  BLI_assert_msg(data == data_start + node_info.vbo.len,
                 "Node VBO data extraction did not fill the expected amount");
}

/**
 * \brief Number of indices per index buffer family that a node requires.
 */
struct NodeIndexCounts {
  int64_t lines = 0;
  int64_t lines_coarse = 0;
  int64_t tris = 0;
  int64_t tris_coarse = 0;
};

static int count_visible_tris_bmesh(const Set<BMFace *, 0> &faces)
{
  return std::count_if(faces.begin(), faces.end(), [&](const BMFace *face) {
    return !BM_elem_flag_test_bool(face, BM_ELEM_HIDDEN);
  });
}

/**
 * \brief Compute the number of indices of all index buffer families for a single node.
 *
 * Each node might have a different number of indices per family, as visibility changes are
 * accounted for by the index buffers (hidden faces are not indexed). The counts are
 * used to determine the space each node requires inside the shared index buffers.
 *
 * \note The counts of the coarse families are equal to their full-resolution counterpart for
 * meshes and BMesh, as only grids have a separate coarse "fast navigate" level.
 */
static NodeIndexCounts count_node_indices(const Object &object,
                                          const bke::pbvh::Tree &pbvh,
                                          const OrigMeshData &orig_mesh_data,
                                          const int node_index)
{
  NodeIndexCounts counts;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const bke::pbvh::MeshNode &node = pbvh.nodes<bke::pbvh::MeshNode>()[node_index];
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int> face_indices = node.faces();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      int visible_corners_num = 0;
      int visible_tris_num = 0;
      if (hide_poly.is_empty()) {
        visible_corners_num = node.corners_num();
        visible_tris_num = poly_to_tri_count(face_indices.size(), node.corners_num());
      }
      else {
        for (const int face : face_indices) {
          if (hide_poly[face]) {
            continue;
          }
          const int face_size = faces[face].size();
          visible_corners_num += face_size;
          visible_tris_num += bke::mesh::face_triangles_num(face_size);
        }
      }
      counts.lines = counts.lines_coarse = visible_corners_num * 2;
      counts.tris = counts.tris_coarse = visible_tris_num * 3;
      break;
    }
    case bke::pbvh::Type::Grids: {
      const bke::pbvh::GridsNode &node = pbvh.nodes<bke::pbvh::GridsNode>()[node_index];
      const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const int gridsize = key.grid_size;
      const int totgrid = node.grids().size();

      /* Full resolution. */
      const int quads_per_axis = gridsize - 1;
      counts.lines = 4 * totgrid * quads_per_axis * (quads_per_axis + 1);
      counts.tris = bke::pbvh::count_grid_quads(
                        subdiv_ccg.grid_hidden, node.grids(), gridsize, gridsize) *
                    6;

      /* Coarse "fast navigate" level (level 0). */
      const int skip = gridsize > 2 ? 1 << (key.level - 1) : 1;
      const int coarse_quads_per_axis = (gridsize - 1) / skip;
      counts.lines_coarse = 4 * totgrid * coarse_quads_per_axis * (coarse_quads_per_axis + 1);
      counts.tris_coarse = bke::pbvh::count_grid_quads(
                               subdiv_ccg.grid_hidden, node.grids(), gridsize, 2) *
                           6;
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const bke::pbvh::BMeshNode &node = pbvh.nodes<bke::pbvh::BMeshNode>()[node_index];
      const int visible_tris_num = count_visible_tris_bmesh(node.bm_faces_);
      counts.lines = counts.lines_coarse = visible_tris_num * 3 * 2;
      counts.tris = counts.tris_coarse = visible_tris_num * 3;
      break;
    }
  }
  return counts;
}

DrawCacheImpl::~DrawCacheImpl()
{
  GPU_BATCH_DISCARD_SAFE(lines_batches_.shared);
  GPU_BATCH_DISCARD_SAFE(lines_batches_coarse_.shared);
  for (BatchCache &batch_cache : tris_batches_.values()) {
    GPU_BATCH_DISCARD_SAFE(batch_cache.shared);
  }
}

void DrawCacheImpl::free_nodes_with_changed_topology(const bke::pbvh::Tree &pbvh)
{
  PRF_scope(ProfileCategory::Draw);
  /* NOTE: Theoretically we shouldn't need to free batches with a changed triangle count, but
   * currently it's the simplest way to reallocate all the GPU data while keeping everything in a
   * consistent state. */
  IndexMaskMemory memory;
  const IndexMask nodes_to_free = IndexMask::from_bits(dirty_topology_, memory);
  if (nodes_to_free.is_empty()) {
    return;
  }

  dirty_topology_.clear_and_shrink();

  free_ibos(ibo_lines_.ibos, nodes_to_free);
  free_ibos(ibo_lines_coarse_.ibos, nodes_to_free);
  free_ibos(ibo_tris_.ibos, nodes_to_free);
  free_ibos(ibo_tris_coarse_.ibos, nodes_to_free);

  if (pbvh.type() == bke::pbvh::Type::BMesh) {
    /* For BMesh, the VBOs are filled with data for all visible triangles, and the topology can
     * also completely change due to dynamic topology, so the attribute data must be rebuilt from
     * scratch. For other types, actual topology doesn't change, and visibility changes are
     * accounted for by the index buffers. */
    for (AttributeData &data : attribute_vbos_.values()) {
      data.tag_dirty(nodes_to_free);
    }
  }
}

void DrawCacheImpl::AttributeData::ensure_nodes_capacity(const int64_t nodes_len)
{
  this->dirty_nodes.resize(nodes_len, true);
}
void DrawCacheImpl::AttributeData::ensure_buffer_capacity(const GPUVertFormat &format,
                                                          const uint64_t needed_vertex_len)
{
  /* Check if existing VBO needs to be resized. */
  if (vbo) {
    if (vbo->vertex_len < needed_vertex_len) {
      gpu::VertBufPtr new_vbo = gpu::VertBufPtr(
          GPU_vertbuf_create_with_format_ex(format, GPU_USAGE_DYNAMIC));
      new_vbo->allocate(uint32_t(needed_vertex_len));
      new_vbo->upload();
      /* Preserve the data of the existing vertices by copying the used vertex data to the new
       * buffer. The source buffer is guaranteed to be uploaded by the caller. */
      new_vbo->copy_sub(*vbo, 0, 0, vbo->vertex_len);
      vbo.swap(new_vbo);
    }
  }
  else {
    /* No VBO allocated yet. */
    vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format_ex(format, GPU_USAGE_DYNAMIC));
    vbo->allocate(needed_vertex_len);
    vbo->upload();
  }
}

void DrawCacheImpl::AttributeData::update(const NodeInfo &node_info)
{
  gpu::VertBuf *vbo = this->vbo.get();
  /* Nodes with no visible geometry (e.g. fully hidden BMesh nodes after dyntopo) don't have any
   * space assigned to them. Their buffer range is invalid (`offset = -1, len = 0`), so there is
   * nothing to upload and the derived byte offsets would be out of bounds. */
  if (node_info.vbo.len <= 0 || !node_info.vbo.is_space_acquired) {
    return;
  }
  /* The VBO can be missing or smaller than the buffer space if the attribute's update function
   * skipped resizing it. Never upload out of bounds ranges. */
  if (vbo == nullptr ||
      uint64_t(node_info.vbo.offset) + uint64_t(node_info.vbo.len) > uint64_t(vbo->vertex_alloc))
  {
    return;
  }
  GPU_vertbuf_use(vbo);
  uint8_t *data = vbo->data<uint8_t>().data();
  data += node_info.vbo.offset * vbo->format.stride;
  vbo->update_sub(
      node_info.vbo.offset * vbo->format.stride, node_info.vbo.len * vbo->format.stride, data);
}

static void update_positions_mesh(const Object &object,
                                  const DrawCacheImpl &draw_cache,
                                  DrawCacheImpl::AttributeData &attr_vbo,
                                  const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval_from_eval(object);
  attr_vbo.ensure_buffer_capacity(position_format(), draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        extract_data_vert_mesh<float3>(
            faces, corner_verts, vert_positions, nodes[i].faces(), *attr_vbo.vbo, node_info);
      },
      exec_mode::grain_size(1));
}

static void update_normals_mesh(const Object &object,
                                const DrawCacheImpl &draw_cache,
                                DrawCacheImpl::AttributeData &attr_vbo,
                                const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval_from_eval(object);
  const Span<float3> face_normals = bke::pbvh::face_normals_eval_from_eval(object);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
  attr_vbo.ensure_buffer_capacity(normal_format(), draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        short4 *data = attr_vbo.vbo->data<short4>().data() + node_info.vbo.offset;
        short4 *data_start = data;

        for (const int face : nodes[i].faces()) {
          if (!sharp_faces.is_empty() && sharp_faces[face]) {
            const int face_size = faces[face].size();
            std::fill_n(data, face_size, normal_float_to_short(face_normals[face]));
            data += face_size;
          }
          else {
            for (const int vert : corner_verts.slice(faces[face])) {
              *data = normal_float_to_short(vert_normals[vert]);
              data++;
            }
          }
        }
        UNUSED_VARS_NDEBUG(data_start);
        BLI_assert_msg(data == data_start + node_info.vbo.len,
                       "Node VBO data extraction did not fill the expected amount");
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_masks_mesh(const Object &object,
                                           const OrigMeshData &orig_mesh_data,
                                           const DrawCacheImpl &draw_cache,
                                           DrawCacheImpl::AttributeData &attr_vbo,
                                           const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const VArraySpan mask = *orig_mesh_data.attributes.lookup<float>(".sculpt_mask",
                                                                   bke::AttrDomain::Point);
  attr_vbo.ensure_buffer_capacity(mask_format(), draw_cache.get_vert_len());
  if (!mask.is_empty()) {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          float *data = attr_vbo.vbo->data<float>().data() + node_info.vbo.offset;
          float *data_start = data;
          for (const int face : nodes[i].faces()) {
            for (const int vert : corner_verts.slice(faces[face])) {
              *data = mask[vert];
              data++;
            }
          }
          UNUSED_VARS_NDEBUG(data_start);
          BLI_assert_msg(data == data_start + node_info.vbo.len,
                         "Node VBO data extraction did not fill the expected amount");
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<float>().slice(node_info.vbo.index_range()).fill(0.0f);
        },
        exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_face_sets_mesh(const Object &object,
                                               const OrigMeshData &orig_mesh_data,
                                               const DrawCacheImpl &draw_cache,
                                               DrawCacheImpl::AttributeData &attr_vbo,
                                               const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const VArraySpan face_sets = *orig_mesh_data.attributes.lookup<int>(".sculpt_face_set",
                                                                      bke::AttrDomain::Face);
  attr_vbo.ensure_buffer_capacity(face_set_format(), draw_cache.get_vert_len());
  if (!face_sets.is_empty()) {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          uchar4 *data = attr_vbo.vbo->data<uchar4>().data() + node_info.vbo.offset;
          uchar4 *data_start = data;
          for (const int face : nodes[i].faces()) {
            const int id = face_sets[face];

            uchar4 fset_color(UCHAR_MAX);
            if (id != color_default) {
              BKE_paint_face_set_overlay_color_get(id, color_seed, fset_color);
            }
            else {
              /* Skip for the default color face set to render it white. */
              fset_color[0] = fset_color[1] = fset_color[2] = UCHAR_MAX;
            }

            const int face_size = faces[face].size();
            std::fill_n(data, face_size, fset_color);
            data += face_size;
          }
          UNUSED_VARS_NDEBUG(data_start);
          BLI_assert_msg(data == data_start + node_info.vbo.len,
                         "Node VBO data extraction did not fill the expected amount");
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<uchar4>().slice(node_info.vbo.index_range()).fill(uchar4(255));
        },
        exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_generic_attribute_mesh(const Object &object,
                                                       const OrigMeshData &orig_mesh_data,
                                                       const DrawCacheImpl &draw_cache,
                                                       DrawCacheImpl::AttributeData &attr_vbo,
                                                       const StringRef name,
                                                       const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const bke::GAttributeReader attr = attributes.lookup(name);
  if (!attr || attr.domain == bke::AttrDomain::Edge) {
    return;
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(attr.varray.type());
  attr_vbo.ensure_buffer_capacity(attribute_format(orig_mesh_data, name, data_type),
                                  draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        bke::attribute_math::to_static_type(attr.varray.type(), [&]<typename T>() {
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            const VArraySpan<T> src = attr.varray.typed<T>();
            switch (attr.domain) {
              case bke::AttrDomain::Point:
                extract_data_vert_mesh<T>(
                    faces, corner_verts, src, nodes[i].faces(), *attr_vbo.vbo, node_info);
                break;
              case bke::AttrDomain::Face:
                extract_data_face_mesh<T>(faces, src, nodes[i].faces(), *attr_vbo.vbo, node_info);
                break;
              case bke::AttrDomain::Corner:
                extract_data_corner_mesh<T>(
                    faces, src, nodes[i].faces(), *attr_vbo.vbo, node_info);
                break;
              default:
                BLI_assert_unreachable();
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_positions_grids(const Object &object,
                                                const DrawCacheImpl &draw_cache,
                                                DrawCacheImpl::AttributeData &attr_vbo,
                                                const BitSpan use_flat_layout,
                                                const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const Span<float3> positions = subdiv_ccg.positions;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  attr_vbo.ensure_buffer_capacity(position_format(), draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        float3 *data = attr_vbo.vbo->data<float3>().data() + node_info.vbo.offset;
        float3 *data_start = data;
        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            for (int y = 0; y < grid_size_1; y++) {
              for (int x = 0; x < grid_size_1; x++) {
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y + 1)];
                data++;
              }
            }
          }
        }
        else {
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            std::copy_n(grid_positions.data(), grid_positions.size(), data);
            data += grid_positions.size();
          }
        }
        UNUSED_VARS_NDEBUG(data_start);
        BLI_assert_msg(data == data_start + node_info.vbo.len,
                       "Node VBO data extraction did not fill the expected amount");
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_normals_grids(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const DrawCacheImpl &draw_cache,
                                              DrawCacheImpl::AttributeData &attr_vbo,
                                              const BitSpan use_flat_layout,
                                              const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const Span<float3> positions = subdiv_ccg.positions;
  const Span<float3> normals = subdiv_ccg.normals;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
  attr_vbo.ensure_buffer_capacity(normal_format(), draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        short4 *data = attr_vbo.vbo->data<short4>().data() + node_info.vbo.offset;
        short4 *data_start = data;

        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            const Span<float3> grid_normals = normals.slice(bke::ccg::grid_range(key, grid));
            if (!sharp_faces.is_empty() && sharp_faces[grid_to_face_map[grid]]) {
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  float3 no;
                  normal_quad_v3(no,
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y + 1)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y)]);
                  std::fill_n(data, 4, normal_float_to_short(no));
                  data += 4;
                }
              }
            }
            else {
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  std::fill_n(data,
                              4,
                              normal_float_to_short(
                                  grid_normals[CCG_grid_xy_to_index(key.grid_size, x, y)]));
                  data += 4;
                }
              }
            }
          }
        }
        else {
          /* The non-flat VBO layout does not support sharp faces. */
          for (const int grid : nodes[i].grids()) {
            for (const float3 &normal : normals.slice(bke::ccg::grid_range(key, grid))) {
              *data = normal_float_to_short(normal);
              data++;
            }
          }
        }
        UNUSED_VARS_NDEBUG(data_start);
        BLI_assert_msg(data == data_start + node_info.vbo.len,
                       "Node VBO data extraction did not fill the expected amount");
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_masks_grids(const Object &object,
                                            const DrawCacheImpl &draw_cache,
                                            DrawCacheImpl::AttributeData &attr_vbo,
                                            const BitSpan use_flat_layout,
                                            const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float> masks = subdiv_ccg.masks;
  attr_vbo.ensure_buffer_capacity(mask_format(), draw_cache.get_vert_len());
  if (!masks.is_empty()) {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          float *data = attr_vbo.vbo->data<float>().data() + node_info.vbo.offset;
          float *data_start = data;
          if (use_flat_layout[i]) {
            const int grid_size_1 = key.grid_size - 1;
            for (const int grid : nodes[i].grids()) {
              const Span<float> grid_masks = masks.slice(bke::ccg::grid_range(key, grid));
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y)];
                  data++;
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y)];
                  data++;
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)];
                  data++;
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y + 1)];
                  data++;
                }
              }
            }
          }
          else {
            for (const int grid : nodes[i].grids()) {
              const Span<float> grid_masks = masks.slice(bke::ccg::grid_range(key, grid));
              std::copy_n(grid_masks.data(), grid_masks.size(), data);
              data += grid_masks.size();
            }
          }
          UNUSED_VARS_NDEBUG(data_start);
          BLI_assert_msg(data == data_start + node_info.vbo.len,
                         "Node VBO data extraction did not fill the expected amount");
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<float>().slice(node_info.vbo.index_range()).fill(0.0f);
        },
        exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_face_sets_grids(const Object &object,
                                                const OrigMeshData &orig_mesh_data,
                                                const DrawCacheImpl &draw_cache,
                                                DrawCacheImpl::AttributeData &attr_vbo,
                                                const BitSpan use_flat_layout,
                                                const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  attr_vbo.ensure_buffer_capacity(face_set_format(), draw_cache.get_vert_len());
  if (const VArray<int> face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                            bke::AttrDomain::Face))
  {
    const VArraySpan<int> face_sets_span(face_sets);
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          const Span<int> grids = nodes[i].grids();
          const int verts_per_grid = use_flat_layout[i] ? square_i(key.grid_size - 1) * 4 :
                                                          square_i(key.grid_size);
          uchar4 *data = attr_vbo.vbo->data<uchar4>().data() + node_info.vbo.offset;
          uchar4 *data_start = data;
          for (const int j : grids.index_range()) {
            uchar4 color{UCHAR_MAX};
            const int fset = face_sets[grid_to_face_map[grids[j]]];
            if (fset != color_default) {
              BKE_paint_face_set_overlay_color_get(fset, color_seed, color);
            }

            std::fill_n(data, verts_per_grid, color);
            data += verts_per_grid;
          }
          UNUSED_VARS_NDEBUG(data_start);
          BLI_assert_msg(data == data_start + node_info.vbo.len,
                         "Node VBO data extraction did not fill the expected amount");
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<uchar4>().slice(node_info.vbo.index_range()).fill(uchar4{UCHAR_MAX});
        },
        exec_mode::grain_size(1));
  }
}

static void calc_quad_uv(const SubdivCCG &subdiv_ccg,
                         const int uv_map_index,
                         const float grid_size_1_inv,
                         const int ptex_face_index,
                         const int grid_start,
                         const int corner,
                         const int x,
                         const int y,
                         const int offset,
                         MutableSpan<float2> result)
{
  const float grid_u = x * grid_size_1_inv;
  const float grid_v = y * grid_size_1_inv;
  float u;
  float v;
  bke::subdiv::rotate_grid_to_quad(corner, grid_u, grid_v, &u, &v);
  bke::subdiv::eval_face_varying(
      subdiv_ccg.subdiv, uv_map_index, ptex_face_index, u, v, result[grid_start + offset]);
}

static void calc_non_quad_uv(const SubdivCCG &subdiv_ccg,
                             const int uv_map_index,
                             const float grid_size_1_inv,
                             const int ptex_face_index,
                             const int grid_start,
                             const int x,
                             const int y,
                             const int offset,
                             MutableSpan<float2> result)
{
  const float u = 1.0f - (y * grid_size_1_inv);
  const float v = 1.0f - (x * grid_size_1_inv);
  bke::subdiv::eval_face_varying(
      subdiv_ccg.subdiv, uv_map_index, ptex_face_index, u, v, result[grid_start + offset]);
}

static void calc_node_uvs_for_flat_layout(const SubdivCCG &subdiv_ccg,
                                          const int uv_map_index,
                                          Span<int> grids,
                                          MutableSpan<float2> result)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int grid_size_1 = key.grid_size - 1;
  const int grid_area_1 = grid_size_1 * grid_size_1;
  const int verts_per_grid = grid_area_1 * 4;

  const Span<int> face_ptex_offset = bke::subdiv::face_ptex_offset_get(subdiv_ccg.subdiv);
  const float grid_size_1_inv = 1.0f / (key.grid_size - 1);

  threading::parallel_for(grids.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const int grid = grids[i];
      const int face_index = subdiv_ccg.grid_to_face_map[grid];
      const IndexRange face = subdiv_ccg.faces[face_index];
      const int corner = grid - face.start();
      if (face.size() == 4) {
        const int ptex_face_index = face_ptex_offset[face_index];
        for (int y = 0; y < grid_size_1; y++) {
          for (int x = 0; x < grid_size_1; x++) {
            const int start = verts_per_grid * i + CCG_grid_xy_to_index(grid_size_1, x, y) * 4;
            calc_quad_uv(subdiv_ccg,
                         uv_map_index,
                         grid_size_1_inv,
                         ptex_face_index,
                         start,
                         corner,
                         x,
                         y,
                         0,
                         result);
            calc_quad_uv(subdiv_ccg,
                         uv_map_index,
                         grid_size_1_inv,
                         ptex_face_index,
                         start,
                         corner,
                         x + 1,
                         y,
                         1,
                         result);
            calc_quad_uv(subdiv_ccg,
                         uv_map_index,
                         grid_size_1_inv,
                         ptex_face_index,
                         start,
                         corner,
                         x + 1,
                         y + 1,
                         2,
                         result);
            calc_quad_uv(subdiv_ccg,
                         uv_map_index,
                         grid_size_1_inv,
                         ptex_face_index,
                         start,
                         corner,
                         x,
                         y + 1,
                         3,
                         result);
          }
        }
      }
      else {
        const int ptex_face_index = face_ptex_offset[face_index] + corner;
        for (int y = 0; y < grid_size_1; y++) {
          for (int x = 0; x < grid_size_1; x++) {
            const int start = verts_per_grid * i + CCG_grid_xy_to_index(grid_size_1, x, y) * 4;
            calc_non_quad_uv(subdiv_ccg,
                             uv_map_index,
                             grid_size_1_inv,
                             ptex_face_index,
                             start,
                             x,
                             y,
                             0,
                             result);
            calc_non_quad_uv(subdiv_ccg,
                             uv_map_index,
                             grid_size_1_inv,
                             ptex_face_index,
                             start,
                             x + 1,
                             y,
                             1,
                             result);
            calc_non_quad_uv(subdiv_ccg,
                             uv_map_index,
                             grid_size_1_inv,
                             ptex_face_index,
                             start,
                             x + 1,
                             y + 1,
                             2,
                             result);
            calc_non_quad_uv(subdiv_ccg,
                             uv_map_index,
                             grid_size_1_inv,
                             ptex_face_index,
                             start,
                             x,
                             y + 1,
                             3,
                             result);
          }
        }
      }
    }
  });
}

static void calc_node_uvs_for_standard_layout(const SubdivCCG &subdiv_ccg,
                                              const int uv_map_index,
                                              Span<int> grids,
                                              MutableSpan<float2> result)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int grid_size = key.grid_size;
  const int grid_area = key.grid_area;

  const Span<int> face_ptex_offset = bke::subdiv::face_ptex_offset_get(subdiv_ccg.subdiv);
  const float grid_size_1_inv = 1.0f / (grid_size - 1);
  threading::parallel_for(grids.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const int grid = grids[i];
      const int face_index = subdiv_ccg.grid_to_face_map[grid];
      const IndexRange face = subdiv_ccg.faces[face_index];
      const int corner = grid - face.start();
      if (face.size() == 4) {
        const int ptex_face_index = face_ptex_offset[face_index];
        for (int y = 0; y < grid_size; y++) {
          const float grid_v = y * grid_size_1_inv;
          for (int x = 0; x < grid_size; x++) {
            const float grid_u = x * grid_size_1_inv;
            float u;
            float v;
            bke::subdiv::rotate_grid_to_quad(corner, grid_u, grid_v, &u, &v);
            const int element = i * grid_area + CCG_grid_xy_to_index(grid_size, x, y);
            bke::subdiv::eval_face_varying(
                subdiv_ccg.subdiv, uv_map_index, ptex_face_index, u, v, result[element]);
          }
        }
      }
      else {
        const int ptex_face_index = face_ptex_offset[face_index] + corner;
        for (int y = 0; y < grid_size; y++) {
          const float u = 1.0f - (y * grid_size_1_inv);
          for (int x = 0; x < grid_size; x++) {
            const float v = 1.0f - (x * grid_size_1_inv);
            const int element = i * grid_area + CCG_grid_xy_to_index(grid_size, x, y);
            bke::subdiv::eval_face_varying(
                subdiv_ccg.subdiv, uv_map_index, ptex_face_index, u, v, result[element]);
          }
        }
      }
    }
  });
}

static void calc_node_uvs(const SubdivCCG &subdiv_ccg,
                          const int uv_map_index,
                          const bke::pbvh::GridsNode &node,
                          const bool use_flat_layout,
                          MutableSpan<float2> result)
{
  const Span<int> grids = node.grids();
  if (use_flat_layout) {
    calc_node_uvs_for_flat_layout(subdiv_ccg, uv_map_index, grids, result);
  }
  if (!use_flat_layout) {
    calc_node_uvs_for_standard_layout(subdiv_ccg, uv_map_index, grids, result);
  }
}

BLI_NOINLINE static void fill_uvs_grids(const Object &object,
                                        const OrigMeshData &orig_mesh_data,
                                        const DrawCacheImpl &draw_cache,
                                        DrawCacheImpl::AttributeData &attr_vbo,
                                        const BitSpan use_flat_layout,
                                        const IndexMask &node_mask,
                                        const StringRef name)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  attr_vbo.ensure_buffer_capacity(attribute_format(orig_mesh_data, name, bke::AttrType::Float2),
                                  draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        float2 *data = attr_vbo.vbo->data<float2>().data() + node_info.vbo.offset;

        const int uv_channel = orig_mesh_data.uv_map_names.index_of_try(name);
        if (uv_channel == -1) [[unlikely]] {
          BLI_assert_msg(uv_channel != -1,
                         "Unable to find appropriate FVar channel for given UV map");
          std::fill_n(data, node_info.vbo.len, float2(0));
        }
        else {
          calc_node_uvs(
              subdiv_ccg, uv_channel, nodes[i], use_flat_layout[i], {data, node_info.vbo.len});
        }
      },
      exec_mode::grain_size(1));
}

static float4 quad_weights_from_grid(const int corner,
                                     const int x,
                                     const int y,
                                     const float grid_size_1_inv)
{
  const float grid_u = x * grid_size_1_inv;
  const float grid_v = y * grid_size_1_inv;
  float u;
  float v;
  bke::subdiv::rotate_grid_to_quad(corner, grid_u, grid_v, &u, &v);
  return bke::subdiv::quad_weights_from_uv(u, v);
}

static float4 quad_weights_from_irregular_corner(const int x,
                                                 const int y,
                                                 const float grid_size_1_inv)
{
  const float u = 1.0f - (y * grid_size_1_inv);
  const float v = 1.0f - (x * grid_size_1_inv);

  return bke::subdiv::quad_weights_from_uv(u, v);
}

template<typename T>
static void interpolate_vert_data_for_flat_layout(
    const SubdivCCG &subdiv_ccg,
    const Span<int> corner_verts,
    const Span<int> grids,
    const Span<T> src,
    MutableSpan<typename AttributeConverter<T>::VBOType> vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int grid_size_1 = key.grid_size - 1;
  const int grid_area_1 = grid_size_1 * grid_size_1;
  const int verts_per_grid = grid_area_1 * 4;
  const float grid_size_1_inv = 1.0f / (key.grid_size - 1);
  MutableSpan<VBOType> dst = vbo;

  threading::parallel_for(grids.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const int grid = grids[i];
      const int face_index = subdiv_ccg.grid_to_face_map[grid];
      const IndexRange face = subdiv_ccg.faces[face_index];
      const int corner = grid - face.start();
      if (face.size() == 4) {
        Span<int> face_verts = corner_verts.slice(face);
        for (int y = 0; y < grid_size_1; y++) {
          for (int x = 0; x < grid_size_1; x++) {
            const int start = verts_per_grid * i + CCG_grid_xy_to_index(grid_size_1, x, y) * 4;
            dst[start] = Converter::convert(
                bke::attribute_math::mix4(quad_weights_from_grid(corner, x, y, grid_size_1_inv),
                                          src[face_verts[0]],
                                          src[face_verts[1]],
                                          src[face_verts[2]],
                                          src[face_verts[3]]));
            dst[start + 1] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_grid(corner, x + 1, y, grid_size_1_inv),
                src[face_verts[0]],
                src[face_verts[1]],
                src[face_verts[2]],
                src[face_verts[3]]));
            dst[start + 2] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_grid(corner, x + 1, y + 1, grid_size_1_inv),
                src[face_verts[0]],
                src[face_verts[1]],
                src[face_verts[2]],
                src[face_verts[3]]));
            dst[start + 3] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_grid(corner, x, y + 1, grid_size_1_inv),
                src[face_verts[0]],
                src[face_verts[1]],
                src[face_verts[2]],
                src[face_verts[3]]));
          }
        }
      }
      else {
        const std::array<T, 4> values =
            bke::subdiv::interpolate_vert_data_for_subdivided_irregular_face<T>(
                corner_verts, face, grid, src);
        for (int y = 0; y < grid_size_1; y++) {
          for (int x = 0; x < grid_size_1; x++) {
            const int start = verts_per_grid * i + CCG_grid_xy_to_index(grid_size_1, x, y) * 4;
            dst[start] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x, y, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
            dst[start + 1] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x + 1, y, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
            dst[start + 2] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x + 1, y + 1, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
            dst[start + 3] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x, y + 1, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
          }
        }
      }
    }
  });
}

template<typename T>
static void interpolate_vert_data_for_standard_layout(
    const SubdivCCG &subdiv_ccg,
    const Span<int> corner_verts,
    const Span<int> grids,
    const Span<T> src,
    MutableSpan<typename AttributeConverter<T>::VBOType> vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int grid_size = key.grid_size;
  const int grid_area = key.grid_area;

  const float grid_size_1_inv = 1.0f / (grid_size - 1);
  MutableSpan<VBOType> dst = vbo;
  threading::parallel_for(grids.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const int grid = grids[i];
      const int face_index = subdiv_ccg.grid_to_face_map[grid];
      const IndexRange face = subdiv_ccg.faces[face_index];
      const int corner = grid - face.start();
      if (face.size() == 4) {
        const Span<int> face_verts = corner_verts.slice(face);
        for (int y = 0; y < grid_size; y++) {
          for (int x = 0; x < grid_size; x++) {
            const int element = i * grid_area + CCG_grid_xy_to_index(grid_size, x, y);
            dst[element] = Converter::convert(
                bke::attribute_math::mix4(quad_weights_from_grid(corner, x, y, grid_size_1_inv),
                                          src[face_verts[0]],
                                          src[face_verts[1]],
                                          src[face_verts[2]],
                                          src[face_verts[3]]));
          }
        }
      }
      else {
        const std::array<T, 4> values =
            bke::subdiv::interpolate_vert_data_for_subdivided_irregular_face<T>(
                corner_verts, face, grid, src);
        for (int y = 0; y < grid_size; y++) {
          for (int x = 0; x < grid_size; x++) {
            const int element = i * grid_area + CCG_grid_xy_to_index(grid_size, x, y);
            dst[element] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x, y, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
          }
        }
      }
    }
  });
}

BLI_NOINLINE static void calc_generic_vert_attribute_grids(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           const DrawCacheImpl &draw_cache,
                                                           DrawCacheImpl::AttributeData &attr_vbo,
                                                           const BitSpan use_flat_layout,
                                                           const IndexMask &node_mask,
                                                           const StringRef name)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const GVArraySpan attr = *attributes.lookup(name);

  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const Span<int> corner_verts = mesh.corner_verts();

  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        bke::attribute_math::to_static_type(attr.type(), [&]<typename T>() {
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            MutableSpan<typename AttributeConverter<T>::VBOType> dst =
                attr_vbo.vbo->data<typename AttributeConverter<T>::VBOType>().slice(
                    node_info.vbo.index_range());
            if (use_flat_layout[i]) {
              interpolate_vert_data_for_flat_layout<T>(
                  subdiv_ccg, corner_verts, nodes[i].grids(), attr.typed<T>(), dst);
            }
            else {
              interpolate_vert_data_for_standard_layout<T>(
                  subdiv_ccg, corner_verts, nodes[i].grids(), attr.typed<T>(), dst);
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

template<typename T>
static void interpolate_corner_data_for_flat_layout(
    const SubdivCCG &subdiv_ccg,
    const Span<int> grids,
    const Span<T> src,
    MutableSpan<typename AttributeConverter<T>::VBOType> vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int grid_size_1 = key.grid_size - 1;
  const int grid_area_1 = grid_size_1 * grid_size_1;
  const int verts_per_grid = grid_area_1 * 4;
  const float grid_size_1_inv = 1.0f / (key.grid_size - 1);
  MutableSpan<VBOType> dst = vbo;

  threading::parallel_for(grids.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const int grid = grids[i];
      const int face_index = subdiv_ccg.grid_to_face_map[grid];
      const IndexRange face = subdiv_ccg.faces[face_index];
      const int corner = grid - face.start();
      if (face.size() == 4) {
        for (int y = 0; y < grid_size_1; y++) {
          for (int x = 0; x < grid_size_1; x++) {
            const int start = verts_per_grid * i + CCG_grid_xy_to_index(grid_size_1, x, y) * 4;
            dst[start] = Converter::convert(
                bke::attribute_math::mix4(quad_weights_from_grid(corner, x, y, grid_size_1_inv),
                                          src[face[0]],
                                          src[face[1]],
                                          src[face[2]],
                                          src[face[3]]));
            dst[start + 1] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_grid(corner, x + 1, y, grid_size_1_inv),
                src[face[0]],
                src[face[1]],
                src[face[2]],
                src[face[3]]));
            dst[start + 2] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_grid(corner, x + 1, y + 1, grid_size_1_inv),
                src[face[0]],
                src[face[1]],
                src[face[2]],
                src[face[3]]));
            dst[start + 3] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_grid(corner, x, y + 1, grid_size_1_inv),
                src[face[0]],
                src[face[1]],
                src[face[2]],
                src[face[3]]));
          }
        }
      }
      else {
        const std::array<T, 4> values =
            bke::subdiv::interpolate_corner_data_for_subdivided_irregular_face<T>(face, grid, src);
        for (int y = 0; y < grid_size_1; y++) {
          for (int x = 0; x < grid_size_1; x++) {
            const int start = verts_per_grid * i + CCG_grid_xy_to_index(grid_size_1, x, y) * 4;
            dst[start] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x, y, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
            dst[start + 1] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x + 1, y, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
            dst[start + 2] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x + 1, y + 1, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
            dst[start + 3] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x, y + 1, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
          }
        }
      }
    }
  });
}

template<typename T>
static void interpolate_corner_data_for_standard_layout(
    const SubdivCCG &subdiv_ccg,
    const Span<int> grids,
    const Span<T> src,
    MutableSpan<typename AttributeConverter<T>::VBOType> vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int grid_size = key.grid_size;
  const int grid_area = key.grid_area;

  const float grid_size_1_inv = 1.0f / (grid_size - 1);
  MutableSpan<VBOType> dst = vbo;
  threading::parallel_for(grids.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const int grid = grids[i];
      const int face_index = subdiv_ccg.grid_to_face_map[grid];
      const IndexRange face = subdiv_ccg.faces[face_index];
      const int corner = grid - face.start();
      if (face.size() == 4) {
        for (int y = 0; y < grid_size; y++) {
          for (int x = 0; x < grid_size; x++) {
            const int element = i * grid_area + CCG_grid_xy_to_index(grid_size, x, y);
            dst[element] = Converter::convert(
                bke::attribute_math::mix4(quad_weights_from_grid(corner, x, y, grid_size_1_inv),
                                          src[face[0]],
                                          src[face[1]],
                                          src[face[2]],
                                          src[face[3]]));
          }
        }
      }
      else {
        const std::array<T, 4> values =
            bke::subdiv::interpolate_corner_data_for_subdivided_irregular_face<T>(face, grid, src);
        for (int y = 0; y < grid_size; y++) {
          for (int x = 0; x < grid_size; x++) {
            const int element = i * grid_area + CCG_grid_xy_to_index(grid_size, x, y);
            dst[element] = Converter::convert(bke::attribute_math::mix4(
                quad_weights_from_irregular_corner(x, y, grid_size_1_inv),
                values[0],
                values[1],
                values[2],
                values[3]));
          }
        }
      }
    }
  });
}

BLI_NOINLINE static void calc_generic_corner_attribute_grids(
    const Object &object,
    const OrigMeshData &orig_mesh_data,
    const DrawCacheImpl &draw_cache,
    DrawCacheImpl::AttributeData &attr_vbo,
    const BitSpan use_flat_layout,
    const IndexMask &node_mask,
    const StringRef name)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const GVArraySpan attr = *attributes.lookup(name);

  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        bke::attribute_math::to_static_type(attr.type(), [&]<typename T>() {
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            MutableSpan<typename AttributeConverter<T>::VBOType> dst =
                attr_vbo.vbo->data<typename AttributeConverter<T>::VBOType>().slice(
                    node_info.vbo.index_range());
            if (use_flat_layout[i]) {
              interpolate_corner_data_for_flat_layout<T>(
                  subdiv_ccg, nodes[i].grids(), attr.typed<T>(), dst);
            }
            else {
              interpolate_corner_data_for_standard_layout<T>(
                  subdiv_ccg, nodes[i].grids(), attr.typed<T>(), dst);
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

template<typename T>
static void fill_face_data_grids(const SubdivCCG &subdiv_ccg,
                                 const bool use_flat_layout,
                                 const Span<int> grids,
                                 const Span<T> src,
                                 MutableSpan<typename AttributeConverter<T>::VBOType> vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int verts_per_grid = use_flat_layout ? (key.grid_size - 1) * (key.grid_size - 1) * 4 :
                                               key.grid_area;
  VBOType *dst = vbo.data();
  for (const int i : grids.index_range()) {
    const int face_index = subdiv_ccg.grid_to_face_map[grids[i]];
    std::fill_n(dst, verts_per_grid, Converter::convert(src[face_index]));
    dst += verts_per_grid;
  }
}

BLI_NOINLINE static void fill_generic_face_attribute_grids(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           const DrawCacheImpl &draw_cache,
                                                           DrawCacheImpl::AttributeData &attr_vbo,
                                                           const BitSpan use_flat_layout,
                                                           const IndexMask &node_mask,
                                                           const StringRef name)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const GVArraySpan attr = *attributes.lookup(name);

  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        bke::attribute_math::to_static_type(attr.type(), [&]<typename T>() {
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            MutableSpan<typename AttributeConverter<T>::VBOType> dst =
                attr_vbo.vbo->data<typename AttributeConverter<T>::VBOType>().slice(
                    node_info.vbo.index_range());
            fill_face_data_grids<T>(
                subdiv_ccg, use_flat_layout[i], nodes[i].grids(), attr.typed<T>(), dst);
          }
        });
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_generic_attribute_grids(const Object &object,
                                                        const OrigMeshData &orig_mesh_data,
                                                        const DrawCacheImpl &draw_cache,
                                                        DrawCacheImpl::AttributeData &attr_vbo,
                                                        const StringRef name,
                                                        const BitSpan use_flat_layout,
                                                        const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const bke::GAttributeReader attr = attributes.lookup(name);
  if (attr.domain == bke::AttrDomain::Edge) {
    return;
  }

  if (!attr) {
    attr_vbo.ensure_buffer_capacity(
        attribute_format(orig_mesh_data, "Dummy", bke::AttrType::Float3),
        draw_cache.get_vert_len());
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<float3>().slice(node_info.vbo.index_range()).fill(float3(0.0f));
        },
        exec_mode::grain_size(1));
    return;
  }

  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(attr.varray.type());

  if (attr.domain == bke::AttrDomain::Corner && data_type == bke::AttrType::Float2) {
    fill_uvs_grids(object, orig_mesh_data, draw_cache, attr_vbo, use_flat_layout, node_mask, name);
    return;
  }

  switch (attr.domain) {
    case bke::AttrDomain::Point:
      attr_vbo.ensure_buffer_capacity(attribute_format(orig_mesh_data, name, data_type),
                                      draw_cache.get_vert_len());
      calc_generic_vert_attribute_grids(
          object, orig_mesh_data, draw_cache, attr_vbo, use_flat_layout, node_mask, name);
      break;
    case bke::AttrDomain::Corner:
      attr_vbo.ensure_buffer_capacity(attribute_format(orig_mesh_data, name, data_type),
                                      draw_cache.get_vert_len());
      calc_generic_corner_attribute_grids(
          object, orig_mesh_data, draw_cache, attr_vbo, use_flat_layout, node_mask, name);
      break;
    case bke::AttrDomain::Face:
      attr_vbo.ensure_buffer_capacity(attribute_format(orig_mesh_data, name, data_type),
                                      draw_cache.get_vert_len());
      fill_generic_face_attribute_grids(
          object, orig_mesh_data, draw_cache, attr_vbo, use_flat_layout, node_mask, name);
      break;
    default:
      BLI_assert_unreachable();
  }
}

BLI_NOINLINE static void update_positions_bmesh(const Object &object,
                                                const DrawCacheImpl &draw_cache,
                                                DrawCacheImpl::AttributeData &attr_vbo,
                                                const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  attr_vbo.ensure_buffer_capacity(position_format(), draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        float3 *data = attr_vbo.vbo->data<float3>().data() + node_info.vbo.offset;
        float3 *data_start = data;
        for (const BMFace *face :
             BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
        {
          if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
            continue;
          }
          const BMLoop *l = face->l_first;
          *data = l->prev->v->co;
          data++;
          *data = l->v->co;
          data++;
          *data = l->next->v->co;
          data++;
        }
        UNUSED_VARS_NDEBUG(data_start);
        BLI_assert_msg(data == data_start + node_info.vbo.len,
                       "Node VBO data extraction did not fill the expected amount");
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_normals_bmesh(const Object &object,
                                              const DrawCacheImpl &draw_cache,
                                              DrawCacheImpl::AttributeData &attr_vbo,
                                              const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  attr_vbo.ensure_buffer_capacity(normal_format(), draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        short4 *data = attr_vbo.vbo->data<short4>().data() + node_info.vbo.offset;
        short4 *data_start = data;
        for (const BMFace *face :
             BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
        {
          if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
            continue;
          }
          if (BM_elem_flag_test(face, BM_ELEM_SMOOTH)) {
            const BMLoop *l = face->l_first;
            *data = normal_float_to_short(l->prev->v->no);
            data++;
            *data = normal_float_to_short(l->v->no);
            data++;
            *data = normal_float_to_short(l->next->v->no);
            data++;
          }
          else {
            std::fill_n(data, 3, normal_float_to_short(face->no));
            data += 3;
          }
        }
        UNUSED_VARS_NDEBUG(data_start);
        BLI_assert_msg(data == data_start + node_info.vbo.len,
                       "Node VBO data extraction did not fill the expected amount");
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_masks_bmesh(const Object &object,
                                            const DrawCacheImpl &draw_cache,
                                            DrawCacheImpl::AttributeData &attr_vbo,
                                            const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const int cd_offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
  attr_vbo.ensure_buffer_capacity(mask_format(), draw_cache.get_vert_len());
  if (cd_offset != -1) {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          float *data = attr_vbo.vbo->data<float>().data() + node_info.vbo.offset;
          float *data_start = data;
          for (const BMFace *face :
               BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
          {
            if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
              continue;
            }
            const BMLoop *l = face->l_first;
            *data = bmesh_cd_vert_get<float>(*l->prev->v, cd_offset);
            data++;
            *data = bmesh_cd_vert_get<float>(*l->v, cd_offset);
            data++;
            *data = bmesh_cd_vert_get<float>(*l->next->v, cd_offset);
            data++;
          }
          UNUSED_VARS_NDEBUG(data_start);
          BLI_assert_msg(data == data_start + node_info.vbo.len,
                         "Node VBO data extraction did not fill the expected amount");
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<float>().slice(node_info.vbo.index_range()).fill(0.0f);
        },
        exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_face_sets_bmesh(const Object &object,
                                                const OrigMeshData &orig_mesh_data,
                                                const DrawCacheImpl &draw_cache,
                                                DrawCacheImpl::AttributeData &attr_vbo,
                                                const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const int offset = CustomData_get_offset_named(&bm.pdata, CD_PROP_INT32, ".sculpt_face_set");
  attr_vbo.ensure_buffer_capacity(face_set_format(), draw_cache.get_vert_len());
  if (offset != -1) {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          uchar4 *data = attr_vbo.vbo->data<uchar4>().data() + node_info.vbo.offset;
          uchar4 *data_start = data;
          for (const BMFace *face :
               BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
          {
            if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
              continue;
            }
            uchar4 color{UCHAR_MAX};
            const int fset = bmesh_cd_face_get<int>(*face, offset);
            if (fset != color_default) {
              BKE_paint_face_set_overlay_color_get(fset, color_seed, color);
            }
            std::fill_n(data, 3, color);
            data += 3;
          }
          UNUSED_VARS_NDEBUG(data_start);
          BLI_assert_msg(data == data_start + node_info.vbo.len,
                         "Node VBO data extraction did not fill the expected amount");
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<uchar4>().slice(node_info.vbo.index_range()).fill(uchar4(255));
        },
        exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_generic_attribute_bmesh(const Object &object,
                                                        const OrigMeshData &orig_mesh_data,
                                                        const DrawCacheImpl &draw_cache,
                                                        DrawCacheImpl::AttributeData &attr_vbo,
                                                        const StringRef name,
                                                        const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const BMDataLayerLookup attr = BM_data_layer_lookup(bm, name);
  if (attr.domain == bke::AttrDomain::Edge) {
    return;
  }

  if (!attr) {
    attr_vbo.ensure_buffer_capacity(attribute_format(orig_mesh_data, name, bke::AttrType::Float3),
                                    draw_cache.get_vert_len());
    node_mask.foreach_index(
        [&](const int i) {
          const NodeInfo &node_info = draw_cache.get_node_infos()[i];
          attr_vbo.vbo->data<float3>().slice(node_info.vbo.index_range()).fill(float3(0.0f));
        },
        exec_mode::grain_size(1));
    return;
  }

  attr_vbo.ensure_buffer_capacity(attribute_format(orig_mesh_data, name, attr.type),
                                  draw_cache.get_vert_len());
  node_mask.foreach_index(
      [&](const int i) {
        const NodeInfo &node_info = draw_cache.get_node_infos()[i];
        bke::attribute_math::to_static_type(attr.type, [&]<typename T>() {
          const auto &faces = BKE_pbvh_bmesh_node_faces(
              &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            switch (attr.domain) {
              case bke::AttrDomain::Point:
                extract_data_vert_bmesh<T>(faces, attr.offset, *attr_vbo.vbo, node_info);
                break;
              case bke::AttrDomain::Face:
                extract_data_face_bmesh<T>(faces, attr.offset, *attr_vbo.vbo, node_info);
                break;
              case bke::AttrDomain::Corner:
                extract_data_corner_bmesh<T>(faces, attr.offset, *attr_vbo.vbo, node_info);
                break;
              default:
                BLI_assert_unreachable();
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

static void create_lines_index_faces(const OffsetIndices<int> faces,
                                     const Span<bool> hide_poly,
                                     const Span<int> face_indices,
                                     const NodeInfo &node_info,
                                     const NodeInfo::BufferRange &ibo_info,
                                     uint32_t *host_data)
{
  int corners_count = 0;
  for (const int face : face_indices) {
    if (!hide_poly.is_empty() && hide_poly[face]) {
      continue;
    }
    corners_count += faces[face].size();
  }
  BLI_assert(uint(ibo_info.len) == 2 * corners_count);

  MutableSpan<uint2> data = MutableSpan<uint32_t>(host_data, ibo_info.len).cast<uint2>();

  int node_corner_offset = 0;
  int line_index = 0;
  for (const int face_index : face_indices) {
    const int face_size = faces[face_index].size();
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      node_corner_offset += face_size;
      continue;
    }
    for (const int i : IndexRange(face_size)) {
      const int next = (i == face_size - 1) ? 0 : i + 1;
      data[line_index] = uint2(i, next) + node_corner_offset + node_info.vbo.offset;
      line_index++;
    }

    node_corner_offset += face_size;
  }
}

/* NOTE: This is a temporary solution. The index buffers are only needed to offset the vertices
 * into the shared (per-object) vertex buffers. Once multi draw indirect is used (each draw can
 * specify its own base vertex) these index buffers can be removed. */
static void create_tri_index_bmesh(const Set<BMFace *, 0> &faces,
                                   const NodeInfo &node_info,
                                   const NodeInfo::BufferRange &ibo_info,
                                   uint32_t *host_data)
{
  MutableSpan<uint3> data = MutableSpan<uint32_t>(host_data, ibo_info.len).cast<uint3>();

  int tri_index = 0;
  int vert_index = node_info.vbo.offset;

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }

    data[tri_index] = uint3(vert_index, vert_index + 1, vert_index + 2);
    tri_index++;

    vert_index += 3;
  }
}

/* NOTE: Temporary solution, see the note on #create_tri_index_bmesh. */
static void create_lines_index_bmesh(const Set<BMFace *, 0> &faces,
                                     const NodeInfo &node_info,
                                     const NodeInfo::BufferRange &ibo_info,
                                     uint32_t *host_data)
{
  MutableSpan<uint2> data = MutableSpan<uint32_t>(host_data, ibo_info.len).cast<uint2>();

  int line_index = 0;
  int vert_index = node_info.vbo.offset;

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }

    data[line_index] = uint2(vert_index, vert_index + 1);
    line_index++;
    data[line_index] = uint2(vert_index + 1, vert_index + 2);
    line_index++;
    data[line_index] = uint2(vert_index + 2, vert_index);
    line_index++;

    vert_index += 3;
  }
}

static int create_tri_index_grids(const Span<int> grid_indices,
                                  const BitGroupVector<> &grid_hidden,
                                  const int gridsize,
                                  const int skip,
                                  const int totgrid,
                                  const NodeInfo &node_info,
                                  MutableSpan<uint3> data)
{
  int tri_index = 0;
  int offset = node_info.vbo.offset;
  const int grid_vert_len = gridsize * gridsize;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    uint v0, v1, v2, v3;

    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }
        /* Indices in a Clockwise QUAD disposition. */
        v0 = offset + CCG_grid_xy_to_index(gridsize, x, y);
        v1 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y);
        v2 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y + skip);
        v3 = offset + CCG_grid_xy_to_index(gridsize, x, y + skip);

        data[tri_index] = uint3(v0, v2, v1);
        tri_index++;
        data[tri_index] = uint3(v0, v3, v2);
        tri_index++;
      }
    }
  }

  return tri_index;
}

static int create_tri_index_grids_flat_layout(const Span<int> grid_indices,
                                              const BitGroupVector<> &grid_hidden,
                                              const int gridsize,
                                              const int skip,
                                              const int totgrid,
                                              const NodeInfo &node_info,
                                              MutableSpan<uint3> data)
{
  int tri_index = 0;
  int offset = node_info.vbo.offset;
  const int grid_vert_len = square_uint(gridsize - 1) * 4;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    uint v0, v1, v2, v3;
    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }

        v0 = (y * (gridsize - 1) + x) * 4;

        if (skip > 1) {
          v1 = (y * (gridsize - 1) + x + skip - 1) * 4;
          v2 = ((y + skip - 1) * (gridsize - 1) + x + skip - 1) * 4;
          v3 = ((y + skip - 1) * (gridsize - 1) + x) * 4;
        }
        else {
          v1 = v2 = v3 = v0;
        }

        /* VBO data are in a Clockwise QUAD disposition.  Note
         * that vertices might be in different quads if we're
         * building a coarse index buffer.
         */
        v0 += offset;
        v1 += offset + 1;
        v2 += offset + 2;
        v3 += offset + 3;

        data[tri_index] = uint3(v0, v2, v1);
        tri_index++;
        data[tri_index] = uint3(v0, v3, v2);
        tri_index++;
      }
    }
  }
  return tri_index;
}

static void create_lines_index_grids(const Span<int> grid_indices,
                                     int display_gridsize,
                                     const BitGroupVector<> &grid_hidden,
                                     const int gridsize,
                                     const int skip,
                                     const int totgrid,
                                     const NodeInfo &node_info,
                                     MutableSpan<uint2> data)
{
  int line_index = 0;
  int offset = node_info.vbo.offset;
  const int grid_vert_len = gridsize * gridsize;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    uint v0, v1, v2, v3;
    bool grid_visible = false;

    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }
        /* Indices in a Clockwise QUAD disposition. */
        v0 = offset + CCG_grid_xy_to_index(gridsize, x, y);
        v1 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y);
        v2 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y + skip);
        v3 = offset + CCG_grid_xy_to_index(gridsize, x, y + skip);

        data[line_index] = uint2(v0, v1);
        line_index++;
        data[line_index] = uint2(v0, v3);
        line_index++;

        if (y / skip + 2 == display_gridsize) {
          data[line_index] = uint2(v2, v3);
          line_index++;
        }
        grid_visible = true;
      }

      if (grid_visible) {
        data[line_index] = uint2(v1, v2);
        line_index++;
      }
    }
  }
}

static void create_lines_index_grids_flat_layout(const Span<int> grid_indices,
                                                 int display_gridsize,
                                                 const BitGroupVector<> &grid_hidden,
                                                 const int gridsize,
                                                 const int skip,
                                                 const int totgrid,
                                                 const NodeInfo &node_info,
                                                 MutableSpan<uint2> data)
{
  int line_index = 0;
  int offset = node_info.vbo.offset;
  const int grid_vert_len = square_uint(gridsize - 1) * 4;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    bool grid_visible = false;
    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    uint v0, v1, v2, v3;
    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }

        v0 = (y * (gridsize - 1) + x) * 4;

        if (skip > 1) {
          v1 = (y * (gridsize - 1) + x + skip - 1) * 4;
          v2 = ((y + skip - 1) * (gridsize - 1) + x + skip - 1) * 4;
          v3 = ((y + skip - 1) * (gridsize - 1) + x) * 4;
        }
        else {
          v1 = v2 = v3 = v0;
        }

        /* VBO data are in a Clockwise QUAD disposition.  Note
         * that vertices might be in different quads if we're
         * building a coarse index buffer.
         */
        v0 += offset;
        v1 += offset + 1;
        v2 += offset + 2;
        v3 += offset + 3;

        data[line_index] = uint2(v0, v1);
        line_index++;
        data[line_index] = uint2(v0, v3);
        line_index++;

        if (y / skip + 2 == display_gridsize) {
          data[line_index] = uint2(v2, v3);
          line_index++;
        }
        grid_visible = true;
      }

      if (grid_visible) {
        data[line_index] = uint2(v1, v2);
        line_index++;
      }
    }
  }
}

static Array<int> calc_material_indices(const Object &object, const OrigMeshData &orig_mesh_data)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray material_indices = *attributes.lookup<int>("material_index",
                                                              bke::AttrDomain::Face);
      if (!material_indices) {
        return {};
      }
      Array<int> node_materials(nodes.size());
      threading::parallel_for(nodes.index_range(), 64, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> face_indices = nodes[i].faces();
          if (face_indices.is_empty()) {
            continue;
          }
          node_materials[i] = material_indices[face_indices.first()];
        }
      });
      return node_materials;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      /* Use original mesh data because evaluated mesh is empty. */
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArray material_indices = *attributes.lookup<int>("material_index",
                                                              bke::AttrDomain::Face);
      if (!material_indices) {
        return {};
      }
      Array<int> node_materials(nodes.size());
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const Span<int> grid_faces = subdiv_ccg.grid_to_face_map;
      threading::parallel_for(nodes.index_range(), 64, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> grids = nodes[i].grids();
          if (grids.is_empty()) {
            continue;
          }
          node_materials[i] = material_indices[grid_faces[grids.first()]];
        }
      });
      return node_materials;
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

static BitVector<> calc_use_flat_layout(const Object &object, const OrigMeshData &orig_mesh_data)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      /* NOTE: Theoretically it would be possible to used vertex indexed buffers if there are no
       * face corner attributes, sharp faces, or face sets. */
      return {};
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
      if (sharp_faces.is_empty()) {
        return BitVector<>(nodes.size(), false);
      }

      const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;

      /* Use boolean array instead of #BitVector for parallelized writing. */
      Array<bool> use_flat_layout(nodes.size());
      threading::parallel_for(nodes.index_range(), 4, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> grids = nodes[i].grids();
          if (grids.is_empty()) {
            continue;
          }
          use_flat_layout[i] = std::any_of(grids.begin(), grids.end(), [&](const int grid) {
            return sharp_faces[grid_to_face_map[grid]];
          });
        }
      });
      return BitVector<>(use_flat_layout);
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

/**
 * \brief Return the number of vertices that a node needs to store in a GPU buffer.
 */
static int calc_gpu_vertex_count(const Object &object,
                                 const bke::pbvh::Tree &pbvh,
                                 int node_index,
                                 const BitSpan &use_flat_layout)
{
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const bke::pbvh::MeshNode &node = pbvh.nodes<bke::pbvh::MeshNode>()[node_index];
      return node.corners_num();
    }
    case bke::pbvh::Type::Grids: {
      const bke::pbvh::GridsNode &node = pbvh.nodes<bke::pbvh::GridsNode>()[node_index];
      const SculptSession &ss = *object.runtime->sculpt_session;
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const int verts_per_grid = use_flat_layout[node_index] ?
                                     square_i(subdiv_ccg.grid_size - 1) * 4 :
                                     square_i(subdiv_ccg.grid_size);
      const int verts_num = node.grids().size() * verts_per_grid;
      return verts_num;
    }
    case bke::pbvh::Type::BMesh: {
      const bke::pbvh::BMeshNode &node = pbvh.nodes<bke::pbvh::BMeshNode>()[node_index];
      return count_visible_tris_bmesh(node.bm_faces_) * 3;
    }
  }

  BLI_assert_unreachable();
  return 0;
}

static void create_tri_index_mesh(const OffsetIndices<int> faces,
                                  const Span<int3> corner_tris,
                                  const Span<bool> hide_poly,
                                  const bke::pbvh::MeshNode &node,
                                  const NodeInfo &node_info,
                                  const NodeInfo::BufferRange &ibo_info,
                                  uint32_t *host_data)
{
  const Span<int> face_indices = node.faces();
  int tris_num = 0;
  if (hide_poly.is_empty()) {
    tris_num = poly_to_tri_count(face_indices.size(), node.corners_num());
  }
  else {
    for (const int face : face_indices) {
      if (hide_poly[face]) {
        continue;
      }
      tris_num += bke::mesh::face_triangles_num(faces[face].size());
    }
  }
  BLI_assert(uint(ibo_info.len) == 3 * tris_num);

  MutableSpan<uint3> data = MutableSpan<uint32_t>(host_data, ibo_info.len).cast<uint3>();

  int tri_index = 0;
  int node_corner_offset = 0;
  for (const int face_index : face_indices) {
    const IndexRange face = faces[face_index];
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      node_corner_offset += face.size();
      continue;
    }
    for (const int3 &tri : corner_tris.slice(bke::mesh::face_triangles_range(faces, face_index))) {
      for (int i : IndexRange(3)) {
        const int corner = tri[i];
        const int index_in_face = corner - face.first();
        data[tri_index][i] = node_corner_offset + index_in_face + node_info.vbo.offset;
      }
      tri_index++;
    }
    node_corner_offset += face.size();
  }
}

static void create_tri_index_grids(const CCGKey &key,
                                   const BitGroupVector<> &grid_hidden,
                                   const bool do_coarse,
                                   const Span<int> grid_indices,
                                   const bool use_flat_layout,
                                   const NodeInfo &node_info,
                                   const NodeInfo::BufferRange &ibo_info,
                                   uint32_t *host_data)
{
  int gridsize = key.grid_size;
  int display_gridsize = gridsize;
  int totgrid = grid_indices.size();
  int skip = 1;

  const int display_level = do_coarse ? 0 : key.level;

  if (display_level < key.level) {
    display_gridsize = (1 << display_level) + 1;
    skip = 1 << (key.level - display_level - 1);
  }

  uint visible_quad_len = bke::pbvh::count_grid_quads(
      grid_hidden, grid_indices, key.grid_size, display_gridsize);
  UNUSED_VARS_NDEBUG(visible_quad_len);
  BLI_assert(uint(ibo_info.len) == 2 * visible_quad_len * 3);

  MutableSpan<uint3> data = MutableSpan<uint32_t>(host_data, ibo_info.len).cast<uint3>();

  int tri_count;
  if (use_flat_layout) {
    tri_count = create_tri_index_grids_flat_layout(
        grid_indices, grid_hidden, gridsize, skip, totgrid, node_info, data);
  }
  else {
    tri_count = create_tri_index_grids(
        grid_indices, grid_hidden, gridsize, skip, totgrid, node_info, data);
  }
  BLI_assert(tri_count * 3 <= uint(ibo_info.len));
  UNUSED_VARS(tri_count);
}

static void create_lines_index_grids(const CCGKey &key,
                                     const BitGroupVector<> &grid_hidden,
                                     const bool do_coarse,
                                     const Span<int> grid_indices,
                                     const bool use_flat_layout,
                                     const NodeInfo &node_info,
                                     const NodeInfo::BufferRange &ibo_info,
                                     uint32_t *host_data)
{
  int gridsize = key.grid_size;
  int display_gridsize = gridsize;
  int totgrid = grid_indices.size();
  int skip = 1;

  const int display_level = do_coarse ? 0 : key.level;

  if (display_level < key.level) {
    display_gridsize = (1 << display_level) + 1;
    skip = 1 << (key.level - display_level - 1);
  }

  const int quads_per_axis = (gridsize - 1) / skip;
  UNUSED_VARS_NDEBUG(quads_per_axis);
  BLI_assert(uint(ibo_info.len) == 4 * totgrid * quads_per_axis * (quads_per_axis + 1));
  MutableSpan<uint2> data = MutableSpan<uint32_t>(host_data, ibo_info.len).cast<uint2>();
  /* The buffer might contain hidden elements which are not initialized but still accounted. We
   * don't count them to skip from allocation, so must fill that gaps by 0 to hide redundant edges.
   */
  data.fill(uint2(0));

  if (use_flat_layout) {
    create_lines_index_grids_flat_layout(
        grid_indices, display_gridsize, grid_hidden, gridsize, skip, totgrid, node_info, data);
  }
  else {
    create_lines_index_grids(
        grid_indices, display_gridsize, grid_hidden, gridsize, skip, totgrid, node_info, data);
  }
}

Span<gpu::IndexBufPtr> DrawCacheImpl::ensure_lines_indices(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           const IndexMask &node_mask,
                                                           const bool coarse)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  IBOFamily &ibo_family = coarse ? ibo_lines_coarse_ : ibo_lines_;
  Vector<gpu::IndexBufPtr> &ibos = ibo_family.ibos;
  gpu::IndexBuf &ibo_shared = *ibo_family.ibo;
  ibos.resize(pbvh.nodes_num());

  IndexMaskMemory memory;
  const IndexMask nodes_to_calculate = IndexMask::from_predicate(
      node_mask, memory, [&](const int i) {
        return !ibos[i] && (coarse ? node_infos_[i].ibo_lines_coarse : node_infos_[i].ibo_lines)
                               .is_space_acquired;
      });

  /* Build offsets for only the nodes being calculated, so the scratch buffer is sized
   * to just the sum of the needed lengths. */
  Array<int64_t> scratch_offsets(nodes_to_calculate.size());
  int64_t offset = 0;
  nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
    const NodeInfo::BufferRange &src = coarse ? node_infos_[i].ibo_lines_coarse :
                                                node_infos_[i].ibo_lines;
    scratch_offsets[pos] = offset;
    offset += src.len;
  });

  Array<uint32_t> scratch_buffer(offset);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const OffsetIndices<int> faces = mesh.faces();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      nodes_to_calculate.foreach_index(
          [&](const int i, const int64_t pos) {
            const NodeInfo::BufferRange ibo_info{false, 0, node_infos_[i].ibo_lines.len};
            create_lines_index_faces(faces,
                                     hide_poly,
                                     nodes[i].faces(),
                                     node_infos_[i],
                                     ibo_info,
                                     scratch_buffer.data() + scratch_offsets[pos]);
          },
          exec_mode::grain_size(1));
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        const int64_t ibo_len = node_infos_[i].ibo_lines.len;
        update_ibos_subrange(
            ibo_shared, node_infos_[i].ibo_lines, scratch_buffer.data() + scratch_offsets[pos]);
        ibos[i] = gpu::IndexBufPtr(GPU_indexbuf_create_subrange(
            &ibo_shared, uint(node_infos_[i].ibo_lines.offset), uint(ibo_len)));
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      nodes_to_calculate.foreach_index(
          [&](const int i, const int64_t pos) {
            const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
            const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
            const NodeInfo::BufferRange &src = coarse ? node_infos_[i].ibo_lines_coarse :
                                                        node_infos_[i].ibo_lines;
            const NodeInfo::BufferRange ibo_info{false, 0, src.len};
            create_lines_index_grids(key,
                                     subdiv_ccg.grid_hidden,
                                     coarse,
                                     nodes[i].grids(),
                                     use_flat_layout_[i],
                                     node_infos_[i],
                                     ibo_info,
                                     scratch_buffer.data() + scratch_offsets[pos]);
          },
          exec_mode::grain_size(1));
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        const NodeInfo::BufferRange &src = coarse ? node_infos_[i].ibo_lines_coarse :
                                                    node_infos_[i].ibo_lines;
        update_ibos_subrange(ibo_shared, src, scratch_buffer.data() + scratch_offsets[pos]);
        ibos[i] = gpu::IndexBufPtr(
            GPU_indexbuf_create_subrange(&ibo_shared, uint(src.offset), uint(src.len)));
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      nodes_to_calculate.foreach_index(
          [&](const int i, const int64_t pos) {
            const Set<BMFace *, 0> &faces = BKE_pbvh_bmesh_node_faces(
                &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
            const NodeInfo::BufferRange ibo_info{false, 0, node_infos_[i].ibo_lines.len};
            create_lines_index_bmesh(
                faces, node_infos_[i], ibo_info, scratch_buffer.data() + scratch_offsets[pos]);
          },
          exec_mode::grain_size(1));
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        const int64_t ibo_len = node_infos_[i].ibo_lines.len;
        update_ibos_subrange(
            ibo_shared, node_infos_[i].ibo_lines, scratch_buffer.data() + scratch_offsets[pos]);
        ibos[i] = gpu::IndexBufPtr(GPU_indexbuf_create_subrange(
            &ibo_shared, uint(node_infos_[i].ibo_lines.offset), uint(ibo_len)));
      });

      break;
    }
  }

  return ibos;
}

BitSpan DrawCacheImpl::ensure_use_flat_layout(const Object &object,
                                              const OrigMeshData &orig_mesh_data)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (use_flat_layout_.size() != pbvh.nodes_num()) {
    use_flat_layout_ = calc_use_flat_layout(object, orig_mesh_data);
  }
  return use_flat_layout_;
}

BLI_NOINLINE static void flush_vbo_data(const DrawCacheImpl &draw_cache,
                                        DrawCacheImpl::AttributeData &attr_vbo,
                                        const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  node_mask.foreach_index([&](const int i) {
    const NodeInfo &node_info = draw_cache.get_node_infos()[i];
    attr_vbo.update(node_info);
  });
}

DrawCacheImpl::AttributeData &DrawCacheImpl::ensure_attribute_data(
    const Object &object,
    const OrigMeshData &orig_mesh_data,
    const AttributeRequest &attr,
    const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  AttributeData &attr_vbo = attribute_vbos_.lookup_or_add_default(attr);
  attr_vbo.ensure_nodes_capacity(pbvh.nodes_num() + 1);

  /* Compute mask that needs to be recomputed. */
  IndexMaskMemory memory;
  const IndexMask dirty_mask = IndexMask::from_bits(
      node_mask.slice_content(attr_vbo.dirty_nodes.index_range()), attr_vbo.dirty_nodes, memory);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            update_positions_mesh(object, *this, attr_vbo, dirty_mask);
            break;
          case CustomRequest::Normal:
            update_normals_mesh(object, *this, attr_vbo, dirty_mask);
            break;
          case CustomRequest::Mask:
            update_masks_mesh(object, orig_mesh_data, *this, attr_vbo, dirty_mask);
            break;
          case CustomRequest::FaceSet:
            update_face_sets_mesh(object, orig_mesh_data, *this, attr_vbo, dirty_mask);
            break;
        }
      }
      else {
        update_generic_attribute_mesh(
            object, orig_mesh_data, *this, attr_vbo, std::get<GenericRequest>(attr), dirty_mask);
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            update_positions_grids(object, *this, attr_vbo, use_flat_layout_, dirty_mask);
            break;
          case CustomRequest::Normal:
            update_normals_grids(
                object, orig_mesh_data, *this, attr_vbo, use_flat_layout_, dirty_mask);
            break;
          case CustomRequest::Mask:
            update_masks_grids(object, *this, attr_vbo, use_flat_layout_, dirty_mask);
            break;
          case CustomRequest::FaceSet:
            update_face_sets_grids(
                object, orig_mesh_data, *this, attr_vbo, use_flat_layout_, dirty_mask);
            break;
        }
      }
      else {
        update_generic_attribute_grids(object,
                                       orig_mesh_data,
                                       *this,
                                       attr_vbo,
                                       std::get<GenericRequest>(attr),
                                       use_flat_layout_,
                                       dirty_mask);
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            update_positions_bmesh(object, *this, attr_vbo, dirty_mask);
            break;
          case CustomRequest::Normal:
            update_normals_bmesh(object, *this, attr_vbo, dirty_mask);
            break;
          case CustomRequest::Mask:
            update_masks_bmesh(object, *this, attr_vbo, dirty_mask);
            break;
          case CustomRequest::FaceSet:
            update_face_sets_bmesh(object, orig_mesh_data, *this, attr_vbo, dirty_mask);
            break;
        }
      }
      else {
        update_generic_attribute_bmesh(
            object, orig_mesh_data, *this, attr_vbo, std::get<GenericRequest>(attr), dirty_mask);
      }
      break;
    }
  }

  /* TODO: It would be good to deallocate the bit vector if all of the bits have been reset to
   * avoid unnecessary processing in subsequent redraws. */
  dirty_mask.foreach_index_optimized<int>([&](const int i) { attr_vbo.dirty_nodes[i].reset(); });

  flush_vbo_data(*this, attr_vbo, dirty_mask);

  return attr_vbo;
}

Span<gpu::IndexBufPtr> DrawCacheImpl::ensure_tri_indices(const Object &object,
                                                         const OrigMeshData &orig_mesh_data,
                                                         const IndexMask &node_mask,
                                                         const bool coarse)
{
  PRF_scope(ProfileCategory::Draw);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      Vector<gpu::IndexBufPtr> &ibos = ibo_tris_.ibos;
      gpu::IndexBuf &ibo_shared = *ibo_tris_.ibo;
      ibos.resize(nodes.size());

      /* Whenever a node's visible triangle count has changed the index buffers are freed, so we
       * only recalculate null IBOs here. A new mask is recalculated for more even task
       * distribution between threads. */
      IndexMaskMemory memory;
      const IndexMask nodes_to_calculate = IndexMask::from_predicate(
          node_mask, memory, [&](const int i) {
            return !ibos[i] && node_infos_[i].ibo_tris.is_space_acquired;
          });

      /* Build offsets for only the nodes being calculated, so the scratch buffer is sized
       * to just the sum of the needed lengths. */
      Array<int64_t> scratch_offsets(nodes_to_calculate.size());
      int64_t offset = 0;
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        scratch_offsets[pos] = offset;
        offset += node_infos_[i].ibo_tris.len;
      });

      Array<uint32_t> scratch_buffer(offset);

      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int3> corner_tris = mesh.corner_tris();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      nodes_to_calculate.foreach_index(
          [&](const int i, const int64_t pos) {
            const NodeInfo &node_info = node_infos_[i];
            const NodeInfo::BufferRange ibo_info{false, 0, node_infos_[i].ibo_tris.len};
            create_tri_index_mesh(faces,
                                  corner_tris,
                                  hide_poly,
                                  nodes[i],
                                  node_info,
                                  ibo_info,
                                  scratch_buffer.data() + scratch_offsets[pos]);
          },
          exec_mode::grain_size(1));
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        const int64_t ibo_len = node_infos_[i].ibo_tris.len;
        update_ibos_subrange(
            ibo_shared, node_infos_[i].ibo_tris, scratch_buffer.data() + scratch_offsets[pos]);
        ibos[i] = gpu::IndexBufPtr(GPU_indexbuf_create_subrange(
            &ibo_shared, uint(node_infos_[i].ibo_tris.offset), uint(ibo_len)));
      });
      return ibos;
    }
    case bke::pbvh::Type::Grids: {
      /* Unlike the other geometry types, multires grids use indexed vertex buffers because when
       * there are no flat faces, vertices can be shared between neighboring quads. This results in
       * a 4x decrease in the amount of data uploaded. Theoretically it also means freeing VBOs
       * because of visibility changes is unnecessary.
       *
       * TODO: With the "flat layout" and no hidden faces, the index buffers are unnecessary, we
       * should avoid creating them in that case. */
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

      IBOFamily &ibo_family = coarse ? ibo_tris_coarse_ : ibo_tris_;
      Vector<gpu::IndexBufPtr> &ibos = ibo_family.ibos;
      gpu::IndexBuf &ibo_shared = *ibo_family.ibo;
      ibos.resize(nodes.size());

      /* Whenever a node's visible triangle count has changed the index buffers are freed, so we
       * only recalculate null IBOs here. A new mask is recalculated for more even task
       * distribution between threads. */
      IndexMaskMemory memory;
      const IndexMask nodes_to_calculate = IndexMask::from_predicate(
          node_mask, memory, [&](const int i) {
            return !ibos[i] && (coarse ? node_infos_[i].ibo_tris_coarse : node_infos_[i].ibo_tris)
                                   .is_space_acquired;
          });

      /* Build offsets for only the nodes being calculated, so the scratch buffer is sized
       * to just the sum of the needed lengths. */
      Array<int64_t> scratch_offsets(nodes_to_calculate.size());
      int64_t offset = 0;
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        const NodeInfo::BufferRange &src = coarse ? node_infos_[i].ibo_tris_coarse :
                                                    node_infos_[i].ibo_tris;
        scratch_offsets[pos] = offset;
        offset += src.len;
      });

      Array<uint32_t> scratch_buffer(offset);

      const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

      nodes_to_calculate.foreach_index(
          [&](const int i, const int64_t pos) {
            const NodeInfo::BufferRange &src = coarse ? node_infos_[i].ibo_tris_coarse :
                                                        node_infos_[i].ibo_tris;
            const NodeInfo::BufferRange ibo_info{false, 0, src.len};
            create_tri_index_grids(key,
                                   subdiv_ccg.grid_hidden,
                                   coarse,
                                   nodes[i].grids(),
                                   use_flat_layout_[i],
                                   node_infos_[i],
                                   ibo_info,
                                   scratch_buffer.data() + scratch_offsets[pos]);
          },
          exec_mode::grain_size(1));
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        const NodeInfo::BufferRange &src = coarse ? node_infos_[i].ibo_tris_coarse :
                                                    node_infos_[i].ibo_tris;
        update_ibos_subrange(ibo_shared, src, scratch_buffer.data() + scratch_offsets[pos]);
        ibos[i] = gpu::IndexBufPtr(
            GPU_indexbuf_create_subrange(&ibo_shared, uint(src.offset), uint(src.len)));
      });
      return ibos;
    }
    case bke::pbvh::Type::BMesh: {
      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();

      Vector<gpu::IndexBufPtr> &ibos = ibo_tris_.ibos;
      gpu::IndexBuf &ibo_shared = *ibo_tris_.ibo;
      ibos.resize(nodes.size());

      /* Whenever a node's visible triangle count has changed the index buffers are freed, so we
       * only recalculate null IBOs here. A new mask is recalculated for more even task
       * distribution between threads. */
      IndexMaskMemory memory;
      const IndexMask nodes_to_calculate = IndexMask::from_predicate(
          node_mask, memory, [&](const int i) {
            return !ibos[i] && node_infos_[i].ibo_tris.is_space_acquired;
          });

      /* Build offsets for only the nodes being calculated, so the scratch buffer is sized
       * to just the sum of the needed lengths. */
      Array<int64_t> scratch_offsets(nodes_to_calculate.size());
      int64_t offset = 0;
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        scratch_offsets[pos] = offset;
        offset += node_infos_[i].ibo_tris.len;
      });

      Array<uint32_t> scratch_buffer(offset);

      nodes_to_calculate.foreach_index(
          [&](const int i, const int64_t pos) {
            const Set<BMFace *, 0> &faces = BKE_pbvh_bmesh_node_faces(
                &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
            const NodeInfo::BufferRange ibo_info{false, 0, node_infos_[i].ibo_tris.len};
            create_tri_index_bmesh(
                faces, node_infos_[i], ibo_info, scratch_buffer.data() + scratch_offsets[pos]);
          },
          exec_mode::grain_size(1));
      nodes_to_calculate.foreach_index([&](const int i, const int64_t pos) {
        const int64_t ibo_len = node_infos_[i].ibo_tris.len;
        update_ibos_subrange(
            ibo_shared, node_infos_[i].ibo_tris, scratch_buffer.data() + scratch_offsets[pos]);
        ibos[i] = gpu::IndexBufPtr(GPU_indexbuf_create_subrange(
            &ibo_shared, uint(node_infos_[i].ibo_tris.offset), uint(ibo_len)));
      });

      return ibos;
    }
  }
  BLI_assert_unreachable();
  return {};
}

void DrawCacheImpl::IBOFamily::ensure_capacity()
{
  const uint32_t needed_len = uint32_t(space.size());
  if (ibo && ibo->index_len_get() >= needed_len) {
    return;
  }

  /* Create new buffer. */
  gpu::IndexBufPtr new_ibo = gpu::IndexBufPtr(
      GPU_indexbuf_build_on_device(std::max(needed_len, 1u)));
  GPU_indexbuf_use(new_ibo.get());
  /* Copy over data from previous buffer. */
  if (ibo) {
    new_ibo->copy_sub(*ibo, 0, 0, ibo->index_len_get());
    /* Rebuild all subranges. */
    for (gpu::IndexBufPtr &ibo_sub : ibos) {
      if (ibo_sub) {
        gpu::IndexBufPtr new_sub = gpu::IndexBufPtr(GPU_indexbuf_create_subrange(
            new_ibo.get(), ibo_sub->index_start_get(), ibo_sub->index_len_get()));
        ibo_sub.swap(new_sub);
      }
    }
  }
  ibo.swap(new_ibo);
}

void DrawCacheImpl::ensure_batch(BatchCache &cache,
                                 GPUPrimType prim_type,
                                 gpu::IndexBuf *ibo,
                                 Span<gpu::VertBuf *> vbos)
{
  /* The raw batch pointer is captured by drawing passes during sync and used at submit, so it
   * must stay valid for the entire frame. A rebuild therefore reconfigures the batch in place
   * rather than discarding and reallocating it. */
  if (cache.shared == nullptr) {
    cache.shared = GPU_batch_calloc();
    cache.dirty = true;
  }
  if (cache.dirty) {
    GPU_batch_clear(cache.shared);
    GPU_batch_init(cache.shared, prim_type, nullptr, ibo);
    for (gpu::VertBuf *vbo : vbos) {
      GPU_batch_vertbuf_add(cache.shared, vbo, false);
    }
    cache.dirty = false;
  }
}

SculptBatch DrawCacheImpl::ensure_tris_batch(const Object &object,
                                             const ViewportRequest &request,
                                             const IndexMask &leaf_nodes)
{
  PRF_scope(ProfileCategory::Draw);
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data{*id_cast<const Mesh *>(object_orig.data)};
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  this->free_nodes_with_changed_topology(pbvh);

  const bool coarse = request.use_coarse_grids;
  IBOFamily &ibo_family = (coarse ? ibo_tris_coarse_ : ibo_tris_);
  ibo_family.ensure_capacity();

  ensure_tri_indices(object, orig_mesh_data, leaf_nodes, coarse);

  for (const AttributeRequest &attr : request.attributes) {
    ensure_attribute_data(object, orig_mesh_data, attr, leaf_nodes);
  }

  /* Collect VBO pointers in a second pass because #ensure_attribute_data may resize the map. */
  Vector<gpu::VertBuf *> attr_vbos;
  for (const AttributeRequest &attr : request.attributes) {
    if (const AttributeData *attr_data = attribute_vbos_.lookup_ptr(attr)) {
      attr_vbos.append(attr_data->vbo.get());
    }
  }

  BatchCache &batch_cache = tris_batches_.lookup_or_add_default(request);
  ensure_batch(batch_cache, GPU_PRIM_TRIS, ibo_family.ibo.get(), attr_vbos.as_span());

  /* Build one record per leaf node so that `records[k]` corresponds to `leaves[k]`. Nodes without
   * visible geometry get `vertex_len == 0` and are discarded by `append_records`. `res_id` and
   * `custom_id` are left at 0 and expected to be stamped by the caller (draw_sculpt). */
  SculptBatch result;
  result.shared_batch = batch_cache.shared;
  result.records.reserve(leaf_nodes.size());

  leaf_nodes.foreach_index([&](const int node_index) {
    const NodeInfo::BufferRange &range = coarse ? node_infos_[node_index].ibo_tris_coarse :
                                                  node_infos_[node_index].ibo_tris;
    command::DrawRecord rec{};
    rec.instance_len = 1;
    /* `vertex_first` is the first index element of the node inside the shared IBO; `base_index`
     * (vertexOffset / baseVertex) stays 0 because the shared IBO stores absolute vertex indices
     * into the shared VBO. Empty nodes keep `vertex_len == 0` and are not drawn. */
    rec.vertex_len = (range.len > 0) ? uint(range.len) : 0;
    rec.vertex_first = (range.len > 0) ? uint(range.offset) : 0;
    rec.base_index = 0;
    rec.instance_first = 0;
    rec.res_id = 0;
    rec.custom_id = 0;
    result.records.append(rec);
  });

  return result;
}

SculptBatch DrawCacheImpl::ensure_lines_batch(const Object &object,
                                              const ViewportRequest &request,
                                              const IndexMask &leaf_nodes)
{
  PRF_scope(ProfileCategory::Draw);
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data(*id_cast<const Mesh *>(object_orig.data));
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  this->free_nodes_with_changed_topology(pbvh);

  const bool coarse = request.use_coarse_grids;
  IBOFamily &ibo_family = (coarse ? ibo_lines_coarse_ : ibo_lines_);
  ibo_family.ensure_capacity();

  AttributeData &pos_attr = this->ensure_attribute_data(
      object, orig_mesh_data, CustomRequest::Position, leaf_nodes);
  ensure_lines_indices(object, orig_mesh_data, leaf_nodes, coarse);

  gpu::VertBuf *position_vbo = pos_attr.vbo.get();
  BLI_assert(position_vbo != nullptr);
  Span<gpu::VertBuf *> position_span{&position_vbo, 1};

  BatchCache &batch_cache = coarse ? lines_batches_coarse_ : lines_batches_;
  ensure_batch(batch_cache, GPU_PRIM_LINES, ibo_family.ibo.get(), position_span);

  SculptBatch result;
  result.shared_batch = batch_cache.shared;
  result.records.reserve(leaf_nodes.size());

  leaf_nodes.foreach_index([&](const int node_index) {
    const NodeInfo::BufferRange &range = coarse ? node_infos_[node_index].ibo_lines_coarse :
                                                  node_infos_[node_index].ibo_lines;
    if (range.len <= 0) {
      return;
    }
    command::DrawRecord rec{};
    rec.vertex_len = uint(range.len);
    rec.instance_len = 1;
    rec.vertex_first = uint(range.offset);
    rec.base_index = 0;
    rec.instance_first = 0;
    rec.res_id = 0;
    rec.custom_id = 0;
    result.records.append(rec);
  });

  return result;
}

Span<int> DrawCacheImpl::ensure_material_indices(const Object &object)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data(*id_cast<const Mesh *>(object_orig.data));
  if (orig_mesh_data.attributes.contains("material_index") &&
      material_indices_.size() != pbvh.nodes_num())
  {
    material_indices_ = calc_material_indices(object, orig_mesh_data);
  }
  return material_indices_;
}

/**
 * \brief Ensure the limit of a space is at least as large as its current size.
 *
 * Returns true when the space has grown, in which case the caller should reallocate the underlying
 * buffer and rebuild the cached batches.
 */
static bool ensure_size(RangeAllocator &space, uint64_t &space_limit)
{
  const uint64_t size = uint64_t(space.size());
  if (size <= space_limit) {
    return false;
  }
  space_limit = size;
  return true;
}

static bool update_node_buffer_range(NodeInfo::BufferRange &range,
                                     RangeAllocator &space,
                                     const int64_t new_len)
{
  bool changed = false;
  if (range.len != new_len) {
    if (range.is_space_acquired) {
      space.release(range);
      changed = true;
    }
    range.offset = -1;
    range.len = new_len;
    range.is_space_acquired = false;
  }
  return changed;
}

static bool update_node_buffer_space(NodeInfo &node_info,
                                     const int64_t vert_len,
                                     RangeAllocator &vbo_space,
                                     const NodeIndexCounts &counts,
                                     RangeAllocator &ibo_lines_space,
                                     RangeAllocator &ibo_lines_coarse_space,
                                     RangeAllocator &ibo_tris_space,
                                     RangeAllocator &ibo_tris_coarse_space)
{
  bool changed = false;
  changed |= update_node_buffer_range(node_info.vbo, vbo_space, vert_len);
  changed |= update_node_buffer_range(node_info.ibo_lines, ibo_lines_space, counts.lines);
  changed |= update_node_buffer_range(
      node_info.ibo_lines_coarse, ibo_lines_coarse_space, counts.lines_coarse);
  changed |= update_node_buffer_range(node_info.ibo_tris, ibo_tris_space, counts.tris);
  changed |= update_node_buffer_range(
      node_info.ibo_tris_coarse, ibo_tris_coarse_space, counts.tris_coarse);
  return changed;
}

Span<NodeInfo> DrawCacheImpl::ensure_node_infos(const Object &object,
                                                const IndexMask &all_leaf_nodes)
{
  PRF_scope(ProfileCategory::Draw);
  if (all_leaf_nodes.is_empty()) {
    return node_infos_;
  }

  /* Ensure that the layout is known for each node. */
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data{*id_cast<const Mesh *>(object_orig.data)};
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const int64_t nodes_num = pbvh.nodes_num();
  BitSpan use_flat_layout = this->ensure_use_flat_layout(object, orig_mesh_data);

  /* Make sure that all the given nodes have a valid location. */
  /* - Ensure enough space to store max node index. */
  if (node_infos_.size() < nodes_num) {
    node_infos_.resize(nodes_num);
  }

  /* - Free nodes that require different space inside the GPU buffers. */
  dirty_topology_.resize(nodes_num);
  all_leaf_nodes.foreach_index([&](const int node_index) {
    NodeInfo &node_info = node_infos_[node_index];

    if (!node_info.is_dirty) {
      return;
    }
    node_info.is_dirty = false;

    const NodeIndexCounts counts = count_node_indices(object, pbvh, orig_mesh_data, node_index);
    const int64_t vert_len = (pbvh.type() == bke::pbvh::Type::BMesh) ?
                                 int64_t(counts.tris) :
                                 calc_gpu_vertex_count(object, pbvh, node_index, use_flat_layout);

    if (update_node_buffer_space(node_info,
                                 vert_len,
                                 vbo_space_,
                                 counts,
                                 ibo_lines_.space,
                                 ibo_lines_coarse_.space,
                                 ibo_tris_.space,
                                 ibo_tris_coarse_.space))
    {
      dirty_topology_[node_index].set();
    }
  });

  /* Assign space for nodes that don't have any assigned space.
   *
   * A zero-length range is acquired too. It must be acquired so that leaved without visible
   * geometry are drawn correctly. */
  all_leaf_nodes.foreach_index([&](const int node_index) {
    NodeInfo &node_info = node_infos_[node_index];
    if (!node_info.vbo.is_space_acquired) {
      vbo_space_.acquire(node_info.vbo);
    }
    if (!node_info.ibo_lines.is_space_acquired) {
      ibo_lines_.space.acquire(node_info.ibo_lines);
    }
    if (!node_info.ibo_lines_coarse.is_space_acquired) {
      ibo_lines_coarse_.space.acquire(node_info.ibo_lines_coarse);
    }
    if (!node_info.ibo_tris.is_space_acquired) {
      ibo_tris_.space.acquire(node_info.ibo_tris);
    }
    if (!node_info.ibo_tris_coarse.is_space_acquired) {
      ibo_tris_coarse_.space.acquire(node_info.ibo_tris_coarse);
    }
  });

  /* Mark cached batches dirty when a buffer they reference is reallocated. */
  const bool vbo_reallocated = ensure_size(vbo_space_, vbo_space_limit_);
  const bool lines_reallocated = ensure_size(ibo_lines_.space, ibo_lines_.space_limit);
  const bool lines_coarse_reallocated = ensure_size(ibo_lines_coarse_.space,
                                                    ibo_lines_coarse_.space_limit);
  const bool tris_reallocated = ensure_size(ibo_tris_.space, ibo_tris_.space_limit);
  const bool tris_coarse_reallocated = ensure_size(ibo_tris_coarse_.space,
                                                   ibo_tris_coarse_.space_limit);
  lines_batches_.dirty = vbo_reallocated || lines_reallocated;
  lines_batches_coarse_.dirty = vbo_reallocated || lines_coarse_reallocated;
  /* It is rare that tris and coardse tris are not reallocated at the same time. */
  if (vbo_reallocated || tris_reallocated || tris_coarse_reallocated) {
    for (BatchCache &batch_cache : tris_batches_.values()) {
      batch_cache.dirty = true;
    }
  }

  /* Return all nodes (not only the nodes that are being updated) to keep the indices alligned
   * with PBVH data structure. */
  return node_infos_;
}

}  // namespace draw::pbvh
}  // namespace blender
