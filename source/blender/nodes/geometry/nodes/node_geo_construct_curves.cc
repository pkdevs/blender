/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_offset_indices.hh"

#include "BKE_curves.hh"
#include "BKE_lib_id.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_geometry_nodes_values.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_usage_inference.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_construct_curves_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Curves"_ustr);

  b.add_input<decl::Vector>("Positions"_ustr)
      .structure_type(StructureType::List)
      .hide_value()
      .description("A list of the point positions in all curves");
  b.add_input<decl::Int>("Curve Offsets"_ustr)
      .structure_type(StructureType::List)
      .hide_value()
      .description(
          "A list with a size one larger than the number of curves storing the first point index "
          "of each curve and a final value for the total number of points. This is a prefix sum "
          "of the curve sizes");
}

static Curves *create_curves_from_topology_info(GeoNodeExecParams &params,
                                                const List<float3> &positions_list,
                                                const List<int> &curve_offsets_list)
{
  const int points_num = positions_list.size();
  /* Curve offsets are shared with the list data. So don't create the offsets array here. */
  Curves *curves_id = bke::curves_new_nomain(bke::curves_new_no_attributes(points_num, 0));
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  const auto *positions_array_data = std::get_if<nodes::GList::ArrayData>(&positions_list.data());
  if (positions_array_data) {
    /* Share the position data with the list to avoid copying it. */
    curves.attributes_for_write().add<float3>(
        "position",
        bke::AttrDomain::Point,
        bke::AttributeInitShared(positions_array_data->data, *positions_array_data->sharing_info));
  }
  else if (const auto *positions_single_value = std::get_if<nodes::GList::SingleData>(
               &positions_list.data()))
  {
    curves.attributes_for_write().add<float3>(
        "position",
        bke::AttrDomain::Point,
        bke::AttributeInitValue(*static_cast<const float3 *>(positions_single_value->value)));
  }
  else {
    BLI_assert_unreachable();
  }

  const auto *curve_offset_data = std::get_if<nodes::GList::ArrayData>(&curve_offsets_list.data());
  BLI_assert(curve_offset_data != nullptr);

  const int curves_num = curve_offsets_list.size() - 1;
  curves.attribute_storage.wrap().resize(AttrDomain::Curve, curves_num);
  /* Share the offset data with the list to avoid copying it. */
  implicit_sharing::copy_shared_pointer(
      static_cast<int *>(const_cast<void *>(curve_offset_data->data)),
      &(*curve_offset_data->sharing_info),
      &curves.curve_offsets,
      &curves.runtime->curve_offsets_sharing_info);
  curves.curve_num = curves_num;

  const Span<int> curve_offsets = curves.offsets();
  if (curve_offsets.first() != 0) {
    params.error_message_add(NodeWarningType::Error, "First curve offset must be 0");
    BKE_id_free_ex(nullptr, curves_id, LIB_ID_FREE_NO_MAIN, false);
    return nullptr;
  }
  if (curve_offsets.last() != points_num) {
    params.error_message_add(NodeWarningType::Error,
                             "Curve sizes must sum to the number of points");
    BKE_id_free_ex(nullptr, curves_id, LIB_ID_FREE_NO_MAIN, false);
    return nullptr;
  }

  const bool sizes_greater_zero = threading::parallel_reduce(
      curve_offsets.drop_back(1).index_range(),
      4096,
      true,
      [&](const IndexRange range, const bool sizes_greater_zero) {
        if (!sizes_greater_zero) {
          return false;
        }
        for (const int i : range) {
          const int curr = curve_offsets[i];
          const int next = curve_offsets[i + 1];
          if (next <= curr) {
            return false;
          }
        }
        return true;
      },
      std::logical_and<>());
  if (!sizes_greater_zero) {
    params.error_message_add(NodeWarningType::Error, "Curve sizes must be greater than zero");
    BKE_id_free_ex(nullptr, curves_id, LIB_ID_FREE_NO_MAIN, false);
    return nullptr;
  }

  curves.fill_curve_types(CURVE_TYPE_POLY);

  return curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GListPtr positions_list = params.extract_input<GListPtr>("Positions"_ustr);
  const GListPtr curve_offsets_list = params.extract_input<GListPtr>("Curve Offsets"_ustr);
  if (!positions_list) {
    /* Empty Curves. */
    params.set_default_remaining_outputs();
    return;
  }
  if (positions_list->size() < 1) {
    params.error_message_add(NodeWarningType::Error, "Number of points must be greater than zero");
    params.set_default_remaining_outputs();
    return;
  }
  if (!curve_offsets_list || curve_offsets_list->size() < 2) {
    params.error_message_add(NodeWarningType::Error, "Number of curve offsets must be at least 2");
    params.set_default_remaining_outputs();
    return;
  }
  if (positions_list->size() < curve_offsets_list->size() - 1) {
    params.error_message_add(
        NodeWarningType::Error,
        "Number of curves must be less than or equal to the number of points");
    params.set_default_remaining_outputs();
    return;
  }
  if (params.output_is_required("Curves"_ustr)) {
    BLI_assert(positions_list->cpp_type().is<float3>());
    BLI_assert(curve_offsets_list->cpp_type().is<int>());
    Curves *curves = create_curves_from_topology_info(
        params, positions_list->typed<float3>(), curve_offsets_list->typed<int>());
    params.set_output("Curves"_ustr, GeometrySet::from_curves(curves));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeConstructCurves"_ustr);
  ntype.ui_name = "Construct Curves";
  ntype.ui_description = "Construct curves from raw topology data";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_construct_curves_cc
