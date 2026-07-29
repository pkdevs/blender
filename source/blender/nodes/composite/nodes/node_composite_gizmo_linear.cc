/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_gizmo_linear_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Value"_ustr).dimensions(3).hide_value();
  // b.add_input<decl::Float>("Value"_ustr).hide_value();
}

static void node_register()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeGizmoLinear"_ustr, CMP_NODE_GIZMO_LINEAR);
  ntype.ui_name = "Linear Gizmo";
  ntype.ui_description = "Show a linear gizmo in the image editor for a value";
  ntype.enum_name_legacy = "GIZMO_LINEAR";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_gizmo_linear_cc
