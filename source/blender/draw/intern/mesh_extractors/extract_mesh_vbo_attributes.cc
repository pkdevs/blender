/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "BLI_array_utils.hh"
#include "BLI_string.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"

#include "NOD_geometry_nodes_viewer_bundles.hh"

#include "attribute_convert.hh"
#include "draw_attributes.hh"
#include "draw_subdivision.hh"
#include "extract_mesh.hh"

#include "GPU_vertex_buffer.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Attributes
 * \{ */

static GPUVertFormat attribute_vbo_format(const MeshRenderData &mr,
                                          const StringRef name,
                                          const bke::AttrType type)
{
  char attr_name[32], attr_safe_name[GPU_MAX_SAFE_ATTR_NAME];
  GPU_vertformat_safe_attr_name(name, attr_safe_name, GPU_MAX_SAFE_ATTR_NAME);
  /* Attributes use auto-name. */
  SNPRINTF(attr_name, "a%s", attr_safe_name);

  GPUVertFormat format = init_format_for_attribute(type, attr_name);
  GPU_vertformat_deinterleave(&format);

  if (mr.active_color_name && name == mr.active_color_name) {
    GPU_vertformat_alias_add(&format, "ac");
  }
  if (mr.default_color_name && name == mr.default_color_name) {
    GPU_vertformat_alias_add(&format, "c");
  }
  return format;
}

static gpu::VertBufPtr vbo_create(const GPUVertFormat &format, const int size)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
  GPU_vertbuf_data_alloc(*vbo, size);
  return vbo;
}

static int domain_size(const MeshRenderData &mr, const bke::AttrDomain domain)
{
  switch (domain) {
    case bke::AttrDomain::Point:
      return mr.verts_num;
    case bke::AttrDomain::Edge:
      return mr.edges_num;
    case bke::AttrDomain::Face:
      return mr.faces_num;
    case bke::AttrDomain::Corner:
      return mr.corners_num;
    default:
      BLI_assert_unreachable();
      return 0;
  }
}

/**
 * Copy values from vertices, edges, or faces to face corners on the GPU.
 * \param src: A value for every element in \a domain.
 * \param dst: A device-only buffer with the same format as \a src and a value for every face
 * corner, optionally followed by values for loose geometry.
 */
static void copy_to_corners_gpu(const MeshRenderData &mr,
                                MeshBufferCache &cache,
                                const bke::AttrDomain domain,
                                gpu::VertBuf &src,
                                gpu::VertBuf &dst)
{
  switch (domain) {
    case bke::AttrDomain::Point:
      gather_vert_to_corner_gpu(mr, cache, src, dst);
      break;
    case bke::AttrDomain::Edge:
      gather_edge_to_corner_gpu(mr, cache, src, dst);
      break;
    case bke::AttrDomain::Face:
      scatter_face_to_corner_gpu(mr, cache, src, dst);
      break;
    default:
      BLI_assert_unreachable();
  }
}

static gpu::VertBufPtr copy_to_corners_gpu(const MeshRenderData &mr,
                                           MeshBufferCache &cache,
                                           const bke::AttrDomain domain,
                                           gpu::VertBuf &src,
                                           const GPUVertFormat &format)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_on_device(format, mr.corners_num));
  copy_to_corners_gpu(mr, cache, domain, src, *vbo);
  return vbo;
}

/** Convert BMesh attribute values to the GPU format, in the index order of their domain. */
static void extract_data_bmesh(const MeshRenderData &mr,
                               const BMDataLayerLookup &attr,
                               gpu::VertBuf &vbo)
{
  BMesh &bm = *mr.bm;
  bke::attribute_math::to_static_type(attr.type, [&]<typename T>() {
    using Converter = AttributeConverter<T>;
    using VBOType = typename Converter::VBOType;
    if constexpr (!std::is_void_v<VBOType>) {
      MutableSpan data = vbo.data<VBOType>();
      const auto convert = [&](const BMHeader &head) {
        return Converter::convert(*static_cast<const T *>(POINTER_OFFSET(head.data, attr.offset)));
      };
      switch (attr.domain) {
        case bke::AttrDomain::Point:
          threading::parallel_for(IndexRange(bm.totvert), 4096, [&](const IndexRange range) {
            for (const int i : range) {
              data[i] = convert(BM_vert_at_index(&bm, i)->head);
            }
          });
          break;
        case bke::AttrDomain::Edge:
          threading::parallel_for(IndexRange(bm.totedge), 4096, [&](const IndexRange range) {
            for (const int i : range) {
              data[i] = convert(BM_edge_at_index(&bm, i)->head);
            }
          });
          break;
        case bke::AttrDomain::Face:
          threading::parallel_for(IndexRange(bm.totface), 4096, [&](const IndexRange range) {
            for (const int i : range) {
              data[i] = convert(BM_face_at_index(&bm, i)->head);
            }
          });
          break;
        case bke::AttrDomain::Corner:
          threading::parallel_for(IndexRange(bm.totface), 2048, [&](const IndexRange range) {
            for (const int face_index : range) {
              const BMFace &face = *BM_face_at_index(&bm, face_index);
              const BMLoop *loop = BM_FACE_FIRST_LOOP(&face);
              for ([[maybe_unused]] const int i : IndexRange(face.len)) {
                data[BM_elem_index_get(loop)] = convert(loop->head);
                loop = loop->next;
              }
            }
          });
          break;
        default:
          BLI_assert_unreachable();
      }
    }
  });
}

static gpu::VertBufPtr extract_attribute_data(const MeshRenderData &mr,
                                              MeshBufferCache &cache,
                                              const BMDataLayerLookup &attr,
                                              const GPUVertFormat &format)
{
  gpu::VertBufPtr src = vbo_create(format, domain_size(mr, attr.domain));
  extract_data_bmesh(mr, attr, *src);
  if (attr.domain == bke::AttrDomain::Corner) {
    return src;
  }
  return copy_to_corners_gpu(mr, cache, attr.domain, *src, format);
}

static gpu::VertBufPtr extract_attribute_data(const MeshRenderData &mr,
                                              MeshBufferCache &cache,
                                              const bke::GAttributeReader &attr,
                                              const GPUVertFormat &format)
{
  if (attr.varray.is_single()) {
    gpu::VertBufPtr vbo = vbo_create(format, mr.corners_num);
    bke::attribute_math::to_static_type(attr.varray.type(), [&]<typename T>() {
      const VArray<T> &src = attr.varray.typed<T>();
      if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
        using Converter = AttributeConverter<T>;
        using VBOType = typename Converter::VBOType;
        MutableSpan data = vbo->data<VBOType>();
        data.fill(Converter::convert(src.get_internal_single()));
      }
    });
    return vbo;
  }
  gpu::VertBufPtr src = vbo_create(format, domain_size(mr, attr.domain));
  vertbuf_data_extract_direct(GVArraySpan(*attr), *src);
  if (attr.domain == bke::AttrDomain::Corner) {
    return src;
  }
  return copy_to_corners_gpu(mr, cache, attr.domain, *src, format);
}

gpu::VertBufPtr extract_attribute(const MeshRenderData &mr,
                                  MeshBufferCache &cache,
                                  const StringRef name)
{
  if (mr.extract_type == MeshExtractType::BMesh) {
    const BMDataLayerLookup attr = BM_data_layer_lookup(*mr.bm, name);
    if (!attr) {
      return {};
    }
    return extract_attribute_data(mr, cache, attr, attribute_vbo_format(mr, name, attr.type));
  }
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const bke::GAttributeReader attr = attributes.lookup(name);
  if (!attr) {
    return {};
  }
  const bke::AttrType type = bke::cpp_type_to_attribute_type(attr.varray.type());
  return extract_attribute_data(mr, cache, attr, attribute_vbo_format(mr, name, type));
}

gpu::VertBufPtr extract_attribute_subdiv(const MeshRenderData &mr,
                                         MeshBufferCache &cache,
                                         const DRWSubdivCache &subdiv_cache,
                                         const StringRef name)
{
  BLI_assert(mr.corners_num == subdiv_cache.mesh->corners_num);

  /* Extract the attribute on the coarse face corners, to be interpolated to the subdivided face
   * corners. The compute shader only expects floats. */
  gpu::VertBufPtr coarse_vbo;
  bke::AttrType type;
  if (mr.extract_type == MeshExtractType::BMesh) {
    const BMDataLayerLookup attr = BM_data_layer_lookup(*mr.bm, name);
    if (!attr) {
      return {};
    }
    type = attr.type;
    coarse_vbo = extract_attribute_data(mr, cache, attr, init_format_for_attribute(type, "data"));
  }
  else {
    const bke::AttributeAccessor attributes = mr.mesh->attributes();
    const bke::GAttributeReader attr = attributes.lookup(name);
    if (!attr) {
      return {};
    }
    type = bke::cpp_type_to_attribute_type(attr.varray.type());
    coarse_vbo = extract_attribute_data(mr, cache, attr, init_format_for_attribute(type, "data"));
  }

  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_on_device(
      attribute_vbo_format(mr, name, type), subdiv_cache.num_subdiv_loops));

  bke::attribute_math::to_static_type(type, [&]<typename T>() {
    using Converter = AttributeConverter<T>;
    if constexpr (!std::is_void_v<typename Converter::VBOType>) {
      draw_subdiv_interp_custom_data(subdiv_cache,
                                     *coarse_vbo,
                                     *vbo,
                                     Converter::gpu_component_type,
                                     Converter::gpu_component_len,
                                     0);
    }
  });

  return vbo;
}

gpu::VertBufPtr extract_attr_viewer(const MeshRenderData &mr)
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "attribute_value", gpu::VertAttrType::SFLOAT_32_32_32_32);

  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
  GPU_vertbuf_data_alloc(*vbo, mr.corners_num);
  MutableSpan vbo_data = vbo->data<ColorGeometry4f>();

  const StringRefNull attr_name = ".viewer";
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const bke::AttributeReader attribute = attributes.lookup_or_default<ColorGeometry4f>(
      attr_name, bke::AttrDomain::Corner, {1.0f, 0.0f, 1.0f, 1.0f});
  attribute.varray.materialize(vbo_data);
  return vbo;
}

static void extract_viewer_color_loose(const MeshRenderData &mr,
                                       const bke::AttributeAccessor attributes,
                                       const StringRef name,
                                       const ColorGeometry4f &default_color,
                                       MutableSpan<ColorGeometry4f> loose_data)
{
  if (loose_data.is_empty()) {
    return;
  }
  MutableSpan loose_edge_data = loose_data.take_front(mr.loose_edges.size() * 2);
  MutableSpan loose_vert_data = loose_data.take_back(mr.loose_verts.size());

  /* Fill loose vertices and loose edges from their corresponding domain. */
  const VArraySpan vert_colors = *attributes.lookup_or_default<ColorGeometry4f>(
      name, bke::AttrDomain::Point, default_color);
  const std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(name);
  if (meta_data && meta_data->domain == bke::AttrDomain::Edge) {
    const VArraySpan edge_colors = *attributes.lookup<ColorGeometry4f>(name,
                                                                       bke::AttrDomain::Edge);
    mr.loose_edges.foreach_index(
        [&](const int i, const int pos) {
          loose_edge_data[pos * 2 + 0] = edge_colors[i];
          loose_edge_data[pos * 2 + 1] = edge_colors[i];
        },
        exec_mode::grain_size(4096));
  }
  else {
    extract_mesh_loose_edge_data(vert_colors, mr.edges, mr.loose_edges, loose_edge_data);
  }
  array_utils::gather(vert_colors, mr.loose_verts, loose_vert_data);
}

gpu::VertBufPtr extract_viewer_color(const MeshRenderData &mr, MeshBufferCache &cache)
{
  using nodes::viewer_bundles::GeometryOverlayBundle;
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "attribute_value", gpu::VertAttrType::SFLOAT_32_32_32_32);
  const ColorGeometry4f default_color = GeometryOverlayBundle::default_color;
  const StringRef name = GeometryOverlayBundle::color_attribute_name;
  const int vbo_size = mr.corners_num + mr.loose_indices_num;

  if (mr.extract_type != MeshExtractType::Mesh) {
    gpu::VertBufPtr vbo = vbo_create(format, vbo_size);
    vbo->data<ColorGeometry4f>().fill(default_color);
    return vbo;
  }

  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const bke::AttributeReader attr = attributes.lookup<ColorGeometry4f>(name);

  if (attr && !attr.varray.is_single() && attr.domain != bke::AttrDomain::Corner) {
    /* Upload values for the attribute's domain and copy them to face corners on the GPU. */
    gpu::VertBufPtr src = vbo_create(format, domain_size(mr, attr.domain));
    attr.varray.materialize(src->data<ColorGeometry4f>());
    gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_on_device(format, vbo_size));
    copy_to_corners_gpu(mr, cache, attr.domain, *src, *vbo);

    Array<ColorGeometry4f> loose_data(mr.loose_indices_num);
    extract_viewer_color_loose(mr, attributes, name, default_color, loose_data);
    loose_data_upload(mr, loose_data.as_span(), *vbo);
    return vbo;
  }

  gpu::VertBufPtr vbo = vbo_create(format, vbo_size);
  MutableSpan vbo_data = vbo->data<ColorGeometry4f>();
  MutableSpan corner_data = vbo_data.take_front(mr.corners_num);
  if (!attr) {
    corner_data.fill(default_color);
  }
  attr.varray.materialize(corner_data);
  extract_viewer_color_loose(
      mr, attributes, name, default_color, vbo_data.drop_front(mr.corners_num));
  return vbo;
}

/** \} */

}  // namespace blender::draw
