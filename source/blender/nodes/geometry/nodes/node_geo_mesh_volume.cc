/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.h"
#include "BKE_mesh.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_mesh_volume_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh to calculate the enclosed volume of");
  b.add_output<decl::Float>("Volume"_ustr)
      .translation_context(BLT_I18NCONTEXT_AMOUNT)
      .description("The volume enclosed by the mesh");
}

static bool mesh_is_closed_and_consistently_oriented(const Mesh &mesh)
{
  Array<int> edge_use_counts(mesh.edges_num, 0);
  Array<int> first_corner_verts(mesh.edges_num, -1);
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();
  for (const int corner : corner_verts.index_range()) {
    const int edge = corner_edges[corner];
    const int vert = corner_verts[corner];
    edge_use_counts[edge]++;
    if (edge_use_counts[edge] > 2) {
      return false;
    }
    if (first_corner_verts[edge] == -1) {
      first_corner_verts[edge] = vert;
    }
    else if (first_corner_verts[edge] == vert) {
      return false;
    }
  }
  return std::all_of(edge_use_counts.begin(), edge_use_counts.end(), [](const int count) {
    return count == 2;
  });
}

static float mesh_volume(const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int3> corner_tris = mesh.corner_tris();
  if (positions.is_empty() || corner_tris.is_empty()) {
    return 0.0f;
  }

  float volume;
  BKE_mesh_calc_volume(reinterpret_cast<const float (*)[3]>(positions.data()),
                       positions.size(),
                       corner_tris.data(),
                       corner_tris.size(),
                       mesh.corner_verts().data(),
                       &volume,
                       nullptr);
  return volume;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  const Mesh *mesh = geometry_set.get_mesh();
  if (mesh == nullptr || mesh->faces_num == 0) {
    params.set_output("Volume"_ustr, 0.0f);
    return;
  }
  if (!mesh_is_closed_and_consistently_oriented(*mesh)) {
    params.error_message_add(
        NodeWarningType::Warning,
        "Mesh volume requires a closed mesh with consistently oriented faces; output is zero for "
        "boundary, non-manifold, or inconsistent edges");
    params.set_output("Volume"_ustr, 0.0f);
    return;
  }

  params.set_output("Volume"_ustr, mesh_volume(*mesh));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeMeshVolume"_ustr);
  ntype.ui_name = "Mesh Volume";
  ntype.ui_description = "Calculate the volume enclosed by a mesh";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_mesh_volume_cc
