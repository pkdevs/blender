/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_merge_verts.hh"
#include "GEO_point_merge.hh"

#include "node_geometry_util.hh"
#include <fmt/format.h>

namespace blender::nodes::node_geo_merge_by_distance_cc {

NODE_STORAGE_FUNCS(NodeGeometryMergeByDistance)

static EnumPropertyItem mode_items[] = {
    {GEO_NODE_MERGE_BY_DISTANCE_MODE_ALL,
     "ALL",
     0,
     N_("All"),
     N_("Merge all close selected points, whether or not they are connected")},
    {GEO_NODE_MERGE_BY_DISTANCE_MODE_CONNECTED,
     "CONNECTED",
     0,
     N_("Connected"),
     N_("Only merge mesh vertices along existing edges. This method can be much faster")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type({GeometryComponent::Type::PointCloud, GeometryComponent::Type::Mesh})
      .description("Point cloud or mesh to merge points of");
  b.add_output<decl::Geometry>("Geometry"_ustr).propagate_all_geometry().align_with_previous();
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .hide_value()
      .evaluated_geometry_field();
  b.add_input<decl::Menu>("Mode"_ustr).static_items(mode_items).optional_label();
  b.add_input<decl::Float>("Distance"_ustr).default_value(0.001f).min(0.0f).subtype(PROP_DISTANCE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  /* Still used for forward compatibility. */
  node->storage = MEM_new<NodeGeometryMergeByDistance>(__func__);
}

static PointCloud *pointcloud_merge_by_distance(const PointCloud &src_points,
                                                const float merge_distance,
                                                const Field<bool> &selection_field,
                                                const AttributeFilter &attribute_filter)
{
  const bke::PointCloudFieldContext context{src_points};
  FieldEvaluator evaluator{context, src_points.totpoint};
  evaluator.add(selection_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_as_mask(0);
  if (selection.is_empty()) {
    return nullptr;
  }

  return geometry::point_merge_by_distance(
      src_points, merge_distance, selection, attribute_filter);
}

static std::optional<Mesh *> mesh_merge_by_distance_connected(const Mesh &mesh,
                                                              const float merge_distance,
                                                              const Field<bool> &selection_field)
{
  Array<bool> selection(mesh.verts_num);
  const bke::MeshFieldContext context{mesh, AttrDomain::Point};
  FieldEvaluator evaluator{context, mesh.verts_num};
  evaluator.add_with_destination(selection_field, selection.as_mutable_span());
  evaluator.evaluate();

  return geometry::mesh_merge_by_distance_connected(mesh, selection, merge_distance, false);
}

static std::optional<Mesh *> mesh_merge_by_distance_all(const Mesh &mesh,
                                                        const float merge_distance,
                                                        const Field<bool> &selection_field)
{
  const bke::MeshFieldContext context{mesh, AttrDomain::Point};
  FieldEvaluator evaluator{context, mesh.verts_num};
  evaluator.add(selection_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_as_mask(0);
  if (selection.is_empty()) {
    return std::nullopt;
  }

  return geometry::mesh_merge_by_distance_all(mesh, selection, merge_distance);
}

static void add_removed_counts_info(const GeoNodeExecParams &params,
                                    const bool has_mesh,
                                    const int verts,
                                    const bool has_points,
                                    const int points)
{
  /* Always report, including zero, matching edit-mode Merge by Distance. */
  if (has_mesh && has_points) {
    const char *message_fmt;
    if (verts == 1 && points == 1) {
      message_fmt = TIP_("Removed {} vertex and {} point");
    }
    else if (verts == 1) {
      message_fmt = TIP_("Removed {} vertex and {} points");
    }
    else if (points == 1) {
      message_fmt = TIP_("Removed {} vertices and {} point");
    }
    else {
      message_fmt = TIP_("Removed {} vertices and {} points");
    }
    params.error_message_add(NodeWarningType::Info,
                             fmt::format(fmt::runtime(message_fmt), verts, points));
  }
  else if (has_mesh) {
    params.error_message_add(NodeWarningType::Info,
                             fmt::format(fmt::runtime(verts == 1 ? TIP_("Removed {} vertex") :
                                                                   TIP_("Removed {} vertices")),
                                         verts));
  }
  else if (has_points) {
    params.error_message_add(NodeWarningType::Info,
                             fmt::format(fmt::runtime(points == 1 ? TIP_("Removed {} point") :
                                                                    TIP_("Removed {} points")),
                                         points));
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry"_ustr);
  const auto mode = params.get_input<GeometryNodeMergeByDistanceMode>("Mode"_ustr);
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection"_ustr);
  const float merge_distance = params.extract_input<float>("Distance"_ustr);

  std::atomic<int> points_merged = 0;
  std::atomic<int> verts_merged = 0;
  std::atomic<bool> has_points = false;
  std::atomic<bool> has_mesh = false;

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (const PointCloud *pointcloud = geometry_set.get_pointcloud()) {
      if (mode == GEO_NODE_MERGE_BY_DISTANCE_MODE_ALL) {
        has_points = true;
        const int before = pointcloud->totpoint;

        PointCloud *result = pointcloud_merge_by_distance(
            *pointcloud, merge_distance, selection, params.get_attribute_filter("Geometry"_ustr));
        if (result) {
          geometry_set.replace_pointcloud(result);

          points_merged += before - result->totpoint;
        }
      }
    }
    if (const Mesh *mesh = geometry_set.get_mesh()) {
      has_mesh = true;
      const int before = mesh->verts_num;

      std::optional<Mesh *> result;
      switch (mode) {
        case GEO_NODE_MERGE_BY_DISTANCE_MODE_ALL:
          result = mesh_merge_by_distance_all(*mesh, merge_distance, selection);
          break;
        case GEO_NODE_MERGE_BY_DISTANCE_MODE_CONNECTED:
          result = mesh_merge_by_distance_connected(*mesh, merge_distance, selection);
          break;
        default:
          BLI_assert_unreachable();
      }
      if (result) {
        geometry_set.replace_mesh(*result);
        verts_merged += before - (*result)->verts_num;
      }
    }
  });

  params.set_output("Geometry"_ustr, std::move(geometry_set));

  add_removed_counts_info(
      params, has_mesh.load(), verts_merged.load(), has_points.load(), points_merged.load());
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMergeByDistance"_ustr, GEO_NODE_MERGE_BY_DISTANCE);
  ntype.ui_name = "Merge by Distance";
  ntype.ui_description = "Merge vertices or points within a given distance";
  ntype.enum_name_legacy = "MERGE_BY_DISTANCE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  bke::node_type_storage(ntype,
                         "NodeGeometryMergeByDistance",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_merge_by_distance_cc
