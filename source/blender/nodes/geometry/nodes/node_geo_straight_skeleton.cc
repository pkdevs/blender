/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <fmt/format.h>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_bounds_types.hh"
#include "BLI_index_mask.hh"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_filter.hh"
#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.hh"

#include "FN_field.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_join_geometries.hh"

#include "NOD_socket_usage_inference.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_straight_skeleton_cc {

enum class StraightSkeletonMode : int8_t {
  FullSkeleton = 0,
  Offset = 1,
};

static const EnumPropertyItem mode_items[] = {
    {int(StraightSkeletonMode::FullSkeleton),
     "FULL_SKELETON",
     0,
     N_("Full Skeleton"),
     N_("Compute complete straight skeleton / full roof tiling")},
    {int(StraightSkeletonMode::Offset),
     "OFFSET",
     0,
     N_("Offset"),
     N_("Inset or outset by the specified distance")},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class StraightSkeletonDirection : int8_t {
  Inside = 0,
  Outside = 1,
  Both = 2,
};

static const EnumPropertyItem direction_items[] = {
    {int(StraightSkeletonDirection::Inside),
     "INSIDE",
     0,
     N_("Inside"),
     N_("Propagate wavefronts inward (between outer boundary and holes)")},
    {int(StraightSkeletonDirection::Outside),
     "OUTSIDE",
     0,
     N_("Outside"),
     N_("Propagate wavefronts outward towards an expanded bounding box")},
    {int(StraightSkeletonDirection::Both),
     "BOTH",
     0,
     N_("Both"),
     N_("Propagate wavefronts both inward and outward")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::GreasePencil})
      .description(
          "The input geometry (projected onto the X-Y plane) as a planar straight line graph "
          "(faces, holes, and wire edges)");
  b.add_input<decl::Int>("Group ID"_ustr)
      .evaluated_geometry_field()
      .hide_value()
      .description(
          "An ID used to group elements together. Straight skeleton computation is done "
          "separately for each group");
  b.add_input<decl::Menu>("Direction"_ustr)
      .static_items(direction_items)
      .default_value(MenuValue(StraightSkeletonDirection::Inside))
      .optional_label()
      .description("Wavefront propagation direction");
  b.add_input<decl::Menu>("Mode"_ustr)
      .static_items(mode_items)
      .default_value(MenuValue(StraightSkeletonMode::FullSkeleton))
      .optional_label();
  b.add_input<decl::Float>("Distance"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Offset distance")
      .usage_inference(
          [](const socket_usage_inference::SocketUsageParams &params) -> std::optional<bool> {
            return params.menu_input_may_be("Mode"_ustr, int(StraightSkeletonMode::Offset));
          });
  b.add_input<decl::Int>("Segments"_ustr)
      .default_value(1)
      .min(1)
      .max(1000)
      .description("Number of equal-width offset bands to subdivide into");
  b.add_input<decl::Float>("Bounding Box Margin %"_ustr)
      .default_value(10.0f)
      .min(0.0f)
      .max(1000.0f)
      .description("Expansion percentage for enclosing bounding box in Outside/Both modes");
  b.add_input<decl::Float>("Elevation Scale"_ustr)
      .default_value(1.0f)
      .description("Scale factor to set vertex Z height from collapse time / offset distance");
  b.add_input<decl::Float>("Epsilon"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Coincidence threshold (0.0 uses default 1e-8)");

  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all_geometry();
  b.add_output<decl::Float>("Distance"_ustr)
      .anonymous_attribute_output()
      .description("Collapse time / offset distance from boundary at each point");
  b.add_output<decl::Bool>("Is Skeleton"_ustr)
      .anonymous_attribute_output()
      .description("True for newly created skeleton vertices");
  b.add_output<decl::Int>("Face Band"_ustr)
      .anonymous_attribute_output()
      .description("Band index (0 to segments-1) of each face");
  b.add_output<decl::Int>("Original Face"_ustr)
      .anonymous_attribute_output()
      .description("Boundary face index the output face was derived from");
}

/* -------------------------------------------------------------------- */
/** \name Group Data Structures
 * \{ */

enum class SourceComponent : int8_t {
  Mesh = 0,
  Curve = 1,
};

struct GroupMasks {
  IndexMask mesh_verts;
  IndexMask mesh_loose_edges;
  IndexMask mesh_faces;
  IndexMask curves;
  IndexMask curve_points;
};

struct GroupResult {
  straightskeleton::StraightSkeletonOutput ss_output;
  std::string error_message;
  bool is_error{false};
  bool is_noop{false};

  SourceComponent domain{SourceComponent::Mesh};
  Array<int> dst_point_to_src_point;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curves Topology Ingestion
 * \{ */

static void handle_pslg_zero_offset(Span<double2> all_verts,
                                    Span<int> face_offsets_data,
                                    Span<int> face_vert_indices,
                                    Span<int2> edges,
                                    GroupResult &r_result)
{
  r_result.is_noop = true;
  const int n = int(all_verts.size());
  r_result.ss_output.vert = Array<double2>(all_verts);
  r_result.ss_output.vert_time.reinitialize(n);
  r_result.ss_output.vert_time.fill(0.0);
  r_result.ss_output.vert_orig.reinitialize(n);
  for (int i = 0; i < n; i++) {
    r_result.ss_output.vert_orig[i] = i;
  }
  Vector<int2> all_edges;
  for (const int2 &e : edges) {
    all_edges.append(e);
  }
  for (int f = 0; f < face_offsets_data.size() - 1; f++) {
    const int start = face_offsets_data[f];
    const int end = face_offsets_data[f + 1];
    const int count = end - start;
    for (int i = 0; i < count; i++) {
      all_edges.append(
          int2(face_vert_indices[start + i], face_vert_indices[start + (i + 1) % count]));
    }
  }
  r_result.ss_output.edge = Array<int2>(all_edges.as_span());
  r_result.ss_output.face_offsets_data = Array<int>(face_offsets_data);
  r_result.ss_output.face_vert_indices = Array<int>(face_vert_indices);
}

static void calc_curve_group(const bke::CurvesGeometry &curves,
                             const IndexMask &curve_mask,
                             const StraightSkeletonMode mode,
                             const double offset,
                             const int segments,
                             const double epsilon,
                             const straightskeleton::PropagationDirection direction,
                             const double bbox_expansion_pct,
                             GroupResult &r_result)
{
  r_result.domain = SourceComponent::Curve;
  const OffsetIndices points_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> positions = curves.evaluated_positions();
  const VArray<bool> cyclic = curves.cyclic();

  Vector<double2> all_verts;
  Vector<int> orig_pt_map;
  Vector<int> face_offsets_data;
  Vector<int> face_vert_indices;
  Vector<int2> edges;

  face_offsets_data.append(0);

  const double weld_tol = (epsilon > 0.0) ? double(epsilon) : 1e-4;
  const double inv_weld_tol = 1.0 / weld_tol;

  Map<int64_t, int> coord_map;
  auto get_or_add_vert = [&](int p) -> int {
    const double2 co(positions[p].x, positions[p].y);
    const int64_t qx = int64_t(std::floor(co.x * inv_weld_tol));
    const int64_t qy = int64_t(std::floor(co.y * inv_weld_tol));

    for (int dx = -1; dx <= 1; dx++) {
      for (int dy = -1; dy <= 1; dy++) {
        const int64_t key = ((qx + dx) * 73856093) ^ ((qy + dy) * 19349663);
        if (const int *found = coord_map.lookup_ptr(key)) {
          if (math::distance(all_verts[*found], co) <= weld_tol) {
            return *found;
          }
        }
      }
    }
    const int idx = int(all_verts.size());
    all_verts.append(co);
    orig_pt_map.append(p);
    const int64_t key = (qx * 73856093) ^ (qy * 19349663);
    coord_map.add(key, idx);
    return idx;
  };

  curve_mask.foreach_index([&](const int64_t curve_idx) {
    const IndexRange pts = points_by_curve[curve_idx];
    if (pts.size() < 2) {
      return;
    }
    const bool is_closed = cyclic[curve_idx];
    if (is_closed && pts.size() >= 3) {
      const int face_start = int(face_vert_indices.size());
      for (const int p : pts) {
        const int local_idx = get_or_add_vert(p);
        if (face_vert_indices.size() == face_start || face_vert_indices.last() != local_idx) {
          face_vert_indices.append(local_idx);
        }
      }
      if (face_vert_indices.size() > face_start + 1 &&
          face_vert_indices.last() == face_vert_indices[face_start])
      {
        face_vert_indices.pop_last();
      }
      if (face_vert_indices.size() - face_start >= 3) {
        face_offsets_data.append(int(face_vert_indices.size()));
      }
      else {
        face_vert_indices.resize(face_start);
      }
    }
    else {
      int prev_local = -1;
      for (const int p : pts) {
        const int curr_local = get_or_add_vert(p);
        if (prev_local != -1 && prev_local != curr_local) {
          edges.append(int2(prev_local, curr_local));
        }
        prev_local = curr_local;
      }
    }
  });

  if (all_verts.is_empty()) {
    r_result.is_noop = true;
    return;
  }

  if (mode == StraightSkeletonMode::Offset && offset == 0.0) {
    handle_pslg_zero_offset(all_verts.as_span(),
                            face_offsets_data.as_span(),
                            face_vert_indices.as_span(),
                            edges.as_span(),
                            r_result);
    r_result.dst_point_to_src_point = Array<int>(orig_pt_map.as_span());
    return;
  }

  const OffsetIndices<int> face_offsets(face_offsets_data.as_span());

  straightskeleton::StraightSkeletonInput input;
  input.vert = all_verts.as_span();
  input.face_offsets = face_offsets;
  input.face_vert_indices = face_vert_indices.as_span();
  input.edge = edges.as_span();
  input.direction = direction;
  input.bbox_expansion_pct = bbox_expansion_pct;
  input.offset = offset;
  input.segments = math::max(1, segments);
  input.epsilon = epsilon;

  r_result.ss_output = straightskeleton::straight_skeleton_calc(input);
  if (!r_result.ss_output.is_valid || r_result.ss_output.hit_max_events) {
    r_result.is_error = true;
    r_result.error_message = r_result.ss_output.error_message.empty() ?
                                 "Straight skeleton calculation failed on curve group." :
                                 r_result.ss_output.error_message;
    return;
  }

  r_result.dst_point_to_src_point.reinitialize(r_result.ss_output.vert.size());
  r_result.dst_point_to_src_point.fill(-1);
  for (int i = 0; i < r_result.ss_output.vert.size(); i++) {
    const int orig_local = r_result.ss_output.vert_orig[i];
    if (orig_local >= 0 && orig_local < orig_pt_map.size()) {
      r_result.dst_point_to_src_point[i] = orig_pt_map[orig_local];
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh PSLG Calculation
 * \{ */

static void calc_mesh_group(const Mesh &mesh,
                            const GroupMasks &group,
                            const StraightSkeletonMode mode,
                            const double offset,
                            const int segments,
                            const double epsilon,
                            const straightskeleton::PropagationDirection direction,
                            const double bbox_expansion_pct,
                            GroupResult &r_result)
{
  r_result.domain = SourceComponent::Mesh;
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> mesh_edges = mesh.edges();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  Vector<double2> all_verts;
  Vector<int> orig_pt_map;
  Map<int, int> vert_to_local;

  auto get_or_add_vert = [&](int v) -> int {
    if (const int *p = vert_to_local.lookup_ptr(v)) {
      return *p;
    }
    const int idx = int(all_verts.size());
    all_verts.append(double2(positions[v].x, positions[v].y));
    orig_pt_map.append(v);
    vert_to_local.add(v, idx);
    return idx;
  };

  Vector<int> face_offsets_data;
  Vector<int> face_vert_indices;
  face_offsets_data.append(0);

  group.mesh_faces.foreach_index([&](const int64_t f_idx) {
    const IndexRange r = faces[f_idx];
    if (r.size() < 3) {
      return;
    }
    for (const int c : r) {
      const int v = corner_verts[c];
      face_vert_indices.append(get_or_add_vert(v));
    }
    face_offsets_data.append(int(face_vert_indices.size()));
  });

  Vector<int2> edges;
  group.mesh_loose_edges.foreach_index([&](const int64_t e_idx) {
    const int v1 = mesh_edges[e_idx][0];
    const int v2 = mesh_edges[e_idx][1];
    edges.append(int2(get_or_add_vert(v1), get_or_add_vert(v2)));
  });

  if (all_verts.is_empty()) {
    r_result.is_noop = true;
    return;
  }

  if (mode == StraightSkeletonMode::Offset && offset == 0.0) {
    handle_pslg_zero_offset(all_verts.as_span(),
                            face_offsets_data.as_span(),
                            face_vert_indices.as_span(),
                            edges.as_span(),
                            r_result);
    r_result.dst_point_to_src_point = Array<int>(orig_pt_map.as_span());
    return;
  }

  const OffsetIndices<int> face_offsets(face_offsets_data.as_span());

  straightskeleton::StraightSkeletonInput input;
  input.vert = all_verts.as_span();
  input.face_offsets = face_offsets;
  input.face_vert_indices = face_vert_indices.as_span();
  input.edge = edges.as_span();
  input.direction = direction;
  input.bbox_expansion_pct = bbox_expansion_pct;
  input.offset = offset;
  input.segments = math::max(1, segments);
  input.epsilon = epsilon;

  r_result.ss_output = straightskeleton::straight_skeleton_calc(input);
  if (!r_result.ss_output.is_valid || r_result.ss_output.hit_max_events) {
    r_result.is_error = true;
    r_result.error_message = r_result.ss_output.error_message.empty() ?
                                 "Straight skeleton calculation failed on mesh group." :
                                 r_result.ss_output.error_message;
    return;
  }

  r_result.dst_point_to_src_point.reinitialize(r_result.ss_output.vert.size());
  r_result.dst_point_to_src_point.fill(-1);
  for (int i = 0; i < r_result.ss_output.vert.size(); i++) {
    const int orig_local = r_result.ss_output.vert_orig[i];
    if (orig_local >= 0 && orig_local < orig_pt_map.size()) {
      r_result.dst_point_to_src_point[i] = orig_pt_map[orig_local];
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Attribute Propagation Helper
 * \{ */

static void gather_point_attributes(const AttributeAccessor &src_attributes,
                                    const IndexRange result_vert_range,
                                    const Span<int> dst_to_src_map,
                                    const AttributeFilter &attribute_filter,
                                    MutableAttributeAccessor &dst_attributes)
{
  IndexMaskMemory memory;
  const IndexMask non_skeleton = IndexMask::from_predicate(
      dst_to_src_map.index_range(), memory, [&](const int64_t i) {
        return dst_to_src_map[i] != -1;
      });

  src_attributes.foreach_attribute([&](const AttributeIter &iter) {
    if (iter.domain != bke::AttrDomain::Point) {
      return;
    }
    if (iter.data_type == bke::AttrType::String) {
      return;
    }
    if (attribute_filter.allow_skip(iter.name)) {
      return;
    }
    if (iter.is_builtin && !dst_attributes.is_builtin(iter.name)) {
      return;
    }
    const GVArray src = *iter.get(bke::AttrDomain::Point);
    const CommonVArrayInfo info = src.common_info();
    if (info.type == CommonVArrayInfo::Type::Single) {
      const bke::AttributeInitValue init(GPointer(src.type(), info.data));
      if (dst_attributes.add(iter.name, iter.domain, iter.data_type, init)) {
        return;
      }
    }

    GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_span(
        iter.name, bke::AttrDomain::Point, iter.data_type, bke::AttributeInitDefaultValue());
    if (!dst) {
      return;
    }
    bke::attribute_math::gather(
        src, dst_to_src_map, non_skeleton, dst.span.slice(result_vert_range));
    dst.finish();
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh Assembly
 * \{ */

static Mesh *build_result_mesh(const Span<GroupResult> results,
                               const Mesh *src_mesh,
                               const bke::CurvesGeometry *src_curves,
                               const float elevation_scale,
                               const AttributeFilter &attribute_filter,
                               const std::optional<std::string> &distance_attr_id,
                               const std::optional<std::string> &is_skeleton_attr_id,
                               const std::optional<std::string> &face_band_attr_id,
                               const std::optional<std::string> &orig_face_attr_id)
{
  Array<int> vert_counts(results.size() + 1);
  Array<int> edge_counts(results.size() + 1);
  Array<int> face_counts(results.size() + 1);
  Array<int> corner_counts(results.size() + 1);

  for (int i = 0; i < results.size(); i++) {
    if (results[i].is_error) {
      vert_counts[i] = 0;
      edge_counts[i] = 0;
      face_counts[i] = 0;
      corner_counts[i] = 0;
      continue;
    }
    const straightskeleton::StraightSkeletonOutput &out = results[i].ss_output;
    vert_counts[i] = out.vert.size();
    edge_counts[i] = out.edge.size();
    face_counts[i] = out.face_offsets().size();
    corner_counts[i] = out.face_vert_indices.size();
  }

  const OffsetIndices vert_groups = offset_indices::accumulate_counts_to_offsets(
      vert_counts.as_mutable_span());
  const OffsetIndices edge_groups = offset_indices::accumulate_counts_to_offsets(
      edge_counts.as_mutable_span());
  const OffsetIndices face_groups = offset_indices::accumulate_counts_to_offsets(
      face_counts.as_mutable_span());
  const OffsetIndices corner_groups = offset_indices::accumulate_counts_to_offsets(
      corner_counts.as_mutable_span());

  Mesh *dst_mesh = BKE_mesh_new_nomain(vert_groups.total_size(),
                                       edge_groups.total_size(),
                                       face_groups.total_size(),
                                       corner_groups.total_size());

  MutableSpan<float3> all_positions = dst_mesh->vert_positions_for_write();
  MutableSpan<int2> all_edges = dst_mesh->edges_for_write();
  MutableSpan<int> all_face_offsets = dst_mesh->face_offsets_for_write();
  MutableSpan<int> all_corner_verts = dst_mesh->corner_verts_for_write();

  threading::parallel_for(results.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      if (results[i].is_error) {
        continue;
      }
      const straightskeleton::StraightSkeletonOutput &out = results[i].ss_output;
      const IndexRange verts_range = vert_groups[i];
      const IndexRange edges_range = edge_groups[i];
      const IndexRange faces_range = face_groups[i];
      const IndexRange corners_range = corner_groups[i];

      MutableSpan<float3> positions = all_positions.slice(verts_range);
      for (const int v : out.vert.index_range()) {
        const float z = float(out.vert_time[v]) * elevation_scale;
        positions[v] = float3(float(out.vert[v].x), float(out.vert[v].y), z);
      }

      MutableSpan<int2> edges = all_edges.slice(edges_range);
      for (const int e : out.edge.index_range()) {
        edges[e] = int2(out.edge[e][0] + verts_range.start(),
                        out.edge[e][1] + verts_range.start());
      }

      MutableSpan<int> face_offsets = all_face_offsets.slice(faces_range);
      MutableSpan<int> corner_verts = all_corner_verts.slice(corners_range);
      for (const int f : out.face_offsets().index_range()) {
        face_offsets[f] = out.face_offsets()[f].start() + corners_range.start();
      }
      for (const int c : out.face_vert_indices.index_range()) {
        corner_verts[c] = out.face_vert_indices[c] + verts_range.start();
      }
    }
  });

  MutableAttributeAccessor dst_attributes = dst_mesh->attributes_for_write();

  /* Gather point attributes for original vertices. */
  for (int i = 0; i < results.size(); i++) {
    if (results[i].is_error) {
      continue;
    }
    const IndexRange verts_range = vert_groups[i];
    const AttributeAccessor src_accessor = (results[i].domain == SourceComponent::Mesh &&
                                            src_mesh) ?
                                               src_mesh->attributes() :
                                               src_curves->attributes();
    gather_point_attributes(src_accessor,
                            verts_range,
                            results[i].dst_point_to_src_point.as_span(),
                            bke::attribute_filter_with_skip_ref(attribute_filter, {"position"}),
                            dst_attributes);
  }

  /* Output Anonymous Attribute: Distance (Point domain). */
  if (distance_attr_id) {
    if (SpanAttributeWriter<float> dst_dist = dst_attributes.lookup_or_add_for_write_span<float>(
            *distance_attr_id, bke::AttrDomain::Point))
    {
      for (int i = 0; i < results.size(); i++) {
        if (results[i].is_error) {
          continue;
        }
        const IndexRange verts_range = vert_groups[i];
        const straightskeleton::StraightSkeletonOutput &out = results[i].ss_output;
        MutableSpan<float> slice = dst_dist.span.slice(verts_range);
        for (const int v : out.vert_time.index_range()) {
          slice[v] = float(out.vert_time[v]);
        }
      }
      dst_dist.finish();
    }
  }

  /* Output Anonymous Attribute: Is Skeleton (Point domain). */
  if (is_skeleton_attr_id) {
    if (SpanAttributeWriter<bool> dst_skel = dst_attributes.lookup_or_add_for_write_span<bool>(
            *is_skeleton_attr_id, bke::AttrDomain::Point))
    {
      for (int i = 0; i < results.size(); i++) {
        if (results[i].is_error) {
          continue;
        }
        const IndexRange verts_range = vert_groups[i];
        const straightskeleton::StraightSkeletonOutput &out = results[i].ss_output;
        MutableSpan<bool> slice = dst_skel.span.slice(verts_range);
        for (const int v : out.vert_orig.index_range()) {
          slice[v] = (out.vert_orig[v] == -1);
        }
      }
      dst_skel.finish();
    }
  }

  /* Output Anonymous Attribute: Face Band (Face domain). */
  if (face_band_attr_id && face_groups.total_size() > 0) {
    if (SpanAttributeWriter<int> dst_band = dst_attributes.lookup_or_add_for_write_span<int>(
            *face_band_attr_id, bke::AttrDomain::Face))
    {
      for (int i = 0; i < results.size(); i++) {
        if (results[i].is_error) {
          continue;
        }
        const IndexRange faces_range = face_groups[i];
        const straightskeleton::StraightSkeletonOutput &out = results[i].ss_output;
        MutableSpan<int> slice = dst_band.span.slice(faces_range);
        for (const int f : out.face_band.index_range()) {
          slice[f] = out.face_band[f];
        }
      }
      dst_band.finish();
    }
  }

  /* Output Anonymous Attribute: Original Face (Face domain). */
  if (orig_face_attr_id && face_groups.total_size() > 0) {
    if (SpanAttributeWriter<int> dst_orig_f = dst_attributes.lookup_or_add_for_write_span<int>(
            *orig_face_attr_id, bke::AttrDomain::Face))
    {
      for (int i = 0; i < results.size(); i++) {
        if (results[i].is_error) {
          continue;
        }
        const IndexRange faces_range = face_groups[i];
        const straightskeleton::StraightSkeletonOutput &out = results[i].ss_output;
        MutableSpan<int> slice = dst_orig_f.span.slice(faces_range);
        for (const int f : out.face_orig.index_range()) {
          slice[f] = out.face_orig[f];
        }
      }
      dst_orig_f.finish();
    }
  }

  bke::mesh_calc_edges(*dst_mesh, true, false);
  bke::mesh_smooth_set(*dst_mesh, false);
  dst_mesh->tag_overlapping_none();

  return dst_mesh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Execution
 * \{ */

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  const Field<int> group_index = params.extract_input<Field<int>>("Group ID"_ustr);
  const StraightSkeletonDirection direction_input =
      params.extract_input<StraightSkeletonDirection>("Direction"_ustr);
  straightskeleton::PropagationDirection direction =
      straightskeleton::PropagationDirection::Inside;
  if (direction_input == StraightSkeletonDirection::Outside) {
    direction = straightskeleton::PropagationDirection::Outside;
  }
  else if (direction_input == StraightSkeletonDirection::Both) {
    direction = straightskeleton::PropagationDirection::Both;
  }
  const float bbox_expansion = params.extract_input<float>("Bounding Box Margin %"_ustr);
  const double bbox_expansion_pct = double(bbox_expansion);

  const StraightSkeletonMode mode = params.extract_input<StraightSkeletonMode>("Mode"_ustr);
  const float distance = params.extract_input<float>("Distance"_ustr);
  const int segments = params.extract_input<int>("Segments"_ustr);
  const float elevation_scale = params.extract_input<float>("Elevation Scale"_ustr);
  const float epsilon = params.extract_input<float>("Epsilon"_ustr);

  const double offset = (mode == StraightSkeletonMode::FullSkeleton) ?
                            std::numeric_limits<double>::infinity() :
                            math::max(0.0, double(distance));

  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh"_ustr);
  const std::optional<std::string> distance_attr_id =
      params.get_output_anonymous_attribute_id_if_needed("Distance"_ustr);
  const std::optional<std::string> is_skeleton_attr_id =
      params.get_output_anonymous_attribute_id_if_needed("Is Skeleton"_ustr);
  const std::optional<std::string> face_band_attr_id =
      params.get_output_anonymous_attribute_id_if_needed("Face Band"_ustr);
  const std::optional<std::string> orig_face_attr_id =
      params.get_output_anonymous_attribute_id_if_needed("Original Face"_ustr);

  geometry::foreach_real_geometry(geometry, [&](GeometrySet &geo) {
    if (geo.has_mesh() || geo.has_curves()) {
      const Mesh *mesh = geo.get_mesh();
      const Curves *curves_id = geo.get_curves();
      const bke::CurvesGeometry *curves = curves_id ? &curves_id->geometry.wrap() : nullptr;

      IndexMaskMemory memory;
      Vector<IndexMask> mesh_masks;
      Vector<IndexMask> curve_masks;
      VectorSet<int> mesh_ids;
      VectorSet<int> curve_ids;

      if (mesh && mesh->verts_num > 0) {
        const bke::GeometryFieldContext context{*mesh, bke::AttrDomain::Point};
        FieldEvaluator evaluator(context, mesh->verts_num);
        evaluator.add(group_index);
        evaluator.evaluate();
        const VArray<int> group_ids = evaluator.get_evaluated<int>(0);
        mesh_masks = IndexMask::from_group_ids(group_ids, memory);
        mesh_ids.reserve(mesh_masks.size());
        for (const IndexMask &mask : mesh_masks) {
          mesh_ids.add_new(group_ids[mask.first()]);
        }
      }

      if (curves && !curves->is_empty()) {
        const bke::CurvesFieldContext context(*curves, bke::AttrDomain::Curve);
        FieldEvaluator evaluator(context, curves->curves_num());
        evaluator.add(group_index);
        evaluator.evaluate();
        const VArray<int> group_ids = evaluator.get_evaluated<int>(0);
        curve_masks = IndexMask::from_group_ids(group_ids, memory);
        curve_ids.reserve(curve_masks.size());
        for (const IndexMask &mask : curve_masks) {
          curve_ids.add_new(group_ids[mask.first()]);
        }
      }

      VectorSet<int> all_group_ids = mesh_ids;
      all_group_ids.add_multiple(curve_ids);

      if (!all_group_ids.is_empty()) {
        Array<GroupResult> results(all_group_ids.size());
        const Span<int2> mesh_edges = mesh ? mesh->edges() : Span<int2>();
        const OffsetIndices<int> mesh_faces = mesh ? mesh->faces() : OffsetIndices<int>();
        const Span<int> mesh_corner_verts = mesh ? mesh->corner_verts() : Span<int>();

        threading::parallel_for(results.index_range(), 1024, [&](const IndexRange range) {
          for (const int i : range) {
            const int group_id = all_group_ids[i];
            const int mesh_idx = mesh_ids.index_of_try(group_id);
            const int curve_idx = curve_ids.index_of_try(group_id);

            if (mesh_idx != -1 && curve_idx != -1) {
              results[i].is_error = true;
              results[i].error_message = fmt::format(
                  "Group {} contains both Mesh and Curve geometry. Please assign distinct Group "
                  "IDs.",
                  group_id);
              continue;
            }

            GroupMasks group;
            IndexMaskMemory thread_memory;
            if (mesh_idx != -1) {
              group.mesh_verts = mesh_masks[mesh_idx];
              BitVector<> vert_in_group(mesh->verts_num);
              group.mesh_verts.to_bits(vert_in_group);
              group.mesh_loose_edges = IndexMask::from_predicate(
                  mesh->loose_edges(), thread_memory, [&](const int64_t edge) {
                    return vert_in_group[mesh_edges[edge][0]] &&
                           vert_in_group[mesh_edges[edge][1]];
                  });
              group.mesh_faces = IndexMask::from_predicate(
                  mesh_faces.index_range(), thread_memory, [&](const int64_t face) {
                    return std::ranges::all_of(
                        mesh_corner_verts.slice(mesh_faces[face]),
                        [&](const int vert) { return vert_in_group[vert]; });
                  });
              calc_mesh_group(*mesh,
                              group,
                              mode,
                              offset,
                              segments,
                              epsilon > 0.0f ? double(epsilon) : 0.0,
                              direction,
                              bbox_expansion_pct,
                              results[i]);
            }
            else if (curve_idx != -1) {
              group.curves = curve_masks[curve_idx];
              calc_curve_group(*curves,
                               group.curves,
                               mode,
                               offset,
                               segments,
                               epsilon > 0.0f ? double(epsilon) : 0.0,
                               direction,
                               bbox_expansion_pct,
                               results[i]);
            }
          }
        });

        /* Report warnings/errors on main thread. */
        for (const GroupResult &res : results) {
          if (res.is_error && !res.error_message.empty()) {
            params.error_message_add(NodeWarningType::Error, res.error_message);
          }
        }

        Mesh *result_mesh = build_result_mesh(results.as_span(),
                                              mesh,
                                              curves,
                                              elevation_scale,
                                              attribute_filter,
                                              distance_attr_id,
                                              is_skeleton_attr_id,
                                              face_band_attr_id,
                                              orig_face_attr_id);
        geo.replace_mesh(result_mesh);
      }
    }

    /* Grease pencil support: process each layer as curves. */
    if (geo.has_grease_pencil()) {
      using namespace blender::bke::greasepencil;
      const GreasePencil &grease_pencil = *geo.get_grease_pencil();
      Vector<Mesh *> mesh_by_layer(grease_pencil.layers().size(), nullptr);
      for (const int layer_index : grease_pencil.layers().index_range()) {
        const Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
        if (drawing == nullptr) {
          continue;
        }
        const bke::CurvesGeometry &curves = drawing->strokes();
        if (curves.is_empty()) {
          continue;
        }
        bke::GreasePencilLayerFieldContext layer_context(
            grease_pencil, bke::AttrDomain::Curve, layer_index);
        IndexMaskMemory memory;
        FieldEvaluator evaluator(layer_context, curves.curves_num());
        evaluator.add(group_index);
        evaluator.evaluate();
        const VArray<int> group_ids = evaluator.get_evaluated<int>(0);
        Vector<IndexMask> curve_masks = IndexMask::from_group_ids(group_ids, memory);

        Array<GroupResult> layer_results(curve_masks.size());
        for (const int i : curve_masks.index_range()) {
          calc_curve_group(curves,
                           curve_masks[i],
                           mode,
                           offset,
                           segments,
                           epsilon > 0.0f ? double(epsilon) : 0.0,
                           direction,
                           bbox_expansion_pct,
                           layer_results[i]);
        }
        for (const GroupResult &res : layer_results) {
          if (res.is_error && !res.error_message.empty()) {
            params.error_message_add(NodeWarningType::Error, res.error_message);
          }
        }
        mesh_by_layer[layer_index] = build_result_mesh(layer_results.as_span(),
                                                       nullptr,
                                                       &curves,
                                                       elevation_scale,
                                                       attribute_filter,
                                                       distance_attr_id,
                                                       is_skeleton_attr_id,
                                                       face_band_attr_id,
                                                       orig_face_attr_id);
      }

      if (!mesh_by_layer.is_empty()) {
        std::unique_ptr<bke::Instances> instances = std::make_unique<bke::Instances>(
            mesh_by_layer.size());
        MutableSpan<int> handles = instances->reference_handles_for_write();
        instances->transforms_for_write().fill(float4x4::identity());
        for (const int i : mesh_by_layer.index_range()) {
          Mesh *mesh = mesh_by_layer[i];
          if (!mesh) {
            handles[i] = instances->add_reference(bke::InstanceReference());
            continue;
          }
          handles[i] = instances->add_reference(
              bke::InstanceReference{GeometrySet::from_mesh(mesh)});
        }
        InstancesComponent &dst_component = geo.get_component_for_write<InstancesComponent>();
        GeometrySet new_instances = geometry::join_geometries(
            {GeometrySet::from_instances(dst_component.release()),
             GeometrySet::from_instances(std::move(instances))},
            {});
        dst_component.replace(
            new_instances.get_component_for_write<InstancesComponent>().release());
      }
    }

    geo.keep_only({GeometryComponent::Type::Mesh, GeometryComponent::Type::Instance});
  });

  params.set_output("Mesh"_ustr, std::move(geometry));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeStraightSkeleton"_ustr);
  ntype.ui_name = "Straight Skeleton";
  ntype.ui_description =
      "Compute the 2D straight skeleton and offset bands for polygons with holes or open edge "
      "chains "
      "projected onto the X-Y plane";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.default_width = bke::NodeWidth::_160;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_straight_skeleton_cc
