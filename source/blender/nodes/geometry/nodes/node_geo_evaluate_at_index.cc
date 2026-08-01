/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "NOD_socket_search_link.hh"

namespace blender::nodes::node_geo_evaluate_at_index_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Menu>("Domain"_ustr)
      .static_items(rna_enum_attribute_domain_items)
      .default_value(AttrDomain::Point)
      .description("Domain the field is evaluated in")
      .optional_label();
  b.add_input<decl::Menu>("Data Type"_ustr)
      .static_items(rna_enum_attribute_type_items)
      .default_value(CD_PROP_FLOAT)
      .description("Type of data the field evaluates")
      .optional_label();

  const bNode *node = b.node_or_null();
  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node->custom2);
    b.add_input(data_type, "Value"_ustr).hide_value().structure_type(StructureType::Field);
    b.add_output(data_type, "Value"_ustr)
        .structure_type(StructureType::Field)
        .propagate_references()
        .align_with_previous();
  }
  b.add_input<decl::Int>("Index"_ustr).min(0).structure_type(StructureType::Field);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const bke::bNodeType &node_type = params.node_type();
  const std::optional<eCustomDataType> type = bke::socket_type_to_custom_data_type(
      params.other_socket().type);
  if (type && *type != CD_PROP_STRING) {
    params.add_item(IFACE_("Value"), [node_type, type](LinkSearchOpParams &params) {
      bNode &node = params.add_node(node_type);
      node.custom2 = *type;
      params.update_and_connect_available_socket(node, "Value"_ustr);
    });
    if (params.in_out() == SOCK_IN) {
      params.add_item(
          IFACE_("Index"),
          [node_type, type](LinkSearchOpParams &params) {
            bNode &node = params.add_node(node_type);
            node.custom2 = *type;
            params.update_and_connect_available_socket(node, "Index"_ustr);
          },
          -1);
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();
  const AttrDomain domain = AttrDomain(node.custom1);

  GField output_field = GField::from_input<bke::EvaluateAtIndexInput>(
      params.extract_input<Field<int>>("Index"_ustr),
      params.extract_input<GField>("Value"_ustr),
      domain);
  params.set_output<GField>("Value"_ustr, std::move(output_field));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFieldAtIndex"_ustr, GEO_NODE_EVALUATE_AT_INDEX);
  ntype.ui_name = "Evaluate at Index";
  ntype.ui_description = "Retrieve data of other elements in the context's geometry";
  ntype.enum_name_legacy = "FIELD_AT_INDEX";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_searches;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_evaluate_at_index_cc
