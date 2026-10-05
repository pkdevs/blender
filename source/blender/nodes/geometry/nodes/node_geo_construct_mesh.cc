/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_construct_mesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Positions"_ustr).structure_type(StructureType::List).hide_value();
  b.add_input<decl::Int>("Corner Vertices"_ustr).structure_type(StructureType::List).hide_value();
  b.add_input<decl::Int>("Face Offsets"_ustr).structure_type(StructureType::List).hide_value();

#if 0
  /* TODO: This needs to use `int2` as the data type. */
  b.add_input<decl::IntVector>("Loose Edges"_ustr).structure_type(StructureType::List).hide_value();
#endif

  b.add_output<decl::Geometry>("Mesh"_ustr);
}

static Mesh *create_mesh_from_lists(GeoNodeExecParams &params,
                                    const List<float3> &positions_list,
                                    const ListPtr<int> &corner_vertices_list,
                                    const ListPtr<int> &face_offset_list)
{
  const int verts_num = positions_list.size();
  const IndexRange verts(verts_num);

  const int faces_num = face_offset_list ? face_offset_list->size() - 1 : 0;
  const int corners_num = corner_vertices_list ? corner_vertices_list->size() : 0;

  Mesh *mesh = bke::mesh_new_no_attributes(verts_num, 0, faces_num, corners_num);

  const auto *positions_array_data = std::get_if<nodes::GList::ArrayData>(&positions_list.data());
  if (positions_array_data) {
    mesh->attributes_for_write().add<float3>(
        "position",
        bke::AttrDomain::Point,
        bke::AttributeInitShared(positions_array_data->data, *positions_array_data->sharing_info));
  }
  else if (const auto *positions_single_value = std::get_if<nodes::GList::SingleData>(
               &positions_list.data()))
  {
    mesh->attributes_for_write().add<float3>(
        "position",
        bke::AttrDomain::Point,
        bke::AttributeInitValue(*static_cast<const float3 *>(positions_single_value->value)));
  }
  else {
    BLI_assert_unreachable();
  }

  if (faces_num > 0) {
    const auto *face_offset_data = std::get_if<nodes::GList::ArrayData>(&face_offset_list->data());
    BLI_assert(face_offset_data != nullptr);

    implicit_sharing::copy_shared_pointer(
        static_cast<int *>(const_cast<void *>(face_offset_data->data)),
        &(*face_offset_data->sharing_info),
        &mesh->face_offset_indices,
        &mesh->runtime->face_offsets_sharing_info);

    const Span<int> face_offsets = mesh->face_offsets();
    const bool sizes_greater_zero = threading::parallel_reduce(
        face_offsets.drop_back(1).index_range(),
        4096,
        true,
        [&](const IndexRange range, const bool sizes_greater_zero) {
          if (!sizes_greater_zero) {
            return false;
          }
          for (const int i : range) {
            const int curr = face_offsets[i];
            const int next = face_offsets[i + 1];
            if (next <= curr) {
              return false;
            }
          }
          return true;
        },
        std::logical_and<>());
    if (!sizes_greater_zero) {
      params.error_message_add(NodeWarningType::Error, "Face sizes must be greater than zero");
      BKE_id_free_ex(nullptr, mesh, LIB_ID_FREE_NO_MAIN, false);
      return nullptr;
    }

    if (const auto *corner_vertices_array_data = std::get_if<nodes::GList::ArrayData>(
            &corner_vertices_list->data()))
    {
      mesh->attributes_for_write().add<int>(
          ".corner_vert",
          bke::AttrDomain::Corner,
          bke::AttributeInitShared(corner_vertices_array_data->data,
                                   *corner_vertices_array_data->sharing_info));
    }
    else if (const auto *corner_vertices_single_value = std::get_if<nodes::GList::SingleData>(
                 &corner_vertices_list->data()))
    {
      mesh->attributes_for_write().add<int>(
          ".corner_vert",
          bke::AttrDomain::Corner,
          bke::AttributeInitValue(*static_cast<const int *>(corner_vertices_single_value->value)));
    }
    else {
      BLI_assert_unreachable();
    }

    /* TODO: Error handling. */

    bke::mesh_calc_edges(*mesh, false, false);
  }

  return mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GListPtr positions_list = params.extract_input<GListPtr>("Positions"_ustr);
  if (!positions_list) {
    params.set_default_remaining_outputs();
    return;
  }
  if (positions_list->size() < 1) {
    params.error_message_add(NodeWarningType::Error, "Number of points must be greater than zero");
    params.set_default_remaining_outputs();
    return;
  }
  const GListPtr corner_vertices_list = params.extract_input<GListPtr>("Corner Vertices"_ustr);
  const GListPtr face_offsets_list = params.extract_input<GListPtr>("Face Offsets"_ustr);
  if (corner_vertices_list && !face_offsets_list) {
    params.error_message_add(NodeWarningType::Error, "Number of faces must be greater than zero");
    params.set_default_remaining_outputs();
    return;
  }
  if (face_offsets_list && face_offsets_list->size() < 2) {
    params.error_message_add(NodeWarningType::Error, "Number of face offsets must be at least 2");
    params.set_default_remaining_outputs();
    return;
  }
  if (face_offsets_list && (!corner_vertices_list || corner_vertices_list->size() < 1)) {
    params.error_message_add(NodeWarningType::Error,
                             "Number of corner vertices must be greater than zero");
    params.set_default_remaining_outputs();
    return;
  }
  if (params.output_is_required("Mesh"_ustr)) {
    BLI_assert(positions_list->cpp_type().is<float3>());
    BLI_assert(!corner_vertices_list || corner_vertices_list->cpp_type().is<int>());
    BLI_assert(!face_offsets_list || face_offsets_list->cpp_type().is<int>());
    Mesh *mesh = create_mesh_from_lists(params,
                                        positions_list->typed<float3>(),
                                        corner_vertices_list.typed<int>(),
                                        face_offsets_list.typed<int>());
    params.set_output("Mesh"_ustr, GeometrySet::from_mesh(mesh));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeConstructMesh"_ustr);
  ntype.ui_name = "Construct Mesh";
  ntype.ui_description = "Construct a mesh from raw topology data";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_construct_mesh_cc
