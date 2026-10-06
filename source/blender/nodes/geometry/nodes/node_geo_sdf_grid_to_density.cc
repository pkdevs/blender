/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/LevelSetUtil.h>
#endif

#include "BKE_volume_grid.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sdf_grid_to_density_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("SDF Grid"_ustr).hide_value().structure_type(StructureType::Grid);
  b.add_output<decl::Float>("Density Grid"_ustr).structure_type(StructureType::Grid);
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  auto grid = params.extract_input<bke::VolumeGrid<float>>("SDF Grid"_ustr);
  if (!grid) {
    params.set_default_remaining_outputs();
    return;
  }

  bke::VolumeTreeAccessToken tree_token;
  openvdb::FloatGrid &vdb_grid = grid.grid_for_write(tree_token);
  vdb_grid.setGridClass(openvdb::GRID_FOG_VOLUME);

  /* Convert the level set to a fog volume. This also sets the background value to zero. Inside the
   * fog there will be a density of 1. */
  openvdb::tools::sdfToFogVolume(vdb_grid);

  params.set_output("Density Grid"_ustr, std::move(grid));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSDFGridToDensity"_ustr);
  ntype.ui_name = "SDF Grid to Density";
  ntype.ui_description = "Generate a fog volume inside an SDF grid";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sdf_grid_to_density_cc
