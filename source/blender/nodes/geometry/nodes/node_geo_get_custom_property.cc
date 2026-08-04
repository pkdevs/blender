/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_socket_search_link.hh"

#include "BKE_idprop.hh"
#include "BLI_math_vector.hh"

#include "UI_resources.hh"

namespace blender::nodes::node_geo_get_custom_property {

static const EnumPropertyItem type_items[] = {
    {SOCK_FLOAT, "FLOAT", ICON_NODE_SOCKET_FLOAT, N_("Float"), N_("Floating-point value")},
    {SOCK_BOOLEAN, "BOOLEAN", ICON_NODE_SOCKET_BOOLEAN, N_("Boolean"), N_("Boolean value")},
    {SOCK_INT, "INTEGER", ICON_NODE_SOCKET_INT, N_("Integer"), N_("Integer value")},
    {SOCK_STRING, "STRING", ICON_NODE_SOCKET_STRING, N_("String"), N_("String value")},
    {SOCK_VECTOR, "VECTOR", ICON_NODE_SOCKET_VECTOR, N_("Vector"), N_("Vector value")},
    {SOCK_RGBA, "COLOR", ICON_NODE_SOCKET_RGBA, N_("Color"), N_("Color value")},
    {SOCK_ROTATION, "ROTATION", ICON_NODE_SOCKET_ROTATION, N_("Rotation"), N_("Rotation value")},
    {SOCK_MATRIX, "MATRIX", ICON_NODE_SOCKET_MATRIX, N_("Matrix"), N_("Matrix value")},
    {0, nullptr, 0, nullptr, nullptr},
};

#define ADD_TYPED_OUTPUT(decl_type, id, idp_type) \
  b.add_output<decl_type>("Value"_ustr, id) \
      .make_available([](bNode &node) { \
        bNodeSocket &type_socket = *bke::node_find_socket(node, SOCK_IN, "Type"_ustr); \
        type_socket.default_value_typed<bNodeSocketValueMenu>()->value = idp_type; \
      }) \
      .usage_by_single_menu(idp_type) \
      .optional_label();

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Menu>("Type"_ustr)
      .default_value(SOCK_FLOAT)
      .static_items(type_items)
      .optional_label();

  ADD_TYPED_OUTPUT(decl::Bool, "ValueBool"_ustr, SOCK_BOOLEAN)
  ADD_TYPED_OUTPUT(decl::Float, "ValueFloat"_ustr, SOCK_FLOAT)
  ADD_TYPED_OUTPUT(decl::Int, "ValueInt"_ustr, SOCK_INT)
  ADD_TYPED_OUTPUT(decl::String, "ValueString"_ustr, SOCK_STRING)
  ADD_TYPED_OUTPUT(decl::Vector, "ValueVector"_ustr, SOCK_VECTOR)
  ADD_TYPED_OUTPUT(decl::Rotation, "ValueRotation"_ustr, SOCK_ROTATION)
  ADD_TYPED_OUTPUT(decl::Color, "ValueColor"_ustr, SOCK_RGBA)
  ADD_TYPED_OUTPUT(decl::Matrix, "ValueMatrix"_ustr, SOCK_MATRIX)

  b.add_output<decl::Bool>("Exists"_ustr).default_value(false);

  b.add_input<decl::DataBlockID>("ID"_ustr).description("ID to get the custom property of");
  b.add_input<decl::String>("Name"_ustr).description("Name of the custom property");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  ID *id = params.extract_input<ID *>("ID"_ustr);

  if (id == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  const std::string name = params.extract_input<std::string>("Name"_ustr);
  const eNodeSocketDatatype type = params.extract_input<eNodeSocketDatatype>("Type"_ustr);

  // TODO: IDP have groups.
  // The user properties are in a top-level group called `user_properties`.
  // We need to be able to recurse into the groups, however we can't rely on the names to make an
  // addressing scheme using `.` as a separator, for example.
  // We could add yet another socket type for this but that's probably overkill.
  // Groups can be recursive.
  // For now, just get top-level custom props.
  IDProperty *group = IDP_GetProperties(id);
  if (group == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  IDProperty *prop = IDP_GetPropertyFromGroup(group, name);
  if (prop == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output("Exists"_ustr, true);

  // TODO: i think we need to forcibly disconnect outputs when the type changes.

  switch (type) {
    case SOCK_STRING:
      params.set_output("ValueString"_ustr, IDP_coerce_to_string_or_empty(prop));
      break;
    case SOCK_INT:
      params.set_output("ValueInt"_ustr, IDP_coerce_to_int_or_zero(prop));
      break;
    case SOCK_FLOAT:
      params.set_output("ValueFloat"_ustr, IDP_coerce_to_float_or_zero(prop));
      break;
    case SOCK_BOOLEAN:
      params.set_output("ValueBool"_ustr, IDP_coerce_to_bool_or_false(prop));
      break;
    case SOCK_RGBA:
      params.set_output("ValueColor"_ustr,
                        blender::ColorGeometry4f(IDP_coerce_to_float4_or_zero(prop)));
      break;
    case SOCK_VECTOR:
      params.set_output("ValueVector"_ustr, IDP_coerce_to_float3_or_zero(prop));
      break;
    case SOCK_ROTATION:
      params.set_output("ValueRotation"_ustr,
                        math::Quaternion(IDP_coerce_to_float4_or_zero(prop)));
      break;
    case SOCK_MATRIX:
      params.set_output("ValueMatrix"_ustr, IDP_coerce_to_float4x4_or_identity(prop));
      break;
    default:
      // BLI_assert_unreachable();
      break;
  }

  params.set_default_remaining_outputs();
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGetCustomProperty"_ustr);
  ntype.ui_name = "Get Custom Property";
  ntype.ui_description = "Get the custom property of an ID";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.default_width = bke::NodeWidth::_160;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_custom_property
