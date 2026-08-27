/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_delaunay_2d.hh"
#include "BLI_index_mask.hh"

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"

#include "FN_field.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_join_geometries.hh"

#include "NOD_socket_usage_inference.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_delaunay_triangulation_cc {

enum class TriangulationMode : int8_t {
  Full = 0,
  Inside = 1,
  InsideWithHoles = 2,
};

enum class TriangulationType : int8_t {
  Triangles = 0,
  NGons = 1,
};

enum class FillRule : int8_t {
  EvenOdd = 0,
  NonZero = 1,
};

static const EnumPropertyItem triangulation_mode_items[] = {
    {int(TriangulationMode::Full),
     "FULL",
     0,
     N_("Full"),
     N_("All triangles. The outer boundary is the convex hull of input points")},
    {int(TriangulationMode::Inside),
     "INSIDE",
     0,
     N_("Inside"),
     N_("All triangles fully enclosed by constraint edges or faces")},
    {int(TriangulationMode::InsideWithHoles),
     "INSIDE_WITH_HOLES",
     0,
     N_("Inside With Holes"),
     N_("Triangles fully enclosed by constraint edges or faces excluding triangles inside "
        "detected holes")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem triangulation_type_items[] = {
    {int(TriangulationType::Triangles), "TRIANGLES", 0, N_("Triangles"), ""},
    {int(TriangulationType::NGons), "NGONS", 0, N_("N-gons"), ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem fill_rule_items[] = {
    {int(FillRule::EvenOdd),
     "EVEN_ODD",
     0,
     N_("Even-Odd"),
     N_("Alternate inside/outside based on crossing count")},
    {int(FillRule::NonZero),
     "NON_ZERO",
     0,
     N_("Non-Zero"),
     N_("Overlapping curves with the same winding direction are filled as a union")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::GreasePencil,
                       GeometryComponent::Type::PointCloud})
      .description(
          "The geometries that are used to constrain the triangulation using the points, edges, "
          "and faces");
  b.add_input<decl::Int>("Group ID"_ustr)
      .evaluated_geometry_field()
      .hide_value()
      .description(
          "An ID used to group points together. Triangulation is done separately for each group");
  b.add_input<decl::Menu>("Mode"_ustr)
      .static_items(triangulation_mode_items)
      .default_value(MenuValue(TriangulationMode::Full))
      .optional_label();
  b.add_input<decl::Menu>("Type"_ustr)
      .static_items(triangulation_type_items)
      .default_value(MenuValue(TriangulationType::Triangles))
      .optional_label()
      .usage_inference(
          [](const socket_usage_inference::SocketUsageParams &params) -> std::optional<bool> {
            return params.menu_input_may_be("Mode"_ustr, int(TriangulationMode::Inside)) ||
                   params.menu_input_may_be("Mode"_ustr, int(TriangulationMode::InsideWithHoles));
          });
  b.add_input<decl::Menu>("Fill Rule"_ustr)
      .static_items(fill_rule_items)
      .default_value(MenuValue(FillRule::EvenOdd))
      .optional_label()
      .usage_inference(
          [](const socket_usage_inference::SocketUsageParams &params) -> std::optional<bool> {
            return params.menu_input_may_be("Mode"_ustr, int(TriangulationMode::InsideWithHoles));
          });
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all_geometry();
  b.add_output<decl::Bool>("Intersection Points"_ustr)
      .anonymous_attribute_output()
      .description("A selection of newly created intersection points")
      .no_muted_links();
}

static CDT_output_type get_cdt_output_type(const TriangulationMode mode,
                                           const TriangulationType output_type,
                                           const FillRule fill_rule)
{
  switch (mode) {
    case TriangulationMode::Full:
      return CDT_FULL;
    case TriangulationMode::Inside:
      if (output_type == TriangulationType::Triangles) {
        return CDT_INSIDE;
      }
      return CDT_CONSTRAINTS_VALID_BMESH;
    case TriangulationMode::InsideWithHoles:
      switch (fill_rule) {
        case FillRule::EvenOdd: {
          if (output_type == TriangulationType::Triangles) {
            return CDT_INSIDE_WITH_HOLES;
          }
          return CDT_CONSTRAINTS_VALID_BMESH_WITH_HOLES;
        }
        case FillRule::NonZero: {
          if (output_type == TriangulationType::Triangles) {
            return CDT_INSIDE_WITH_HOLES_NONZERO;
          }
          return CDT_CONSTRAINTS_VALID_BMESH_WITH_HOLES_NONZERO;
        }
      }
  }
  return CDT_FULL;
}

/* The kind of geometry a CDT point source comes from. */
enum class SourceComponent : int8_t {
  Mesh = 0,
  Curve = 1,
  PointCloud = 2,
};

/* The points, edges, and faces of the original geometry that make up a single group. The masks
 * index into the original geometry's domains (mesh vertices, curve evaluated points, point cloud
 * points). */
struct GroupMasks {
  IndexMask mesh_verts;
  IndexMask mesh_loose_edges;
  IndexMask mesh_faces;
  IndexMask curves;
  IndexMask curve_points;
  IndexMask points;

  /* The point sources contributing to this group, in the order they're concatenated into the CDT
   * input vertex array, with #point_offsets giving the range used by each. */
  Vector<SourceComponent, 3> point_source_domains;
  Vector<int> point_offsets;

  OffsetIndices<int> points_by_component() const
  {
    return OffsetIndices<int>(point_offsets.as_span());
  }
};

struct TriangulationResult {
  meshintersect::CDT_result<double> cdt_result;

  /* Mirror of #GroupMasks::point_source_domains so attributes can be gathered from the original
   * geometry without keeping the masks alive. */
  Vector<SourceComponent, 3> point_source_domains;

  Vector<int> component_points_offsets;
  Array<int> src_point_by_dst_point;

  Vector<int> src_by_intersection_offsets;
  Array<int> intersection_components;
  Array<int> src_points_by_intersection;
  Array<float> weights_by_intersection;

  IndexRange intersection_points;

  OffsetIndices<int> dst_points_range_by_component() const
  {
    return OffsetIndices<int>(component_points_offsets.as_span());
  }

  GroupedSpan<int> intersection_src_component_by_dst() const
  {
    return GroupedSpan<int>(src_by_intersection_offsets.as_span(),
                            intersection_components.as_span());
  }

  GroupedSpan<int> intersection_src_by_dst() const
  {
    return GroupedSpan<int>(src_by_intersection_offsets.as_span(),
                            src_points_by_intersection.as_span());
  }

  GroupedSpan<float> intersection_src_weight_by_dst() const
  {
    return GroupedSpan<float>(src_by_intersection_offsets.as_span(),
                              weights_by_intersection.as_span());
  }
};

static const IndexMask &group_point_mask(const GroupMasks &group, const SourceComponent domain)
{
  switch (domain) {
    case SourceComponent::Mesh:
      return group.mesh_verts;
    case SourceComponent::Curve:
      return group.curve_points;
    case SourceComponent::PointCloud:
      return group.points;
  }
  BLI_assert_unreachable();
  return group.points;
}

static void gather_2d_positions(const Span<float3> src_positions,
                                const IndexMask &mask,
                                MutableSpan<double2> dst_positions)
{
  mask.foreach_index_optimized<int>(
      [&](const int index, const int pos) {
        dst_positions[pos] = double2(src_positions[index].x, src_positions[index].y);
      },
      exec_mode::grain_size(8192));
}

static Array<TriangulationResult> calc_triangulations(const Mesh *mesh,
                                                      const bke::CurvesGeometry *curves,
                                                      const fn::FieldContext *curves_field_context,
                                                      const PointCloud *pointcloud,
                                                      const Field<int> &group_index,
                                                      const CDT_output_type output_type)
{
  IndexMaskMemory memory;
  Vector<IndexMask> mesh_masks;
  Vector<IndexMask> curve_masks;
  Vector<IndexMask> point_masks;
  VectorSet<int> mesh_ids;
  VectorSet<int> curve_ids;
  VectorSet<int> point_ids;

  std::optional<FieldEvaluator> mesh_evaluator;
  std::optional<FieldEvaluator> curve_evaluator;
  std::optional<FieldEvaluator> point_evaluator;

  if (mesh && mesh->verts_num > 0) {
    const bke::GeometryFieldContext context{*mesh, bke::AttrDomain::Point};
    mesh_evaluator.emplace(context, mesh->verts_num);
    mesh_evaluator->add(group_index);
    mesh_evaluator->evaluate();
    mesh_masks = IndexMask::from_group_ids(
        mesh_evaluator->get_evaluated<int>(0), memory, mesh_ids);
  }
  if (curves && !curves->is_empty()) {
    curve_evaluator.emplace(*curves_field_context, curves->curves_num());
    curve_evaluator->add(group_index);
    curve_evaluator->evaluate();
    curve_masks = IndexMask::from_group_ids(
        curve_evaluator->get_evaluated<int>(0), memory, curve_ids);
  }
  if (pointcloud && pointcloud->totpoint > 0) {
    const bke::GeometryFieldContext context{*pointcloud};
    point_evaluator.emplace(context, pointcloud->totpoint);
    point_evaluator->add(group_index);
    point_evaluator->evaluate();
    point_masks = IndexMask::from_group_ids(
        point_evaluator->get_evaluated<int>(0), memory, point_ids);
  }

  VectorSet<int> all_group_ids;
  all_group_ids.add_multiple(mesh_ids);
  all_group_ids.add_multiple(curve_ids);
  all_group_ids.add_multiple(point_ids);
  if (all_group_ids.is_empty()) {
    return {};
  }

  Span<int2> mesh_edges;
  OffsetIndices<int> mesh_faces;
  Span<int> mesh_corner_verts;
  if (mesh) {
    mesh_edges = mesh->edges();
    mesh_faces = mesh->faces();
    mesh_corner_verts = mesh->corner_verts();
  }

  Array<TriangulationResult> results(all_group_ids.size());
  threading::parallel_for(
      results.index_range(),
      1024,
      [&](const IndexRange range) {
        for (const int i : range) {
          const int group_id = all_group_ids[i];
          GroupMasks group;

          IndexMaskMemory memory;
          if (mesh) {
            const int index = mesh_ids.index_of_try(group_id);
            if (index != -1) {
              group.mesh_verts = mesh_masks[index];
              /* A loose edge or face belongs to the group only if all of its vertices do. */
              BitVector<> vert_in_group(mesh->verts_num);
              group.mesh_verts.to_bits(vert_in_group);
              group.mesh_loose_edges = IndexMask::from_predicate(
                  mesh->loose_edges(), memory, [&](const int64_t edge) {
                    return vert_in_group[mesh_edges[edge][0]] &&
                           vert_in_group[mesh_edges[edge][1]];
                  });
              group.mesh_faces = IndexMask::from_predicate(
                  mesh_faces.index_range(), memory, [&](const int64_t face) {
                    return std::ranges::all_of(
                        mesh_corner_verts.slice(mesh_faces[face]),
                        [&](const int vert) { return vert_in_group[vert]; });
                  });
            }
          }
          if (curves) {
            const int index = curve_ids.index_of_try(group_id);
            if (index != -1) {
              group.curves = curve_masks[index];
              group.curve_points = IndexMask::from_ranges(
                  curves->evaluated_points_by_curve(), group.curves, memory);
            }
          }
          if (pointcloud) {
            const int index = point_ids.index_of_try(group_id);
            if (index != -1) {
              group.points = point_masks[index];
            }
          }

          IndexMask face_curves;
          IndexMask edge_curves;
          int64_t curve_segment_total = 0;
          if (curves && !group.curves.is_empty()) {
            const VArray<bool> cyclic = curves->cyclic();
            face_curves = IndexMask::from_bools(group.curves, cyclic, memory);
            const OffsetIndices<int> points_by_curve = curves->evaluated_points_by_curve();
            face_curves = IndexMask::from_predicate(face_curves, memory, [&](const int curve) {
              return points_by_curve[curve].size() > 2;
            });
            edge_curves = face_curves.complement(group.curves, memory);
          }

          /* Gather the point sources (and the offsets describing their place in the CDT vertex
           * array) in a fixed order: mesh vertices, curve evaluated points, then point cloud
           * points. */
          int mesh_point_source = -1;
          int curve_point_source = -1;
          auto add_point_source =
              [&](const SourceComponent domain, const int64_t count, int &r_index) {
                r_index = int(group.point_source_domains.append_and_get_index(domain));
                group.point_offsets.append(int(count));
              };
          int unused_index = -1;
          if (mesh && !group.mesh_verts.is_empty()) {
            add_point_source(SourceComponent::Mesh, group.mesh_verts.size(), mesh_point_source);
          }
          if (curves && !group.curve_points.is_empty()) {
            add_point_source(
                SourceComponent::Curve, group.curve_points.size(), curve_point_source);
          }
          if (pointcloud && !group.points.is_empty()) {
            add_point_source(SourceComponent::PointCloud, group.points.size(), unused_index);
          }
          if (group.point_source_domains.is_empty()) {
            continue;
          }

          /* Edge sources, in the same fixed order. */
          Vector<SourceComponent, 2> edge_source_domains;
          Vector<int, 3> edge_offsets;
          if (mesh && !group.mesh_loose_edges.is_empty()) {
            edge_source_domains.append(SourceComponent::Mesh);
            edge_offsets.append(group.mesh_loose_edges.size());
          }
          if (curve_segment_total > 0) {
            edge_source_domains.append(SourceComponent::Curve);
            edge_offsets.append(int(curve_segment_total));
          }

          /* Face sources, in the same fixed order. */
          Vector<SourceComponent, 2> face_source_domains;
          Vector<int, 3> faces_by_source_data;
          if (mesh && !group.mesh_faces.is_empty()) {
            face_source_domains.append(SourceComponent::Mesh);
            faces_by_source_data.append(group.mesh_faces.size());
          }
          if (!face_curves.is_empty()) {
            face_source_domains.append(SourceComponent::Curve);
            faces_by_source_data.append(face_curves.size());
          }

          group.point_offsets.append(0);
          edge_offsets.append(0);
          faces_by_source_data.append(0);
          const OffsetIndices<int> points_by_source = offset_indices::accumulate_counts_to_offsets(
              group.point_offsets);
          const OffsetIndices<int> edges_by_source = offset_indices::accumulate_counts_to_offsets(
              edge_offsets);
          const OffsetIndices<int> faces_by_source = offset_indices::accumulate_counts_to_offsets(
              faces_by_source_data);

          const int total_source_points = points_by_source.total_size();

          /* Add 2D points. */
          Array<double2> cdt_verts(total_source_points);
          for (const int source_i : group.point_source_domains.index_range()) {
            const IndexRange dst_range = points_by_source[source_i];
            MutableSpan<double2> dst_positions_2d = cdt_verts.as_mutable_span().slice(dst_range);
            switch (group.point_source_domains[source_i]) {
              case SourceComponent::Mesh:
                gather_2d_positions(mesh->vert_positions(), group.mesh_verts, dst_positions_2d);
                break;
              case SourceComponent::Curve:
                gather_2d_positions(
                    curves->evaluated_positions(), group.curve_points, dst_positions_2d);
                break;
              case SourceComponent::PointCloud:
                gather_2d_positions(pointcloud->positions(), group.points, dst_positions_2d);
                break;
            }
          }

          /* Map from original point indices to their destination indices in the CDT vertex array,
           * for remapping edge and face constraints. */
          Array<int> mesh_vert_to_dst;
          if (mesh && (!group.mesh_loose_edges.is_empty() || !group.mesh_faces.is_empty())) {
            mesh_vert_to_dst.reinitialize(mesh->verts_num);
            index_mask::build_reverse_map<int>(group.mesh_verts, mesh_vert_to_dst);
          }
          Array<int> curve_point_to_dst;
          if (curves && (curve_segment_total > 0 || !face_curves.is_empty())) {
            curve_point_to_dst.reinitialize(curves->evaluated_points_num());
            index_mask::build_reverse_map<int>(group.curve_points, curve_point_to_dst);
          }

          /* Add edge constraints. */
          Array<std::pair<int, int>> cdt_edges(edges_by_source.total_size());
          for (const int source_i : edge_source_domains.index_range()) {
            MutableSpan<std::pair<int, int>> dst_edges = cdt_edges.as_mutable_span().slice(
                edges_by_source[source_i]);
            switch (edge_source_domains[source_i]) {
              case SourceComponent::Mesh: {
                const Span<int2> edges = mesh->edges();
                const int dst_point_offset = points_by_source[mesh_point_source].start();
                group.mesh_loose_edges.foreach_index_optimized<int>(
                    [&](const int index, const int pos) {
                      dst_edges[pos] = {mesh_vert_to_dst[edges[index][0]] + dst_point_offset,
                                        mesh_vert_to_dst[edges[index][1]] + dst_point_offset};
                    },
                    exec_mode::grain_size(4096));
                break;
              }
              case SourceComponent::Curve: {
                const OffsetIndices<int> points_by_curve = curves->evaluated_points_by_curve();

                Array<int> segment_offsets(edge_curves.size() + 1);
                edge_curves.foreach_index_optimized<int>(
                    [&](const int curve, const int pos) {
                      /* No need for #bke::curves::segments_num because these curves are known to
                       * be non-cyclic. Cyclic curves are processed as faces. */
                      segment_offsets[pos] = points_by_curve[curve].size() - 1;
                    },
                    exec_mode::grain_size(4096));
                const OffsetIndices<int> edges_by_curve =
                    offset_indices::accumulate_counts_to_offsets(
                        segment_offsets.as_mutable_span());

                const int dst_point_offset = points_by_source[curve_point_source].start();
                edge_curves.foreach_index_optimized<int>(
                    [&](const int curve, const int pos) {
                      const IndexRange points = points_by_curve[curve];
                      MutableSpan<std::pair<int, int>> dst_edges_by_curve = dst_edges.slice(
                          edges_by_curve[pos]);
                      for (const int point : points.index_range().drop_back(1)) {
                        dst_edges_by_curve[point] = {
                            curve_point_to_dst[points[point]] + dst_point_offset,
                            curve_point_to_dst[points[point] + 1] + dst_point_offset};
                      }
                    },
                    exec_mode::grain_size(4096));
                break;
              }
              case SourceComponent::PointCloud:
                break;
            }
          }

          /* Count total face sizes. */
          Array<int> cdt_face_offsets(faces_by_source.total_size() + 1);
          for (const int source_i : face_source_domains.index_range()) {
            MutableSpan<int> dst_face_sizes = cdt_face_offsets.as_mutable_span().slice(
                faces_by_source[source_i]);
            switch (face_source_domains[source_i]) {
              case SourceComponent::Mesh:
                offset_indices::gather_group_sizes(
                    mesh->faces(), group.mesh_faces, dst_face_sizes);
                break;
              case SourceComponent::Curve:
                offset_indices::gather_group_sizes(
                    curves->evaluated_points_by_curve(), face_curves, dst_face_sizes);
                break;
              case SourceComponent::PointCloud:
                break;
            }
          }
          const OffsetIndices<int> cdt_faces = offset_indices::accumulate_counts_to_offsets(
              cdt_face_offsets);

          /* Add face constraints. */
          Array<int> cdt_face_vert_indices(cdt_faces.total_size());
          for (const int source_i : face_source_domains.index_range()) {
            const OffsetIndices<int> dst_faces = cdt_faces.slice(faces_by_source[source_i]);
            switch (face_source_domains[source_i]) {
              case SourceComponent::Mesh: {
                const int dst_point_offset = points_by_source[mesh_point_source].start();
                const OffsetIndices faces = mesh->faces();
                const Span<int> corner_verts = mesh->corner_verts();
                group.mesh_faces.foreach_index(
                    [&](const int index, const int pos) {
                      const IndexRange face = faces[index];
                      MutableSpan<int> dst_face = cdt_face_vert_indices.as_mutable_span().slice(
                          dst_faces[pos]);
                      for (const int i : face.index_range()) {
                        dst_face[i] = mesh_vert_to_dst[corner_verts[face[i]]] + dst_point_offset;
                      }
                    },
                    exec_mode::grain_size(2048));
                break;
              }
              case SourceComponent::Curve: {
                const int dst_point_offset = points_by_source[curve_point_source].start();
                const OffsetIndices<int> points_by_curve = curves->evaluated_points_by_curve();
                threading::parallel_for(
                    face_curves.index_range(), 1024, [&](const IndexRange range) {
                      for (const int i : range) {
                        const int curve = face_curves[i];
                        const IndexRange points = points_by_curve[curve];
                        MutableSpan<int> dst_face = cdt_face_vert_indices.as_mutable_span().slice(
                            dst_faces[i]);
                        for (const int j : points.index_range()) {
                          dst_face[j] = curve_point_to_dst[points[j]] + dst_point_offset;
                        }
                      }
                    });
                break;
              }
              case SourceComponent::PointCloud:
                break;
            }
          }

          const meshintersect::CDT_input<double> cdt_input{
              .vert = cdt_verts,
              .face_offsets = cdt_faces,
              .face_vert_indices = cdt_face_vert_indices,
              .needed_ids = CDT_ORIG_VERTS | CDT_INTERSECTED_EDGES};

          results[i].cdt_result = meshintersect::delaunay_2d_calc(cdt_input, output_type);

          results[i].point_source_domains = group.point_source_domains;

          const int total_dst_verts = results[i].cdt_result.vert_orig.size();
          const OffsetIndices src_points_by_component = group.points_by_component();
          const Span<Vector<uint>> verts_orig = results[i].cdt_result.vert_orig.as_span();
          const Span<int2> intersected_edges_orig =
              results[i].cdt_result.intersected_edges_orig.as_span();

          Array<int> dst_point_to_src_point(total_dst_verts, -1);
          Vector<int, 4> component_points_offsets;
          int64_t component_i = 0;
          int64_t count = 0;
          int64_t intersections_start = -1;
          for (const int dst_point : verts_orig.index_range()) {
            const Span<uint> verts = verts_orig[dst_point].as_span();
            if (!verts.is_empty()) {
              /* Only use the first point and discard the rest of potentially merged vertices. The
               * index refers to a position in the concatenated CDT input vertex array. */
              const int src_point = verts.first();
              if (!src_points_by_component[component_i].contains(src_point)) {
                BLI_assert(component_i + 1 < group.point_source_domains.size());
                BLI_assert(src_points_by_component[component_i + 1].contains(src_point));
                component_i++;
                component_points_offsets.append(count);
                count = 0;
              }
              const IndexRange src_range = src_points_by_component[component_i];
              const int local_point = src_point - int(src_range.start());
              /* Map the local point back to its index in the original geometry through the mask.
               */
              const IndexMask &mask = group_point_mask(group,
                                                       group.point_source_domains[component_i]);
              dst_point_to_src_point[dst_point] = int(mask[local_point]);
              count++;
            }
            else {
              /* Assume that all the intersection points are added at the end of the output. */
              intersections_start = dst_point;
              break;
            }
          }
          component_points_offsets.append(count);
          /* Append one more element to account for the final offset. */
          component_points_offsets.append(0);
          offset_indices::accumulate_counts_to_offsets(component_points_offsets.as_mutable_span());

          results[i].component_points_offsets = std::move(component_points_offsets);
          results[i].src_point_by_dst_point = std::move(dst_point_to_src_point);

          const uint32_t face_edge_offset = results[i].cdt_result.face_edge_offset;
          auto verts_from_cdt_edge = [&](const int edge) -> std::pair<int, int> {
            if (edge < face_edge_offset) {
              return cdt_edges[edge];
            }
            /* See CDT_result::edge_orig for how edge indices are encoded. */
            const int src_face = (edge / face_edge_offset) - 1;
            const int src_face_offset = edge % face_edge_offset;
            const IndexRange face_range = cdt_faces[src_face];
            const Span<int> face = cdt_face_vert_indices.as_span().slice(face_range);
            return {face[src_face_offset], face[(src_face_offset + 1) % face_range.size()]};
          };

          if (intersections_start != -1) {
            const IndexRange intersection_points = IndexRange::from_begin_end(intersections_start,
                                                                              total_dst_verts);
            results[i].intersection_points = intersection_points;

            Array<int> src_component_by_point(total_source_points);
            for (const int component_i : group.point_source_domains.index_range()) {
              src_component_by_point.as_mutable_span()
                  .slice(src_points_by_component[component_i])
                  .fill(component_i);
            }

            const int total_src_points_per_intersection = intersection_points.size() * 4;

            Vector<int> src_by_intersection_offsets;
            Array<int> intersection_components(total_src_points_per_intersection);
            Array<int> src_points_by_intersection(total_src_points_per_intersection);
            Array<float> weights_by_intersection(total_src_points_per_intersection);

            for (const int intersection_point : intersection_points.index_range()) {
              const int dst_point = intersection_points[intersection_point];
              const int edge1 = intersected_edges_orig[dst_point][0];
              const int edge2 = intersected_edges_orig[dst_point][1];
              BLI_assert(edge1 != -1 && edge2 != -1);

              const auto [vert1, vert2] = verts_from_cdt_edge(edge1);
              const auto [vert3, vert4] = verts_from_cdt_edge(edge2);
              const Array<int> src_points{vert1, vert2, vert3, vert4};
              Array<float2> src_positions(src_points.size());
              for (const int i : src_points.index_range()) {
                const int src_point = src_points[i];
                const int src_component = src_component_by_point[src_point];
                src_positions[i] = float2(cdt_verts[src_point]);

                const IndexRange src_range = src_points_by_component[src_component];
                const int local_point = src_point - int(src_range.start());
                const IndexMask &mask = group_point_mask(
                    group, group.point_source_domains[src_component]);

                intersection_components[intersection_point * 4 + i] = src_component;
                src_points_by_intersection[intersection_point * 4 + i] = int(mask[local_point]);
              }

              /* Compute the intersection weight from the four positions */
              const float2 pos1 = src_positions[0];
              const float2 pos2 = src_positions[1];
              const float2 pos3 = src_positions[2];
              const float2 pos4 = src_positions[3];

              /* double area = cross(B - A, C - A) */
              const float d1 = math::cross(pos4 - pos3, pos1 - pos3);
              const float d2 = math::cross(pos4 - pos3, pos2 - pos3);
              const float d3 = math::cross(pos2 - pos1, pos3 - pos1);
              const float d4 = math::cross(pos2 - pos1, pos4 - pos1);

              /* Factors along each edge for intersection. */
              const float t = math::safe_divide(d1, (d1 - d2));
              const float u = math::safe_divide(d3, (d3 - d4));
              BLI_assert(t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f);

              /* Weights of each point contributing to the intersection. */
              const float w1 = (1.0f - t) / 2.0f;
              const float w2 = t / 2.0f;
              const float w3 = (1.0f - u) / 2.0f;
              const float w4 = u / 2.0f;

              weights_by_intersection[intersection_point * 4 + 0] = w1;
              weights_by_intersection[intersection_point * 4 + 1] = w2;
              weights_by_intersection[intersection_point * 4 + 2] = w3;
              weights_by_intersection[intersection_point * 4 + 3] = w4;

              src_by_intersection_offsets.append(src_points.size());
            }
            src_by_intersection_offsets.append(0);
            offset_indices::accumulate_counts_to_offsets(
                src_by_intersection_offsets.as_mutable_span());

            results[i].src_by_intersection_offsets = std::move(src_by_intersection_offsets);
            results[i].intersection_components = std::move(intersection_components);
            results[i].src_points_by_intersection = std::move(src_points_by_intersection);
            results[i].weights_by_intersection = std::move(weights_by_intersection);
          }
        }
      },
      threading::individual_task_sizes([&](const int i) {
        const int group_id = all_group_ids[i];
        int size = 0;
        if (mesh) {
          const int index = mesh_ids.index_of_try(group_id);
          if (index != -1) {
            size += mesh_masks[index].size();
          }
        }
        if (curves) {
          const int index = curve_ids.index_of_try(group_id);
          if (index != -1) {
            size += curve_masks[index].size();
          }
        }
        if (pointcloud) {
          const int index = point_ids.index_of_try(group_id);
          if (index != -1) {
            size += point_masks[index].size();
          }
        }
        return size;
      }));

  return results;
}

static void gather_attributes_for_result_for_component(const AttributeAccessor &src_attributes,
                                                       const bke::AttrDomain domain,
                                                       const IndexRange result_range,
                                                       const IndexRange component_range,
                                                       const Span<int> dst_to_src_map,
                                                       const AttributeFilter &attribute_filter,
                                                       MutableAttributeAccessor &dst_attributes)
{
  src_attributes.foreach_attribute([&](const AttributeIter &iter) {
    if (iter.domain != domain) {
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
    const GVArray src = *iter.get(domain);
    const CommonVArrayInfo info = src.common_info();
    if (info.type == CommonVArrayInfo::Type::Single) {
      const bke::AttributeInitValue init(GPointer(src.type(), info.data));
      /* NOTE: Important to fail when another component has added the attribute already, in case
       * the single values are different. */
      if (dst_attributes.add(iter.name, iter.domain, iter.data_type, init)) {
        return;
      }
    }

    /* We might not write to the full range, so ensure that we default initialize the attribute. */
    GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_span(
        iter.name, domain, iter.data_type, bke::AttributeInitDefaultValue());
    if (!dst) {
      return;
    }
    bke::attribute_math::gather(
        src, dst_to_src_map, dst.span.slice(result_range).slice(component_range));
    dst.finish();
  });
}

static void mix_intersection_attributes_for_result(
    const Span<AttributeAccessor> src_accessors,
    const IndexRange intersection_points,
    const GroupedSpan<int> intersection_src_component_by_dst,
    const GroupedSpan<int> intersection_src_by_dst,
    const GroupedSpan<float> intersection_src_weight_by_dst,
    const AttributeFilter &attribute_filter,
    MutableAttributeAccessor &dst_attributes)
{
  dst_attributes.foreach_attribute([&](const AttributeIter &iter) {
    if (iter.data_type == bke::AttrType::String) {
      return;
    }
    if (attribute_filter.allow_skip(iter.name)) {
      return;
    }

    GSpanAttributeWriter dst_generic = dst_attributes.lookup_for_write_span(iter.name);
    BLI_assert(dst_generic);

    Vector<GAttributeReader> src_attributes;
    for (const AttributeAccessor &accessor : src_accessors) {
      src_attributes.append(accessor.lookup_or_default(iter.name, iter.domain, iter.data_type));
    }

    bke::attribute_math::to_static_type(dst_generic.span.type(), [&]<typename T>() {
      MutableSpan<T> dst = dst_generic.span.slice(intersection_points).typed<T>();
      bke::attribute_math::DefaultMixer<T> mixer{dst};
      threading::parallel_for(dst.index_range(), 1024, [&](const IndexRange range) {
        for (const int64_t dst_point : range) {
          /* All these spans should have 4 elements (two end points of two segments that
           * intersect in the destination point). */
          const Span<int> src_components = intersection_src_component_by_dst[dst_point];
          const Span<int> src_indices = intersection_src_by_dst[dst_point];
          const Span<float> src_weights = intersection_src_weight_by_dst[dst_point];
          BLI_assert(src_components.size() == 4 && src_indices.size() == 4 &&
                     src_weights.size() == 4);
          for (const int i : IndexRange(4)) {
            const int component_i = src_components[i];
            const AttributeReader<T> src_attribute = src_attributes[component_i].typed<T>();
            const int src_point = src_indices[i];
            mixer.mix_in(dst_point, src_attribute.varray[src_point], src_weights[i]);
          }
        }
        mixer.finalize(range);
      });
    });
    dst_generic.finish();
  });
}

static AttributeAccessor src_attributes_for_domain(const Mesh *mesh,
                                                   const bke::CurvesGeometry *curves,
                                                   const PointCloud *pointcloud,
                                                   const SourceComponent domain)
{
  switch (domain) {
    case SourceComponent::Mesh:
      return mesh->attributes();
    case SourceComponent::Curve:
      return curves->attributes();
    case SourceComponent::PointCloud:
      return pointcloud->attributes();
  }
  return mesh->attributes();
}

static Mesh *cdts_to_mesh(const Span<TriangulationResult> results,
                          const Mesh *mesh,
                          const bke::CurvesGeometry *curves,
                          const PointCloud *pointcloud,
                          const std::optional<std::string> dst_intersection_points_attribute_id,
                          const AttributeFilter &attribute_filter)
{
  Array<int> vert_groups_data(results.size() + 1);
  Array<int> edge_groups_data(results.size() + 1);
  Array<int> face_groups_data(results.size() + 1);
  Array<int> corner_groups_data(results.size() + 1);
  threading::parallel_for(results.index_range(), 1024, [&](const IndexRange results_range) {
    for (const int i_result : results_range) {
      const meshintersect::CDT_result<double> &result = results[i_result].cdt_result;
      vert_groups_data[i_result] = result.vert.size();
      edge_groups_data[i_result] = result.edge.size();
      face_groups_data[i_result] = result.face.size();
      int corners_num = 0;
      for (const Vector<int> &face : result.face) {
        corners_num += face.size();
      }
      corner_groups_data[i_result] = corners_num;
    }
  });

  const OffsetIndices vert_groups = offset_indices::accumulate_counts_to_offsets(vert_groups_data);
  const OffsetIndices edge_groups = offset_indices::accumulate_counts_to_offsets(edge_groups_data);
  const OffsetIndices face_groups = offset_indices::accumulate_counts_to_offsets(face_groups_data);
  const OffsetIndices corner_groups = offset_indices::accumulate_counts_to_offsets(
      corner_groups_data);

  Mesh *dst_mesh = BKE_mesh_new_nomain(vert_groups.total_size(),
                                       edge_groups.total_size(),
                                       face_groups.total_size(),
                                       corner_groups.total_size());

  MutableSpan<float3> all_positions = dst_mesh->vert_positions_for_write();
  MutableSpan<int2> all_edges = dst_mesh->edges_for_write();
  MutableSpan<int> all_face_offsets = dst_mesh->face_offsets_for_write();
  MutableSpan<int> all_corner_verts = dst_mesh->corner_verts_for_write();

  threading::parallel_for(results.index_range(), 1024, [&](const IndexRange results_range) {
    for (const int i_result : results_range) {
      const meshintersect::CDT_result<double> &result = results[i_result].cdt_result;
      const IndexRange verts_range = vert_groups[i_result];
      const IndexRange edges_range = edge_groups[i_result];
      const IndexRange faces_range = face_groups[i_result];
      const IndexRange loops_range = corner_groups[i_result];

      MutableSpan<float3> positions = all_positions.slice(verts_range);
      for (const int i : result.vert.index_range()) {
        positions[i] = float3(float(result.vert[i].x), float(result.vert[i].y), 0.0f);
      }

      MutableSpan<int2> edges = all_edges.slice(edges_range);
      for (const int i : result.edge.index_range()) {
        edges[i] = int2(result.edge[i][0] + verts_range.start(),
                        result.edge[i][1] + verts_range.start());
      }

      MutableSpan<int> face_offsets = all_face_offsets.slice(faces_range);
      MutableSpan<int> corner_verts = all_corner_verts.slice(loops_range);
      int i_face_corner = 0;
      for (const int i_face : result.face.index_range()) {
        face_offsets[i_face] = i_face_corner + loops_range.start();
        for (const int i_corner : result.face[i_face].index_range()) {
          corner_verts[i_face_corner] = result.face[i_face][i_corner] + verts_range.start();
          i_face_corner++;
        }
      }
    }
  });

  MutableAttributeAccessor dst_attributes = dst_mesh->attributes_for_write();
  threading::parallel_for(results.index_range(), 1024, [&](const IndexRange range) {
    for (const int result_i : range) {
      const IndexRange verts_range = vert_groups[result_i];

      const TriangulationResult &result = results[result_i];
      const OffsetIndices dst_points_range_by_component = result.dst_points_range_by_component();
      const Span<int> src_point_by_dst_point = result.src_point_by_dst_point.as_span();
      for (const int component_i : result.point_source_domains.index_range()) {
        const SourceComponent domain = result.point_source_domains[component_i];
        const IndexRange dst_range = dst_points_range_by_component[component_i];

        const AttributeAccessor src_attributes = src_attributes_for_domain(
            mesh, curves, pointcloud, domain);
        gather_attributes_for_result_for_component(
            src_attributes,
            bke::AttrDomain::Point,
            verts_range,
            dst_range,
            src_point_by_dst_point.slice(dst_range),
            bke::attribute_filter_with_skip_ref(attribute_filter, {"position"}),
            dst_attributes);
      }
    }
  });
  threading::parallel_for(results.index_range(), 1024, [&](const IndexRange range) {
    for (const int result_i : range) {
      const TriangulationResult &result = results[result_i];
      const IndexRange intersection_points = result.intersection_points;
      const GroupedSpan<int> intersection_src_component_by_dst =
          result.intersection_src_component_by_dst();
      const GroupedSpan<int> intersection_src_by_dst = result.intersection_src_by_dst();
      const GroupedSpan<float> intersection_src_weight_by_dst =
          result.intersection_src_weight_by_dst();

      Vector<AttributeAccessor> src_accessors;
      for (const SourceComponent component : result.point_source_domains) {
        src_accessors.append(src_attributes_for_domain(mesh, curves, pointcloud, component));
      }

      mix_intersection_attributes_for_result(src_accessors.as_span(),
                                             intersection_points,
                                             intersection_src_component_by_dst,
                                             intersection_src_by_dst,
                                             intersection_src_weight_by_dst,
                                             bke::attribute_filter_with_skip_ref(attribute_filter,
                                                                                 {"position",
                                                                                  ".edge_verts",
                                                                                  ".corner_vert",
                                                                                  ".corner_edge",
                                                                                  ".select_vert",
                                                                                  ".select_edge",
                                                                                  ".select_poly"}),
                                             dst_attributes);
    }
  });

  if (dst_intersection_points_attribute_id) {
    if (SpanAttributeWriter dst_intersection_points =
            dst_attributes.lookup_or_add_for_write_span<bool>(
                *dst_intersection_points_attribute_id, AttrDomain::Point))
    {
      threading::parallel_for(results.index_range(), 1024, [&](const IndexRange results_range) {
        for (const int result_i : results_range) {
          const IndexRange verts_range = vert_groups[result_i];
          const IndexRange intersection_points = results[result_i].intersection_points;
          dst_intersection_points.span.slice(verts_range).slice(intersection_points).fill(true);
        }
      });
      dst_intersection_points.finish();
    }
  }

  /* The delaunay triangulation doesn't seem to return all of the necessary all_edges, even in
   * triangulation mode. */
  bke::mesh_calc_edges(*dst_mesh, true, false);
  bke::mesh_smooth_set(*dst_mesh, false);

  dst_mesh->tag_overlapping_none();

  return dst_mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  Field<int> group_index = params.extract_input<Field<int>>("Group ID"_ustr);

  const TriangulationMode mode = params.extract_input<TriangulationMode>("Mode"_ustr);
  const TriangulationType type = params.extract_input<TriangulationType>("Type"_ustr);
  const FillRule fill_rule = params.extract_input<FillRule>("Fill Rule"_ustr);
  const CDT_output_type output_type = get_cdt_output_type(mode, type, fill_rule);

  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh"_ustr);
  std::optional<std::string> dst_intersection_points_attribute_id =
      params.get_output_anonymous_attribute_id_if_needed("Intersection Points"_ustr);

  geometry::foreach_real_geometry(geometry, [&](GeometrySet &geometry) {
    if (geometry.has_mesh() || geometry.has_curves() || geometry.has_pointcloud()) {
      const Mesh *mesh = geometry.get_mesh();
      const Curves *curves_id = geometry.get_curves();
      const bke::CurvesGeometry *curves = curves_id ? &curves_id->geometry.wrap() : nullptr;
      const PointCloud *pointcloud = geometry.get_pointcloud();
      std::optional<bke::CurvesFieldContext> curves_field_context;
      if (curves) {
        curves_field_context.emplace(bke::CurvesFieldContext(*curves, bke::AttrDomain::Curve));
      }
      Array<TriangulationResult> geometry_results = calc_triangulations(
          mesh,
          curves,
          curves_field_context.has_value() ? &*curves_field_context : nullptr,
          pointcloud,
          group_index,
          output_type);
      Mesh *result_mesh = cdts_to_mesh(geometry_results,
                                       mesh,
                                       curves,
                                       pointcloud,

                                       dst_intersection_points_attribute_id,
                                       attribute_filter);
      geometry.replace_mesh(result_mesh);
    }

    if (geometry.has_grease_pencil()) {
      using namespace blender::bke::greasepencil;
      const GreasePencil &grease_pencil = *geometry.get_grease_pencil();
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
        bke::GreasePencilLayerFieldContext layer_field_context(
            grease_pencil, bke::AttrDomain::Curve, layer_index);
        Array<TriangulationResult> geometry_results = calc_triangulations(
            nullptr, &curves, &layer_field_context, nullptr, group_index, output_type);
        mesh_by_layer[layer_index] = cdts_to_mesh(geometry_results,
                                                  nullptr,
                                                  &curves,
                                                  nullptr,
                                                  dst_intersection_points_attribute_id,
                                                  attribute_filter);
      }
      if (!mesh_by_layer.is_empty()) {
        auto instances = std::make_unique<bke::Instances>(mesh_by_layer.size());
        MutableSpan<int> handles = instances->reference_handles_for_write();
        instances->transforms_for_write().fill(float4x4::identity());
        for (const int i : mesh_by_layer.index_range()) {
          Mesh *mesh = mesh_by_layer[i];
          if (!mesh) {
            /* Add an empty reference so the number of layers and instances match.
             * This makes it easy to reconstruct the layers afterwards and keep their attributes.
             * Although in this particular case we don't propagate the attributes. */
            handles[i] = instances->add_reference(bke::InstanceReference());
            continue;
          }
          GeometrySet temp_set = GeometrySet::from_mesh(mesh);
          handles[i] = instances->add_reference(bke::InstanceReference{temp_set});
        }
        auto &dst_component = geometry.get_component_for_write<InstancesComponent>();
        GeometrySet new_instances = geometry::join_geometries(
            {GeometrySet::from_instances(dst_component.release()),
             GeometrySet::from_instances(std::move(instances))},
            {});
        dst_component.replace(
            new_instances.get_component_for_write<InstancesComponent>().release());
      }
    }

    geometry.keep_only({GeometryComponent::Type::Mesh, GeometryComponent::Type::Instance});
  });

  params.set_output("Mesh"_ustr, std::move(geometry));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeDelaunayTriangulation"_ustr);
  ntype.ui_name = "Delaunay Triangulation";
  ntype.ui_description =
      "Generate a triangulated mesh from a set of points in the X-Y plane. Uses edges and faces "
      "as triangulation constraints";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.default_width = bke::NodeWidth::_160;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_delaunay_triangulation_cc
