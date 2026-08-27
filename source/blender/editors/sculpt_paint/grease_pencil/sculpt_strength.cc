/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_index_mask.hh"
#include "BLI_task.hh"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_key.hh"
#include "BKE_paint.hh"

#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "grease_pencil_intern.hh"

namespace blender::ed::sculpt_paint::greasepencil {

class StrengthOperation : public GreasePencilStrokeOperationCommon {
 public:
  using GreasePencilStrokeOperationCommon::GreasePencilStrokeOperationCommon;

  void on_stroke_begin(const bContext &C, const InputSample &start_sample) override;
  void on_stroke_extended(const bContext &C, const InputSample &extension_sample) override;
  void on_stroke_done(const bContext & /*C*/) override {}
};

void StrengthOperation::on_stroke_begin(const bContext &C, const InputSample &start_sample)
{
  this->init_stroke(C, start_sample);
  this->init_auto_masking(C, start_sample);
}

void StrengthOperation::on_stroke_extended(const bContext &C, const InputSample &extension_sample)
{
  Object &object = *CTX_data_active_object(&C);
  GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object.data);
  Key *key = grease_pencil.key;
  KeyBlock *actkb = BKE_keyblock_find_by_index(key, object.shapenr - 1);
  Paint &paint = *BKE_paint_get_active_from_context(&C);
  const Brush &brush = *BKE_paint_brush(&paint);
  const bool invert = this->is_inverted(brush);

  this->foreach_editable_drawing_with_automask(
      C, [&](const GreasePencilStrokeParams &params, const IndexMask &point_mask) {
        const Array<float2> view_positions = view_positions_from_point_mask(params, point_mask);

        if (key && actkb && object.shapenr != 1) {
          const float keyblock_influence = actkb->curval;

          if (keyblock_influence == 0.0f) {
            return false;
          }

          bke::CurvesGeometry &curves = params.drawing.strokes_for_write();

          bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
          bke::SpanAttributeWriter<float> key_opacity =
              attributes.lookup_or_add_for_write_span<float>(
                  bke::greasepencil::shapekey_attribute_name_opacity(actkb->name),
                  bke::AttrDomain::Point);

          point_mask.foreach_index(
              [&](const int64_t point_i) {
                const float influence = brush_point_influence(paint,
                                                              brush,
                                                              view_positions[point_i],
                                                              extension_sample,
                                                              params.multi_frame_falloff);
                /* Brush influence mapped to opacity by a factor of 0.125. */
                key_opacity.span[point_i] += (invert ? -influence : influence) * 0.125f;
              },
              exec_mode::grain_size(4096));

          key_opacity.finish();
        }
        else {
          MutableSpan<float> opacities = params.drawing.opacities_for_write();

          point_mask.foreach_index(
              [&](const int64_t point_i) {
                float &opacity = opacities[point_i];
                const float influence = brush_point_influence(paint,
                                                              brush,
                                                              view_positions[point_i],
                                                              extension_sample,
                                                              params.multi_frame_falloff);
                /* Brush influence mapped to opacity by a factor of 0.125. */
                const float delta_opacity = (invert ? -influence : influence) * 0.125f;
                opacity = std::clamp(opacity + delta_opacity, 0.0f, 1.0f);
              },
              exec_mode::grain_size(4096));
        }

        return true;
      });
  this->stroke_extended(extension_sample);
}

std::unique_ptr<GreasePencilStrokeOperation> new_strength_operation(
    const BrushStrokeMode stroke_mode)
{
  return std::make_unique<StrengthOperation>(stroke_mode);
}

}  // namespace blender::ed::sculpt_paint::greasepencil
