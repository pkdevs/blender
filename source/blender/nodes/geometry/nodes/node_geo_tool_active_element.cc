/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "RNA_enum_types.hh"

#include "BKE_node.hh"

#include "NOD_rna_define.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_tool_active_element_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  static const EnumPropertyItem domain_items[] = {
      {int(AttrDomain::Point), "POINT", 0, "Point", ""},
      {int(AttrDomain::Edge), "EDGE", 0, "Edge", ""},
      {int(AttrDomain::Face), "FACE", 0, "Face", ""},
      {int(AttrDomain::Layer), "LAYER", 0, "Layer", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  b.add_input<decl::Menu>("Domain"_ustr)
      .static_items(domain_items)
      .default_value(AttrDomain::Point)
      .description("Domain of the active element")
      .optional_label();
  b.add_output<decl::Int>("Index"_ustr)
      .description("Index of the active element in the specified domain");
  b.add_output<decl::Bool>("Exists"_ustr)
      .description("True if an active element exists in the mesh, false otherwise");
}

static void node_exec(GeoNodeExecParams params)
{
  if (!check_tool_context_and_error(params)) {
    return;
  }

  const GeoNodesOperatorData *operator_data = params.user_data()->call_data->operator_data;
  const AttrDomain domain = params.extract_input<AttrDomain>("Domain"_ustr);

  /* Active Point, Edge, and Face are only supported in Edit Mode. */
  if (operator_data->mode != OB_MODE_EDIT &&
      ELEM(domain, AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face))
  {
    params.set_default_remaining_outputs();
    return;
  }

  switch (domain) {
    case AttrDomain::Point:
      params.set_output("Exists"_ustr, operator_data->active_point_index >= 0);
      params.set_output("Index"_ustr, std::max(0, operator_data->active_point_index));
      break;
    case AttrDomain::Edge:
      params.set_output("Exists"_ustr, operator_data->active_edge_index >= 0);
      params.set_output("Index"_ustr, std::max(0, operator_data->active_edge_index));
      break;
    case AttrDomain::Face:
      params.set_output("Exists"_ustr, operator_data->active_face_index >= 0);
      params.set_output("Index"_ustr, std::max(0, operator_data->active_face_index));
      break;
    case AttrDomain::Layer:
      params.set_output("Exists"_ustr, operator_data->active_layer_index >= 0);
      params.set_output("Index"_ustr, std::max(0, operator_data->active_layer_index));
      break;
    default:
      params.set_default_remaining_outputs();
      BLI_assert_unreachable();
      break;
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeToolActiveElement"_ustr, GEO_NODE_TOOL_ACTIVE_ELEMENT);
  ntype.ui_name = "Active Element";
  ntype.ui_description = "Active element indices of the edited geometry, for tool execution";
  ntype.enum_name_legacy = "TOOL_ACTIVE_ELEMENT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_exec;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = search_link_ops_for_tool_node;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_tool_active_element_cc
