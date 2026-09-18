/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */

#include "BLI_math_matrix.hh"
#include "BLI_sys_types.hh"

#include "BLT_translation.hh"

#include "BKE_attribute.h"
#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.hh"

#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"

#include "GEO_join_geometries.hh"
#include "GEO_mesh_from_bmesh_operator.hh"
#include "GEO_mesh_primitive_cuboid.hh"
#include "GEO_mesh_primitive_cylinder_cone.hh"
#include "GEO_mesh_primitive_grid.hh"
#include "GEO_mesh_primitive_ico_sphere.hh"
#include "GEO_mesh_primitive_uv_sphere.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "mesh_intern.hh" /* own include */

namespace blender {

#define MESH_ADD_VERTS_MAXI 10000000

/* ********* add primitive operators ************* */

static void mesh_select_all(Mesh &mesh)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  for (const auto &[name, domain] : {std::pair(".select_vert", bke::AttrDomain::Point),
                                     std::pair(".select_edge", bke::AttrDomain::Edge),
                                     std::pair(".select_poly", bke::AttrDomain::Face)})
  {
    bke::SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_only_span<bool>(
        name, domain);
    selection.span.fill(true);
    selection.finish();
  }
}

static void primitive_face_set_init(const Mesh *object_mesh, Mesh *primitive_mesh)
{
  bke::AttributeAccessor object_attributes = object_mesh->attributes();
  bke::AttributeReader<int> object_face_sets = object_attributes.lookup<int>(".sculpt_face_set");
  if (!object_face_sets) {
    return;
  }

  bke::MutableAttributeAccessor primitive_attributes = primitive_mesh->attributes_for_write();
  bke::SpanAttributeWriter<int> primitive_face_sets =
      primitive_attributes.lookup_or_add_for_write_span<int>(".sculpt_face_set",
                                                             bke::AttrDomain::Face);

  primitive_face_sets.span.fill(ed::sculpt_paint::face_set::find_next_available_id(*object_mesh));
  primitive_face_sets.finish();
}

/**
 * The UV map primitives write into, empty when the mesh has none.
 */
static StringRefNull mesh_uv_map_name_for_primitive(const Mesh &mesh)
{
  const StringRefNull name = mesh.active_or_default_uv_map_name();
  if (BKE_editmesh_bmesh_get(&mesh)) {
    /* The name is read from the BMesh layers so it's never stale,
     * unlike #Mesh::uv_map_names which doesn't include layers added in edit-mode. */
    return name;
  }
  const VectorSet<StringRefNull> uv_map_names = mesh.uv_map_names();
  if (uv_map_names.contains(name)) {
    return name;
  }
  /* A mesh can have UV maps with no active map set,
   * the active name may also be stale (referencing a removed UV map). */
  return uv_map_names.is_empty() ? StringRefNull() : uv_map_names[0];
}

static void primitive_uv_map_rename_to_object(Mesh &object_mesh, Mesh &primitive_mesh)
{
  const VectorSet<StringRefNull> uv_map_names = primitive_mesh.uv_map_names();
  if (uv_map_names.is_empty()) {
    return;
  }
  BLI_assert(uv_map_names.size() == 1);
  const std::string primitive_name = uv_map_names[0];

  ED_mesh_uv_ensure(&object_mesh, nullptr);
  const std::string object_name = mesh_uv_map_name_for_primitive(object_mesh);
  BLI_assert(!object_name.empty());

  if (object_name != primitive_name) {
    /* Use the ID level rename so any UV pin/select sub-layers are renamed too. */
    AttributeOwner owner = AttributeOwner::from_id(&primitive_mesh.id);
    BKE_attribute_rename(owner, primitive_name, object_name, nullptr);
  }
}

static void mesh_join_sculpt(Object &ob, Mesh *primitive_mesh)
{
  BLI_assert(ob.type == OB_MESH);
  BLI_assert(primitive_mesh->id.tag & ID_TAG_NO_MAIN);
  Mesh *object_mesh = id_cast<Mesh *>(ob.data);

  /* Only existing UV maps are written to, see #add_primitive_uv_map. */
  BLI_assert(primitive_mesh->uv_map_names().is_empty() ||
             object_mesh->uv_map_names().contains(primitive_mesh->uv_map_names()[0]));
  primitive_face_set_init(object_mesh, primitive_mesh);
  /* Match the object's material slots, otherwise joining adds a new slot. */
  for (const int i : IndexRange(object_mesh->totcol)) {
    BKE_id_material_eval_assign(&primitive_mesh->id, i + 1, object_mesh->mat[i]);
  }

  bke::GeometrySet joined = geometry::join_geometries(
      {bke::GeometrySet::from_mesh(object_mesh, bke::GeometryOwnershipType::ReadOnly),
       bke::GeometrySet::from_mesh(primitive_mesh, bke::GeometryOwnershipType::ReadOnly)},
      {});

  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();

  BKE_id_free(nullptr, primitive_mesh);
  BKE_mesh_nomain_to_mesh(result, object_mesh, &ob);

  BKE_sculptsession_free_pbvh(ob);
}

static void mesh_join_editmesh(Object &ob, Mesh *primitive_mesh)
{
  BLI_assert(ob.type == OB_MESH);
  BLI_assert(primitive_mesh->id.tag & ID_TAG_NO_MAIN);
  Mesh *object_mesh = id_cast<Mesh *>(ob.data);
  BMesh *bm = BKE_editmesh_bmesh_get_for_write(object_mesh);
  BLI_assert(bm != nullptr);

  primitive_uv_map_rename_to_object(*object_mesh, *primitive_mesh);
  /* Merging applies the primitive's active & default UV maps to the BMesh, keep the object's. */
  primitive_mesh->uv_maps_active_set(object_mesh->active_uv_map_name());
  primitive_mesh->uv_maps_default_set(object_mesh->default_uv_map_name());
  /* Merging also applies the primitive's active face (0 by default), keep the object's. */
  primitive_mesh->act_face = -1;
  /* Selection is applied when merging into the BMesh, so the new geometry ends up selected. */
  mesh_select_all(*primitive_mesh);

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = true;
  convert_params.calc_vert_normal = true;
  BM_mesh_bm_from_me(bm, primitive_mesh, &convert_params);
  BKE_id_free(nullptr, primitive_mesh);

  EDBM_selectmode_flush_ex(bm, SCE_SELECT_VERTEX);
  /* TODO(@ideasman42): maintain UV sync for newly created data. */
  EDBM_uvselect_clear(bm);

  EDBMUpdate_Params params{};
  params.calc_looptris = true;
  params.calc_normals = false;
  params.is_destructive = true;
  EDBM_update(object_mesh, &params);
}

static void mesh_assign(Object &ob, Mesh *primitive_mesh)
{
  BLI_assert(ob.type == OB_MESH);
  BLI_assert(primitive_mesh->id.tag & ID_TAG_NO_MAIN);
  Mesh *object_mesh = id_cast<Mesh *>(ob.data);

  /* Primitives are added selected, as expected when entering edit-mode. */
  mesh_select_all(*primitive_mesh);
  bke::mesh_ensure_active_uv_map(*primitive_mesh);
  bke::mesh_ensure_default_uv_map(*primitive_mesh);
  BKE_mesh_nomain_to_mesh(primitive_mesh, object_mesh, &ob);
}

static std::optional<std::string> add_primitive_uv_map(const bContext *C, const wmOperator *op)
{
  BLI_assert(RNA_struct_find_property(op->ptr, "calc_uvs"));
  if (!RNA_boolean_get(op->ptr, "calc_uvs")) {
    return std::nullopt;
  }
  if (CTX_data_mode_enum(C) == CTX_MODE_SCULPT) {
    /* Only write into an existing UV map so meshes without one don't gain a UV map
     * from adding a primitive, matching the behavior before sculpt mode supported UV's. */
    const Mesh &mesh = *id_cast<const Mesh *>(CTX_data_active_object(C)->data);
    const StringRefNull name = mesh_uv_map_name_for_primitive(mesh);
    if (name.is_empty()) {
      return std::nullopt;
    }
    return std::string(name);
  }
  /* Matches the default from #ED_mesh_uv_add. */
  return std::string(DATA_("UVMap"));
}

/**
 * Add `primitive_mesh` to the active object (in sculpt & edit-mode) or to a new object.
 *
 * \param primitive_mesh: Ownership is taken, the mesh is always freed.
 */
static wmOperatorStatus add_primitive_from_mesh(bContext *C,
                                                wmOperator *op,
                                                const char *name,
                                                const char view_align_axis,
                                                Mesh *primitive_mesh)
{
  BLI_assert(primitive_mesh != nullptr);
  BLI_assert(primitive_mesh->uv_map_names().size() == (add_primitive_uv_map(C, op) ? 1 : 0));

  float loc[3], rot[3], scale[3];
  bool enter_editmode;
  ushort local_view_bits;
  ed::object::add_generic_get_opts(
      C, op, view_align_axis, loc, rot, scale, &enter_editmode, &local_view_bits, nullptr);

  const eContextObjectMode mode = CTX_data_mode_enum(C);
  Object *ob;
  if (mode == CTX_MODE_EDIT_MESH) {
    /* The mode is derived from the edit-object which may differ from the active object. */
    ob = CTX_data_edit_object(C);
  }
  else if (mode == CTX_MODE_SCULPT) {
    ob = CTX_data_active_object(C);
  }
  else {
    /* Permit adding objects in a variety of modes, even those which are not typically associated
     * with mesh-editing actions (e.g. Vertex & Texture Paint). This has been the case since 2.7x
     * and as a result, the operators are often used in user created scripts. */
    ob = ed::object::add_type(C, OB_MESH, name, loc, rot, false, local_view_bits);
  }

  float4x4 matrix;
  ed::object::new_primitive_matrix(C, ob, loc, rot, scale, matrix.ptr());
  bke::mesh_transform(*primitive_mesh, matrix, false);

  switch (mode) {
    case CTX_MODE_SCULPT:
      ed::sculpt_paint::undo::geometry_begin(*CTX_data_scene(C), *ob, op);
      mesh_join_sculpt(*ob, primitive_mesh);
      ed::sculpt_paint::undo::geometry_end(*ob);
      break;
    case CTX_MODE_EDIT_MESH:
      mesh_join_editmesh(*ob, primitive_mesh);
      break;
    default:
      mesh_assign(*ob, primitive_mesh);
      if (enter_editmode) {
        ed::object::editmode_enter_ex(CTX_data_main(C), CTX_data_scene(C), ob, 0);
      }
      break;
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus add_primitive_plane_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  const float size = RNA_float_get(op->ptr, "size");
  Mesh *primitive_mesh = geometry::create_grid_mesh(2, 2, size, size, add_primitive_uv_map(C, op));
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Plane"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_plane_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Plane";
  ot->description = "Construct a filled planar mesh with 4 vertices";
  ot->idname = "MESH_OT_primitive_plane_add";

  /* API callbacks. */
  ot->exec = add_primitive_plane_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ed::object::add_unit_props_size(ot);
  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus add_primitive_cube_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  const float size = RNA_float_get(op->ptr, "size");
  Mesh *primitive_mesh = geometry::create_cuboid_mesh(
      float3(size), 2, 2, 2, add_primitive_uv_map(C, op));
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Cube"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_cube_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Cube";
  ot->description = "Construct a cube mesh that consists of six square faces";
  ot->idname = "MESH_OT_primitive_cube_add";

  /* API callbacks. */
  ot->exec = add_primitive_cube_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ed::object::add_unit_props_size(ot);
  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static const EnumPropertyItem fill_type_items[] = {
    {0, "NOTHING", 0, "Nothing", "Don't fill at all"},
    {1, "NGON", 0, "N-Gon", "Use n-gons"},
    {2, "TRIFAN", 0, "Triangle Fan", "Use triangle fans"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus add_primitive_circle_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  const int fill_type = RNA_enum_get(op->ptr, "fill_type");
  Mesh *primitive_mesh = geometry::create_mesh_from_bmesh_operator(
      add_primitive_uv_map(C, op),
      float4x4::identity(),
      "create_circle segments=%i radius=%f cap_ends=%b cap_tris=%b",
      RNA_int_get(op->ptr, "vertices"),
      RNA_float_get(op->ptr, "radius"),
      fill_type != 0,
      fill_type == 2);
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Circle"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_circle_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Circle";
  ot->description = "Construct a circle mesh";
  ot->idname = "MESH_OT_primitive_circle_add";

  /* API callbacks. */
  ot->exec = add_primitive_circle_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_int(ot->srna, "vertices", 32, 3, MESH_ADD_VERTS_MAXI, "Vertices", "", 3, 500);
  ed::object::add_unit_props_radius(ot);
  RNA_def_enum(ot->srna, "fill_type", fill_type_items, 0, "Fill Type", "");

  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus add_primitive_cylinder_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  const float radius = RNA_float_get(op->ptr, "radius");
  const int side_segments = 1;
  const int fill_segments = 1;
  geometry::ConeAttributeOutputs attributes;
  attributes.uv_map_id = add_primitive_uv_map(C, op);
  Mesh *primitive_mesh = geometry::create_cylinder_or_cone_mesh(
      radius,
      radius,
      RNA_float_get(op->ptr, "depth"),
      RNA_int_get(op->ptr, "vertices"),
      side_segments,
      fill_segments,
      geometry::ConeFillType(RNA_enum_get(op->ptr, "end_fill_type")),
      attributes);
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Cylinder"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_cylinder_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Cylinder";
  ot->description = "Construct a cylinder mesh";
  ot->idname = "MESH_OT_primitive_cylinder_add";

  /* API callbacks. */
  ot->exec = add_primitive_cylinder_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_int(ot->srna, "vertices", 32, 3, MESH_ADD_VERTS_MAXI, "Vertices", "", 3, 500);
  ed::object::add_unit_props_radius(ot);
  RNA_def_float_distance(
      ot->srna, "depth", 2.0f, 0.0, OBJECT_ADD_SIZE_MAXF, "Depth", "", 0.001, 100.00);
  RNA_def_enum(ot->srna, "end_fill_type", fill_type_items, 1, "Cap Fill Type", "");

  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus add_primitive_cone_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  const int side_segments = 1;
  const int fill_segments = 1;
  geometry::ConeAttributeOutputs attributes;
  attributes.uv_map_id = add_primitive_uv_map(C, op);
  /* The operator's "radius1" is the bottom radius,
   * `create_cylinder_or_cone_mesh` takes the top radius first. */
  Mesh *primitive_mesh = geometry::create_cylinder_or_cone_mesh(
      RNA_float_get(op->ptr, "radius2"),
      RNA_float_get(op->ptr, "radius1"),
      RNA_float_get(op->ptr, "depth"),
      RNA_int_get(op->ptr, "vertices"),
      side_segments,
      fill_segments,
      geometry::ConeFillType(RNA_enum_get(op->ptr, "end_fill_type")),
      attributes);
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Cone"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_cone_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Cone";
  ot->description = "Construct a conic mesh";
  ot->idname = "MESH_OT_primitive_cone_add";

  /* API callbacks. */
  ot->exec = add_primitive_cone_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_int(ot->srna, "vertices", 32, 3, MESH_ADD_VERTS_MAXI, "Vertices", "", 3, 500);
  RNA_def_float_distance(
      ot->srna, "radius1", 1.0f, 0.0, OBJECT_ADD_SIZE_MAXF, "Radius 1", "", 0.001, 100.00);
  RNA_def_float_distance(
      ot->srna, "radius2", 0.0f, 0.0, OBJECT_ADD_SIZE_MAXF, "Radius 2", "", 0.0, 100.00);
  RNA_def_float_distance(
      ot->srna, "depth", 2.0f, 0.0, OBJECT_ADD_SIZE_MAXF, "Depth", "", 0.001, 100.00);
  RNA_def_enum(ot->srna, "end_fill_type", fill_type_items, 1, "Base Fill Type", "");

  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus add_primitive_grid_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  const float size = RNA_float_get(op->ptr, "size");
  Mesh *primitive_mesh = geometry::create_grid_mesh(RNA_int_get(op->ptr, "x_subdivisions") + 1,
                                                    RNA_int_get(op->ptr, "y_subdivisions") + 1,
                                                    size,
                                                    size,
                                                    add_primitive_uv_map(C, op));
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Grid"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_grid_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Grid";
  ot->description = "Construct a subdivided plane mesh";
  ot->idname = "MESH_OT_primitive_grid_add";

  /* API callbacks. */
  ot->exec = add_primitive_grid_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  /* Note that if you use MESH_ADD_VERTS_MAXI for both x and y at the same time
   * you will still reach impossible values (10^12 vertices or so...). */
  RNA_def_int(
      ot->srna, "x_subdivisions", 10, 1, MESH_ADD_VERTS_MAXI, "X Subdivisions", "", 1, 1000);
  RNA_def_int(
      ot->srna, "y_subdivisions", 10, 1, MESH_ADD_VERTS_MAXI, "Y Subdivisions", "", 1, 1000);

  ed::object::add_unit_props_size(ot);
  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus add_primitive_monkey_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  const float dia = RNA_float_get(op->ptr, "size") / 2.0f;
  Mesh *primitive_mesh = geometry::create_mesh_from_bmesh_operator(
      add_primitive_uv_map(C, op), math::from_scale<float4x4>(float3(dia)), "create_monkey");
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Suzanne"), 'Y', primitive_mesh);
}

void MESH_OT_primitive_monkey_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Monkey";
  ot->description = "Construct a Suzanne mesh";
  ot->idname = "MESH_OT_primitive_monkey_add";

  /* API callbacks. */
  ot->exec = add_primitive_monkey_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  ed::object::add_unit_props_size(ot);
  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static const EnumPropertyItem quadsphere_method_items[] = {
    {QUADSPHERE_METHOD_EQUI_ANGULAR_EVEN_AREA,
     "EVEN_AREA",
     0,
     "Even Area",
     "Distribute the area evenly between faces"},
    {QUADSPHERE_METHOD_EQUI_ANGULAR,
     "EVEN_ANGLE",
     0,
     "Even Angle",
     "Distribute vertices at an even angle, spacing each axis as an exact circle"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus add_primitive_quadsphere_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  Mesh *primitive_mesh = geometry::create_mesh_from_bmesh_operator(
      add_primitive_uv_map(C, op),
      float4x4::identity(),
      "create_quadsphere segments=%i method=%i radius=%f",
      RNA_int_get(op->ptr, "segments"),
      RNA_enum_get(op->ptr, "method"),
      RNA_float_get(op->ptr, "radius"));
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Quadsphere"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_quad_sphere_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Quad Sphere";
  ot->description =
      "Construct a spherical mesh from a subdivided cube, consisting entirely of four-sided faces";
  ot->idname = "MESH_OT_primitive_quad_sphere_add";
  /* API callbacks. */
  ot->exec = add_primitive_quadsphere_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_int(ot->srna, "segments", 4, 1, 500, "Segments", "", 1, 200);
  RNA_def_enum(ot->srna,
               "method",
               quadsphere_method_items,
               QUADSPHERE_METHOD_EQUI_ANGULAR_EVEN_AREA,
               "Method",
               "Mapping from the cube onto the sphere");
  ed::object::add_unit_props_radius(ot);
  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus add_primitive_uvsphere_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  Mesh *primitive_mesh = geometry::create_uv_sphere_mesh(RNA_float_get(op->ptr, "radius"),
                                                         RNA_int_get(op->ptr, "segments"),
                                                         RNA_int_get(op->ptr, "ring_count"),
                                                         add_primitive_uv_map(C, op));
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Sphere"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_uv_sphere_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add UV Sphere";
  ot->description =
      "Construct a spherical mesh with quad faces, except for triangle faces at the top and "
      "bottom";
  ot->idname = "MESH_OT_primitive_uv_sphere_add";

  /* API callbacks. */
  ot->exec = add_primitive_uvsphere_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_int(ot->srna, "segments", 32, 3, MESH_ADD_VERTS_MAXI / 100, "Segments", "", 3, 500);
  RNA_def_int(ot->srna, "ring_count", 16, 3, MESH_ADD_VERTS_MAXI / 100, "Rings", "", 3, 500);

  ed::object::add_unit_props_radius(ot);
  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus add_primitive_icosphere_exec(bContext *C, wmOperator *op)
{
  WM_operator_view3d_unit_defaults(C, op);
  Mesh *primitive_mesh = geometry::create_ico_sphere_mesh(RNA_int_get(op->ptr, "subdivisions"),
                                                          RNA_float_get(op->ptr, "radius"),
                                                          add_primitive_uv_map(C, op));
  return add_primitive_from_mesh(
      C, op, CTX_DATA_(BLT_I18NCONTEXT_ID_MESH, "Icosphere"), 'Z', primitive_mesh);
}

void MESH_OT_primitive_ico_sphere_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Ico Sphere";
  ot->description = "Construct a spherical mesh that consists of equally sized triangles";
  ot->idname = "MESH_OT_primitive_ico_sphere_add";

  /* API callbacks. */
  ot->exec = add_primitive_icosphere_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_int(ot->srna, "subdivisions", 2, 1, 10, "Subdivisions", "", 1, 8);

  ed::object::add_unit_props_radius(ot);
  ed::object::add_mesh_props(ot);
  ed::object::add_generic_props(ot, true);
}

}  // namespace blender
