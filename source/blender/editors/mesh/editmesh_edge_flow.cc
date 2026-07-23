/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 *
 * Editor operator for edge flow.
 */

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_string.hh"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"

#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "mesh_intern.hh"

namespace blender {

static bool edbm_edge_flow_poll_property(const bContext * /*C*/, wmOperator *op, const PropertyRNA *prop)
{
  const char *prop_id = RNA_property_identifier(prop);
  const int mode = RNA_enum_get(op->ptr, "mode");

  if (STR_ELEM(prop_id, "iterations", "min_angle", "blend_mode", "blend_start_int", "blend_end_int", "blend_start_float", 
    "blend_end_float")) {
      if (mode != EDGE_FLOW_FLOW) {
        return false;
      }
      const int blend_mode = RNA_enum_get(op->ptr, "blend_mode");
      if (blend_mode == 0 && STR_ELEM(prop_id, "blend_start_float", "blend_end_float")) {
        return false;
      }
      if (blend_mode == 1 && STR_ELEM(prop_id, "blend_start_int", "blend_end_int")) {
        return false;
      }
      return true;
  }
  
  if (STREQ(prop_id, "tension")) {
    return mode != EDGE_FLOW_LINEAR;
  }
  if (STREQ(prop_id, "use_rail")) {
    return mode == EDGE_FLOW_CURVE;
  }
  if (STR_ELEM(prop_id, "rail_mode", "rail_start", "rail_end")) {
    return mode == EDGE_FLOW_CURVE && RNA_boolean_get(op->ptr, "use_rail");
  }

  return true;
}

static wmOperatorStatus edbm_edge_flow_exec(bContext *C, wmOperator *op)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  bool changed = false;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    const int mode = RNA_enum_get(op->ptr, "mode");
    const float mix = RNA_float_get(op->ptr, "mix");
    const bool space_evenly = RNA_boolean_get(op->ptr, "space_evenly");
    const int tension    = RNA_int_get(op->ptr, "tension");
    const int iterations = RNA_int_get(op->ptr, "iterations");
    const int blend_mode = RNA_enum_get(op->ptr, "blend_mode");
    float blend_start, blend_end;
    if (blend_mode == 0) {
      blend_start = float(RNA_int_get(op->ptr, "blend_start_int"));
      blend_end = float(RNA_int_get(op->ptr, "blend_end_int"));
    }
    else {
      blend_start = RNA_float_get(op->ptr, "blend_start_float");
      blend_end = RNA_float_get(op->ptr, "blend_end_float");
    }
    const int blend_type = RNA_enum_get(op->ptr, "blend_type");
    const int min_angle = RNA_int_get(op->ptr, "min_angle");

    const bool use_rail = RNA_boolean_get(op->ptr, "use_rail");
    const int rail_mode = RNA_enum_get(op->ptr, "rail_mode");
    const float rail_start = RNA_float_get(op->ptr, "rail_start");
    const float rail_end = RNA_float_get(op->ptr, "rail_end");

    if (!EDBM_op_callf(em,
                       op,
                       "edge_flow edges=%he mode=%i mix=%f space_evenly=%b tension=%i iterations=%i blend_mode=%i blend_start=%f blend_end=%f blend_type=%i min_angle=%i use_rail=%b rail_mode=%i rail_start=%f rail_end=%f",
                       BM_ELEM_SELECT,
                       mode,
                       mix,
                       space_evenly,
                       tension,
                       iterations,
                       blend_mode,
                       blend_start,
                       blend_end,
                       blend_type,
                       min_angle,
                       use_rail,
                       rail_mode,
                       rail_start,
                       rail_end))
    {
      continue;
    }

    changed = true;
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = false;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }

  return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static wmOperatorStatus edbm_edge_flow_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent */*event*/)
{
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "tension");
  if (!RNA_property_is_set(op->ptr, prop)) {
    const int mode = RNA_enum_get(op->ptr, "mode");
    RNA_int_set(op->ptr, "tension", mode == EDGE_FLOW_CURVE ? 100 : 180);
  }
  return edbm_edge_flow_exec(C, op);
}

static const EnumPropertyItem mode_items[] = {
    {EDGE_FLOW_FLOW, "FLOW", 0, "Flow", "Adjust loop to match surrounding geometry"},
    {EDGE_FLOW_LINEAR, "LINEAR", 0, "Linear", "Straighten the loop between endpoints"},
    {EDGE_FLOW_CURVE, "CURVE", 0, "Curve", "Fit loop to smooth curve between endpoints"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem blend_mode_items[] = {
    {0, "ABSOLUTE", 0, "Absolute", "Blend a fixed number of vertices from each end"},
    {1, "FACTOR", 0, "Factor", "Blend a fraction of the loop from each end"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem blend_type_items[] = {
    {0, "LINEAR", 0, "Linear", "Linear falloff"},
    {1, "SMOOTH", 0, "Smooth", "Smooth falloff"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rail_mode_items[] = {
    {0, "ABSOLUTE", 0, "Absolute", "Rail length in absolute units"},
    {1, "FACTOR", 0, "Factor", "Rail length as a factor of the end edge"},
    {0, nullptr, 0, nullptr, nullptr},
};

void MESH_OT_edge_flow(wmOperatorType *ot)
{
  ot->name = "Edge Flow";
  ot->idname = "MESH_OT_edge_flow";
  ot->description = "Set edge flow for the selected edges";

  ot->exec = edbm_edge_flow_exec;
  ot->invoke = edbm_edge_flow_invoke;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = edbm_edge_flow_poll_property;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "mode", mode_items, EDGE_FLOW_FLOW, "Mode", "");

  RNA_def_float(ot->srna,
                "mix",
                1.0f,
                0.0f,
                1.0f,
                "Mix",
                "Mix between original and edge flow position",
                0.0f,
                1.0f);

  RNA_def_boolean( ot->srna, "space_evenly", false, "Space Evenly", "Space edges evenly along the loop");
  RNA_def_int(ot->srna, "tension", 180, -500, 500, "Tension", "Tension of curve for flow mode", -500, 500);
  RNA_def_int(ot->srna, "iterations", 8, 1, 32, "Iterations", "Number of iterations for flow algorithm", 1, 32);

  RNA_def_enum(ot->srna, "blend_mode", blend_mode_items, 0, "Blend Mode", "Blend start/end as vertex counts or loop fractions");
  RNA_def_int(ot->srna, "blend_start_int", 0, 0, INT_MAX, "Blend Start", "Vertices from the loop start to blend", 0, 100);
  RNA_def_int(ot->srna, "blend_end_int", 0, 0, INT_MAX, "Blend End", "Vertices from the loop end to blend", 0, 100);
  RNA_def_float(ot->srna, "blend_start_float", 0.0f, 0.0f, 1.0f, "Blend Start", "Loop fraction from the start to blend", 0.0f, 1.0f);
  RNA_def_float(ot->srna, "blend_end_float", 0.0f, 0.0f, 1.0f, "Blend End", "Loop fraction from the end to blend", 0.0f, 1.0f);
  RNA_def_enum(ot->srna, "blend_type", blend_type_items, 0, "Blend Curve", "Falloff used when blending");
  RNA_def_int(ot->srna, "min_angle", 0, 0, 180, "Min Angle", "Angle below which loop curvature is ignored", 0, 180);

  RNA_def_boolean(ot->srna, "use_rail", false, "Use Rail", "Use first and last edge to control curvature");
  RNA_def_enum(ot->srna, "rail_mode", rail_mode_items, 1, "Rail Mode", "Rail length as absolute or factor of end edge");
  RNA_def_float(ot->srna, "rail_start", 1.0f, -FLT_MAX, FLT_MAX, "Rail Start", "Rail length at start of loop", -100.0f, 100.0f);
  RNA_def_float(ot->srna, "rail_end", 1.0f, -FLT_MAX, FLT_MAX, "Rail End", "Rail length at end of loop", -100.0f, 100.0f);
}

}  // namespace blender
