/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_object_deform.h"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "BLI_assert.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_brush_types.h"
#include "DNA_grease_pencil_types.h"

#include "DNA_scene_types.h"
#include "ED_grease_pencil.hh"
#include "ED_image.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "ANIM_keyframing.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"

#include "MEM_guardedalloc.h"

#include "grease_pencil_intern.hh"
#include "../paint_intern.hh"

#include <memory>
#include <utility>

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Common Paint Operator Functions
 * \{ */

static float xr_stroke_spacing(const Brush &brush)
{
  constexpr float min_spacing = 1.0e-4f;
  if (brush.flag & BRUSH_LOCK_SIZE) {
    return math::max(min_spacing, (brush.unprojected_size * 0.5f) *
                                      (float(brush.spacing) / 100.0f));
  }
  return min_spacing;
}

static bool xr_brush_supports_stroke_line(const Brush &brush,
                                          const PaintMode mode,
                                          const BrushSwitchMode brush_switch_mode)
{
  if (mode != PaintMode::GPencil || brush_switch_mode == BrushSwitchMode::Erase) {
    return false;
  }
  if (eBrushGPaintType(brush.gpencil_brush_type) != GPAINT_BRUSH_TYPE_DRAW) {
    return false;
  }
  if (brush.gpencil_settings == nullptr) {
    return false;
  }

  const bool use_stroke = (brush.gpencil_settings->flag2 & GP_BRUSH_USE_STROKE) != 0;
  const bool use_fill = (brush.gpencil_settings->flag2 & GP_BRUSH_USE_FILL) != 0;
  return use_stroke && !use_fill;
}

static std::unique_ptr<GreasePencilStrokeOperation> get_xr_stroke_operation(bContext &C,
                                                                            wmOperator *op)
{
  const Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (paint == nullptr || brush == nullptr) {
    return nullptr;
  }

  const PaintMode mode = BKE_paintmode_get_active_from_context(&C);
  const auto brush_switch_mode = BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle"));

  if (!xr_brush_supports_stroke_line(*brush, mode, brush_switch_mode)) {
    return nullptr;
  }
  return greasepencil::new_xr_paint_operation();
}

struct GreasePencilXRPaintStroke {
  std::unique_ptr<GreasePencilStrokeOperation> operation;
  float3 last_controller_position = float3(0.0f);
  float stroke_distance = 0.0f;

  wmOperatorStatus modal(bContext *C, wmOperator *op, const wmEvent *event);
  void finish(bContext *C);
};

wmOperatorStatus GreasePencilXRPaintStroke::modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  if (event->type != EVT_XR_ACTION) {
    return OPERATOR_PASS_THROUGH;
  }

  BLI_assert(event->custom == EVT_DATA_XR);
  const wmXrActionData *actiondata = static_cast<const wmXrActionData *>(event->customdata);
  if (actiondata == nullptr) {
    return OPERATOR_PASS_THROUGH;
  }

  if (event->val == KM_RELEASE) {
    this->finish(C);
    return OPERATOR_FINISHED;
  }

  InputSample sample;
  sample.mouse_position = float2(event->mval);
  sample.controller_position = float3(actiondata->controller_loc);
  sample.pressure = actiondata->state[0];

  if (!this->operation) {
    this->operation = get_xr_stroke_operation(*C, op);
    if (!this->operation) {
      return OPERATOR_CANCELLED;
    }
    this->operation->on_stroke_begin(*C, sample);
    this->last_controller_position = sample.controller_position;
    ED_region_tag_redraw(CTX_wm_region(C));
    return OPERATOR_RUNNING_MODAL;
  }

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (paint == nullptr || brush == nullptr) {
    this->finish(C);
    return OPERATOR_CANCELLED;
  }

  const float distance = math::distance(sample.controller_position, this->last_controller_position);
  this->stroke_distance += distance;
  if (this->stroke_distance < xr_stroke_spacing(*brush)) {
    return OPERATOR_RUNNING_MODAL;
  }

  this->stroke_distance = 0.0f;
  this->last_controller_position = sample.controller_position;
  this->operation->on_stroke_extended(*C, sample);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_RUNNING_MODAL;
}

void GreasePencilXRPaintStroke::finish(bContext *C)
{
  if (this->operation) {
    this->operation->on_stroke_done(*C);
    this->operation.reset();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Stroke Operator
 * \{ */

static bool grease_pencil_xr_brush_stroke_poll(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);

  if (!WM_xr_session_is_ready(&wm->xr)) {
    return false;
  }
  if (!ed::greasepencil::grease_pencil_painting_poll(C)) {
    return false;
  }
  if (!WM_toolsystem_active_tool_is_brush(C)) {
    return false;
  }
  return true;
}

static bool wm_xr_operator_gpencil_test_event(const wmOperator *op, const wmEvent *event)
{
  if (event->type != EVT_XR_ACTION) {
    return false;
  }

  BLI_assert(event->custom == EVT_DATA_XR);
  BLI_assert(event->customdata);
  if (event->customdata == nullptr) {
    return false;
  }

  wmXrActionData *actiondata = static_cast<wmXrActionData *>(event->customdata);
  const bool op_is_grease_pencil_xr_stroke = STREQ(op->type->idname,
                                                   "GREASE_PENCIL_XR_OT_brush_stroke");
  const bool action_is_grease_pencil_xr_stroke =
      actiondata->ot && STREQ(actiondata->ot->idname, "GREASE_PENCIL_XR_OT_brush_stroke");
  const bool matched = actiondata->ot == op->type ||
                       (op_is_grease_pencil_xr_stroke && action_is_grease_pencil_xr_stroke);
  return matched;
}

static wmOperatorStatus grease_pencil_xr_brush_stroke_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent *event)
{
  if (!wm_xr_operator_gpencil_test_event(op, event)) {
    return OPERATOR_PASS_THROUGH;
  }

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (paint == nullptr || brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const auto brush_switch_mode = BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle"));
  if (!xr_brush_supports_stroke_line(*brush, mode, brush_switch_mode)) {
    return OPERATOR_CANCELLED;
  }

  wmOperatorStatus return_value = ed::greasepencil::grease_pencil_draw_operator_invoke(
      C, op, false);
  if (return_value != OPERATOR_RUNNING_MODAL) {
    return return_value;
  }

  GreasePencilXRPaintStroke *stroke = MEM_new<GreasePencilXRPaintStroke>(__func__);
  op->customdata = stroke;

  return_value = op->type->modal(C, op, event);
  OPERATOR_RETVAL_CHECK(return_value);
  if (ELEM(return_value, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(stroke);
    op->customdata = nullptr;
    return return_value;
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus grease_pencil_xr_brush_stroke_modal(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  if (!wm_xr_operator_gpencil_test_event(op, event)) {
    return OPERATOR_PASS_THROUGH;
  }

  GreasePencilXRPaintStroke *stroke = static_cast<GreasePencilXRPaintStroke *>(op->customdata);
  if (stroke == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const wmOperatorStatus retval = stroke->modal(C, op, event);

  if (ELEM(retval, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(stroke);
    op->customdata = nullptr;
  }

  return retval;
}

static void grease_pencil_xr_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  GreasePencilXRPaintStroke *stroke = static_cast<GreasePencilXRPaintStroke *>(op->customdata);
  if (stroke == nullptr) {
    return;
  }

  stroke->finish(C);
  MEM_delete(stroke);
  op->customdata = nullptr;
}

/** \} */

}  // namespace blender::ed::sculpt_paint

namespace blender {

void ED_operatortypes_grease_pencil_draw_xr()
{
  WM_operatortype_append(GREASE_PENCIL_XR_OT_brush_stroke);
}

void GREASE_PENCIL_XR_OT_brush_stroke(wmOperatorType *ot)
{
  using namespace ed::sculpt_paint;
  ot->name = "Grease Pencil XR Draw";
  ot->idname = "GREASE_PENCIL_XR_OT_brush_stroke";
  ot->description = "Draw a new XR stroke in the active Grease Pencil object";

  ot->poll = grease_pencil_xr_brush_stroke_poll;
  ot->invoke = grease_pencil_xr_brush_stroke_invoke;
  ot->modal = grease_pencil_xr_brush_stroke_modal;
  ot->cancel = grease_pencil_xr_brush_stroke_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  paint_stroke_operator_properties(ot);
}

}  // namespace blender
