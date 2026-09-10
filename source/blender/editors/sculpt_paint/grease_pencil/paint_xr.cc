/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"

#include "BLI_color_types.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_time.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "grease_pencil_intern.hh"

namespace blender::ed::sculpt_paint::greasepencil {

template<typename T>
static inline void linear_interpolation(const T &from,
                                        const T &to,
                                        MutableSpan<T> dst,
                                        const bool include_first_point)
{
  if (include_first_point) {
    const float step = math::safe_rcp(float(dst.size() - 1));
    for (const int i : dst.index_range()) {
      dst[i] = bke::attribute_math::mix2(float(i) * step, from, to);
    }
  }
  else {
    const float step = 1.0f / float(dst.size());
    for (const int i : dst.index_range()) {
      dst[i] = bke::attribute_math::mix2(float(i + 1) * step, from, to);
    }
  }
}

static float stroke_segment_max_spacing(const Brush &brush,
                                        const BrushGpencilSettings &settings,
                                        const RegionView3D *rv3d,
                                        const ARegion *region,
                                        const float4x4 &placement_to_world,
                                        const float3 &position)
{
  const float spacing_radius = ed::greasepencil::radius_from_input_sample(
      rv3d, region, &brush, 1.0f, position, placement_to_world, &settings);
  return math::max(spacing_radius * (float(brush.spacing) / 100.0f), 1.0e-4f);
}

class GreasePencilXRPaintOperation : public GreasePencilStrokeOperation {
 private:
  Scene *scene_;
  Object *object_;
  bke::greasepencil::Drawing *drawing_;
  int frame_number_;
  Vector<ed::greasepencil::MutableDrawingInfo> multi_frame_drawings_;

  float4x2 texture_space_ = float4x2::identity();
  ed::greasepencil::DrawingPlacement placement_;
  float accum_distance_ = 0.0f;
  double delta_time_ = 0.0;

  RandomNumberGenerator rng_;

  float stroke_random_radius_factor_ = 0.0f;
  float stroke_random_opacity_factor_ = 0.0f;
  float stroke_random_rotation_factor_ = 0.0f;

  float stroke_random_hue_factor_ = 0.0f;
  float stroke_random_sat_factor_ = 0.0f;
  float stroke_random_val_factor_ = 0.0f;

  double start_time_;

  friend struct XRPaintOperationExecutor;

 public:
  void on_stroke_begin(const bContext &C, const InputSample &start_sample) override;
  void on_stroke_extended(const bContext &C, const InputSample &extension_sample) override;
  void on_stroke_done(const bContext &C) override;
};

struct XRPaintOperationExecutor {
  Scene *scene_;
  Brush *brush_;
  BrushGpencilSettings *settings_;
  std::optional<BrushColorJitterSettings> jitter_settings_;

  ColorGeometry4f vertex_color_ = ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f);
  float softness_;
  float aspect_ratio_;

  bool use_vertex_color_;
  bool use_settings_random_;

  XRPaintOperationExecutor(Scene &scene)
  {
    scene_ = &scene;
    Paint *paint = &scene_->toolsettings->gp_paint->paint;
    brush_ = BKE_paint_brush(paint);
    settings_ = brush_->gpencil_settings;

    use_settings_random_ = (settings_->flag & GP_BRUSH_GROUP_RANDOM) != 0;
    use_vertex_color_ = brush_using_vertex_color(scene_->toolsettings->gp_paint, brush_);
    if (use_vertex_color_) {
      ColorGeometry4f color_base;
      copy_v3_v3(color_base, brush_->color);
      color_base.a = settings_->vertex_factor;
      if (settings_->flag2 & GP_BRUSH_USE_STROKE) {
        vertex_color_ = color_base;
      }
    }
    softness_ = 1.0f - settings_->hardness;
    aspect_ratio_ = settings_->aspect_ratio[0] / math::max(settings_->aspect_ratio[1], 1e-8f);
    jitter_settings_ = BKE_brush_color_jitter_get_settings(paint, brush_);
  }

  void process_start_sample(GreasePencilXRPaintOperation &self,
                            const bContext &C,
                            const InputSample &start_sample,
                            const float3 start_location,
                            const int material_index)
  {
    const RegionView3D *rv3d = CTX_wm_region_view3d(&C);
    const ARegion *region = CTX_wm_region(&C);

    float start_radius = ed::greasepencil::radius_from_input_sample(
        rv3d,
        region,
        brush_,
        start_sample.pressure,
        start_location,
        self.placement_.to_world_space(),
        settings_);
    start_radius = ed::greasepencil::randomize_radius(
        *settings_, self.stroke_random_radius_factor_, 0.0f, start_radius, start_sample.pressure);

    float start_opacity = ed::greasepencil::opacity_from_input_sample(
        start_sample.pressure, brush_, settings_);
    start_opacity = ed::greasepencil::randomize_opacity(*settings_,
                                                        self.stroke_random_opacity_factor_,
                                                        0.0f,
                                                        start_opacity,
                                                        start_sample.pressure);

    if (use_vertex_color_) {
      vertex_color_ = ed::greasepencil::randomize_color(*settings_,
                                                        jitter_settings_,
                                                        self.stroke_random_hue_factor_,
                                                        self.stroke_random_sat_factor_,
                                                        self.stroke_random_val_factor_,
                                                        0.0f,
                                                        vertex_color_,
                                                        start_sample.pressure);
    }

    const bool on_back = (scene_->toolsettings->gpencil_flags & GP_TOOL_FLAG_PAINT_ONBACK) != 0;
    ed::greasepencil::add_single_curve(*self.drawing_, on_back == false);
    bke::CurvesGeometry &curves = self.drawing_->strokes_for_write();

    const int active_curve = on_back ? curves.curves_range().first() :
                                       curves.curves_range().last();
    const IndexRange curve_points = curves.points_by_curve()[active_curve];
    const int last_active_point = curve_points.last();

    Set<std::string> point_attributes_to_skip;
    Set<std::string> curve_attributes_to_skip;
    bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
    curves.positions_for_write()[last_active_point] = start_location;
    self.drawing_->radii_for_write()[last_active_point] = start_radius;
    self.drawing_->opacities_for_write()[last_active_point] = start_opacity;
    point_attributes_to_skip.add_multiple({"position", "radius", "opacity"});
    if (use_vertex_color_ || attributes.contains("vertex_color")) {
      self.drawing_->vertex_colors_for_write()[last_active_point] = vertex_color_;
      point_attributes_to_skip.add("vertex_color");
    }
    bke::SpanAttributeWriter<float> delta_times = attributes.convert_or_add_for_write_span<float>(
        "delta_time", bke::AttrDomain::Point);
    delta_times.span[last_active_point] = 0.0f;
    point_attributes_to_skip.add("delta_time");
    delta_times.finish();

    bke::SpanAttributeWriter<int> materials = attributes.convert_or_add_for_write_span<int>(
        "material_index", bke::AttrDomain::Curve);
    bke::SpanAttributeWriter<bool> cyclic = attributes.convert_or_add_for_write_span<bool>(
        "cyclic", bke::AttrDomain::Curve);
    cyclic.span[active_curve] = false;
    materials.span[active_curve] = material_index;
    curve_attributes_to_skip.add_multiple({"material_index", "cyclic"});
    cyclic.finish();
    materials.finish();

    bke::SpanAttributeWriter<float> softness = attributes.convert_or_add_for_write_span<float>(
        "softness", bke::AttrDomain::Curve);
    softness.span[active_curve] = softness_;
    curve_attributes_to_skip.add("softness");
    softness.finish();
    bke::SpanAttributeWriter u_scale = attributes.convert_or_add_for_write_span<float>(
        "u_scale", bke::AttrDomain::Curve, bke::AttributeInitValue(1.0f));
    u_scale.span[active_curve] = 1.0f;
    curve_attributes_to_skip.add("u_scale");
    u_scale.finish();
    bke::SpanAttributeWriter aspect_ratio = attributes.convert_or_add_for_write_span<float>(
        "aspect_ratio", bke::AttrDomain::Curve, bke::AttributeInitValue(1.0f));
    aspect_ratio.span[active_curve] = aspect_ratio_;
    curve_attributes_to_skip.add("aspect_ratio");
    aspect_ratio.finish();

    if (settings_->uv_random > 0.0f || attributes.contains("rotation")) {
      const float start_rotation = ed::greasepencil::randomize_rotation(
          *settings_, self.rng_, self.stroke_random_rotation_factor_, start_sample.pressure);
      bke::SpanAttributeWriter<float> rotations = attributes.convert_or_add_for_write_span<float>(
          "rotation", bke::AttrDomain::Point);
      rotations.span[last_active_point] = start_rotation;
      point_attributes_to_skip.add("rotation");
      rotations.finish();
    }

    if (settings_->caps_type != GP_STROKE_CAP_TYPE_ROUND || attributes.contains("start_cap")) {
      bke::SpanAttributeWriter<int8_t> start_caps =
          attributes.convert_or_add_for_write_span<int8_t>("start_cap", bke::AttrDomain::Curve);
      start_caps.span[active_curve] = settings_->caps_type;
      curve_attributes_to_skip.add("start_cap");
      start_caps.finish();
    }

    if (settings_->caps_type != GP_STROKE_CAP_TYPE_ROUND || attributes.contains("end_cap")) {
      bke::SpanAttributeWriter<int8_t> end_caps = attributes.convert_or_add_for_write_span<int8_t>(
          "end_cap", bke::AttrDomain::Curve);
      end_caps.span[active_curve] = settings_->caps_type;
      curve_attributes_to_skip.add("end_cap");
      end_caps.finish();
    }

    bke::SpanAttributeWriter<float> init_times = attributes.convert_or_add_for_write_span<float>(
        "init_time", bke::AttrDomain::Curve);
    init_times.span[active_curve] = float(uint64_t(self.start_time_ * 1e3)) / float(1e3);
    curve_attributes_to_skip.add("init_time");
    init_times.finish();

    curves.curve_types_for_write()[active_curve] = CURVE_TYPE_POLY;
    curve_attributes_to_skip.add("curve_type");
    curves.update_curve_types();

    bke::fill_attribute_range_default(
        attributes,
        bke::AttrDomain::Point,
        bke::attribute_filter_from_skip_ref(point_attributes_to_skip),
        IndexRange(last_active_point, 1));
    bke::fill_attribute_range_default(
        attributes,
        bke::AttrDomain::Curve,
        bke::attribute_filter_from_skip_ref(curve_attributes_to_skip),
        IndexRange(active_curve, 1));

    if (on_back) {
      self.drawing_->tag_topology_changed();
    }
    else {
      self.drawing_->tag_topology_changed(IndexRange::from_single(active_curve));
    }
  }
};

static float4x2 calculate_xr_texture_space(const Scene &scene,
                                           const ARegion &region,
                                           const Object &object,
                                           const bke::greasepencil::Layer &layer,
                                           const ed::greasepencil::DrawingPlacement &placement,
                                           const InputSample &start_sample)
{
  float3 u_dir;
  float3 v_dir;
  float3 origin = math::transform_point(math::invert(placement.to_world_space()),
                                        start_sample.controller_position);
  switch (scene.toolsettings->gp_sculpt.lock_axis) {
    case GP_LOCKAXIS_VIEW:
      u_dir = math::normalize(
          placement.project(float2(region.winx, 0.0f) + start_sample.mouse_position) - origin);
      v_dir = math::normalize(
          placement.project(float2(0.0f, region.winy) + start_sample.mouse_position) - origin);
      break;
    case GP_LOCKAXIS_Y:
      u_dir = float3(1.0f, 0.0f, 0.0f);
      v_dir = float3(0.0f, 0.0f, 1.0f);
      break;
    case GP_LOCKAXIS_X:
      u_dir = float3(0.0f, 1.0f, 0.0f);
      v_dir = float3(0.0f, 0.0f, 1.0f);
      break;
    case GP_LOCKAXIS_Z:
      u_dir = float3(1.0f, 0.0f, 0.0f);
      v_dir = float3(0.0f, 1.0f, 0.0f);
      break;
    case GP_LOCKAXIS_CURSOR: {
      const float3x3 mat = scene.cursor.matrix<float3x3>();
      u_dir = mat * float3(1.0f, 0.0f, 0.0f);
      v_dir = mat * float3(0.0f, 1.0f, 0.0f);
      origin = float3(scene.cursor.location);
      break;
    }
  }

  float4x2 texture_space = math::transpose(float2x4(float4(u_dir, -math::dot(u_dir, origin)),
                                                    float4(v_dir, -math::dot(v_dir, origin))));
  if (scene.toolsettings->gp_sculpt.lock_axis != GP_LOCKAXIS_VIEW) {
    texture_space = texture_space * layer.to_object_space(object);
  }
  return texture_space;
}

void GreasePencilXRPaintOperation::on_stroke_begin(const bContext &C,
                                                   const InputSample &start_sample)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  ARegion *region = CTX_wm_region(&C);
  View3D *view3d = CTX_wm_view3d(&C);
  scene_ = CTX_data_scene(&C);
  object_ = CTX_data_active_object(&C);
  Object *eval_object = DEG_get_evaluated(depsgraph, object_);
  GreasePencil *grease_pencil = id_cast<GreasePencil *>(object_->data);

  Paint *paint = &scene_->toolsettings->gp_paint->paint;
  Brush *brush = BKE_paint_brush(paint);

  if (brush->gpencil_settings == nullptr) {
    BKE_brush_init_gpencil_settings(brush);
  }
  BrushGpencilSettings *settings = brush->gpencil_settings;

  BKE_curvemapping_init(settings->curve_sensitivity);
  BKE_curvemapping_init(settings->curve_strength);
  BKE_curvemapping_init(settings->curve_jitter);
  BKE_curvemapping_init(settings->curve_rand_pressure);
  BKE_curvemapping_init(settings->curve_rand_strength);
  BKE_curvemapping_init(settings->curve_rand_uv);
  BKE_curvemapping_init(brush->curve_rand_hue);
  BKE_curvemapping_init(brush->curve_rand_saturation);
  BKE_curvemapping_init(brush->curve_rand_value);

  BLI_assert(grease_pencil->has_active_layer());
  const bke::greasepencil::Layer &layer = *grease_pencil->get_active_layer();

  placement_ = ed::greasepencil::DrawingPlacement(*scene_, *region, *view3d, *eval_object, &layer);
  texture_space_ = calculate_xr_texture_space(
      *scene_, *region, *object_, layer, placement_, start_sample);

  rng_ = RandomNumberGenerator::from_random_seed();
  if ((settings->flag & GP_BRUSH_GROUP_RANDOM) != 0) {
    stroke_random_radius_factor_ = rng_.get_float() * 2.0f - 1.0f;
    stroke_random_opacity_factor_ = rng_.get_float() * 2.0f - 1.0f;
    stroke_random_rotation_factor_ = rng_.get_float() * 2.0f - 1.0f;

    stroke_random_hue_factor_ = rng_.get_float() * 2.0f - 1.0f;
    stroke_random_sat_factor_ = rng_.get_float() * 2.0f - 1.0f;
    stroke_random_val_factor_ = rng_.get_float() * 2.0f - 1.0f;
  }

  Material *material = BKE_grease_pencil_object_material_ensure_from_brush(
      CTX_data_main(&C), object_, brush);
  const int material_index = BKE_object_material_index_get(object_, material);

  frame_number_ = scene_->r.cfra;
  drawing_ = grease_pencil->get_editable_drawing_at(layer, frame_number_);
  multi_frame_drawings_ = ed::greasepencil::retrieve_editable_drawings(*scene_, *grease_pencil);
  BLI_assert(drawing_ != nullptr);

  grease_pencil->runtime->is_drawing_stroke = true;

  start_time_ = BLI_time_now_seconds();

  const float3 start_location = math::transform_point(math::invert(placement_.to_world_space()),
                                                      start_sample.controller_position);

  XRPaintOperationExecutor executor{*scene_};
  executor.process_start_sample(*this, C, start_sample, start_location, material_index);

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, grease_pencil);
}

void GreasePencilXRPaintOperation::on_stroke_extended(const bContext &C,
                                                      const InputSample &extension_sample)
{
  GreasePencil *grease_pencil = id_cast<GreasePencil *>(object_->data);
  const bool on_back = (scene_->toolsettings->gpencil_flags & GP_TOOL_FLAG_PAINT_ONBACK) != 0;

  XRPaintOperationExecutor executor{*scene_};
  BrushGpencilSettings *settings = executor.settings_;
  bke::CurvesGeometry &curves = drawing_->strokes_for_write();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  const int active_curve = on_back ? curves.curves_range().first() : curves.curves_range().last();
  const IndexRange curve_points = curves.points_by_curve()[active_curve];
  const int last_active_point = curve_points.last();

  const float4x4 placement_to_world = placement_.to_world_space();
  const float3 position = math::transform_point(math::invert(placement_to_world),
                                                extension_sample.controller_position);
  const float3 prev_position = curves.positions()[last_active_point];
  const RegionView3D *rv3d = CTX_wm_region_view3d(&C);
  const ARegion *region = CTX_wm_region(&C);
  float radius = ed::greasepencil::radius_from_input_sample(rv3d,
                                                            region,
                                                            executor.brush_,
                                                            extension_sample.pressure,
                                                            position,
                                                            placement_to_world,
                                                            settings);
  float opacity = ed::greasepencil::opacity_from_input_sample(
      extension_sample.pressure, executor.brush_, settings);
  float prev_radius = drawing_->radii()[last_active_point];
  const float prev_opacity = drawing_->opacities()[last_active_point];
  const ColorGeometry4f vertex_color = executor.use_vertex_color_ ?
                                           ed::greasepencil::randomize_color(
                                               *settings,
                                               executor.jitter_settings_,
                                               stroke_random_hue_factor_,
                                               stroke_random_sat_factor_,
                                               stroke_random_val_factor_,
                                               accum_distance_,
                                               executor.vertex_color_,
                                               extension_sample.pressure) :
                                           executor.vertex_color_;

  const float distance = math::distance(position, prev_position);
  constexpr float point_override_threshold = 1.0e-5f;
  if (distance < point_override_threshold) {
    accum_distance_ += distance;
    if (curve_points.size() > 1) {
      curves.positions_for_write()[last_active_point] = position;
    }
    if (executor.use_settings_random_ && settings->draw_random_press > 0.0f) {
      radius = ed::greasepencil::randomize_radius(*settings,
                                                  stroke_random_radius_factor_,
                                                  accum_distance_,
                                                  radius,
                                                  extension_sample.pressure);
    }
    if (executor.use_settings_random_ && settings->draw_random_strength > 0.0f) {
      opacity = ed::greasepencil::randomize_opacity(*settings,
                                                    stroke_random_opacity_factor_,
                                                    accum_distance_,
                                                    opacity,
                                                    extension_sample.pressure);
    }
    drawing_->radii_for_write()[last_active_point] = math::max(radius, prev_radius);
    drawing_->opacities_for_write()[last_active_point] = math::max(opacity, prev_opacity);
    drawing_->tag_positions_changed();
    DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_GEOM | ND_DATA, grease_pencil);
    return;
  }

  const float max_spacing = stroke_segment_max_spacing(
      *executor.brush_, *settings, rv3d, region, placement_to_world, position);
  const int new_points_num = math::max(int(math::floor(distance / max_spacing)), 1);
  ed::greasepencil::resize_single_curve(
      curves, on_back == false, curve_points.size() + new_points_num);

  Set<std::string> point_attributes_to_skip;
  const IndexRange new_points = curves.points_by_curve()[active_curve].take_back(new_points_num);
  MutableSpan<float3> new_positions = curves.positions_for_write().slice(new_points);
  MutableSpan<float> new_radii = drawing_->radii_for_write().slice(new_points);
  MutableSpan<float> new_opacities = drawing_->opacities_for_write().slice(new_points);

  linear_interpolation<float3>(prev_position, position, new_positions, false);
  linear_interpolation<float>(prev_radius, radius, new_radii, false);
  linear_interpolation<float>(prev_opacity, opacity, new_opacities, false);
  point_attributes_to_skip.add_multiple({"position", "radius", "opacity"});

  if (executor.use_settings_random_ && settings->draw_random_press > 0.0f) {
    for (const int i : IndexRange(new_points_num)) {
      new_radii[i] = ed::greasepencil::randomize_radius(*settings,
                                                        stroke_random_radius_factor_,
                                                        accum_distance_ + max_spacing * i,
                                                        new_radii[i],
                                                        extension_sample.pressure);
    }
  }
  if (executor.use_settings_random_ && settings->draw_random_strength > 0.0f) {
    for (const int i : IndexRange(new_points_num)) {
      new_opacities[i] = ed::greasepencil::randomize_opacity(*settings,
                                                             stroke_random_opacity_factor_,
                                                             accum_distance_ + max_spacing * i,
                                                             new_opacities[i],
                                                             extension_sample.pressure);
    }
  }
  if (executor.use_settings_random_ &&
      (settings->uv_random > 0.0f || attributes.contains("rotation")))
  {
    bke::SpanAttributeWriter<float> rotations = attributes.convert_or_add_for_write_span<float>(
        "rotation", bke::AttrDomain::Point);
    MutableSpan<float> new_rotations = rotations.span.slice(new_points);
    for (const int i : IndexRange(new_points_num)) {
      new_rotations[i] = ed::greasepencil::randomize_rotation(
          *settings, rng_, stroke_random_rotation_factor_, extension_sample.pressure);
    }
    point_attributes_to_skip.add("rotation");
    rotations.finish();
  }
  if (executor.use_vertex_color_ || attributes.contains("vertex_color")) {
    MutableSpan<ColorGeometry4f> new_vertex_colors = drawing_->vertex_colors_for_write().slice(
        new_points);
    linear_interpolation<ColorGeometry4f>(
        drawing_->vertex_colors()[last_active_point], vertex_color, new_vertex_colors, false);
    if (executor.use_settings_random_ || attributes.contains("vertex_color")) {
      for (const int i : IndexRange(new_points_num)) {
        new_vertex_colors[i] = ed::greasepencil::randomize_color(*settings,
                                                                 executor.jitter_settings_,
                                                                 stroke_random_hue_factor_,
                                                                 stroke_random_sat_factor_,
                                                                 stroke_random_val_factor_,
                                                                 accum_distance_ + max_spacing * i,
                                                                 new_vertex_colors[i],
                                                                 extension_sample.pressure);
      }
    }
    point_attributes_to_skip.add("vertex_color");
  }

  const double new_delta_time = BLI_time_now_seconds() - start_time_;
  bke::SpanAttributeWriter<float> delta_times = attributes.convert_or_add_for_write_span<float>(
      "delta_time", bke::AttrDomain::Point);
  linear_interpolation<float>(
      float(delta_time_), float(new_delta_time), delta_times.span.slice(new_points), false);
  point_attributes_to_skip.add("delta_time");
  delta_times.finish();

  accum_distance_ += distance;
  delta_time_ = new_delta_time;

  bke::fill_attribute_range_default(attributes,
                                    bke::AttrDomain::Point,
                                    bke::attribute_filter_from_skip_ref(point_attributes_to_skip),
                                    curves.points_range().take_back(new_points_num));

  drawing_->set_texture_matrices({texture_space_}, IndexRange::from_single(active_curve));
  drawing_->tag_topology_changed(IndexRange::from_single(active_curve));

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, grease_pencil);
}

void GreasePencilXRPaintOperation::on_stroke_done(const bContext &C)
{
  using namespace blender::bke;
  RegionView3D *rv3d = CTX_wm_region_view3d(&C);
  const ARegion *region = CTX_wm_region(&C);
  GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object_->data);

  Paint *paint = &scene_->toolsettings->gp_paint->paint;
  Brush *brush = BKE_paint_brush(paint);
  BrushGpencilSettings *settings = brush->gpencil_settings;
  const bool on_back = (scene_->toolsettings->gpencil_flags & GP_TOOL_FLAG_PAINT_ONBACK) != 0;
  const bool do_post_processing = (settings->flag & GP_BRUSH_GROUP_SETTINGS) != 0;
  const bool do_automerge_endpoints = (scene_->toolsettings->gpencil_flags &
                                       GP_TOOL_FLAG_AUTOMERGE_STROKE) != 0;

  BLI_assert(grease_pencil.has_active_layer());
  bke::greasepencil::Layer &active_layer = *grease_pencil.get_active_layer();
  bke::greasepencil::Drawing &drawing = *drawing_;
  const int active_curve = on_back ? drawing.strokes().curves_range().first() :
                                     drawing.strokes().curves_range().last();

  trim_end_points(drawing, 1e-5f, on_back, active_curve);

  if (do_post_processing) {
    if (settings->draw_subdivide > 0 && settings->simplify_px == 0.0f) {
      subdivide_stroke(drawing, settings->draw_subdivide, active_curve);
    }
    if (settings->draw_smoothfac > 0.0f && settings->draw_smoothlvl > 0) {
      smooth_stroke(drawing, settings->draw_smoothfac, settings->draw_smoothlvl, active_curve);
    }
    if ((scene_->toolsettings->gpencil_flags & GP_TOOL_FLAG_CREATE_WEIGHTS) != 0) {
      process_stroke_weights(*scene_, *object_, drawing, active_curve);
    }
    if ((settings->flag & GP_BRUSH_OUTLINE_STROKE) != 0) {
      const float outline_radius = brush->unprojected_size / 2.0f * settings->outline_fac * 0.5f;
      const int material_index = [&]() {
        Material *material = BKE_grease_pencil_object_material_alt_ensure_from_brush(
            CTX_data_main(&C), object_, brush);
        return BKE_object_material_index_get(object_, material);
      }();
      outline_stroke(drawing,
                     active_curve,
                     float4x4(rv3d->viewmat),
                     placement_,
                     outline_radius,
                     material_index,
                     on_back);
    }
    if (settings->curve_type != CURVE_TYPE_POLY) {
      convert_stroke_type(
          drawing, active_curve, settings->conversion_threshold, settings->curve_type);
    }
  }

  deselect_stroke(scene_, drawing, active_curve);

  drawing.set_texture_matrices({texture_space_}, IndexRange::from_single(active_curve));

  if (do_automerge_endpoints) {
    constexpr float merge_distance = 20.0f;
    const float4x4 layer_to_world = active_layer.to_world_space(*object_);
    const IndexMask selection = IndexRange::from_single(active_curve);
    drawing.strokes_for_write() = ed::greasepencil::curves_merge_endpoints_by_distance(
        *region, drawing.strokes(), layer_to_world, merge_distance, selection, {});
  }

  if (do_automerge_endpoints || on_back) {
    drawing.tag_topology_changed();
  }
  else {
    drawing.tag_topology_changed(IndexRange::from_single(active_curve));
  }

  const bool use_multi_frame_editing = (scene_->toolsettings->gpencil_flags &
                                        GP_USE_MULTI_FRAME_EDITING) != 0;
  if (use_multi_frame_editing) {
    append_stroke_to_multiframe_drawings(
        drawing.strokes(), active_curve, frame_number_, on_back, multi_frame_drawings_);
  }

  grease_pencil.runtime->is_drawing_stroke = false;

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, &grease_pencil.id);
}

std::unique_ptr<GreasePencilStrokeOperation> new_xr_paint_operation()
{
  return std::make_unique<GreasePencilXRPaintOperation>();
}

}  // namespace blender::ed::sculpt_paint::greasepencil
