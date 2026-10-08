/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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

static bool validate_inputs(GeoNodeExecParams &params,
                            const List<float3> &positions_list,
                            const ListPtr<int> &corner_vertices_ptr,
                            const ListPtr<int> &face_offsets_ptr)
{
  const int verts_num = positions_list.size();
  if (verts_num < 1) {
    params.error_message_add(NodeWarningType::Error, "Number of points must be greater than zero");
    return false;
  }
  if (!corner_vertices_ptr && !face_offsets_ptr) {
    /* Create mesh with only vertices. */
    return true;
  }
  if (corner_vertices_ptr && !face_offsets_ptr) {
    params.error_message_add(NodeWarningType::Error, "Number of faces must be greater than zero");
    return false;
  }
  if (!corner_vertices_ptr && face_offsets_ptr) {
    params.error_message_add(NodeWarningType::Error,
                             "Number of corner vertices must be greater than zero");
    return false;
  }
  const List<int> &corner_vertices_list = *corner_vertices_ptr;
  const List<int> &face_offsets_list = *face_offsets_ptr;
  const int corners_num = corner_vertices_list.size();
  const int faces_num = face_offsets_list.size() - 1;
  if (corners_num < 1) {
    params.error_message_add(NodeWarningType::Error,
                             "Number of corner vertices must be greater than zero");
    return false;
  }
  if (faces_num < 1) {
    params.error_message_add(NodeWarningType::Error, "Number of face offsets must be at least 2");
    return false;
  }
  std::variant<Span<int>, const int *> face_offsets_values = face_offsets_list.values();
  if (std::holds_alternative<const int *>(face_offsets_values)) {
    params.error_message_add(NodeWarningType::Error, "Face offsets must contain different values");
    return false;
  }
  const Span<int> face_offsets = std::get<Span<int>>(face_offsets_values);
  if (face_offsets.first() != 0) {
    params.error_message_add(NodeWarningType::Error, "First face offset must be zero");
    return false;
  }
  if (face_offsets.last() != corners_num) {
    params.error_message_add(
        NodeWarningType::Error,
        "Total number of face vertices must be equal to number of corner vertices");
    return false;
  }
  const bool face_offsets_are_valid = threading::parallel_reduce(
      face_offsets.drop_back(1).index_range(),
      4096,
      true,
      [&](const IndexRange range, const bool face_offsets_are_valid) {
        if (!face_offsets_are_valid) {
          return false;
        }
        for (const int i : range) {
          const int curr = face_offsets[i];
          const int next = face_offsets[i + 1];
          if (next <= curr) {
            return false;
          }
          if (next - curr < 3) {
            return false;
          }
        }
        return true;
      },
      std::logical_and<>());
  if (!face_offsets_are_valid) {
    params.error_message_add(NodeWarningType::Error, "Face sizes must be greater than 2");
    return false;
  }
  std::variant<Span<int>, const int *> corner_vertices_values = corner_vertices_list.values();
  if (std::holds_alternative<const int *>(corner_vertices_values)) {
    params.error_message_add(NodeWarningType::Error,
                             "Corner vertices must contain different values");
    return false;
  }
  const IndexRange verts_range(verts_num);
  const OffsetIndices<int> faces(face_offsets, offset_indices::NoSortCheck());
  const Span<int> corner_verts = std::get<Span<int>>(corner_vertices_values);
  const bool corner_vertices_are_valid = threading::parallel_reduce(
      faces.index_range(),
      4096,
      true,
      [&](const IndexRange range, const bool corner_vertices_are_valid) {
        if (!corner_vertices_are_valid) {
          return false;
        }
        for (const int face_i : range) {
          const IndexRange face = faces[face_i];
          for (const int vert : corner_verts.slice(face)) {
            if (!verts_range.contains(vert)) {
              return false;
            }
          }
        }
        return true;
      },
      std::logical_and<>());
  if (!corner_vertices_are_valid) {
    params.error_message_add(NodeWarningType::Error,
                             "Corner vertex indices must be within the number of positions");
    return false;
  }
  const bool corner_vertices_are_unique = threading::parallel_reduce(
      faces.index_range(),
      4096,
      true,
      [&](const IndexRange range, const bool corner_vertices_are_unique) {
        if (!corner_vertices_are_unique) {
          return false;
        }
        for (const int face_i : range) {
          Set<int, 16> set;
          const IndexRange face = faces[face_i];
          for (const int vert : corner_verts.slice(face)) {
            if (!set.add(vert)) {
              return false;
            }
          }
        }
        return true;
      },
      std::logical_and<>());
  if (!corner_vertices_are_unique) {
    params.error_message_add(NodeWarningType::Error,
                             "Corner vertex indices must not repeat within a face");
    return false;
  }
  return true;
}

static Mesh *create_mesh_from_lists(const List<float3> &positions_list,
                                    const ListPtr<int> &corner_vertices_list,
                                    const ListPtr<int> &face_offsets_list)
{
  const int verts_num = positions_list.size();
  const IndexRange verts(verts_num);

  const int faces_num = face_offsets_list ? face_offsets_list->size() - 1 : 0;
  const int corners_num = corner_vertices_list ? corner_vertices_list->size() : 0;

  Mesh *mesh = bke::mesh_new_no_attributes(verts_num, 0, faces_num, corners_num);

  if (const auto *positions_array_data = std::get_if<nodes::GList::ArrayData>(
          &positions_list.data()))
  {
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
    mesh->attributes_for_write().add<float3>(
        "position", bke::AttrDomain::Point, bke::AttributeInitVArray(positions_list.varray()));
  }

  if (faces_num == 0) {
    /* Only positions are needed. */
    return mesh;
  }

  /* If there are faces, we expect there to be corner vertices. */
  BLI_assert(corner_vertices_list);
  const List<int> &corner_vertices = *corner_vertices_list;
  const List<int> &face_offsets = *face_offsets_list;

  const auto *face_offset_data = std::get_if<nodes::GList::ArrayData>(&face_offsets.data());
  BLI_assert(face_offset_data != nullptr);

  implicit_sharing::copy_shared_pointer(
      static_cast<int *>(const_cast<void *>(face_offset_data->data)),
      &(*face_offset_data->sharing_info),
      &mesh->face_offset_indices,
      &mesh->runtime->face_offsets_sharing_info);

  if (const auto *corner_vertices_array_data = std::get_if<nodes::GList::ArrayData>(
          &corner_vertices.data()))
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
    mesh->attributes_for_write().add<int>(".corner_vert",
                                          bke::AttrDomain::Corner,
                                          bke::AttributeInitVArray(corner_vertices.varray()));
  }

  bke::mesh_calc_edges(*mesh, false, false);

  return mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GListPtr positions_list = params.extract_input<GListPtr>("Positions"_ustr);
  if (!positions_list) {
    params.set_default_remaining_outputs();
    return;
  }
  if (params.output_is_required("Mesh"_ustr)) {
    const List<float3> &positions = positions_list->typed<float3>();
    const ListPtr<int> &corner_vertices = corner_vertices_list.typed<int>();
    const ListPtr<int> &face_offsets = face_offsets_list.typed<int>();
    if (!validate_inputs(params, positions, corner_vertices, face_offsets)) {
      params.set_default_remaining_outputs();
      return;
    }
    Mesh *mesh = create_mesh_from_lists(positions_list->typed<float3>(),
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
