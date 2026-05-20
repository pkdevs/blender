/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_constants.hh"

#include "GEO_mesh_primitive_ico_sphere.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_mesh_primitive_ico_sphere_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Distance from the generated points to the origin");
  b.add_input<decl::Int>("Subdivisions"_ustr)
      .default_value(1)
      .min(1)
      .max(7)
      .description("Number of subdivisions on top of the basic icosahedron");
  b.add_output<decl::Geometry>("Mesh"_ustr);
  b.add_output<decl::Vector>("UV Map"_ustr).anonymous_attribute_output();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const int subdivisions = std::min(params.extract_input<int>("Subdivisions"_ustr), 10);
  const float radius = params.extract_input<float>("Radius"_ustr);

  std::optional<std::string> uv_map_id = params.get_output_anonymous_attribute_id_if_needed(
      "UV Map"_ustr);

  Mesh *mesh = geometry::create_ico_sphere_mesh(subdivisions, radius, uv_map_id);
  params.set_output("Mesh"_ustr, GeometrySet::from_mesh(mesh));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMeshIcoSphere"_ustr, GEO_NODE_MESH_PRIMITIVE_ICO_SPHERE);
  ntype.ui_name = "Ico Sphere";
  ntype.ui_description = "Generate a spherical mesh that consists of equally sized triangles";
  ntype.enum_name_legacy = "MESH_PRIMITIVE_ICO_SPHERE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_mesh_primitive_ico_sphere_cc
