/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_render_resolution_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Width"_ustr)
      .description("The final render resolution width in pixels");
  b.add_output<decl::Int>("Height"_ustr)
      .description("The final render resolution height in pixels");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const Scene *scene = DEG_get_input_scene(params.depsgraph());
  int render_width, render_height;
  BKE_render_resolution(&scene->r, false, &render_width, &render_height);
  params.set_output("Width"_ustr, render_width);
  params.set_output("Height"_ustr, render_height);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputRenderResolution"_ustr);
  ntype.ui_name = "Render Resolution";
  ntype.ui_description = "Retrieve the final render resolution of the current scene";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_render_resolution_cc
