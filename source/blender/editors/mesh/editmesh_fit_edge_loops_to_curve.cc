/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "BLT_translation.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "mesh_intern.hh" /* own include */

namespace blender {

static const EnumPropertyItem prop_interpolation_items[] = {
    {CURVE_INTERP_CUBIC, "CUBIC", 0, "Cubic", "Natural cubic spline, smooth results"},
    {CURVE_INTERP_LINEAR, "LINEAR", 0, "Linear", "Simple and fast linear algorithm"},
    {0, nullptr},
};

static const EnumPropertyItem prop_restriction_items[] = {
    {CURVE_RESTRICT_NONE, "NONE", 0, "None", "No restrictions on vertex movement"},
    {CURVE_RESTRICT_EXTRUDE,
     "EXTRUDE",
     0,
     "Extrude Only",
     "Only allow extrusions (no indentations)"},
    {CURVE_RESTRICT_INDENT, "INDENT", 0, "Indent Only", "Only allow indentation (no extrusions)"},
    {0, nullptr},
};

static wmOperatorStatus edbm_fit_edge_loops_to_curve_exec(bContext *C, wmOperator *op)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  const float factor = RNA_float_get(op->ptr, "factor");
  const int restriction = RNA_enum_get(op->ptr, "restriction");
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  const bool regular = RNA_boolean_get(op->ptr, "regular");
  const int interpolation = RNA_enum_get(op->ptr, "interpolation");
  bool lock[3];
  RNA_boolean_get_array(op->ptr, "lock", lock);
  bool has_verts_selected = false;
  bool changed_multi = false;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totvertsel == 0) {
      continue;
    }

    has_verts_selected = true;

    if (!EDBM_op_callf(
            em,
            op,
            "fit_edge_loops_to_curve geom=%hv factor=%f restriction=%i extend=%b regular=%b "
            "lock_x=%b lock_y=%b lock_z=%b interpolation=%i",
            BM_ELEM_SELECT,
            factor,
            restriction,
            extend,
            regular,
            lock[0],
            lock[1],
            lock[2],
            interpolation))
    {
      continue;
    }

    changed_multi = true;
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }

  if (!has_verts_selected) {
    BKE_report(op->reports, RPT_WARNING, "No vertices selected");
    return OPERATOR_CANCELLED;
  }

  return changed_multi ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static void edbm_fit_edge_loops_to_curve_ui(bContext * /*C*/, wmOperator *op)
{
  /* A custom UI function is needed to draw the axis locks (X/Y/Z) as a row of toggle buttons. */
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);

  layout.prop(op->ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "restriction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "extend", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "regular", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &lock_row = layout.row(true, IFACE_("Lock"));
  PropertyRNA *lock_prop = RNA_struct_find_property(op->ptr, "lock");
  lock_row.prop(op->ptr, lock_prop, 0, 0, ui::ITEM_R_TOGGLE, "X", ICON_NONE);
  lock_row.prop(op->ptr, lock_prop, 1, 0, ui::ITEM_R_TOGGLE, "Y", ICON_NONE);
  lock_row.prop(op->ptr, lock_prop, 2, 0, ui::ITEM_R_TOGGLE, "Z", ICON_NONE);

  layout.prop(op->ptr, "interpolation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void MESH_OT_fit_edge_loops_to_curve(wmOperatorType *ot)
{
  ot->name = "To Curve";
  ot->description = "Fits the unselected edge loops to a curve defined by the selected vertices";
  ot->idname = "MESH_OT_fit_edge_loops_to_curve";

  ot->exec = edbm_fit_edge_loops_to_curve_exec;
  ot->poll = ED_operator_editmesh;
  ot->ui = edbm_fit_edge_loops_to_curve_ui;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_float_factor(
      ot->srna, "factor", 1.0f, 0.0f, 1.0f, "Factor", "Curving factor", 0.0f, 1.0f);
  RNA_def_enum(ot->srna,
               "restriction",
               prop_restriction_items,
               CURVE_RESTRICT_NONE,
               "Restriction",
               "Restrictions on how the vertices can be moved");
  RNA_def_boolean(
      ot->srna,
      "extend",
      true,
      "Extend",
      "Limits the tool to work within the boundaries of the selected vertices if false");
  RNA_def_boolean(ot->srna,
                  "regular",
                  true,
                  "Space Evenly",
                  "Distribute vertices at constant distances along the curve");
  RNA_def_boolean_array(ot->srna, "lock", 3, nullptr, "Lock", "Lock editing of the axis");
  RNA_def_enum(ot->srna,
               "interpolation",
               prop_interpolation_items,
               CURVE_INTERP_CUBIC,
               "Interpolation",
               "Algorithm used for interpolation");
}

}  // namespace blender
