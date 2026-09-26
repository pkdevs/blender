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

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::GreasePencil})
      .description(
          "The input geometry (projected onto the X-Y plane). Must form either an outer polygon with "
          "optional holes, or an open edge chain");
  b.add_input<decl::Int>("Group ID"_ustr)
      .evaluated_geometry_field()
      .hide_value()
      .description(
          "An ID used to group elements together. Straight skeleton computation is done "
          "separately for each group");
  b.add_input<decl::Menu>("Mode"_ustr)
      .static_items(mode_items)
      .default_value(MenuValue(StraightSkeletonMode::FullSkeleton))
      .optional_label();
  b.add_input<decl::Float>("Distance"_ustr)
      .default_value(1.0f)
      .description("Inset (positive) or outset (negative) offset distance")
      .usage_inference(
          [](const socket_usage_inference::SocketUsageParams &params) -> std::optional<bool> {
            return params.menu_input_may_be("Mode"_ustr, int(StraightSkeletonMode::Offset));
          });
  b.add_input<decl::Int>("Segments"_ustr)
      .default_value(1)
      .min(1)
      .max(1000)
      .description("Number of equal-width offset bands to subdivide into");
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
/** \name 2D Geometry Helper Functions
 * \{ */

/* 2D signed area of a polygon loop using 2D cross product. Positive if CCW, negative if CW. */
static double polygon_signed_area_2d(const Span<double2> poly)
{
  if (poly.size() < 3) {
    return 0.0;
  }
  double area = 0.0;
  const int64_t n = poly.size();
  for (int64_t i = 0; i < n; i++) {
    const int64_t next = (i + 1 == n) ? 0 : i + 1;
    area += math::cross(poly[i], poly[next]);
  }
  return 0.5 * area;
}

/** \} */

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

static void calc_curve_group(const bke::CurvesGeometry &curves,
                             const IndexMask &curve_mask,
                             const StraightSkeletonMode mode,
                             const double offset,
                             const int segments,
                             const double epsilon,
                             GroupResult &r_result)
{
  r_result.domain = SourceComponent::Curve;
  const OffsetIndices points_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> positions = curves.evaluated_positions();
  const VArray<bool> cyclic = curves.cyclic();

  /* Check whether curves are all open or all cyclic. */
  int open_count = 0;
  int cyclic_count = 0;
  curve_mask.foreach_index([&](const int64_t curve) {
    if (cyclic[curve]) {
      cyclic_count++;
    }
    else {
      open_count++;
    }
  });

  if (open_count > 0 && cyclic_count > 0) {
    r_result.is_error = true;
    r_result.error_message = "Group contains both open curves and closed curves.";
    return;
  }

  /* ------------------------------------------------------------------ */
  /* Case 1: Open curve(s) -> Edge Chain                                */
  /* ------------------------------------------------------------------ */
  if (open_count > 0) {
    if (curve_mask.size() > 1) {
      r_result.is_error = true;
      r_result.error_message =
          "Group contains multiple open curves. Each open edge chain must have its own Group ID.";
      return;
    }

    const int curve_idx = int(curve_mask[0]);
    const IndexRange curve_points = points_by_curve[curve_idx];
    if (curve_points.size() < 2) {
      r_result.is_error = true;
      r_result.error_message = "Edge chain must have at least 2 points.";
      return;
    }

    if (offset < 0.0) {
      r_result.is_error = true;
      r_result.error_message = "Edge chain does not support negative offset / outset.";
      return;
    }

    if (mode == StraightSkeletonMode::FullSkeleton) {
      r_result.is_error = true;
      r_result.error_message =
          "Edge chain requires a finite offset distance (Full Skeleton is not applicable to open "
          "edge chains).";
      return;
    }

    const int n = int(curve_points.size());
    Array<double2> chain_verts(n);
    Array<int> dst_to_src(n);
    for (int i = 0; i < n; i++) {
      const int orig_pt = int(curve_points[i]);
      chain_verts[i] = double2(positions[orig_pt].x, positions[orig_pt].y);
      dst_to_src[i] = orig_pt;
    }

    /* Distance == 0.0 is a no-op returning the original edge chain. */
    if (offset == 0.0) {
      r_result.is_noop = true;
      r_result.ss_output.vert = std::move(chain_verts);
      r_result.ss_output.vert_time.reinitialize(n);
      r_result.ss_output.vert_time.fill(0.0);
      r_result.ss_output.vert_orig.reinitialize(n);
      for (int i = 0; i < n; i++) {
        r_result.ss_output.vert_orig[i] = i;
      }
      r_result.ss_output.edge.reinitialize(n - 1);
      for (int i = 0; i < n - 1; i++) {
        r_result.ss_output.edge[i] = int2(i, i + 1);
      }
      r_result.dst_point_to_src_point = std::move(dst_to_src);
      return;
    }

    Array<int> chain_indices(n);
    for (int i = 0; i < n; i++) {
      chain_indices[i] = i;
    }

    straightskeleton::StraightSkeletonInput input;
    input.vert = chain_verts.as_span();
    input.edge_chain = chain_indices.as_span();
    input.offset = offset;
    input.segments = math::max(1, segments);
    input.epsilon = epsilon;

    r_result.ss_output = straightskeleton::straight_skeleton_calc(input);
    if (!r_result.ss_output.is_valid || r_result.ss_output.hit_max_events) {
      r_result.is_error = true;
      r_result.error_message = r_result.ss_output.error_message.empty() ?
                                   "Straight skeleton calculation failed on edge chain." :
                                   r_result.ss_output.error_message;
      return;
    }

    r_result.dst_point_to_src_point.reinitialize(r_result.ss_output.vert.size());
    r_result.dst_point_to_src_point.fill(-1);
    for (int i = 0; i < r_result.ss_output.vert.size(); i++) {
      const int orig_local = r_result.ss_output.vert_orig[i];
      if (orig_local >= 0 && orig_local < n) {
        r_result.dst_point_to_src_point[i] = dst_to_src[orig_local];
      }
    }
    return;
  }

  /* ------------------------------------------------------------------ */
  /* Case 2: Closed curve(s) -> Outer polygon + holes                   */
  /* ------------------------------------------------------------------ */
  const int curves_num = int(curve_mask.size());
  Vector<Array<double2>> loop_coords(curves_num);
  Vector<Array<float2>> loop_coords_float(curves_num);
  Vector<Array<int>> loop_orig_points(curves_num);
  Vector<double> loop_areas(curves_num);
  Vector<Bounds<float2>> loop_bounds(curves_num);

  for (int i = 0; i < curves_num; i++) {
    const int curve_idx = int(curve_mask[i]);
    const IndexRange curve_points = points_by_curve[curve_idx];
    const int n = int(curve_points.size());
    if (n < 3) {
      r_result.is_error = true;
      r_result.error_message = "Closed curve must have at least 3 points.";
      return;
    }
    loop_coords[i].reinitialize(n);
    loop_coords_float[i].reinitialize(n);
    loop_orig_points[i].reinitialize(n);
    for (int j = 0; j < n; j++) {
      const int orig_pt = int(curve_points[j]);
      const float2 pt_f(positions[orig_pt].x, positions[orig_pt].y);
      loop_coords[i][j] = double2(pt_f);
      loop_coords_float[i][j] = pt_f;
      loop_orig_points[i][j] = orig_pt;
    }
    loop_areas[i] = polygon_signed_area_2d(loop_coords[i].as_span());
    loop_bounds[i] = *bounds::min_max(loop_coords_float[i].as_span());
  }

  /* Compute containment depth for each loop using library functions. */
  Array<int> depth(curves_num, 0);
  for (int i = 0; i < curves_num; i++) {
    for (int j = 0; j < curves_num; j++) {
      if (i == j) {
        continue;
      }
      if (loop_bounds[j].contains(loop_bounds[i].min) &&
          loop_bounds[j].contains(loop_bounds[i].max))
      {
        if (math::isect_point_poly(loop_coords_float[i][0], loop_coords_float[j].as_span())) {
          depth[i]++;
        }
      }
    }
  }

  int outer_idx = -1;
  int outer_count = 0;
  for (int i = 0; i < curves_num; i++) {
    if (depth[i] == 0) {
      outer_count++;
      outer_idx = i;
    }
  }

  if (outer_count == 0) {
    r_result.is_error = true;
    r_result.error_message = "No outer boundary loop found in closed curves.";
    return;
  }
  if (outer_count > 1) {
    r_result.is_error = true;
    r_result.error_message = fmt::format(
        "Group contains multiple outer boundaries ({} disjoint outer loops). Each island must have "
        "its own Group ID.",
        outer_count);
    return;
  }

  /* All other curves must be directly inside the outer curve (depth == 1). */
  for (int i = 0; i < curves_num; i++) {
    if (i != outer_idx && depth[i] != 1) {
      r_result.is_error = true;
      r_result.error_message = "Nested holes within holes are not supported in the same group.";
      return;
    }
  }

  /* Orient outer CCW and holes CW. */
  if (loop_areas[outer_idx] < 0.0) {
    std::ranges::reverse(loop_coords[outer_idx]);
    std::ranges::reverse(loop_orig_points[outer_idx]);
  }

  Vector<int> hole_indices;
  for (int i = 0; i < curves_num; i++) {
    if (i != outer_idx) {
      if (loop_areas[i] > 0.0) {
        std::ranges::reverse(loop_coords[i]);
        std::ranges::reverse(loop_orig_points[i]);
      }
      hole_indices.append(i);
    }
  }

  /* Concatenate vertices into StraightSkeletonInput. */
  Vector<double2> all_verts;
  Vector<int> orig_pt_map;
  Vector<int> outer_polygon;
  for (int j = 0; j < loop_coords[outer_idx].size(); j++) {
    const int local_idx = int(all_verts.append_and_get_index(loop_coords[outer_idx][j]));
    orig_pt_map.append(loop_orig_points[outer_idx][j]);
    outer_polygon.append(local_idx);
  }

  Vector<int> hole_vert_indices;
  Array<int> hole_offsets_data(hole_indices.size() + 1);
  for (int h = 0; h < hole_indices.size(); h++) {
    hole_offsets_data[h] = int(hole_vert_indices.size());
    const int loop_i = hole_indices[h];
    for (int j = 0; j < loop_coords[loop_i].size(); j++) {
      const int local_idx = int(all_verts.append_and_get_index(loop_coords[loop_i][j]));
      orig_pt_map.append(loop_orig_points[loop_i][j]);
      hole_vert_indices.append(local_idx);
    }
  }
  hole_offsets_data.last() = int(hole_vert_indices.size());
  const OffsetIndices<int> hole_offsets = OffsetIndices<int>(hole_offsets_data.as_span());

  /* Distance == 0.0 is a no-op returning the boundary loops. */
  if (mode == StraightSkeletonMode::Offset && offset == 0.0) {
    r_result.is_noop = true;
    const int total_verts = all_verts.size();
    r_result.ss_output.vert = Array<double2>(all_verts.as_span());
    r_result.ss_output.vert_time.reinitialize(total_verts);
    r_result.ss_output.vert_time.fill(0.0);
    r_result.ss_output.vert_orig.reinitialize(total_verts);
    for (int i = 0; i < total_verts; i++) {
      r_result.ss_output.vert_orig[i] = i;
    }
    Vector<int2> boundary_edges;
    for (int i = 0; i < outer_polygon.size(); i++) {
      const int next = (i + 1 == outer_polygon.size()) ? 0 : i + 1;
      boundary_edges.append(int2(outer_polygon[i], outer_polygon[next]));
    }
    for (int h = 0; h < hole_offsets.size(); h++) {
      const IndexRange hr = hole_offsets[h];
      for (int i = 0; i < hr.size(); i++) {
        const int next = (i + 1 == hr.size()) ? 0 : i + 1;
        boundary_edges.append(
            int2(hole_vert_indices[hr[i]], hole_vert_indices[hr[next]]));
      }
    }
    r_result.ss_output.edge = Array<int2>(boundary_edges.as_span());
    r_result.dst_point_to_src_point = Array<int>(orig_pt_map.as_span());
    return;
  }

  straightskeleton::StraightSkeletonInput input;
  input.vert = all_verts.as_span();
  input.outer_polygon = outer_polygon.as_span();
  input.hole_offsets = hole_offsets;
  input.hole_vert_indices = hole_vert_indices.as_span();
  input.offset = offset;
  input.segments = math::max(1, segments);
  input.epsilon = epsilon;

  r_result.ss_output = straightskeleton::straight_skeleton_calc(input);
  if (!r_result.ss_output.is_valid || r_result.ss_output.hit_max_events) {
    r_result.is_error = true;
    r_result.error_message = r_result.ss_output.error_message.empty() ?
                                 "Straight skeleton calculation failed on closed polygon." :
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
/** \name Mesh Topology Ingestion
 * \{ */

static void calc_mesh_group(const Mesh &mesh,
                            const GroupMasks &group,
                            const StraightSkeletonMode mode,
                            const double offset,
                            const int segments,
                            const double epsilon,
                            GroupResult &r_result)
{
  r_result.domain = SourceComponent::Mesh;
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> mesh_edges = mesh.edges();

  /* ------------------------------------------------------------------ */
  /* Subcase 1: Wire mesh (edges only, no faces)                        */
  /* ------------------------------------------------------------------ */
  if (group.mesh_faces.is_empty()) {
    if (group.mesh_loose_edges.is_empty()) {
      return;
    }

    /* Build adjacency map for wire edges in this group. */
    Map<int, Vector<int, 2>> adj;
    group.mesh_loose_edges.foreach_index([&](const int64_t e_idx) {
      const int v1 = mesh_edges[e_idx][0];
      const int v2 = mesh_edges[e_idx][1];
      adj.lookup_or_add_default(v1).append(v2);
      adj.lookup_or_add_default(v2).append(v1);
    });

    int degree_1_count = 0;
    int degree_other_count = 0;
    Vector<int, 2> endpoints;
    for (const auto &[v, neighbors] : adj.items()) {
      const int deg = neighbors.size();
      if (deg == 1) {
        degree_1_count++;
        endpoints.append(v);
      }
      else if (deg != 2) {
        degree_other_count++;
      }
    }

    if (degree_other_count > 0) {
      r_result.is_error = true;
      r_result.error_message =
          "Mesh wire edges contain branching junctions (vertex degree > 2) or isolated vertices.";
      return;
    }

    /* Open Edge Chain: exactly two endpoints with degree 1. */
    if (degree_1_count == 2) {
      if (offset < 0.0) {
        r_result.is_error = true;
        r_result.error_message = "Edge chain does not support negative offset / outset.";
        return;
      }
      if (mode == StraightSkeletonMode::FullSkeleton) {
        r_result.is_error = true;
        r_result.error_message =
            "Edge chain requires a finite offset distance (Full Skeleton is not applicable to "
            "open edge chains).";
        return;
      }

      /* Trace open path from start endpoint to end endpoint. */
      Vector<int> path;
      int curr = endpoints[0];
      int prev = -1;
      Set<int> visited;
      while (true) {
        path.append(curr);
        visited.add(curr);
        if (curr == endpoints[1] && path.size() > 1) {
          break;
        }
        const Vector<int, 2> &neighbors = adj.lookup(curr);
        int next = -1;
        for (const int nb : neighbors) {
          if (nb != prev && !visited.contains(nb)) {
            next = nb;
            break;
          }
        }
        if (next == -1) {
          break;
        }
        prev = curr;
        curr = next;
      }

      if (path.size() != adj.size()) {
        r_result.is_error = true;
        r_result.error_message =
            "Mesh wire edges contain multiple disconnected components. Each edge chain must have "
            "its own Group ID.";
        return;
      }

      const int n = path.size();
      Array<double2> chain_verts(n);
      Array<int> dst_to_src(n);
      for (int i = 0; i < n; i++) {
        chain_verts[i] = double2(positions[path[i]].x, positions[path[i]].y);
        dst_to_src[i] = path[i];
      }

      /* Distance == 0.0 is a no-op returning the wire edge chain. */
      if (offset == 0.0) {
        r_result.is_noop = true;
        r_result.ss_output.vert = std::move(chain_verts);
        r_result.ss_output.vert_time.reinitialize(n);
        r_result.ss_output.vert_time.fill(0.0);
        r_result.ss_output.vert_orig.reinitialize(n);
        for (int i = 0; i < n; i++) {
          r_result.ss_output.vert_orig[i] = i;
        }
        r_result.ss_output.edge.reinitialize(n - 1);
        for (int i = 0; i < n - 1; i++) {
          r_result.ss_output.edge[i] = int2(i, i + 1);
        }
        r_result.dst_point_to_src_point = std::move(dst_to_src);
        return;
      }

      Array<int> chain_indices(n);
      for (int i = 0; i < n; i++) {
        chain_indices[i] = i;
      }

      straightskeleton::StraightSkeletonInput input;
      input.vert = chain_verts.as_span();
      input.edge_chain = chain_indices.as_span();
      input.offset = offset;
      input.segments = math::max(1, segments);
      input.epsilon = epsilon;

      r_result.ss_output = straightskeleton::straight_skeleton_calc(input);
      if (!r_result.ss_output.is_valid || r_result.ss_output.hit_max_events) {
        r_result.is_error = true;
        r_result.error_message = r_result.ss_output.error_message.empty() ?
                                     "Straight skeleton calculation failed on mesh edge chain." :
                                     r_result.ss_output.error_message;
        return;
      }

      r_result.dst_point_to_src_point.reinitialize(r_result.ss_output.vert.size());
      r_result.dst_point_to_src_point.fill(-1);
      for (int i = 0; i < r_result.ss_output.vert.size(); i++) {
        const int orig_local = r_result.ss_output.vert_orig[i];
        if (orig_local >= 0 && orig_local < n) {
          r_result.dst_point_to_src_point[i] = dst_to_src[orig_local];
        }
      }
      return;
    }

    if (degree_1_count != 0) {
      r_result.is_error = true;
      r_result.error_message =
          "Mesh wire edges do not form a single open chain or valid closed loops without "
          "branches.";
      return;
    }

    /* Closed Wire Loops: trace all cycles. */
    Vector<Vector<int>> cycles;
    for (const auto &[start, _] : adj.items()) {
      int curr = start;
      int prev = -1;
      Vector<int> cycle;
      while (true) {
        if (cycle.contains(curr)) {
          break;
        }
        cycle.append(curr);
        const Vector<int, 2> &neighbors = adj.lookup(curr);
        int next = (neighbors[0] == prev) ? neighbors[1] : neighbors[0];
        if (next == start) {
          break;
        }
        prev = curr;
        curr = next;
      }
      if (cycle.size() >= 3) {
        bool already_have = false;
        for (const Vector<int> &c : cycles) {
          if (c.contains(cycle[0])) {
            already_have = true;
            break;
          }
        }
        if (!already_have) {
          cycles.append(std::move(cycle));
        }
      }
    }

    if (cycles.is_empty()) {
      r_result.is_error = true;
      r_result.error_message = "No valid wire loops found in mesh.";
      return;
    }

    /* Convert wire cycles to 2D coords and evaluate containment using library functions. */
    const int cycles_num = int(cycles.size());
    Vector<Array<double2>> loop_coords(cycles_num);
    Vector<Array<float2>> loop_coords_float(cycles_num);
    Vector<double> loop_areas(cycles_num);
    Vector<Bounds<float2>> loop_bounds(cycles_num);
    for (int i = 0; i < cycles_num; i++) {
      const int n = int(cycles[i].size());
      loop_coords[i].reinitialize(n);
      loop_coords_float[i].reinitialize(n);
      for (int j = 0; j < n; j++) {
        const float2 pt_f(positions[cycles[i][j]].x, positions[cycles[i][j]].y);
        loop_coords[i][j] = double2(pt_f);
        loop_coords_float[i][j] = pt_f;
      }
      loop_areas[i] = polygon_signed_area_2d(loop_coords[i].as_span());
      loop_bounds[i] = *bounds::min_max(loop_coords_float[i].as_span());
    }

    Array<int> depth(cycles_num, 0);
    for (int i = 0; i < cycles_num; i++) {
      for (int j = 0; j < cycles_num; j++) {
        if (i == j) {
          continue;
        }
        if (loop_bounds[j].contains(loop_bounds[i].min) &&
          loop_bounds[j].contains(loop_bounds[i].max))
        {
          if (math::isect_point_poly(loop_coords_float[i][0], loop_coords_float[j].as_span())) {
            depth[i]++;
          }
        }
      }
    }

    int outer_idx = -1;
    int outer_count = 0;
    for (int i = 0; i < cycles_num; i++) {
      if (depth[i] == 0) {
        outer_count++;
        outer_idx = i;
      }
    }

    if (outer_count != 1) {
      r_result.is_error = true;
      r_result.error_message =
          outer_count == 0 ? "No outer boundary loop found in wire mesh." :
                             fmt::format(
                                 "Group contains multiple outer wire loops ({} disjoint outer "
                                 "loops). Each island must have its own Group ID.",
                                 outer_count);
      return;
    }

    if (loop_areas[outer_idx] < 0.0) {
      std::ranges::reverse(loop_coords[outer_idx]);
      std::ranges::reverse(cycles[outer_idx]);
    }

    Vector<int> hole_indices;
    for (int i = 0; i < cycles_num; i++) {
      if (i != outer_idx) {
        if (loop_areas[i] > 0.0) {
          std::ranges::reverse(loop_coords[i]);
          std::ranges::reverse(cycles[i]);
        }
        hole_indices.append(i);
      }
    }

    Vector<double2> all_verts;
    Vector<int> orig_pt_map;
    Vector<int> outer_polygon;
    for (int j = 0; j < loop_coords[outer_idx].size(); j++) {
      const int local_idx = int(all_verts.append_and_get_index(loop_coords[outer_idx][j]));
      orig_pt_map.append(cycles[outer_idx][j]);
      outer_polygon.append(local_idx);
    }

    Vector<int> hole_vert_indices;
    Array<int> hole_offsets_data(hole_indices.size() + 1);
    for (int h = 0; h < hole_indices.size(); h++) {
      hole_offsets_data[h] = int(hole_vert_indices.size());
      const int loop_i = hole_indices[h];
      for (int j = 0; j < loop_coords[loop_i].size(); j++) {
        const int local_idx = int(all_verts.append_and_get_index(loop_coords[loop_i][j]));
        orig_pt_map.append(cycles[loop_i][j]);
        hole_vert_indices.append(local_idx);
      }
    }
    hole_offsets_data.last() = int(hole_vert_indices.size());
    const OffsetIndices<int> hole_offsets = OffsetIndices<int>(hole_offsets_data.as_span());

    if (mode == StraightSkeletonMode::Offset && offset == 0.0) {
      r_result.is_noop = true;
      const int total_verts = all_verts.size();
      r_result.ss_output.vert = Array<double2>(all_verts.as_span());
      r_result.ss_output.vert_time.reinitialize(total_verts);
      r_result.ss_output.vert_time.fill(0.0);
      r_result.ss_output.vert_orig.reinitialize(total_verts);
      for (int i = 0; i < total_verts; i++) {
        r_result.ss_output.vert_orig[i] = i;
      }
      Vector<int2> boundary_edges;
      for (int i = 0; i < outer_polygon.size(); i++) {
        const int next = (i + 1 == outer_polygon.size()) ? 0 : i + 1;
        boundary_edges.append(int2(outer_polygon[i], outer_polygon[next]));
      }
      for (int h = 0; h < hole_offsets.size(); h++) {
        const IndexRange hr = hole_offsets[h];
        for (int i = 0; i < hr.size(); i++) {
          const int next = (i + 1 == hr.size()) ? 0 : i + 1;
          boundary_edges.append(
              int2(hole_vert_indices[hr[i]], hole_vert_indices[hr[next]]));
        }
      }
      r_result.ss_output.edge = Array<int2>(boundary_edges.as_span());
      r_result.dst_point_to_src_point = Array<int>(orig_pt_map.as_span());
      return;
    }

    straightskeleton::StraightSkeletonInput input;
    input.vert = all_verts.as_span();
    input.outer_polygon = outer_polygon.as_span();
    input.hole_offsets = hole_offsets;
    input.hole_vert_indices = hole_vert_indices.as_span();
    input.offset = offset;
    input.segments = math::max(1, segments);
    input.epsilon = epsilon;

    r_result.ss_output = straightskeleton::straight_skeleton_calc(input);
    if (!r_result.ss_output.is_valid || r_result.ss_output.hit_max_events) {
      r_result.is_error = true;
      r_result.error_message = r_result.ss_output.error_message.empty() ?
                                   "Straight skeleton calculation failed on closed wire mesh." :
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
    return;
  }

  /* ------------------------------------------------------------------ */
  /* Subcase 2: Mesh with Faces                                         */
  /* ------------------------------------------------------------------ */
  if (!group.mesh_loose_edges.is_empty()) {
    r_result.is_error = true;
    r_result.error_message = "Group contains both loose edges and faces.";
    return;
  }

  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();

  /* Count face incidence for all edges used by faces in this group. */
  Map<int, int> edge_face_count;
  Map<int, int2> boundary_edge_directed;
  group.mesh_faces.foreach_index([&](const int64_t face_idx) {
    const IndexRange face_range = faces[face_idx];
    const int count = int(face_range.size());
    for (int c = 0; c < count; c++) {
      const int corner_i = int(face_range[c]);
      const int next_corner_i = int(face_range[(c + 1 == count) ? 0 : c + 1]);
      const int edge_i = corner_edges[corner_i];
      edge_face_count.lookup_or_add_default(edge_i) += 1;
      boundary_edge_directed.add_overwrite(
          edge_i, int2(corner_verts[corner_i], corner_verts[next_corner_i]));
    }
  });

  /* Filter boundary edges (incident to exactly 1 face in this group). */
  Map<int, int> next_boundary_vert;
  Set<int> boundary_verts_in;
  for (const auto &[edge_i, count] : edge_face_count.items()) {
    if (count > 2) {
      r_result.is_error = true;
      r_result.error_message = "Mesh contains non-manifold edges incident to more than 2 faces.";
      return;
    }
    if (count == 1) {
      const int2 directed = boundary_edge_directed.lookup(edge_i);
      const int u = directed[0];
      const int v = directed[1];
      if (next_boundary_vert.contains(u) || boundary_verts_in.contains(v)) {
        r_result.is_error = true;
        r_result.error_message = "Mesh boundary contains a self-touching pinch vertex.";
        return;
      }
      next_boundary_vert.add(u, v);
      boundary_verts_in.add(v);
    }
  }

  if (next_boundary_vert.is_empty()) {
    r_result.is_error = true;
    r_result.error_message =
        "Mesh has no boundary edges; straight skeleton requires a planar boundary or edge chain.";
    return;
  }

  /* Trace directed boundary cycles. */
  Vector<Vector<int>> boundary_cycles;
  Set<int> visited_v;
  for (const auto &[start, _] : next_boundary_vert.items()) {
    if (visited_v.contains(start)) {
      continue;
    }
    Vector<int> cycle;
    int curr = start;
    while (!visited_v.contains(curr)) {
      visited_v.add(curr);
      cycle.append(curr);
      if (!next_boundary_vert.contains(curr)) {
        break;
      }
      curr = next_boundary_vert.lookup(curr);
      if (curr == start) {
        break;
      }
    }
    if (curr == start && cycle.size() >= 3) {
      boundary_cycles.append(std::move(cycle));
    }
    else {
      r_result.is_error = true;
      r_result.error_message = "Mesh boundary edges do not form closed cycles.";
      return;
    }
  }

  /* Compute 2D signed area for all boundary cycles. */
  const int cycles_num = int(boundary_cycles.size());
  Vector<Array<double2>> loop_coords(cycles_num);
  Vector<double> loop_areas(cycles_num);
  for (int i = 0; i < cycles_num; i++) {
    const int n = int(boundary_cycles[i].size());
    loop_coords[i].reinitialize(n);
    for (int j = 0; j < n; j++) {
      loop_coords[i][j] = double2(positions[boundary_cycles[i][j]].x,
                                  positions[boundary_cycles[i][j]].y);
    }
    loop_areas[i] = polygon_signed_area_2d(loop_coords[i].as_span());
  }

  /* In a manifold mesh with CCW corner ordering, the outer boundary has area > 0 and holes have area < 0. */
  int outer_idx = -1;
  int outer_count = 0;
  for (int i = 0; i < cycles_num; i++) {
    if (loop_areas[i] > 0.0) {
      outer_count++;
      outer_idx = i;
    }
  }

  if (outer_count != 1) {
    r_result.is_error = true;
    r_result.error_message =
        outer_count == 0 ? "No CCW outer boundary loop found in mesh faces." :
                           fmt::format(
                               "Mesh contains multiple outer boundaries ({} disjoint islands). "
                               "Each island must have its own Group ID.",
                               outer_count);
    return;
  }

  /* Verify area coverage: sum of face areas should match outer area + hole areas. */
  double total_face_area = 0.0;
  group.mesh_faces.foreach_index([&](const int64_t face_idx) {
    const IndexRange face_range = faces[face_idx];
    Array<double2> face_coords(face_range.size());
    for (int c = 0; c < face_range.size(); c++) {
      const int v = corner_verts[face_range[c]];
      face_coords[c] = double2(positions[v].x, positions[v].y);
    }
    total_face_area += polygon_signed_area_2d(face_coords.as_span());
  });

  double expected_area = loop_areas[outer_idx];
  Vector<int> hole_indices;
  for (int i = 0; i < cycles_num; i++) {
    if (i != outer_idx) {
      expected_area += loop_areas[i];
      hole_indices.append(i);
    }
  }

  const double outer_area = math::abs(loop_areas[outer_idx]);
  if (math::abs(total_face_area - expected_area) > 1e-3 * outer_area) {
    r_result.is_error = true;
    r_result.error_message =
        "Mesh faces do not properly tile the area between outer boundary and holes.";
    return;
  }

  /* Gather straight skeleton input vertices and indices. */
  Vector<double2> all_verts;
  Vector<int> orig_pt_map;
  Vector<int> outer_polygon;
  for (int j = 0; j < loop_coords[outer_idx].size(); j++) {
    const int local_idx = int(all_verts.append_and_get_index(loop_coords[outer_idx][j]));
    orig_pt_map.append(boundary_cycles[outer_idx][j]);
    outer_polygon.append(local_idx);
  }

  Vector<int> hole_vert_indices;
  Array<int> hole_offsets_data(hole_indices.size() + 1);
  for (int h = 0; h < hole_indices.size(); h++) {
    hole_offsets_data[h] = int(hole_vert_indices.size());
    const int loop_i = hole_indices[h];
    for (int j = 0; j < loop_coords[loop_i].size(); j++) {
      const int local_idx = int(all_verts.append_and_get_index(loop_coords[loop_i][j]));
      orig_pt_map.append(boundary_cycles[loop_i][j]);
      hole_vert_indices.append(local_idx);
    }
  }
  hole_offsets_data.last() = int(hole_vert_indices.size());
  const OffsetIndices<int> hole_offsets = OffsetIndices<int>(hole_offsets_data.as_span());

  /* Distance == 0.0 is a no-op returning the boundary loops. */
  if (mode == StraightSkeletonMode::Offset && offset == 0.0) {
    r_result.is_noop = true;
    const int total_verts = all_verts.size();
    r_result.ss_output.vert = Array<double2>(all_verts.as_span());
    r_result.ss_output.vert_time.reinitialize(total_verts);
    r_result.ss_output.vert_time.fill(0.0);
    r_result.ss_output.vert_orig.reinitialize(total_verts);
    for (int i = 0; i < total_verts; i++) {
      r_result.ss_output.vert_orig[i] = i;
    }
    Vector<int2> boundary_edges;
    for (int i = 0; i < outer_polygon.size(); i++) {
      const int next = (i + 1 == outer_polygon.size()) ? 0 : i + 1;
      boundary_edges.append(int2(outer_polygon[i], outer_polygon[next]));
    }
    for (int h = 0; h < hole_offsets.size(); h++) {
      const IndexRange hr = hole_offsets[h];
      for (int i = 0; i < hr.size(); i++) {
        const int next = (i + 1 == hr.size()) ? 0 : i + 1;
        boundary_edges.append(
            int2(hole_vert_indices[hr[i]], hole_vert_indices[hr[next]]));
      }
    }
    r_result.ss_output.edge = Array<int2>(boundary_edges.as_span());
    r_result.dst_point_to_src_point = Array<int>(orig_pt_map.as_span());
    return;
  }

  straightskeleton::StraightSkeletonInput input;
  input.vert = all_verts.as_span();
  input.outer_polygon = outer_polygon.as_span();
  input.hole_offsets = hole_offsets;
  input.hole_vert_indices = hole_vert_indices.as_span();
  input.offset = offset;
  input.segments = math::max(1, segments);
  input.epsilon = epsilon;

  r_result.ss_output = straightskeleton::straight_skeleton_calc(input);
  if (!r_result.ss_output.is_valid || r_result.ss_output.hit_max_events) {
    r_result.is_error = true;
    r_result.error_message = r_result.ss_output.error_message.empty() ?
                                 "Straight skeleton calculation failed on mesh faces." :
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
    face_counts[i] = out.face_offsets.size();
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
      for (const int f : out.face_offsets.index_range()) {
        face_offsets[f] = out.face_offsets[f].start() + corners_range.start();
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
    const AttributeAccessor src_accessor = (results[i].domain == SourceComponent::Mesh && src_mesh) ?
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
    if (SpanAttributeWriter<float> dst_dist =
            dst_attributes.lookup_or_add_for_write_span<float>(*distance_attr_id,
                                                              bke::AttrDomain::Point))
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
    if (SpanAttributeWriter<bool> dst_skel =
            dst_attributes.lookup_or_add_for_write_span<bool>(*is_skeleton_attr_id,
                                                             bke::AttrDomain::Point))
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
    if (SpanAttributeWriter<int> dst_band =
            dst_attributes.lookup_or_add_for_write_span<int>(*face_band_attr_id,
                                                            bke::AttrDomain::Face))
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
    if (SpanAttributeWriter<int> dst_orig_f =
            dst_attributes.lookup_or_add_for_write_span<int>(*orig_face_attr_id,
                                                            bke::AttrDomain::Face))
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
  const StraightSkeletonMode mode = params.extract_input<StraightSkeletonMode>("Mode"_ustr);
  const float distance = params.extract_input<float>("Distance"_ustr);
  const int segments = params.extract_input<int>("Segments"_ustr);
  const float elevation_scale = params.extract_input<float>("Elevation Scale"_ustr);
  const float epsilon = params.extract_input<float>("Epsilon"_ustr);

  const double offset = (mode == StraightSkeletonMode::FullSkeleton) ?
                            std::numeric_limits<double>::infinity() :
                            double(distance);

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
        mesh_masks = IndexMask::from_group_ids(
            evaluator.get_evaluated<int>(0), memory, mesh_ids);
      }

      if (curves && !curves->is_empty()) {
        const bke::CurvesFieldContext context(*curves, bke::AttrDomain::Curve);
        FieldEvaluator evaluator(context, curves->curves_num());
        evaluator.add(group_index);
        evaluator.evaluate();
        curve_masks = IndexMask::from_group_ids(
            evaluator.get_evaluated<int>(0), memory, curve_ids);
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
        VectorSet<int> curve_ids;
        Vector<IndexMask> curve_masks = IndexMask::from_group_ids(
            evaluator.get_evaluated<int>(0), memory, curve_ids);

        Array<GroupResult> layer_results(curve_ids.size());
        for (int i = 0; i < curve_ids.size(); i++) {
          calc_curve_group(curves,
                           curve_masks[i],
                           mode,
                           offset,
                           segments,
                           epsilon > 0.0f ? double(epsilon) : 0.0,
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
      "Compute the 2D straight skeleton and offset bands for polygons with holes or open edge chains "
      "projected onto the X-Y plane";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.default_width = bke::NodeWidth::_160;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_straight_skeleton_cc
