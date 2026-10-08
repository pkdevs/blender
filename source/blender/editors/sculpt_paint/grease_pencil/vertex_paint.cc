/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_color.hh"
#include "BLI_color_mix.hh"
#include "BLI_math_color.hh"

#include "DNA_brush_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_paint.hh"

#include "grease_pencil_intern.hh"

namespace blender::ed::sculpt_paint::greasepencil {

class VertexPaintOperation : public GreasePencilStrokeOperationCommon {
  using GreasePencilStrokeOperationCommon::GreasePencilStrokeOperationCommon;

 public:
  void on_stroke_begin(const bContext &C, const InputSample &start_sample) override;
  void on_stroke_extended(const bContext &C, const InputSample &extension_sample) override;
  void on_stroke_done(const bContext & /*C*/) override {};

 private:
  /* Vertex color and influence for the points in each drawing.
   * Data is only stored when not accumulating. */
  Array<Array<ColorGeometry4f>> point_pre_colors;
  Array<Array<float>> point_influences;

  /* Fill color and influence for the strokes in each drawing.
   * Data is only stored when not accumulating. */
  Array<Array<ColorGeometry4f>> stroke_pre_colors;
  Array<Array<float>> stroke_influences;
};

void VertexPaintOperation::on_stroke_begin(const bContext &C, const InputSample &start_sample)
{
  this->init_stroke(C, start_sample);

  Paint &paint = *BKE_paint_get_active_from_context(&C);
  const Brush &brush = *BKE_paint_brush(&paint);

  const bool use_accumulate = (brush.flag & BRUSH_ACCUMULATE) != 0;
  if (!use_accumulate) {
    const Vector<MutableDrawingInfo> drawings = get_drawings_for_stroke_operation(C);

    this->point_pre_colors.reinitialize(drawings.size());
    this->point_influences.reinitialize(drawings.size());
    this->stroke_pre_colors.reinitialize(drawings.size());
    this->stroke_influences.reinitialize(drawings.size());

    threading::parallel_for(drawings.index_range(), 16, [&](const IndexRange range) {
      for (const int64_t i : range) {
        const MutableDrawingInfo &info = drawings[i];

        const VArray<ColorGeometry4f> vertex_colors = info.drawing.vertex_colors();
        this->point_pre_colors[i].reinitialize(vertex_colors.size());
        array_utils::copy(vertex_colors, this->point_pre_colors[i].as_mutable_span());
        this->point_influences[i].reinitialize(vertex_colors.size());
        this->point_influences[i].fill(0.0f);

        const VArray<ColorGeometry4f> fill_colors = info.drawing.fill_colors();
        this->stroke_pre_colors[i].reinitialize(fill_colors.size());
        array_utils::copy(fill_colors, this->stroke_pre_colors[i].as_mutable_span());
        this->stroke_influences[i].reinitialize(fill_colors.size());
        this->stroke_influences[i].fill(0.0f);
      }
    });
  }

  this->on_stroke_extended(C, start_sample);
}

void VertexPaintOperation::on_stroke_extended(const bContext &C,
                                              const InputSample &extension_sample)
{
  const Scene &scene = *CTX_data_scene(&C);
  Paint &paint = *BKE_paint_get_active_from_context(&C);
  const Brush &brush = *BKE_paint_brush(&paint);
  const bool invert = this->is_inverted(brush);

  const IMB_BlendMode blend_mode = IMB_BlendMode(brush.blend);
  const bool use_accumulate = (brush.flag & BRUSH_ACCUMULATE) != 0;

  const bool use_selection_masking = ED_grease_pencil_any_vertex_mask_selection(
      scene.toolsettings);

  const bool do_points = do_vertex_color_points(brush);
  const bool do_fill = do_vertex_color_fill(brush);

  float color_linear[3];
  copy_v3_v3(color_linear, BKE_brush_color_get(&paint, &brush));
  const ColorPaint4f mix_color(color_linear[0], color_linear[1], color_linear[2], 1.0f);

  this->foreach_editable_drawing(C, [&](const GreasePencilStrokeParams &params) {
    IndexMaskMemory memory;
    const IndexMask point_selection = point_mask_for_stroke_operation(
        params, use_selection_masking, memory);
    if (!point_selection.is_empty() && do_points) {
      const Array<float2> view_positions = view_positions_from_point_mask(params, point_selection);
      MutableSpan<ColorGeometry4f> vertex_colors = params.drawing.vertex_colors_for_write();

      const Span<ColorGeometry4f> pre_colors =
          use_accumulate ? vertex_colors : this->point_pre_colors[params.drawing_i].as_span();
      MutableSpan<float> total_influences =
          use_accumulate ? MutableSpan<float>() :
                           this->point_influences[params.drawing_i].as_mutable_span();

      if (invert) {
        /* Erase vertex colors. */
        point_selection.foreach_index(
            [&](const int64_t point_i) {
              const float brush_influence = brush_point_influence(paint,
                                                                  brush,
                                                                  view_positions[point_i],
                                                                  extension_sample,
                                                                  params.multi_frame_falloff);

              if (!use_accumulate) {
                total_influences[point_i] = math::max(total_influences[point_i], brush_influence);
              }

              const float influence = use_accumulate ? brush_influence : total_influences[point_i];
              const ColorGeometry4f &color = pre_colors[point_i];

              vertex_colors[point_i].a = math::max(color.a - influence, 0.0f);
            },
            exec_mode::grain_size(4096));
      }
      else {
        /* Mix brush color into vertex colors by influence. */
        point_selection.foreach_index(
            [&](const int64_t point_i) {
              const float brush_influence = brush_point_influence(paint,
                                                                  brush,
                                                                  view_positions[point_i],
                                                                  extension_sample,
                                                                  params.multi_frame_falloff);

              if (!use_accumulate) {
                total_influences[point_i] = math::max(total_influences[point_i], brush_influence);
              }

              const float influence = use_accumulate ? brush_influence : total_influences[point_i];
              const ColorGeometry4f &color = pre_colors[point_i];

              using Color = ColorPaint4f;
              using Traits = color::Traits<Color>;

              const Color linearrgb_color = color::unpremultiply_alpha(color);

              vertex_colors[point_i] = color::premultiply_alpha(
                  color::BLI_mix_colors<Color, Traits>(
                      blend_mode, linearrgb_color, mix_color, Traits::range * influence));
            },
            exec_mode::grain_size(4096));
      }
    }

    const IndexMask fill_selection = fill_mask_for_stroke_operation(
        params, use_selection_masking, memory);
    if (!fill_selection.is_empty() && do_fill) {
      BLI_assert(params.drawing.fills().has_value());
      const GroupedSpan<int> fills = *params.drawing.fills();
      const bke::CurvesGeometry &curves = params.drawing.strokes();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      MutableSpan<ColorGeometry4f> fill_colors = params.drawing.fill_colors_for_write();
      /* TODO. Only calculate needed positions. */
      const Array<float2> view_positions = view_positions_from_curve_mask(params,
                                                                          curves.curves_range());

      const Span<ColorGeometry4f> pre_colors =
          use_accumulate ? fill_colors : this->stroke_pre_colors[params.drawing_i].as_span();
      MutableSpan<float> total_influences =
          use_accumulate ? MutableSpan<float>() :
                           this->stroke_influences[params.drawing_i].as_mutable_span();

      if (invert) {
        fill_selection.foreach_index(
            [&](const int64_t fill_i) {
              const Span<int> fill_curves = fills[fill_i];
              const IndexMask fill_curve_mask = IndexMask::from_indices(fill_curves, memory);
              const int curve_i = fill_curves.first();

              float influence = use_accumulate ? 0.0f : total_influences[curve_i];
              fill_curve_mask.foreach_index([&](const int64_t curve) {
                const IndexRange points = points_by_curve[curve];
                const Span<float2> curve_view_positions = view_positions.as_span().slice(points);
                influence = math::max(influence,
                                      brush_fill_influence(paint,
                                                           brush,
                                                           curve_view_positions,
                                                           extension_sample,
                                                           params.multi_frame_falloff));
              });

              if (!use_accumulate) {
                total_influences[curve_i] = influence;
              }

              const ColorGeometry4f &color = pre_colors[curve_i];

              fill_colors[curve_i].a = math::max(color.a - influence, 0.0f);

              index_mask::masked_fill(fill_colors, fill_colors[curve_i], fill_curve_mask);
            },
            exec_mode::grain_size(1024));
      }
      else {
        fill_selection.foreach_index(
            [&](const int64_t fill_i) {
              const Span<int> fill_curves = fills[fill_i];
              const IndexMask fill_curve_mask = IndexMask::from_indices(fill_curves, memory);
              const int curve_i = fill_curves.first();

              float influence = use_accumulate ? 0.0f : total_influences[curve_i];
              fill_curve_mask.foreach_index([&](const int64_t curve) {
                const IndexRange points = points_by_curve[curve];
                const Span<float2> curve_view_positions = view_positions.as_span().slice(points);
                influence = math::max(influence,
                                      brush_fill_influence(paint,
                                                           brush,
                                                           curve_view_positions,
                                                           extension_sample,
                                                           params.multi_frame_falloff));
              });

              if (!use_accumulate) {
                total_influences[curve_i] = influence;
              }

              const ColorGeometry4f &color = pre_colors[curve_i];

              using Color = ColorPaint4f;
              using Traits = color::Traits<Color>;

              const Color linearrgb_color = color::unpremultiply_alpha(color);

              fill_colors[curve_i] = color::premultiply_alpha(color::BLI_mix_colors<Color, Traits>(
                  blend_mode, linearrgb_color, mix_color, Traits::range * influence));

              index_mask::masked_fill(fill_colors, fill_colors[curve_i], fill_curve_mask);
            },
            exec_mode::grain_size(1024));
      }
    }

    return true;
  });
}

std::unique_ptr<GreasePencilStrokeOperation> new_vertex_paint_operation(
    const BrushStrokeMode stroke_mode)
{
  return std::make_unique<VertexPaintOperation>(stroke_mode);
}

}  // namespace blender::ed::sculpt_paint::greasepencil
