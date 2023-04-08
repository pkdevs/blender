/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_memarena.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BLI_math_geom.h"

#include "GEO_mesh_clip_geometry_by_plane.hh"
#include "GEO_mesh_copy_selection.hh"
#include <variant>

namespace blender::geometry {

/* -------------------------------------------------------------------- */
/** \name Mesh builder
 * \{ */

/*
 * Vertex generated from linear interpolation between two
 */
struct MeshVertexSetCopyMask {
  IndexMask src_indices;
};
/*
 * Vertex generated from linear interpolation between two
 */
struct MeshVertexSetLinear {
  Span<int2> src_indices;
  Span<float> weights;
};

struct MeshEdgeSetCopyMask {
  /*
   * Mask for the edges to copy from the data source.
   */
  IndexMask src_indices;
  /*
   * Optional: Table for remapping the src indices. Leave empty if indices are unchanged.
   */
  Span<int> mapping_table;
};
/*
 * Edge generated between the given vertices.
 */
struct MeshEdgeSetPair {
  /*
   * Indices to the edge connects in the target mesh.
   */
  Span<int2> vertex_indices;
  /*
   * Optional: Edges to inherit values from, support 0, 1, 2 edges to inherit from. Takes the
   * average when possible. Leave invalid entries to -1, or leave it empty if no values are set.
   */
  Span<int2> src_edge_indices;
};

/*
 * Triangles generated from given .
 */
struct MeshTriangleSet {
  /*
   * Indices to the polygon attributes should be copied from.
   */
  Span<int> src_polygon_indices;
  /*
   * Edge index sets, indices point to edges in the target mesh.
   */
  Span<int3> vertex_indices; /* Optional, can be computed by checking matching edge index pairs! */
  Span<int3> edge_indices;
  Span<float3> src_weights;
  Span<int2> src_corners;

  int64_t num_faces() const
  {
    BLI_assert(src_polygon_indices.size() == edge_indices.size());
    BLI_assert(src_polygon_indices.size() * 3 == src_corners.size());
    BLI_assert(src_polygon_indices.size() == src_weights.size());
    return src_polygon_indices.size();
  }

  int64_t num_corners() const
  {
    return edge_indices.size() * 3;
  }

  void fill_corner_offsets(MutableSpan<int> offset_slice) const
  {
    for (const int64_t index : offset_slice.index_range().drop_front(1)) {
      offset_slice[index] = offset_slice[index - 1] + 3;
    }
  }
};
struct MeshFaceSetCopyMask {

  int num_corners;
  /*
   * Mask for the faces to copy from the data source.
   */
  IndexMask src_indices;

  Span<int> mapping_table_verts;
  Span<int> mapping_table_edges;

  void fill_corner_table(const OffsetIndices<int> src_corner_offsets,
                         const Span<int> src_corner_verts,
                         const Span<int> src_corner_edges,
                         MutableSpan<int> offset_slice,
                         MutableSpan<int> dst_corner_verts,
                         MutableSpan<int> dst_corner_edges) const
  {
    for (const int64_t offset : offset_slice.index_range().drop_back(1)) {
      const IndexRange src_range = src_corner_offsets[src_indices[offset]];
      const IndexRange dst_range(offset_slice[offset], src_range.size());
      offset_slice[offset + 1] = dst_range.one_after_last();

      for (const int64_t index : src_range.index_range()) {
        dst_corner_verts[dst_range[index]] =
            mapping_table_verts[src_corner_verts[src_range[index]]];
        dst_corner_edges[dst_range[index]] =
            mapping_table_edges[src_corner_edges[src_range[index]]];
        BLI_assert(dst_corner_verts[dst_range[index]] >= 0);
        BLI_assert(dst_corner_edges[dst_range[index]] >= 0);
      }
    }
  }
};

/*
 * Descriptor variants for mesh vertex groups/sets.
 */
using VariantVertexSet = std::variant<MeshVertexSetCopyMask, MeshVertexSetLinear>;
/*
 * Descriptor variants for mesh edge groups/sets.
 */
using VariantEdgeSet = std::variant<MeshEdgeSetCopyMask, MeshEdgeSetPair>;
/*
 * Descriptor variants for mesh polygon groups/sets.
 */
using VariantPolygonSet = std::variant<MeshFaceSetCopyMask, MeshTriangleSet>;

/*
 * Number of vertex elements in the set description.
 */
IndexRange vertex_range(const VariantVertexSet &desc)
{
  if (auto item = std::get_if<MeshVertexSetCopyMask>(&desc)) {
    return item->src_indices.index_range();
  }
  else if (auto item = std::get_if<MeshVertexSetLinear>(&desc)) {
    return item->src_indices.index_range();
  }
  BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
  return IndexRange();
}

/*
 * Number of edge elements in the set description.
 */
IndexRange edge_range(const VariantEdgeSet &desc)
{
  if (auto item = std::get_if<MeshEdgeSetCopyMask>(&desc)) {
    return item->src_indices.index_range();
  }
  else if (auto item = std::get_if<MeshEdgeSetPair>(&desc)) {
    return item->vertex_indices.index_range();
  }
  BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
  return IndexRange();
}

int vertex_count(const Span<VariantVertexSet> vertex_sets)
{
  int count = 0;
  for (int64_t i = 0; i < vertex_sets.size(); i++) {
    count += vertex_range(vertex_sets[i]).size();
  }
  return count;
}

int edge_count(const Span<VariantEdgeSet> edge_sets)
{
  int count = 0;
  for (int64_t i = 0; i < edge_sets.size(); i++) {
    count += edge_range(edge_sets[i]).size();
  }
  return count;
}

/*
 * Number of (face, corner) elements in the set description.
 */
int2 get_face_shape(const VariantPolygonSet &desc)
{
  if (auto item = std::get_if<MeshTriangleSet>(&desc)) {
    return int2{int(item->num_faces()), int(item->num_corners())};
  }
  else if (auto item = std::get_if<MeshFaceSetCopyMask>(&desc)) {
    return int2{int(item->src_indices.size()), item->num_corners};
  }
  else {
    BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
  }
  return int2{};
}

int2 polygon_count(const Span<VariantPolygonSet> poly_sets)
{
  int face_count = 0;
  int offset = 0;
  for (int64_t set_index = 0; set_index < poly_sets.size(); set_index++) {
    const int2 shape = get_face_shape(poly_sets[set_index]);
    face_count += shape.x;
    offset += shape.y;
  }
  return {face_count, offset};
}

void transfer_vertex_data(const Mesh &src_mesh,
                          Mesh &dst_mesh,
                          const Span<VariantVertexSet> vertex_sets,
                          const bke::AttributeFilter &attribute_filter)
{
  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  Set<std::string> copy_point_skip;

  /* Copy point domain. */
  for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_POINT,
           bke::attribute_filter_with_skip_ref(attribute_filter, copy_point_skip)))
  {
    bke::attribute_math::convert_to_static_type(attribute.meta_data.data_type, [&](auto dummy) {
      using T = decltype(dummy);

      const Span<T> src_data = attribute.src.template typed<T>();
      MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

      int64_t accumulated_offset = 0;
      for (int64_t i = 0; i < vertex_sets.size(); i++) {
        const IndexRange set_range = vertex_range(vertex_sets[i]);

        if (auto item = std::get_if<MeshVertexSetCopyMask>(&vertex_sets[i])) {
          /* Copy */
          const IndexRange dst_range = set_range.shift(accumulated_offset);
          array_utils::gather(src_data, item->src_indices, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshVertexSetLinear>(&vertex_sets[i])) {
          threading::parallel_for(set_range, 512, [&](IndexRange slice) {
            const IndexRange dst_range = slice.shift(accumulated_offset);

            /* Linear interpolate */
            for (const int64_t i : slice.index_range()) {
              const int64_t sample_index = slice[i];
              int2 src_vert_indices = item->src_indices[sample_index];
              dst_data[dst_range[i]] = bke::attribute_math::mix2(item->weights[sample_index],
                                                                 src_data[src_vert_indices.x],
                                                                 src_data[src_vert_indices.y]);
            }
          });
        }
        else {
          BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
        }

        accumulated_offset += set_range.size();
      }
    });
  }
}

void transfer_edge_data(const Mesh &src_mesh,
                        Mesh &dst_mesh,
                        const Span<VariantEdgeSet> edge_sets,
                        const bke::AttributeFilter &attribute_filter)
{
  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  Set<std::string> copy_edge_skip;
  copy_edge_skip.add(".edge_verts");

  /* Assign edge indices */
  {
    Span<int2> src_edges = src_mesh.edges();
    MutableSpan<int2> dst_edges = dst_mesh.edges_for_write();

    int64_t set_offset = 0;
    for (int64_t i = 0; i < edge_sets.size(); i++) {
      const IndexRange dst_range = edge_range(edge_sets[i]).shift(set_offset);
      MutableSpan<int2> dst_edge_slice = dst_edges.slice(dst_range);

      if (auto item = std::get_if<MeshEdgeSetCopyMask>(&edge_sets[i])) {
        /* Copy using updated mapping table */
        item->src_indices.foreach_index([&](int64_t i, int64_t pos) {
          int2 src_index_pair = src_edges[i];
          int2 mapped_pair = int2(item->mapping_table[src_index_pair.x],
                                  item->mapping_table[src_index_pair.y]);
          dst_edge_slice[pos] = mapped_pair;
        });
      }
      else if (auto item = std::get_if<MeshEdgeSetPair>(&edge_sets[i])) {
        /* Copy indices */
        dst_edge_slice.copy_from(item->vertex_indices);
      }
      else {
        BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
      }
      set_offset += dst_range.size();
    }
  }

  /* Copy edge domain. */
  for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_EDGE,
           bke::attribute_filter_with_skip_ref(attribute_filter, copy_edge_skip)))
  {
    const CPPType &cpp_type = *bke::custom_data_type_to_cpp_type(attribute.meta_data.data_type);
    bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
      using T = decltype(dummy);

      const Span<T> src_data = attribute.src.template typed<T>();
      MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

      int64_t set_offset = 0;
      for (int64_t i = 0; i < edge_sets.size(); i++) {
        const IndexRange set_range = edge_range(edge_sets[i]);
        const IndexRange dst_range = set_range.shift(set_offset);

        if (auto item = std::get_if<MeshEdgeSetCopyMask>(&edge_sets[i])) {
          /* Copy */
          array_utils::gather(src_data, item->src_indices, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshEdgeSetPair>(&edge_sets[i])) {
          /* Mix edge data IFF source edges are specified. */
          if (item->src_edge_indices.size() == set_range.size()) {
            threading::parallel_for(set_range, 512, [&](IndexRange slice) {
              for (const int64_t i : slice.index_range()) {
                const int64_t sample_index = slice[i];
                int2 vert_indices = item->src_edge_indices[sample_index];

                T &value = dst_data[dst_range[i]];
                if (vert_indices.x == -1) {
                  cpp_type.default_construct(&value);
                }
                else if (vert_indices.y == -1) {
                  value = src_data[vert_indices.x];
                }
                else {
                  value = bke::attribute_math::mix2(
                      0.5f, src_data[vert_indices.x], src_data[vert_indices.y]);
                }
              }
            });
          }
          else {
            /* Defualt fill. */
            cpp_type.default_construct_n(dst_data.slice(dst_range).data(), dst_range.size());
          }
        }
        else {
          BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
        }
        set_offset += set_range.size();
      }
    });
  }
}

/* Find the sorted common value pair of `a` so that `a.x` is found in b.
 * Assumes the shared value exist!
 *
 */
int2 sort_shared_index(const int2 a, const int2 b)
{
  if (a.x == b.x || a.x == b.y) {
    return a;
  }
  BLI_assert(a.y == b.x || a.y == b.y);
  return int2(a.y, a.x);
}

/* Find the common value in `a` (assumed the shared value exist).
 */
int get_unshared_index(const int2 a, const int b)
{
  BLI_assert(b == a.x || b == a.y);
  return b == a.x ? a.x : a.y;
}

Vector<int> compute_polygon_vert_indices_from_edge_data(const Span<int> dst_face_offsets_slice,
                                                        const Span<int2> dst_edges,
                                                        const Span<int> edge_corner_indices)
{
  Vector<int> face_corner_indices(dst_face_offsets_slice.last() - dst_face_offsets_slice.first());
  threading::parallel_for(dst_face_offsets_slice.index_range(), 4092, [&](IndexRange subrange) {
    for (const int64_t local_face_index : subrange) {
      const int local_first_corner = dst_face_offsets_slice[local_face_index];
      const IndexRange src_range(
          local_first_corner, dst_face_offsets_slice[local_face_index + 1] - local_first_corner);
      const IndexRange dst_range(src_range.first() - dst_face_offsets_slice.first(),
                                 src_range.size());
      const Span<int> edgec_inds = edge_corner_indices.slice(src_range);

      /* Transfer corners */
      const int2 initial_sort = sort_shared_index(dst_edges[edgec_inds.last()],
                                                  dst_edges[edgec_inds.first()]);

      int next_common = initial_sort.y;
      face_corner_indices[dst_range.first()] = next_common;
      for (const int64_t i : edgec_inds.drop_front(1)) {
        next_common = get_unshared_index(dst_edges[i], next_common);
        face_corner_indices[dst_range[i]] = next_common;
      }
    }
  });
  return face_corner_indices;
}

void transfer_polygon_data(const Mesh &src_mesh,
                           Mesh &dst_mesh,
                           const Span<VariantPolygonSet> poly_sets,
                           const bke::AttributeFilter &attribute_filter)
{
  if (dst_mesh.faces_num == 0) {
    return;
  }

  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  Set<std::string> copy_poly_skip;
  copy_poly_skip.add(".corner_vert");
  copy_poly_skip.add(".corner_edge");

  const Span<int> src_vert_corners = src_mesh.corner_verts();
  const Span<int> src_edge_corners = src_mesh.corner_edges();
  const OffsetIndices<int> src_face_offsets(src_mesh.face_offsets());

  /* Assign polygon indices */
  Array<int64_t> set_face_count(poly_sets.size());
  {
    MutableSpan<int> dst_vert_corners = dst_mesh.corner_verts_for_write();
    MutableSpan<int> dst_edge_corners = dst_mesh.corner_edges_for_write();
    MutableSpan<int> dst_face_offsets = dst_mesh.face_offsets_for_write();
    const Span<int2> dst_edges = dst_mesh.edges();

    int64_t tot_corner_offset = 0;
    int64_t tot_face_count = 0;

    Vector<int> computed_corner_indices;

    dst_face_offsets[0] = 0;
    for (int64_t index = 0; index < poly_sets.size(); index++) {

      const int2 face_shape = get_face_shape(poly_sets[index]);
      const IndexRange dst_corner_range(tot_corner_offset, face_shape.y);
      MutableSpan<int> dst_face_offsets_slice = dst_face_offsets.slice(
          IndexRange(tot_face_count, face_shape.x + 1));

      tot_face_count += face_shape.x;
      tot_corner_offset += face_shape.y;
      set_face_count[index] = tot_face_count;

      if (face_shape.x == 0) {
        BLI_assert(face_shape.y == 0);
        continue;
      }

      /* Write face corner offsets */
      if (auto item = std::get_if<MeshTriangleSet>(&poly_sets[index])) {
        /* Write face offsets */
        item->fill_corner_offsets(dst_face_offsets_slice);

        const Span<int> edge_indices = item->edge_indices.cast<int>();
        dst_edge_corners.slice(dst_corner_range).copy_from(edge_indices);

        Span<int> vert_indices;
        if (item->vertex_indices.size() == 0) {
          computed_corner_indices = compute_polygon_vert_indices_from_edge_data(
              dst_face_offsets_slice, dst_edges, edge_indices);
          vert_indices = computed_corner_indices.as_span();
        }
        else {
          vert_indices = item->vertex_indices.cast<int>();
        }
        dst_vert_corners.slice(dst_corner_range).copy_from(vert_indices);
      }
      else if (auto item = std::get_if<MeshFaceSetCopyMask>(&poly_sets[index])) {

        item->fill_corner_table(src_face_offsets,
                                src_vert_corners,
                                src_edge_corners,
                                dst_face_offsets_slice,
                                dst_vert_corners.slice(dst_corner_range),
                                dst_edge_corners.slice(dst_corner_range));
      }
      else {
        BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
      }
    }
  }

  /* Copy face domain. */
  for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_FACE,
           bke::attribute_filter_with_skip_ref(attribute_filter, copy_poly_skip)))
  {
    const CPPType &cpp_type = *bke::custom_data_type_to_cpp_type(attribute.meta_data.data_type);
    bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
      using T = decltype(dummy);

      const Span<T> src_data = attribute.src.template typed<T>();
      MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

      int64_t face_offset = 0;
      for (int64_t index = 0; index < poly_sets.size(); index++) {
        const IndexRange dst_range(face_offset, set_face_count[index] - face_offset);

        if (auto item = std::get_if<MeshTriangleSet>(&poly_sets[index])) {
          array_utils::gather(src_data, item->src_polygon_indices, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshFaceSetCopyMask>(&poly_sets[index])) {
          array_utils::gather(src_data, item->src_indices, dst_data.slice(dst_range));
        }
        else {
          BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
        }
        face_offset += set_face_count[index];
      }
    });
  }

  /* Copy face corner domain. */
  {
    const OffsetIndices dst_face_offsets(dst_mesh.face_offsets());
    for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
             src_attributes,
             dst_attributes,
             ATTR_DOMAIN_MASK_CORNER,
             bke::attribute_filter_with_skip_ref(attribute_filter, copy_poly_skip)))
    {
      const CPPType &cpp_type = *bke::custom_data_type_to_cpp_type(attribute.meta_data.data_type);
      bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
        using T = decltype(dummy);

        const Span<T> src_data = attribute.src.template typed<T>();
        MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

        int64_t face_offset = 0;
        for (int64_t set_index = 0; set_index < poly_sets.size(); set_index++) {
          if (auto item = std::get_if<MeshTriangleSet>(&poly_sets[set_index])) {
            const IndexRange face_range(face_offset, set_face_count[set_index] - face_offset);
            const Span<float> src_weights = item->src_weights.cast<float>();
            const int64_t first_corner = dst_face_offsets[face_range.first()].first();

            threading::parallel_for(face_range, 4092, [&](IndexRange subrange) {
              for (const int64_t face_index : subrange) {
                for (const int64_t dst_corner_index : dst_face_offsets[face_index]) {
                  const int64_t src_index = dst_corner_index - first_corner;
                  const int2 cpair = item->src_corners[src_index];
                  /* Checking if cpair.y == -1 but catching 0.0f weights */
                  BLI_assert(cpair.y >= 0 || src_weights[src_index] == 0.0f);
                  dst_data[dst_corner_index] = src_weights[src_index] == 0.0f ?
                                                   src_data[cpair.x] :
                                                   bke::attribute_math::mix2(
                                                       src_weights[src_index],
                                                       src_data[cpair.x],
                                                       src_data[cpair.y]);
                }
              }
            });
          }
          else if (auto item = std::get_if<MeshFaceSetCopyMask>(&poly_sets[set_index])) {
            const IndexRange local_face_range(set_face_count[set_index] - face_offset);
            threading::parallel_for(local_face_range, 4092, [&](IndexRange subrange) {
              for (const int64_t face_index : subrange) {
                const int64_t src_face_index = item->src_indices[face_index];
                const IndexRange src_range = src_face_offsets[src_face_index];
                const IndexRange dst_range = dst_face_offsets[face_index + face_offset];

                for (const int64_t index : src_range.index_range()) {
                  dst_data[dst_range[index]] = src_data[src_range[index]];
                }
              }
            });
          }
          else {
            BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
          }
          face_offset += set_face_count[set_index];
        }
      });
    }
  }
}

Mesh *build_mesh_from_descriptors(const Mesh &src_mesh,
                                  const Span<VariantVertexSet> vertex_sets,
                                  const Span<VariantEdgeSet> edge_sets,
                                  const Span<VariantPolygonSet> poly_sets)
{
  const int2 pcount = polygon_count(poly_sets);
  return BKE_mesh_new_nomain_from_template(
      &src_mesh, vertex_count(vertex_sets), edge_count(edge_sets), pcount.x, pcount.y);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clip geometry
 * \{ */

enum IntersectionType { Discarded = 0, Intersect = 1, Kept = 2, TypeCount = 3 };

static int triangulation_edge_num(const int num_corner)
{
  return 2 * num_corner - 3;
}

static int clipped_triangulation_edge_num(const int ngon_corner_num, const int border_edge_cut_num)
{
  /* Considering cases such as a concave quad or a jagged shape
   * where all edges are cut with the major part kept:
   *
   *       *       *         Discarded
   *      / \     / \
   * ^^  x ^ x ^ x ^ x ^^^^^ <- Plane
   *    /     \ /     \
   *   /       *       \     Kept
   *  *-----------------*
   *
   * For jagged shapes with uneven corner count additional vertices are:
   *
   *  truncate(num_edges / 2) = 2 (in this example case)
   *
   * Note that:
   * - Number of cuts must be even, truncation allows computing 'worst cases' (given examples).
   * - Number of edges is equal to number of corners.
   */
  const int max_result_vert_num = border_edge_cut_num / 2 + ngon_corner_num;
  return triangulation_edge_num(max_result_vert_num);
}

struct LocalTesselationData {

  Vector<float3> corner_pos;
  Vector<bool> corner_flags;
  MemArena *pf_arena = nullptr;

  void init(const int max_per_face_corner_num)
  {
    if (corner_pos.size() != max_per_face_corner_num) {
      corner_pos.resize(max_per_face_corner_num);
      corner_flags.resize(max_per_face_corner_num);
    }
  }

  ~LocalTesselationData()
  {
    if (pf_arena) {
      BLI_memarena_free(pf_arena);
    }
  }
};

struct LocalClipData {

  Vector<bool> corner_flags;
  Vector<int2> edge_split_pairs;
  Vector<int2> index_vert_edge;
  Vector<float> edge_split_weight;

  void init(const int max_per_face_corner_num)
  {
    if (corner_flags.size() != max_per_face_corner_num) {
      corner_flags.resize(max_per_face_corner_num);

      /* This is worst case */
      const int max_edge_num = clipped_triangulation_edge_num(max_per_face_corner_num,
                                                              max_per_face_corner_num / 2);

      edge_split_pairs.resize(max_edge_num);
      index_vert_edge.resize(max_edge_num);
      edge_split_weight.resize(max_edge_num);
    }
  }
};

/* Compute interpolation weight for an edge.
 */
static inline float edge_weight(const float signed_dist_x, const float signed_dist_y)
{
  const float abs_d1 = abs(signed_dist_x);
  const float tot_dist = abs_d1 + abs(signed_dist_y);
  return math::safe_divide<float>(abs_d1, tot_dist);
};

/* Sort the indices so that x < y.
 */
static inline int2 sorted_edge(const int2 edge)
{
  if (edge.y < edge.x) {
    return {edge.y, edge.x};
  }
  return edge;
}

static inline float sorted_weight(const int2 edge, const float weight)
{
  if (edge.y < edge.x) {
    return 1.0f - weight;
  }
  return weight;
}

static inline bool is_sequential_pair(const int2 ab, const int tot_size)
{
  return (ab.x + 1) % tot_size == ab.y;
}

/*
 *  True if the vertex is 'outside' the plane and clipped.
 */
static inline bool is_clipped(const float signed_distance)
{
  return signed_distance >= 0.0f;
};

static int compute_triangulation_offsets(const Mesh &src_mesh,
                                         const IndexMask intersected_tris,
                                         MutableSpan<int> r_tesselation_offsets)
{
  const OffsetIndices src_polys = src_mesh.faces();

  const int max_per_face_corner_num = intersected_tris.parallel_reduce(
      GrainSize(4096),
      0,
      [&](const int64_t index, const int64_t index_pos, const int & /* identity */) {
        const int face_size = src_polys[index].size();
        const int tri_count = bke::mesh::face_triangles_num(face_size);
        r_tesselation_offsets[index_pos] = tri_count;
        return face_size;
      },
      [](int a, int b) { return std::max<int>(a, b); });

  std::partial_sum(
      r_tesselation_offsets.begin(), r_tesselation_offsets.end(), r_tesselation_offsets.begin());

  return max_per_face_corner_num;
}

static inline IndexMask classify_vertices(const Mesh &src_mesh,
                                          const ClipByPlaneArgs &args,
                                          IndexMaskMemory &memory,
                                          MutableSpan<float> signed_distance,
                                          MutableSpan<bool> vertex_flags)
{
  const Span<float3> src_positions = src_mesh.vert_positions();
  return IndexMask::from_predicate(
      src_positions.index_range(), GrainSize(4096), memory, [&](const int64_t i) {
        const float dist_to_plane = dist_signed_to_plane_v3(src_positions[i], args.plane);
        const bool is_kept = !is_clipped(dist_to_plane);
        vertex_flags[i] = is_kept;
        signed_distance[i] = dist_to_plane;
        return is_kept;
      });
}

static inline void classify_edges(const Mesh &src_mesh,
                                  const Span<bool> vertex_flags,
                                  IndexMaskMemory &memory,
                                  MutableSpan<IndexMask> selections)
{
  const Span<int2> src_edges = src_mesh.edges();
  IndexMask::from_groups<int64_t>(
      IndexMask(src_edges.index_range()),
      memory,
      [&](int64_t i) {
        const int8_t v1_kept = vertex_flags[src_edges[i].x];
        const int8_t v2_kept = vertex_flags[src_edges[i].y];
        return v1_kept + v2_kept; /* ==> IntersectionType. */
      },
      selections);
}

static inline void classify_faces(const Mesh &src_mesh,
                                  const Span<bool> vertex_flags,
                                  IndexMaskMemory &memory,
                                  MutableSpan<IndexMask> selections)
{
  const OffsetIndices src_polys = src_mesh.faces();
  const Span<int> src_corner_verts = src_mesh.corner_verts();

  IndexMask::from_groups<int64_t>(
      IndexMask(src_polys.index_range()),
      memory,
      [&](int64_t index_poly) {
        const IndexRange src_corner_range = src_polys[index_poly];
        const Span<int> corners = src_corner_verts.slice(src_corner_range);

        /* Loop over all edges except the last one (minimum 2 edges intersect).
         */
        const bool initial_kept = vertex_flags[corners.first()];
        for (const int64_t index : corners.index_range().drop_front(1)) {
          if (initial_kept != vertex_flags[corners[index]]) {
            return IntersectionType::Intersect;
          }
        }
        return initial_kept ? IntersectionType::Kept : IntersectionType::Discarded;
      },
      selections);
}

static void triangulate_clipped_geometry(const Mesh &src_mesh,
                                         const IndexMask intersected_tris,
                                         const OffsetIndices<int> tesselation_offsets,
                                         const Span<bool> vertex_flags,
                                         const int max_per_face_corner_num,
                                         MutableSpan<int3> r_tesselation,
                                         MutableSpan<int> r_tess_dst_offsets,
                                         MutableSpan<int> r_edge_split_dst_offsets,
                                         MutableSpan<int> r_edge_formed_dst_offsets,
                                         MutableSpan<int> r_edge_mask_offsets)
{
  const Span<float3> src_positions = src_mesh.vert_positions();
  const OffsetIndices src_polys = src_mesh.faces();
  const Span<int> src_corner_verts = src_mesh.corner_verts();

  threading::EnumerableThreadSpecific<LocalTesselationData> thread_local_tess_arena;
  Vector<int> local_corner_indices(max_per_face_corner_num);
  std::iota(local_corner_indices.begin(), local_corner_indices.end(), 0);

  /* Tesselate
   */
  intersected_tris.foreach_index(
      GrainSize(512), [&](const int64_t index_poly, const int64_t index_pos) {
        const IndexRange src_corner_range = src_polys[index_poly];
        const IndexRange local_corner_range(src_corner_range.size());
        const Span<int> local_corner_span = local_corner_indices.as_span().slice(
            0, src_corner_range.size());

        /* Count number of intersections on the boundary. */
        int prev_vert = src_corner_verts[src_corner_range.last()];
        int boundary_kept_count = 0;
        int boundary_intersect_count = 0;
        for (const int64_t corner : src_corner_range) {
          const int next_vert = src_corner_verts[corner];
          boundary_intersect_count += int(vertex_flags[prev_vert] != vertex_flags[next_vert]);
          boundary_kept_count += int(vertex_flags[prev_vert] && vertex_flags[next_vert]);
          prev_vert = next_vert;
        }
        BLI_assert(boundary_intersect_count % 2 == 0);

        /* Fetch/Initialize local data */
        LocalTesselationData &local_data = thread_local_tess_arena.local();
        local_data.init(max_per_face_corner_num);
        MutableSpan<float3> local_corner_pos = local_data.corner_pos.as_mutable_span().slice(
            0, local_corner_range.size());
        MutableSpan<bool> local_flags = local_data.corner_flags.as_mutable_span().slice(
            0, local_corner_range.size());
        array_utils::gather(
            src_positions, src_corner_verts.slice(src_corner_range), local_corner_pos);
        array_utils::gather(vertex_flags, src_corner_verts.slice(src_corner_range), local_flags);

        /* Tesselate */
        MutableSpan<int3> corner_tess_span = r_tesselation.slice(tesselation_offsets[index_pos]);
        bke::mesh::mesh_calc_tessellation_for_face(local_corner_span,
                                                   local_corner_pos,
                                                   0,
                                                   src_corner_range.size(),
                                                   corner_tess_span.data(),
                                                   &local_data.pf_arena);

        /* Count intersections within the tesselated triangle set. */
        int tris_count = 0; /* Number of tris formed from clipping. */
        int tris_clipped_count = 0;
        int shared_edge_count = 0;
        int internal_edge_count = 0;
        for (int3 &tri : corner_tess_span) {
          const int local_vkept_num = local_flags[tri.x] + local_flags[tri.y] + local_flags[tri.z];
          if (local_vkept_num == 0) {
            continue;
          }
          const int8_t clip_ie1 = int8_t(local_flags[tri.x] != local_flags[tri.y]);
          const int8_t clip_ie2 = int8_t(local_flags[tri.y] != local_flags[tri.z]);
          const int8_t clip_ie3 = int8_t(local_flags[tri.z] != local_flags[tri.x]);

          const int8_t num_cuts = clip_ie1 + clip_ie2 + clip_ie3;
          if (num_cuts) {
            BLI_assert(num_cuts == 2);
            tris_clipped_count++; /* Total num_cuts / 2 */
            /* Adjust tri order to align edges as: cut, cut, in plane edge.
             * Order simplifies logic when generating the tri.
             */
            if (!clip_ie1) {
              const int x = tri.x;
              tri.x = tri.y;
              tri.y = tri.z;
              tri.z = x;
            }
            else if (!clip_ie2) {
              const int y = tri.y;
              tri.y = tri.x;
              tri.x = tri.z;
              tri.z = y;
            }
          }
          /* Note that case for 'local_vkept_num == 0' is already skipped. */
          tris_count += 1 + (local_vkept_num == 2);
          shared_edge_count += local_vkept_num - (local_vkept_num != 3);
          internal_edge_count += local_vkept_num == 3 ? 0 : local_vkept_num;
        }

        /* Store offset counts for the triangulated primitive. */
        const int formed_count = internal_edge_count +
                                 (shared_edge_count - boundary_kept_count) / 2;
        r_tess_dst_offsets[index_pos] = tris_count;
        r_edge_split_dst_offsets[index_pos + 1] = tris_clipped_count -
                                                  boundary_intersect_count / 2;
        r_edge_formed_dst_offsets[index_pos] = formed_count;
        r_edge_mask_offsets[index_pos] = tris_clipped_count; /* any cut => has edge on border */
      });

  /* Accumulate dst offsets */
  threading::parallel_invoke(
      r_tess_dst_offsets.size() > 4096,
      [&]() {
        std::partial_sum(
            r_tess_dst_offsets.begin(), r_tess_dst_offsets.end(), r_tess_dst_offsets.begin());
      },
      [&]() {
        std::partial_sum(r_edge_split_dst_offsets.begin(),
                         r_edge_split_dst_offsets.end(),
                         r_edge_split_dst_offsets.begin());
      },
      [&]() {
        std::partial_sum(r_edge_formed_dst_offsets.begin(),
                         r_edge_formed_dst_offsets.end(),
                         r_edge_formed_dst_offsets.begin());
      },
      [&]() {
        std::partial_sum(
            r_edge_mask_offsets.begin(), r_edge_mask_offsets.end(), r_edge_mask_offsets.begin());
      });
}

std::pair<Mesh *, ClipResult> clip_by_plane(const Mesh &src_mesh,
                                            const ClipByPlaneArgs &args,
                                            const bke::AttributeFilter &attribute_filter)
{
  const Span<int2> src_edges = src_mesh.edges();
  const OffsetIndices src_polys = src_mesh.faces();
  const Span<int> src_corner_verts = src_mesh.corner_verts();
  const Span<int> src_corner_edges = src_mesh.corner_edges();

  /* Classify vertices
   *
   *    *  <- 'Outside' relative to the plane
   *     \
   *  ^^^^ Plane ^^^^^
   *       \
   *        *  <-- 'Inside' relative to plane
   */
  IndexMaskMemory kept_memory;
  Array<bool> vertex_flags(src_mesh.verts_num);
  Array<float, 12> vertex_distance(src_mesh.verts_num);

  IndexMask kept_vertices = classify_vertices(
      src_mesh, args, kept_memory, vertex_distance, vertex_flags);

  /* Determine if there is any overlap at all! */
  const int num_verts_kept = int(kept_vertices.size());
  if (num_verts_kept == 0) {
    return {nullptr, ClipResult::Discard};
  }
  else if (num_verts_kept == src_mesh.verts_num) {
    return {nullptr, ClipResult::Keep};
  }

  /* Classify elements.
   */
  IndexMaskMemory edge_memory, face_memory;
  Array<IndexMask, IntersectionType::TypeCount> edge_selections(IntersectionType::TypeCount);
  Array<IndexMask, IntersectionType::TypeCount> face_selections(IntersectionType::TypeCount);
  classify_edges(src_mesh, vertex_flags, edge_memory, edge_selections);
  classify_faces(src_mesh, vertex_flags, face_memory, face_selections);

  if (edge_selections[IntersectionType::Intersect].size() == 0) {
    std::optional<Mesh *> partial_copy = mesh_copy_by_mask(src_mesh,
                                                           kept_vertices,
                                                           edge_selections[IntersectionType::Kept],
                                                           face_selections[IntersectionType::Kept],
                                                           attribute_filter);
    return {partial_copy.value(), ClipResult::Clipped};
  }

  /* Compute corner offsets for intersected polygons (synchronous) */
  const int num_edges_kept = int(edge_selections[IntersectionType::Kept].size());
  const int64_t num_intersected_polygons = face_selections[IntersectionType::Intersect].size();
  Vector<int> tesselation_offset_data(num_intersected_polygons + 1);
  tesselation_offset_data[0] = 0;

  const int max_per_face_corner_num = compute_triangulation_offsets(
      src_mesh,
      face_selections[IntersectionType::Intersect],
      tesselation_offset_data.as_mutable_span().slice(1, num_intersected_polygons));

  const OffsetIndices<int> tesselation_offsets(tesselation_offset_data);

  /* Compute triangulation (asynchronous) */
  Vector<int3> tesselation(tesselation_offsets.last());
  Vector<int> tesselation_offsets_buffer(num_intersected_polygons + 1);
  Vector<int> edge_split_dst_offset_buffer(num_intersected_polygons + 1);
  Vector<int> edge_formed_dst_offset_buffer(num_intersected_polygons + 1);
  Vector<int> edge_mask_offset_buffer(num_intersected_polygons + 1);
  tesselation_offsets_buffer[0] = 0;
  edge_split_dst_offset_buffer[0] = edge_selections[IntersectionType::Intersect].size();
  edge_formed_dst_offset_buffer[0] = 0;
  edge_mask_offset_buffer[0] = 0;

  triangulate_clipped_geometry(
      src_mesh,
      face_selections[IntersectionType::Intersect],
      tesselation_offsets,
      vertex_flags,
      max_per_face_corner_num,
      tesselation,
      tesselation_offsets_buffer.as_mutable_span().slice(1, num_intersected_polygons),
      edge_split_dst_offset_buffer.as_mutable_span(),
      edge_formed_dst_offset_buffer.as_mutable_span().slice(1, num_intersected_polygons),
      edge_mask_offset_buffer.as_mutable_span().slice(1, num_intersected_polygons));

  const OffsetIndices<int> tesselation_dst_offsets(tesselation_offsets_buffer);
  const OffsetIndices<int> edge_split_dst_offsets(edge_split_dst_offset_buffer);
  const OffsetIndices<int> edge_formed_dst_offsets(edge_formed_dst_offset_buffer);
  const OffsetIndices<int> index_mask_offsets(edge_mask_offset_buffer);
  BLI_assert(index_mask_offsets.total_size() == index_mask_offsets.last());

  /* Reverse maps */
  Array<int, 12> vertex_remap(src_mesh.verts_num);
  Array<int, 12> edge_remap(src_mesh.edges_num);
  index_mask::build_reverse_map<int>(kept_vertices, vertex_remap);
  index_mask::build_reverse_map<int>(edge_selections[IntersectionType::Kept], edge_remap);

  /* Buffers for vertices formed from edge splits, either of two types:
   * 1. Vertices formed by splitting existing edges in the input/source mesh
   * 2. Vertices formed splitting virtual/tesselated edges
   */
  const int64_t tot_split_edges = edge_split_dst_offsets.last();
  const int64_t num_total_edge_created = tot_split_edges + edge_formed_dst_offsets.last();

  Array<int, 12> tesselation_polygon_indices(tesselation_dst_offsets.last());
  Array<int3, 12> tesselation_vertex_indices(tesselation_dst_offsets.last());
  Array<int3, 12> tesselation_edge_indices(tesselation_vertex_indices.size());
  Array<float3, 12> tesselation_corner_weights(tesselation_vertex_indices.size());
  Array<int2, 12> tesselation_corner_vertices(tesselation_vertex_indices.size() * 3);

  Array<float, 12> vertex_split_weights(tot_split_edges);
  Array<int2, 12> vertex_split_vertices(tot_split_edges);

  Array<int2, 12> edge_output_vertex_pairs(num_total_edge_created);
  Array<int2, 12> edge_output_src_edges(num_total_edge_created);

  /* Index mask for tracking edges formed in the cutting plane. */
  Vector<int> edge_border_index_mask(index_mask_offsets.last());

  /* Split an edge intersecting the plane (either original edge or edge formed by tesselation).
   */
  auto fn_split_edge = [&](const int2 src_vert_indices,
                           const int64_t index_offset,
                           const int2 src_edge_indices,
                           float &r_edge_weight) {
    const float signed_dist_x = vertex_distance[src_vert_indices.x];
    const float signed_dist_y = vertex_distance[src_vert_indices.y];
    const int other_vertex = is_clipped(signed_dist_x) ? src_vert_indices.y : src_vert_indices.x;

    const int new_vert_index = num_verts_kept + index_offset;
    const int new_edge_index = num_edges_kept + index_offset;

    r_edge_weight = edge_weight(signed_dist_x, signed_dist_y);
    vertex_split_weights[index_offset] = r_edge_weight;
    vertex_split_vertices[index_offset] = src_vert_indices;

    edge_output_vertex_pairs[index_offset] = int2(vertex_remap[other_vertex], new_vert_index);
    edge_output_src_edges[index_offset] = src_edge_indices;

    return int2{new_vert_index, new_edge_index};
  };

  /* Split existing edges (asynchronous)
   */
  edge_selections[IntersectionType::Intersect].foreach_index(
      GrainSize(4092), [&](const int64_t index_src_edge, const int64_t index_pos) {
        float edge_weight;
        const int2 vert_indices = sorted_edge(src_edges[index_src_edge]);
        const int2 ve_index = fn_split_edge(
            vert_indices, index_pos, int2{int(index_src_edge), -1}, edge_weight);
        edge_remap[index_src_edge] = ve_index.y;
      });

  /* Generate output triangulation (asynchronous)
   */
  threading::EnumerableThreadSpecific<LocalClipData> thread_local_clip_buffers;
  face_selections[IntersectionType::Intersect].foreach_index(
      GrainSize(512), [&](const int64_t index_poly, const int64_t index_pos) {
        const IndexRange src_corner_range = src_polys[index_poly];
        const IndexRange local_corner_range(src_corner_range.size());

        const IndexRange dst_edge_split = edge_split_dst_offsets[index_pos];
        const IndexRange dst_edge_formed = edge_formed_dst_offsets[index_pos];
        const IndexRange dst_tesselation = tesselation_dst_offsets[index_pos];
        const IndexRange dst_edge_cut_index = index_mask_offsets[index_pos];

        MutableSpan<int> local_tess_poly_src = tesselation_polygon_indices.as_mutable_span().slice(
            dst_tesselation);
        MutableSpan<int3> local_tess_inds = tesselation_vertex_indices.as_mutable_span().slice(
            dst_tesselation);
        MutableSpan<int3> local_tess_edges = tesselation_edge_indices.as_mutable_span().slice(
            dst_tesselation);

        MutableSpan<float3> local_tess_corner_weights =
            tesselation_corner_weights.as_mutable_span().slice(dst_tesselation);
        MutableSpan<int2> local_tess_corner_vertices =
            tesselation_corner_vertices.as_mutable_span().slice(dst_tesselation.first() * 3,
                                                                dst_tesselation.size() * 3);

        const Span<int> local_corner_verts = src_corner_verts.slice(src_corner_range);
        const Span<int> local_corner_edges = src_corner_edges.slice(src_corner_range);

        /* Fetch/Initiate local buffers */
        LocalClipData &local_data = thread_local_clip_buffers.local();
        local_data.init(max_per_face_corner_num);

        MutableSpan<bool> local_flags = local_data.corner_flags.as_mutable_span().slice(
            0, local_corner_range.size());
        array_utils::gather(
            vertex_flags.as_span(), src_corner_verts.slice(src_corner_range), local_flags);

        MutableSpan<int2> edge_split_pairs = local_data.edge_split_pairs.as_mutable_span();
        MutableSpan<int2> index_vert_edge = local_data.index_vert_edge.as_mutable_span();
        MutableSpan<float> edge_split_weight = local_data.edge_split_weight.as_mutable_span();

        /* Functionality for fetching and creating internal edges in the triangulation.
         */
        int count_lookup_edges = 0; /* Counts the number of entries in the lookup table. */
        auto fn_split_pair_end = [&]() { return edge_split_pairs.begin() + count_lookup_edges; };

        auto fn_fetch_boundary_edge = [&](const int2 pair) {
          /* Fetch an already cut edge from the N-gon boundary and append it to the local map.
           */
          const int edge_index = edge_remap[local_corner_edges[pair.x]];
          const int offset_virtual_split = edge_index - num_edges_kept;
          const int vertex_index = num_verts_kept + offset_virtual_split;

          const int2 sorted_pair = sorted_edge(pair);
          index_vert_edge[count_lookup_edges] = int2{vertex_index, edge_index};
          edge_split_pairs[count_lookup_edges] = sorted_pair;
          /* Ensure weight is sorted in local order, this may invert an inverted value */
          edge_split_weight[count_lookup_edges] = sorted_weight(
              int2{local_corner_verts[sorted_pair.x], local_corner_verts[sorted_pair.y]},
              vertex_split_weights[offset_virtual_split]);
          count_lookup_edges++;
        };

        /* Fetch the split boundary edges */
        int prev_corner = local_corner_range.last();
        for (const int corner : local_corner_range) {
          if (local_flags[corner] != local_flags[prev_corner]) {
            fn_fetch_boundary_edge(int2{prev_corner, corner});
          }
          prev_corner = corner;
        }

        int edges_formed_count = 0;
        auto new_formed_edge_index = [&]() {
          const int formed_offset = edge_split_dst_offsets.last() + dst_edge_formed.first() +
                                    edges_formed_count;
          edges_formed_count++;
          return int2{formed_offset, num_edges_kept + formed_offset};
        };

        int count_edges_split = 0;
        /* Find existing edge split or create a new edge.
         */
        auto fn_find_or_split_edge = [&](int2 corners) {
          const int2 pair = sorted_edge(corners);
          auto it = std::find(edge_split_pairs.begin(), fn_split_pair_end(), pair);

          if (it == fn_split_pair_end()) {
            /* Split the edge */
            const int2 vert_indices{local_corner_verts[pair.x], local_corner_verts[pair.y]};
            const int64_t offset = dst_edge_split.first() + count_edges_split;

            edge_split_pairs[count_lookup_edges] = pair;
            index_vert_edge[count_lookup_edges] = fn_split_edge(
                vert_indices, offset, int2{-1, -1}, edge_split_weight[count_lookup_edges]);

            count_edges_split++;
            return count_lookup_edges++;
          }
          return int(std::distance(edge_split_pairs.begin(), it));
        };

        /* Form a new edge inside the N-gon that isn't intersected by the plane.
         */
        auto fn_find_or_create_inner_edge = [&](const int2 corners) {
          const int2 pair = sorted_edge(corners);
          const auto it = std::find(edge_split_pairs.begin(), fn_split_pair_end(), pair);

          if (it == fn_split_pair_end()) {
            /* Create a new edge entry! */
            const int2 offset_index = new_formed_edge_index();
            edge_split_pairs[count_lookup_edges] = pair;
            index_vert_edge[count_lookup_edges] = int2(-1, offset_index.y);

            edge_output_vertex_pairs[offset_index.x] = int2{
                vertex_remap[local_corner_verts[corners.x]],
                vertex_remap[local_corner_verts[corners.y]]};
            edge_output_src_edges[offset_index.x] = int2{
                edge_remap[local_corner_edges[corners.x]],
                edge_remap[local_corner_verts[corners.y]]};

            count_lookup_edges++;
            return offset_index.y;
          }
          const int64_t local_split_index = std::distance(edge_split_pairs.begin(), it);
          return index_vert_edge[local_split_index].y;
        };

        int edges_formed_in_plane = 0;
        /* Form a new edge inside the N-gon between two intersections (result is in the cutting
         * plane).
         */
        auto fn_form_edge_in_clip_plane = [&](const int2 new_vert_indices,
                                              const int2 src_edge_indices) {
          const int2 offset_index = new_formed_edge_index();
          edge_output_vertex_pairs[offset_index.x] = new_vert_indices;
          edge_output_src_edges[offset_index.x] = src_edge_indices;

          edge_border_index_mask[dst_edge_cut_index[edges_formed_in_plane]] = offset_index.y;

          edges_formed_in_plane++;
          return offset_index.y;
        };

        /* Form a new edge inside the triangle itself, between a clipped edge and interior vertex
         * (occurs when output from spliting the tesselated tri forms a quad).
         */
        auto fn_form_edge_diagonal = [&](const int split_vert_index,
                                         const int corner_index,
                                         const int split_edge_source_index) {
          const int2 offset_index = new_formed_edge_index();
          edge_output_vertex_pairs[offset_index.x] = int2(
              split_vert_index, vertex_remap[local_corner_verts[corner_index]]);
          edge_output_src_edges[offset_index.x] = int2(
              edge_remap[local_corner_edges[corner_index]], split_edge_source_index);

          return offset_index.y;
        };

        int tri_count = 0;
        for (const int3 &tri : tesselation.as_span().slice(tesselation_offsets[index_pos])) {
          const int local_vkept_num = local_flags[tri.x] + local_flags[tri.y] + local_flags[tri.z];
          if (local_vkept_num == 0) {
            continue;
          }
          local_tess_poly_src[tri_count] = index_poly;

          if (local_vkept_num == 3) {
            /* No intersection... Keep whole and form inner edge(s) */
            const int2 xy = int2{tri.x, tri.y};
            const int2 yz = int2{tri.y, tri.z};
            const int2 zx = int2{tri.z, tri.x};
            const int e0 = is_sequential_pair(xy, local_corner_range.size()) ?
                               edge_remap[local_corner_edges[tri.x]] :
                               fn_find_or_create_inner_edge(xy);
            const int e1 = is_sequential_pair(yz, local_corner_range.size()) ?
                               edge_remap[local_corner_edges[tri.y]] :
                               fn_find_or_create_inner_edge(yz);
            const int e2 = is_sequential_pair(zx, local_corner_range.size()) ?
                               edge_remap[local_corner_edges[tri.z]] :
                               fn_find_or_create_inner_edge(zx);
            local_tess_edges[tri_count] = int3{e0, e1, e2};
            local_tess_inds[tri_count] = int3{vertex_remap[local_corner_verts[tri.x]],
                                              vertex_remap[local_corner_verts[tri.y]],
                                              vertex_remap[local_corner_verts[tri.z]]};

            const int coffset = tri_count * 3;
            local_tess_corner_vertices[coffset + 0] = int2{int(src_corner_range[tri.x]), -1};
            local_tess_corner_vertices[coffset + 1] = int2{int(src_corner_range[tri.y]), -1};
            local_tess_corner_vertices[coffset + 2] = int2{int(src_corner_range[tri.z]), -1};
            local_tess_corner_weights[tri_count] = float3{0.0f, 0.0f, 0.0f};
            tri_count++;
          }
          else if (local_vkept_num == 1) {
            /*
             * Case
             *
             * 0          2
             *  *---------*    Outside
             *   \       /
             *  ^ x ^ ^ x ^ <- Plane
             *   b \   / a
             *      \ /        Inside
             *       * 1, c
             */
            const int split_index_b = fn_find_or_split_edge(int2{tri.x, tri.y});
            const int split_index_a = fn_find_or_split_edge(int2{tri.y, tri.z});

            const int2 ve_clip_a = index_vert_edge[split_index_a];
            const int2 ve_clip_b = index_vert_edge[split_index_b];

            /* Sorted xy, yz index pairs */
            const int2 pair_a = edge_split_pairs[split_index_a];
            const int2 pair_b = edge_split_pairs[split_index_b];

            const int e0 = fn_form_edge_in_clip_plane(int2{ve_clip_b.x, ve_clip_a.x},
                                                      int2{-1, -1});
            const int v1 = local_corner_verts[tri.y];
            const int vc = vertex_remap[v1];
            BLI_assert(vertex_flags[v1]);
            BLI_assert(vc >= 0);

            local_tess_inds[tri_count] = int3{ve_clip_a.x, ve_clip_b.x, vc};
            local_tess_edges[tri_count] = int3{e0, ve_clip_b.y, ve_clip_a.y};

            const int coffset = tri_count * 3;
            local_tess_corner_vertices[coffset + 0] = int2{int(src_corner_range[pair_a.x]),
                                                           int(src_corner_range[pair_a.y])};
            local_tess_corner_vertices[coffset + 1] = int2{int(src_corner_range[pair_b.x]),
                                                           int(src_corner_range[pair_b.y])};
            local_tess_corner_vertices[coffset + 2] = int2{int(src_corner_range[tri.y]), -1};
            local_tess_corner_weights[tri_count] = float3{
                edge_split_weight[split_index_a], edge_split_weight[split_index_b], 0.0f};
            tri_count++;
          }
          else {
            /*
             * Case
             *
             *         1
             *          *              Outside
             *         / \
             *        /   \
             *   ^ ^ x ^ ^ x ^ ^ ^  <- Plane
             *      / II c  \         c == Inner triangulation edge
             *  b  /   c     \  a     I, II == First/Second triangle formed
             *    / c     I   \
             * 2 *-------------*  0    Inside
             *
             *      ^ Inner edge
             */
            const int split_index0 = fn_find_or_split_edge(int2{tri.x, tri.y});
            const int split_index1 = fn_find_or_split_edge(int2{tri.y, tri.z});

            const int2 ve_clip_a = index_vert_edge[split_index0];
            const int2 ve_clip_b = index_vert_edge[split_index1];

            /* Sorted xy, yz index pairs */
            const int2 pair_01 = edge_split_pairs[split_index0];
            const int2 pair_12 = edge_split_pairs[split_index1];

            const int2 zx = int2{tri.z, tri.x};
            const int einner = is_sequential_pair(zx, local_corner_range.size()) ?
                                   edge_remap[local_corner_edges[tri.z]] :
                                   fn_find_or_create_inner_edge(zx);

            const int ecut = fn_form_edge_in_clip_plane(int2{ve_clip_a.x, ve_clip_b.x},
                                                        int2{-1, -1});
            const int ediag = fn_form_edge_diagonal(ve_clip_a.x, tri.z, -1);
            const int v0 = vertex_remap[local_corner_verts[tri.x]];
            const int v2 = vertex_remap[local_corner_verts[tri.z]];

            local_tess_inds[tri_count] = int3{v0, ve_clip_a.x, v2};
            local_tess_inds[tri_count + 1] = int3{v2, ve_clip_a.x, ve_clip_b.x};

            local_tess_edges[tri_count] = int3{ve_clip_a.y, ediag, einner};
            local_tess_edges[tri_count + 1] = int3{ediag, ecut, ve_clip_b.y};

            const int coffset = tri_count * 3;
            local_tess_corner_vertices[coffset + 0] = int2{int(src_corner_range[tri.x]), -1};
            local_tess_corner_vertices[coffset + 1] = int2{int(src_corner_range[pair_01.x]),
                                                           int(src_corner_range[pair_01.y])};
            local_tess_corner_vertices[coffset + 2] = int2{int(src_corner_range[tri.z]), -1};

            local_tess_corner_vertices[coffset + 3] = int2{int(src_corner_range[tri.z]), -1};
            local_tess_corner_vertices[coffset + 4] = int2{int(src_corner_range[pair_01.x]),
                                                           int(src_corner_range[pair_01.y])};
            local_tess_corner_vertices[coffset + 5] = int2{int(src_corner_range[pair_12.x]),
                                                           int(src_corner_range[pair_12.y])};

            local_tess_corner_weights[tri_count] = float3{
                0.0f, edge_split_weight[split_index0], 0.0f};
            local_tess_corner_weights[tri_count + 1] = float3{
                0.0f, edge_split_weight[split_index0], edge_split_weight[split_index1]};

            local_tess_poly_src[tri_count + 1] = index_poly;
            tri_count += 2;
          }
        }
      });

  const int copied_corner_count = face_selections[IntersectionType::Kept].parallel_reduce(
      GrainSize(4096),
      0,
      [&](const int64_t index_face, const int64_t /* index_pos */, const int & /* identity */) {
        return int(src_polys[index_face].size());
      },
      [](int a, int b) { return a + b; });

  /* Set Descriptors */
  std::array<VariantVertexSet, 2> vertex_sets = {
      MeshVertexSetCopyMask{kept_vertices},
      MeshVertexSetLinear{vertex_split_vertices.as_span(), vertex_split_weights.as_span()}};

  std::array<VariantEdgeSet, 2> edge_sets = {
      MeshEdgeSetCopyMask{edge_selections[IntersectionType::Kept], vertex_remap.as_span()},
      MeshEdgeSetPair{edge_output_vertex_pairs.as_span(), edge_output_src_edges.as_span()}};

  std::array<VariantPolygonSet, 2> poly_sets = {
      MeshFaceSetCopyMask{copied_corner_count,
                          face_selections[IntersectionType::Kept],
                          vertex_remap.as_span(),
                          edge_remap.as_span()},
      MeshTriangleSet{tesselation_polygon_indices,
                      tesselation_vertex_indices,
                      tesselation_edge_indices,
                      tesselation_corner_weights,
                      tesselation_corner_vertices}};

  /* Create new mesh */
  Mesh *result = build_mesh_from_descriptors(src_mesh, vertex_sets, edge_sets, poly_sets);
  transfer_vertex_data(src_mesh, *result, vertex_sets, attribute_filter);
  transfer_edge_data(src_mesh, *result, edge_sets, attribute_filter);
  transfer_polygon_data(src_mesh, *result, poly_sets, attribute_filter);

  /* Create output attribute. */
  if (args.plane_selection_attr_id) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    bke::SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_only_span<bool>(
        *args.plane_selection_attr_id, bke::AttrDomain::Edge);
    selection.span.fill(false);

    threading::parallel_for(edge_border_index_mask.index_range(), 200000, [&](IndexRange range) {
      for (const int64_t index : range) {
        selection.span[edge_border_index_mask[index]] = true;
      }
    });
    selection.finish();
  }

  return {result, ClipResult::Clipped};
}

/** \} */

}  // namespace blender::geometry
