/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_constants.hh"
#include "BLI_math_euler.hh"

#include "DNA_mesh_types.h"

#include "BKE_material.hh"

#include "GEO_mesh_primitive_cuboid.hh"
#include "GEO_mesh_primitive_grid.hh"
#include "GEO_mesh_primitive_line.hh"
#include "GEO_transform.hh"

#include "node_geometry_util.hh"
#include <cmath>

namespace blender::nodes::node_geo_mesh_primitive_quad_sphere_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1000)
      .description("Distance from the generated points to the origin");
  b.add_input<decl::Int>("Subdivisions"_ustr)
       .default_value(3)
       .min(2)
       .max(7)
       .description("Number of subdivisions");
  b.add_output<decl::Geometry>("Mesh"_ustr);
  b.add_output<decl::Vector>("UV Map"_ustr).anonymous_attribute_output();
}

static Mesh *create_cube_mesh(const float3 size,
                              const int verts_x,
                              const int verts_y,
                              const int verts_z,
                              const std::optional<StringRef> &uv_map_id)

{
  return geometry::create_cuboid_mesh(size, verts_x, verts_y, verts_z, uv_map_id);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const float3 size = {2.0f, 2.0f, 2.0f};
  const int s = params.extract_input<int>("Subdivisions"_ustr);
  if (s < 1) {
      params.error_message_add(NodeWarningType::Info, TIP_("Subdivisions must be at least 2"));
      params.set_default_remaining_outputs();
      return;
  }
  const float rad = params.extract_input<float>("Radius"_ustr);
  const int subdivisions = (1 << (s - 1)) - 1 + 2;
  const int verts_x = subdivisions;
  const int verts_y = subdivisions;
  const int verts_z = subdivisions;

  std::optional<std::string> uv_map_id = params.get_output_anonymous_attribute_id_if_needed(
      "UV Map"_ustr);

  Mesh *mesh = create_cube_mesh(size, verts_x, verts_y, verts_z, uv_map_id);
  BKE_id_material_eval_ensure_default_slot(&mesh->id);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (float3 &co: positions) {
    // Source https://mathoverflow.net/questions/446657/mapping-a-cube-to-a-sphere
    int p = 500;
    float x = co.x;
    float y = co.y;
    float z = co.z;
    float xc = std::sqrt(std::pow(x, p) + std::pow(y, 2) + std::pow(z, 2)) * std::tan(x * std::atan(1 / (std::sqrt(std::pow(x, p) + std::pow(y, 2) + std::pow(z, 2)))));
	  float yc = std::sqrt(std::pow(x, 2) + std::pow(y, p) + std::pow(z, 2)) * std::tan(y * std::atan(1 / (std::sqrt(std::pow(x, 2) + std::pow(y, p) + std::pow(z, 2)))));
	  float zc = std::sqrt(std::pow(x, 2) + std::pow(y, 2) + std::pow(z, p)) * std::tan(z * std::atan(1 / (std::sqrt(std::pow(x, 2) + std::pow(y, 2) + std::pow(z, p)))));
    co.x = xc / std::sqrt(std::pow(xc, 2) + std::pow(yc, 2) + std::pow(zc, 2));
	  co.y = yc / std::sqrt(std::pow(xc, 2) + std::pow(yc, 2) + std::pow(zc, 2));
	  co.z = zc / std::sqrt(std::pow(xc, 2) + std::pow(yc, 2) + std::pow(zc, 2));
    // Apply the radius
	  co.x = co.x * rad;
	  co.y = co.y * rad;
	  co.z = co.z * rad;
  }
  params.set_output("Mesh"_ustr, GeometrySet::from_mesh(mesh));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMeshQuadSphere"_ustr);
  ntype.ui_name = "Quad Sphere";
  ntype.ui_description = "Generate a spherical mesh made by a Subdivided Cube and consists entirely out of quads";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_mesh_primitive_quad_sphere_cc
