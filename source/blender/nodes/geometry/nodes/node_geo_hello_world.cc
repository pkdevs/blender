/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"

#include "BLI_math_vector_types.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_hello_world_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh"_ustr).supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Vector>("Offset"_ustr).default_value({0, 0, 1});
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  /* Get inputs from sockets. */
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  const float3 offset = params.extract_input<float3>("Offset"_ustr);

  /* Process mesh if available. */
  if (Mesh *mesh = geometry_set.get_mesh_for_write()) {
    MutableSpan<float3> positions = mesh->vert_positions_for_write();
    for (const int i : positions.index_range()) {
      positions[i] += offset;
    }
    mesh->tag_positions_changed();
  }

  /* Set output socket. */
  params.set_output("Mesh"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeHelloWorld"_ustr);
  ntype.ui_name = "Hello World";
  ntype.ui_description = "My first node";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_hello_world_cc
