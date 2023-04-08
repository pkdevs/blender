/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_geom.h"

#include "BKE_mesh.h"

#include "NOD_socket_search_link.hh"

#include "GEO_mesh_clip_geometry_by_plane.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_clip_geometry_cc {

/* Node arg names: */
constexpr char INPUT_PLANE_ORIGIN[7] = "Center";
constexpr char INPUT_PLANE_NORMAL[7] = "Normal";
constexpr char OUTPUT_PLANE_SELECTION[16] = "Intersection";

/*
 * Mesh
 */
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Geometry"));

  b.add_input<decl::Vector>(INPUT_PLANE_ORIGIN)
      .default_value({0.0f, 0.0f, 0.0f})
      .subtype(PROP_XYZ);
  b.add_input<decl::Vector>(INPUT_PLANE_NORMAL)
      .default_value({1.0f, 0.0f, 0.0f})
      .subtype(PROP_XYZ);

  b.add_output<decl::Geometry>(N_("Geometry")).propagate_all();
  b.add_output<decl::Bool>(OUTPUT_PLANE_SELECTION).field_on_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Geometry");

  float3 plane_co = params.extract_input<float3>(INPUT_PLANE_ORIGIN);
  float3 plane_no = params.extract_input<float3>(INPUT_PLANE_NORMAL);

  blender::geometry::ClipByPlaneArgs args;
  args.plane_selection_attr_id = params.get_output_anonymous_attribute_id_if_needed(
      OUTPUT_PLANE_SELECTION);

  /* Check norm to avoid zero vector */
  if (normalize_v3(plane_no) != 0.0f) {
    plane_from_point_normal_v3(args.plane, plane_co, plane_no);

    geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
      if (geometry_set.has_mesh()) {
        const Mesh *mesh_in = geometry_set.get_mesh();

        std::pair<Mesh *, geometry::ClipResult> result = geometry::clip_by_plane(
            *mesh_in, args, attribute_filter);
        if (result.second != geometry::ClipResult::Keep) {
          /* If keep, do nothing and forward original mesh! */
          geometry_set.replace_mesh(result.first, bke::GeometryOwnershipType::Owned);
        }
      }
    });
  }

  params.set_output("Geometry", std::move(geometry_set));
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().static_declaration;
  search_link_ops_for_declarations(params, declaration.inputs);

  const std::optional<eCustomDataType> type = bke::socket_type_to_custom_data_type(
      eNodeSocketDatatype(params.other_socket().type));
  if (type && *type != CD_PROP_STRING) {
    /* The input and output sockets have the same name. */
    params.add_item(IFACE_(INPUT_PLANE_ORIGIN), [type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeClipGeometry");
      params.update_and_connect_available_socket(node, INPUT_PLANE_ORIGIN);
    });
    params.add_item(IFACE_(INPUT_PLANE_NORMAL), [type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeClipGeometry");
      params.update_and_connect_available_socket(node, INPUT_PLANE_NORMAL);
    });
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeClipGeometry");
  ntype.ui_name = "Clip Geometry";
  ntype.ui_description =
      "Clip the geometry by a plane, splitting the input geometry and removing all geometry on "
      "the positive side of the plane";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_clip_geometry_cc
