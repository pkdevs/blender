/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_grease_pencil_fills.hh"
#include "BKE_grease_pencil_vertex_groups.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"

#include "BLI_color_types.hh"
#include "BLI_listbase.hh"
#include "BLI_math_base.hh"
#include "BLI_rand.hh"
#include "BLI_time.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

#include "GEO_fit_curves.hh"
#include "GEO_join_geometries.hh"
#include "GEO_set_curve_type.hh"
#include "GEO_smooth_curves.hh"
#include "GEO_subdivide_curves.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "grease_pencil_intern.hh"

namespace blender::ed::sculpt_paint::greasepencil {

class GreasePencilXRPaintOperation : public GreasePencilXRStrokeOperation {
 private:
  Scene *scene_;
  Object *object_;
  bke::greasepencil::Drawing *drawing_;
  int frame_number_;
  Vector<ed::greasepencil::MutableDrawingInfo> multi_frame_drawings_;

  Vector<float2> screen_space_coords_orig_;
  Vector<Vector<float2>> screen_space_curve_fitted_coords_;
  Vector<float2> screen_space_smoothed_coords_;
  Vector<float2> screen_space_final_coords_;

  float4x2 texture_space_ = float4x2::identity();
  ed::greasepencil::DrawingPlacement placement_;
  float accum_distance_ = 0.0f;

  RandomNumberGenerator rng_;

  float stroke_random_radius_factor_ = 0.0f;
  float stroke_random_opacity_factor_ = 0.0f;
  float stroke_random_rotation_factor_ = 0.0f;

  float stroke_random_hue_factor_ = 0.0f;
  float stroke_random_sat_factor_ = 0.0f;
  float stroke_random_val_factor_ = 0.0f;

  double start_time_;
  double delta_time_;

  bool do_fill_guides_;
  float start_opacity_;

  friend struct XRPaintOperationExecutor;

  Brush *saved_active_brush_ = nullptr;
  Brush *fill_guides_brush_ = nullptr;

 public:
  void on_stroke_begin(const bContext &C, const XRInputSample &start_sample) override;
  void on_stroke_extended(const bContext &C, const XRInputSample &extension_sample) override;
  void on_stroke_done(const bContext &C) override;

  GreasePencilXRPaintOperation(const bool do_fill_guides = false)
      : do_fill_guides_(do_fill_guides)
  {
  }

  void toggle_fill_guides_brush_on(const bContext &C);
  void toggle_fill_guides_brush_off(const bContext &C);
};

struct XRPaintOperationExecutor {
  Scene *scene_;
  Brush *brush_;
  BrushGpencilSettings *settings_;
  std::optional<BrushColorJitterSettings> jitter_settings_;

  ColorGeometry4f vertex_color_ = ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f);
  ColorGeometry4f fill_color_ = ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f);
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
      if (settings_->flag2 & GP_BRUSH_USE_FILL) {
        fill_color_ = color_base;
      }
    }
    softness_ = 1.0f - settings_->hardness;
    aspect_ratio_ = settings_->aspect_ratio[0] / math::max(settings_->aspect_ratio[1], 1e-8f);
    jitter_settings_ = BKE_brush_color_jitter_get_settings(paint, brush_);
  }

  void process_start_sample(GreasePencilXRPaintOperation &self,
                            const bContext &C,
                            const XRInputSample &start_sample,
                            const float3 start_location,
                            const int material_index,
                            const bool use_stroke,
                            const bool use_fill)
  {
    const float2 start_coords = start_sample.mouse_position;
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
    start_radius = ed::greasepencil::randomize_radius(*settings_,
                                                      self.stroke_random_radius_factor_,
                                                      0.0f,
                                                      start_radius,
                                                      start_sample.pressure);

    float start_opacity = ed::greasepencil::opacity_from_input_sample(
        start_sample.pressure, brush_, settings_);
    start_opacity = ed::greasepencil::randomize_opacity(*settings_,
                                                        self.stroke_random_opacity_factor_,
                                                        0.0f,
                                                        start_opacity,
                                                        start_sample.pressure);

    const float start_rotation = ed::greasepencil::randomize_rotation(
        *settings_, self.rng_, self.stroke_random_rotation_factor_, start_sample.pressure);
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
    const bool hide_fill_while_drawing = (settings_->flag & GP_BRUSH_DISSABLE_LASSO) != 0;

    self.screen_space_coords_orig_.append(start_coords);
    self.screen_space_curve_fitted_coords_.append(Vector<float2>({start_coords}));
    self.screen_space_smoothed_coords_.append(start_coords);
    self.screen_space_final_coords_.append(start_coords);

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
    if (use_fill || attributes.contains("fill_color")) {
      self.drawing_->fill_colors_for_write()[active_curve] = fill_color_;
      curve_attributes_to_skip.add("fill_color");
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
    cyclic.span[active_curve] = use_fill && !use_stroke;
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

    if (!use_stroke) {
      bke::SpanAttributeWriter<bool> hide_stroke = attributes.convert_or_add_for_write_span<bool>(
          "hide_stroke", bke::AttrDomain::Curve);
      hide_stroke.span[active_curve] = true;
      curve_attributes_to_skip.add("hide_stroke");
      hide_stroke.finish();
    }
    if (use_fill) {
      if (bke::SpanAttributeWriter<int> fill_id = attributes.lookup_or_add_for_write_span<int>(
              "fill_id", bke::AttrDomain::Curve))
      {
        fill_id.span[active_curve] = 0;
        const int new_fill_id = bke::greasepencil::get_next_available_fill_id(fill_id.span);
        fill_id.span[active_curve] = new_fill_id;
        curve_attributes_to_skip.add("fill_id");
        fill_id.finish();
      }
    }

    if (settings_->uv_random > 0.0f || attributes.contains("rotation")) {
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

    if (use_fill &&
        (start_opacity < 1.0f || attributes.contains("fill_opacity") || hide_fill_while_drawing))
    {
      bke::SpanAttributeWriter<float> fill_opacities =
          attributes.convert_or_add_for_write_span<float>(
              "fill_opacity", bke::AttrDomain::Curve, bke::AttributeInitValue(1.0f));
      self.start_opacity_ = start_opacity;
      fill_opacities.span[active_curve] = !hide_fill_while_drawing ? start_opacity : 0.1f;
      curve_attributes_to_skip.add("fill_opacity");
      fill_opacities.finish();
    }

    bke::SpanAttributeWriter<float> init_times = attributes.convert_or_add_for_write_span<float>(
        "init_time", bke::AttrDomain::Curve);
    init_times.span[active_curve] = float(uint64_t(self.start_time_ * 1e3)) / float(1e3);
    curve_attributes_to_skip.add("init_time");
    init_times.finish();

    if (self.do_fill_guides_) {
      bke::SpanAttributeWriter<bool> is_fill_boundary =
          attributes.convert_or_add_for_write_span<bool>(".is_fill_guide", bke::AttrDomain::Curve);
      is_fill_boundary.span[active_curve] = true;
      curve_attributes_to_skip.add(".is_fill_guide");
      is_fill_boundary.finish();
    }

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

static Brush *create_fill_guide_brush()
{
  Brush *fill_guides_brush = BKE_id_new_nomain<Brush>("Draw Fill Guides");
  fill_guides_brush->ob_mode = OB_MODE_PAINT_GREASE_PENCIL;

  if (fill_guides_brush->gpencil_settings == nullptr) {
    BKE_brush_init_gpencil_settings(fill_guides_brush);
  }
  BrushGpencilSettings *settings = fill_guides_brush->gpencil_settings;

  BKE_curvemapping_init(settings->curve_sensitivity);
  BKE_curvemapping_init(settings->curve_strength);
  BKE_curvemapping_init(settings->curve_jitter);
  BKE_curvemapping_init(settings->curve_rand_pressure);
  BKE_curvemapping_init(settings->curve_rand_strength);
  BKE_curvemapping_init(settings->curve_rand_uv);
  BKE_curvemapping_init(fill_guides_brush->curve_rand_hue);
  BKE_curvemapping_init(fill_guides_brush->curve_rand_saturation);
  BKE_curvemapping_init(fill_guides_brush->curve_rand_value);

  fill_guides_brush->flag |= BRUSH_LOCK_SIZE;
  fill_guides_brush->unprojected_size = 0.01f;

  settings->flag &= ~GP_BRUSH_USE_PRESSURE;

  settings->brush_draw_mode = GP_BRUSH_MODE_VERTEXCOLOR;
  copy_v3_fl3(fill_guides_brush->color, 0.0f, 1.0f, 1.0f);
  settings->vertex_factor = 1.0f;

  settings->active_smooth = 0.35f;
  settings->hardness = 1.0f;
  fill_guides_brush->spacing = 100;

  settings->flag |= GP_BRUSH_GROUP_SETTINGS;
  settings->simplify_px = 0.4f;

  return fill_guides_brush;
}

static float4x2 calculate_xr_texture_space(const Scene &scene,
                                           const ARegion &region,
                                           const Object &object,
                                           const bke::greasepencil::Layer &layer,
                                           const ed::greasepencil::DrawingPlacement &placement,
                                           const XRInputSample &start_sample)
{
  float3 u_dir;
  float3 v_dir;
  float3 origin = math::transform_point(math::invert(placement.to_world_space()),
                                        start_sample.controller_position);
  switch (scene.toolsettings->gp_sculpt.lock_axis) {
    case GP_LOCKAXIS_VIEW:
      u_dir = math::normalize(placement.project(float2(region.winx, 0.0f) +
                                                start_sample.mouse_position) -
                              origin);
      v_dir = math::normalize(placement.project(float2(0.0f, region.winy) +
                                                start_sample.mouse_position) -
                              origin);
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

static void smooth_stroke(bke::greasepencil::Drawing &drawing,
                          const float influence,
                          const int iterations,
                          const int active_curve)
{
  bke::CurvesGeometry &curves = drawing.strokes_for_write();
  const IndexRange stroke = IndexRange::from_single(active_curve);
  const offset_indices::OffsetIndices<int> points_by_curve = drawing.strokes().points_by_curve();
  const VArray<bool> cyclic = curves.cyclic();
  const VArray<bool> point_selection = VArray<bool>::from_single(true, curves.points_num());

  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  bke::GSpanAttributeWriter positions = attributes.lookup_for_write_span("position");
  geometry::smooth_curve_attribute(stroke,
                                   points_by_curve,
                                   point_selection,
                                   cyclic,
                                   iterations,
                                   influence,
                                   false,
                                   true,
                                   positions.span);
  positions.finish();
  drawing.tag_positions_changed();

  if (drawing.opacities().is_span()) {
    bke::GSpanAttributeWriter opacities = attributes.lookup_for_write_span("opacity");
    geometry::smooth_curve_attribute(stroke,
                                     points_by_curve,
                                     point_selection,
                                     cyclic,
                                     iterations,
                                     influence,
                                     true,
                                     false,
                                     opacities.span);
    opacities.finish();
  }
  if (drawing.radii().is_span()) {
    bke::GSpanAttributeWriter radii = attributes.lookup_for_write_span("radius");
    geometry::smooth_curve_attribute(stroke,
                                     points_by_curve,
                                     point_selection,
                                     cyclic,
                                     iterations,
                                     influence,
                                     true,
                                     false,
                                     radii.span);
    radii.finish();
  }
}

static void subdivide_stroke(bke::greasepencil::Drawing &drawing,
                             const float subdivisions,
                             const int active_curve)
{
  bke::CurvesGeometry &curves = drawing.strokes_for_write();
  const IndexRange stroke = IndexRange::from_single(active_curve);
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();

  Array<int> use_cuts(curves.points_num(), 0);
  use_cuts.as_mutable_span().slice(points_by_curve[active_curve]).fill(subdivisions);
  const VArray<int> cuts = VArray<int>::from_span(use_cuts.as_span());

  curves = geometry::subdivide_curves(curves, stroke, cuts);
}

static void add_strokes_to_drawing(const bool on_back,
                                   Curves *strokes,
                                   bke::greasepencil::Drawing &drawing)
{
  Curves *other_curves = bke::curves_new_nomain(std::move(drawing.strokes_for_write()));
  std::array<bke::GeometrySet, 2> geometry_sets;
  if (on_back) {
    geometry_sets = {bke::GeometrySet::from_curves(strokes),
                     bke::GeometrySet::from_curves(other_curves)};
  }
  else {
    geometry_sets = {bke::GeometrySet::from_curves(other_curves),
                     bke::GeometrySet::from_curves(strokes)};
  }
  drawing.strokes_for_write() = std::move(
      geometry::join_geometries(geometry_sets, {}).get_curves_for_write()->geometry.wrap());
  drawing.tag_topology_changed();
}

static void outline_stroke(bke::greasepencil::Drawing &drawing,
                           const int active_curve,
                           const float4x4 &viewmat,
                           const ed::greasepencil::DrawingPlacement &placement,
                           const float outline_radius,
                           const int material_index,
                           const bool on_back)
{
  bke::CurvesGeometry outline = ed::greasepencil::create_curves_outline(
      drawing,
      IndexRange::from_single(active_curve),
      viewmat,
      3,
      outline_radius,
      0.0f,
      material_index);

  placement.reproject(outline.positions(), outline.positions_for_write());
  drawing.strokes_for_write().remove_curves(IndexRange::from_single(active_curve), {});
  add_strokes_to_drawing(on_back, bke::curves_new_nomain(std::move(outline)), drawing);
}

static int trim_end_points(bke::greasepencil::Drawing &drawing,
                           const float epsilon,
                           const bool on_back,
                           const int active_curve)
{
  const IndexRange points = drawing.strokes().points_by_curve()[active_curve];
  bke::CurvesGeometry &curves = drawing.strokes_for_write();
  const VArray<float> radii = drawing.radii();

  int64_t num_points_to_remove = 0;
  for (int64_t index = points.last(); index >= points.first(); index--) {
    if (radii[index] < epsilon) {
      num_points_to_remove++;
    }
    else {
      break;
    }
  }

  if (num_points_to_remove <= 0) {
    return 0;
  }

  if (points.size() - num_points_to_remove < 1) {
    num_points_to_remove = points.size() - 1;
  }

  if (!on_back) {
    curves.resize(curves.points_num() - num_points_to_remove, curves.curves_num());
    curves.offsets_for_write().last() = curves.points_num();
    return num_points_to_remove;
  }

  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  const int last_active_point = curves.points_by_curve()[0].last();

  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain != bke::AttrDomain::Point) {
      return;
    }

    bke::GSpanAttributeWriter dst = attributes.lookup_for_write_span(iter.name);
    bke::attribute_math::shift_left(dst.span, last_active_point, curves.points_num(), 0);
    dst.finish();
  });

  curves.resize(curves.points_num() - num_points_to_remove, curves.curves_num());
  MutableSpan<int> offsets = curves.offsets_for_write();
  for (const int src_curve : curves.curves_range().drop_front(1)) {
    offsets[src_curve] = offsets[src_curve] - num_points_to_remove;
  }
  offsets.last() = curves.points_num();

  return num_points_to_remove;
}

static void deselect_stroke(Scene *scene,
                            bke::greasepencil::Drawing &drawing,
                            const int active_curve)
{
  const IndexRange points = drawing.strokes().points_by_curve()[active_curve];
  bke::CurvesGeometry &curves = drawing.strokes_for_write();
  const bke::AttrDomain selection_domain = ED_grease_pencil_edit_selection_domain_get(
      scene->toolsettings);

  for (const StringRef selection_attribute_name :
       ed::curves::get_curves_selection_attribute_names(curves))
  {
    bke::GSpanAttributeWriter selection = ed::curves::ensure_selection_attribute(
        curves, selection_domain, bke::AttrType::Bool, selection_attribute_name);

    if (selection_domain == bke::AttrDomain::Curve) {
      ed::curves::fill_selection_false(selection.span.slice(IndexRange::from_single(active_curve)));
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      ed::curves::fill_selection_false(selection.span.slice(points));
    }

    selection.finish();
  }
}

static void process_stroke_weights(const Scene &scene,
                                   const Object &object,
                                   bke::greasepencil::Drawing &drawing,
                                   const int active_curve)
{
  bke::CurvesGeometry &curves = drawing.strokes_for_write();
  const IndexRange points = curves.points_by_curve()[active_curve];
  const int def_nr = BKE_object_defgroup_active_index_get(&object) - 1;

  if (def_nr == -1) {
    return;
  }

  const bDeformGroup *defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(&object), def_nr));
  const StringRef vertex_group_name = defgroup->name;

  bke::greasepencil::assign_to_vertex_group_from_mask(
      curves, IndexMask(points), vertex_group_name, scene.toolsettings->vgroup_weight);

  if (scene.toolsettings->vgroup_weight == 0.0f) {
    return;
  }

  bPoseChannel *channel = nullptr;
  Object *ob_arm = nullptr;
  for (ModifierData &md : (&object)->modifiers) {
    if (md.type != eModifierType_GreasePencilArmature) {
      continue;
    }
    if (!(md.mode & eModifierMode_Realtime)) {
      continue;
    }

    GreasePencilArmatureModifierData *amd = reinterpret_cast<GreasePencilArmatureModifierData *>(
        &md);
    if (amd == nullptr) {
      continue;
    }
    if (amd->object->type != OB_ARMATURE || amd->object->pose == nullptr) {
      continue;
    }

    ob_arm = amd->object;
    channel = BKE_pose_channel_find_name(ob_arm->pose, vertex_group_name.data());
    if (channel != nullptr) {
      break;
    }
  }

  if (channel == nullptr) {
    return;
  }

  const float4x4 obinv = math::invert(object.object_to_world());
  const float4x4 postmat = obinv * ob_arm->object_to_world();
  const float4x4 premat = math::invert(postmat);
  const float4x4 matrix = postmat * math::invert(float4x4(channel->chan_mat)) * premat;

  MutableSpan<float3> positions = curves.positions_for_write().slice(points);
  math::transform_points(matrix, positions);
}

static bke::CurvesGeometry get_single_stroke(const bke::CurvesGeometry &src, const int curve)
{
  const IndexRange points = src.points_by_curve()[curve];
  bke::CurvesGeometry dst(points.size(), 1);

  Array<int> src_offsets({points.first(), points.one_after_last()});
  Array<int> dst_offsets({0, int(points.size())});

  copy_attributes_group_to_group(src.attributes(),
                                 bke::AttrDomain::Point,
                                 bke::AttrDomain::Point,
                                 {},
                                 src_offsets.as_span(),
                                 dst_offsets.as_span(),
                                 IndexMask{1},
                                 dst.attributes_for_write());

  src_offsets = {curve, curve + 1};
  dst_offsets = {0, 1};

  copy_attributes_group_to_group(src.attributes(),
                                 bke::AttrDomain::Curve,
                                 bke::AttrDomain::Curve,
                                 {},
                                 src_offsets.as_span(),
                                 dst_offsets.as_span(),
                                 IndexMask{1},
                                 dst.attributes_for_write());
  return dst;
}

static void append_stroke_to_multiframe_drawings(
    const bke::CurvesGeometry &src_strokes,
    const int curve,
    const int current_frame,
    const bool on_back,
    Span<ed::greasepencil::MutableDrawingInfo> drawings)
{
  const bke::CurvesGeometry stroke = get_single_stroke(src_strokes, curve);

  for (const ed::greasepencil::MutableDrawingInfo &drawing_info : drawings) {
    if (drawing_info.frame_number == current_frame) {
      continue;
    }
    add_strokes_to_drawing(on_back, bke::curves_new_nomain(stroke), drawing_info.drawing);
  }
}

static void convert_stroke_type(bke::greasepencil::Drawing &drawing,
                                const int active_curve,
                                const float threshold,
                                const int8_t curve_type)
{
  bke::CurvesGeometry &curves = drawing.strokes_for_write();
  const IndexMask selection = IndexRange::from_single(active_curve);
  const VArray<float> thresholds = VArray<float>::from_single(threshold, curves.curves_num());
  const VArray<bool> corners = VArray<bool>::from_single(false, curves.points_num());
  curves = geometry::fit_poly_to_bezier_curves(
      curves, selection, thresholds, corners, geometry::FitMethod::Refit, {});

  if (curve_type == CURVE_TYPE_CATMULL_ROM) {
    geometry::ConvertCurvesOptions options;
    options.convert_bezier_handles_to_poly_points = false;
    options.convert_bezier_handles_to_catmull_rom_points = false;
    options.keep_bezier_shape_as_nurbs = true;
    options.keep_catmull_rom_shape_as_nurbs = true;
    curves = geometry::convert_curves(curves, selection, CURVE_TYPE_CATMULL_ROM, {}, options);
  }
  else if (curve_type == CURVE_TYPE_NURBS) {
    geometry::ConvertCurvesOptions options;
    options.convert_bezier_handles_to_poly_points = false;
    options.convert_bezier_handles_to_catmull_rom_points = false;
    options.keep_bezier_shape_as_nurbs = true;
    options.keep_catmull_rom_shape_as_nurbs = true;
    curves = geometry::convert_curves(curves, selection, CURVE_TYPE_NURBS, {}, options);
  }
}

void GreasePencilXRPaintOperation::toggle_fill_guides_brush_on(const bContext &C)
{
  Paint *paint = BKE_paint_get_active_from_context(&C);
  Brush *current_brush = BKE_paint_brush(paint);

  fill_guides_brush_ = create_fill_guide_brush();
  BLI_assert(fill_guides_brush_ != nullptr);
  BKE_paint_brush_set(paint, fill_guides_brush_);
  saved_active_brush_ = current_brush;
}

void GreasePencilXRPaintOperation::toggle_fill_guides_brush_off(const bContext &C)
{
  Paint *paint = BKE_paint_get_active_from_context(&C);
  BLI_assert(saved_active_brush_ != nullptr);
  BKE_paint_brush_set(paint, saved_active_brush_);
  saved_active_brush_ = nullptr;
  BKE_id_free_ex(nullptr, fill_guides_brush_, LIB_ID_FREE_NO_MAIN, false);
  fill_guides_brush_ = nullptr;
}

void GreasePencilXRPaintOperation::on_stroke_begin(const bContext &C,
                                                   const XRInputSample &start_sample)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  ARegion *region = CTX_wm_region(&C);
  View3D *view3d = CTX_wm_view3d(&C);
  scene_ = CTX_data_scene(&C);
  object_ = CTX_data_active_object(&C);
  Object *eval_object = DEG_get_evaluated(depsgraph, object_);
  GreasePencil *grease_pencil = id_cast<GreasePencil *>(object_->data);

  if (do_fill_guides_) {
    this->toggle_fill_guides_brush_on(C);
  }

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
  const bool use_stroke = (settings->flag2 & GP_BRUSH_USE_STROKE) != 0;
  const bool use_fill = (settings->flag2 & GP_BRUSH_USE_FILL) != 0;

  frame_number_ = scene_->r.cfra;
  drawing_ = grease_pencil->get_editable_drawing_at(layer, frame_number_);
  multi_frame_drawings_ = ed::greasepencil::retrieve_editable_drawings(*scene_, *grease_pencil);
  BLI_assert(drawing_ != nullptr);

  grease_pencil->runtime->is_drawing_stroke = true;

  start_time_ = BLI_time_now_seconds();
  delta_time_ = 0.0f;

  const float3 start_location = math::transform_point(math::invert(placement_.to_world_space()),
                                                      start_sample.controller_position);

  XRPaintOperationExecutor executor{*scene_};
  executor.process_start_sample(
      *this, C, start_sample, start_location, material_index, use_stroke, use_fill);

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, grease_pencil);
}

void GreasePencilXRPaintOperation::on_stroke_extended(
    const bContext &C, const XRInputSample &extension_sample)
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

  const float3 position = math::transform_point(math::invert(placement_.to_world_space()),
                                                extension_sample.controller_position);
  const float3 prev_position = curves.positions()[last_active_point];
  float radius = ed::greasepencil::radius_from_input_sample(CTX_wm_region_view3d(&C),
                                                            CTX_wm_region(&C),
                                                            executor.brush_,
                                                            extension_sample.pressure,
                                                            position,
                                                            placement_.to_world_space(),
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

  const int new_points_num = 1;
  ed::greasepencil::resize_single_curve(
      curves, on_back == false, curve_points.size() + new_points_num);

  Set<std::string> point_attributes_to_skip;
  const IndexRange new_points = curves.points_by_curve()[active_curve].take_back(new_points_num);
  MutableSpan<float3> new_positions = curves.positions_for_write().slice(new_points);
  MutableSpan<float> new_radii = drawing_->radii_for_write().slice(new_points);
  MutableSpan<float> new_opacities = drawing_->opacities_for_write().slice(new_points);

  new_positions[0] = position;
  new_radii[0] = radius;
  new_opacities[0] = opacity;
  point_attributes_to_skip.add_multiple({"position", "radius", "opacity"});

  if (executor.use_settings_random_ && settings->draw_random_press > 0.0f) {
    new_radii[0] = ed::greasepencil::randomize_radius(*settings,
                                                      stroke_random_radius_factor_,
                                                      accum_distance_,
                                                      new_radii[0],
                                                      extension_sample.pressure);
  }
  if (executor.use_settings_random_ && settings->draw_random_strength > 0.0f) {
    new_opacities[0] = ed::greasepencil::randomize_opacity(*settings,
                                                           stroke_random_opacity_factor_,
                                                           accum_distance_,
                                                           new_opacities[0],
                                                           extension_sample.pressure);
  }
  if (executor.use_settings_random_ && (settings->uv_random > 0.0f ||
                                        attributes.contains("rotation")))
  {
    bke::SpanAttributeWriter<float> rotations = attributes.convert_or_add_for_write_span<float>(
        "rotation", bke::AttrDomain::Point);
    rotations.span.slice(new_points)[0] = ed::greasepencil::randomize_rotation(
        *settings, rng_, stroke_random_rotation_factor_, extension_sample.pressure);
    point_attributes_to_skip.add("rotation");
    rotations.finish();
  }
  if (executor.use_vertex_color_ || attributes.contains("vertex_color")) {
    MutableSpan<ColorGeometry4f> new_vertex_colors =
        drawing_->vertex_colors_for_write().slice(new_points);
    new_vertex_colors[0] = vertex_color;
    point_attributes_to_skip.add("vertex_color");
  }

  const double new_delta_time = BLI_time_now_seconds() - start_time_;
  bke::SpanAttributeWriter<float> delta_times = attributes.convert_or_add_for_write_span<float>(
      "delta_time", bke::AttrDomain::Point);
  delta_times.span.slice(new_points)[0] = float(new_delta_time);
  point_attributes_to_skip.add("delta_time");
  delta_times.finish();

  accum_distance_ += distance;
  delta_time_ = new_delta_time;

  screen_space_coords_orig_.append(extension_sample.mouse_position);
  screen_space_smoothed_coords_.append(extension_sample.mouse_position);
  screen_space_final_coords_.append(extension_sample.mouse_position);
  screen_space_curve_fitted_coords_.append(Vector<float2>({extension_sample.mouse_position}));

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
  const bool use_fill = (settings->flag2 & GP_BRUSH_USE_FILL) != 0;
  const bool hide_fill_while_drawing = (settings->flag & GP_BRUSH_DISSABLE_LASSO) != 0;

  BLI_assert(grease_pencil.has_active_layer());
  bke::greasepencil::Layer &active_layer = *grease_pencil.get_active_layer();
  bke::greasepencil::Drawing &drawing = *drawing_;
  const int active_curve = on_back ? drawing.strokes().curves_range().first() :
                                     drawing.strokes().curves_range().last();
  const offset_indices::OffsetIndices<int> points_by_curve = drawing.strokes().points_by_curve();
  const IndexRange points = points_by_curve[active_curve];

  bke::MutableAttributeAccessor attributes = drawing.strokes_for_write().attributes_for_write();
  bke::SpanAttributeWriter<float2> screen_space_positions =
      attributes.convert_or_add_for_write_only_span<float2>(".draw_tool_screen_space_positions",
                                                            bke::AttrDomain::Point);
  BLI_assert(screen_space_positions);
  screen_space_positions.span.slice(points).copy_from(this->screen_space_final_coords_);
  screen_space_positions.finish();

  if (use_fill && hide_fill_while_drawing) {
    bke::SpanAttributeWriter<float> fill_opacities = attributes.lookup_for_write_span<float>(
        "fill_opacity");
    BLI_assert(fill_opacities);
    fill_opacities.span[active_curve] = start_opacity_;
    fill_opacities.finish();
  }

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
  attributes.remove(".draw_tool_screen_space_positions");

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

  if (do_fill_guides_) {
    this->toggle_fill_guides_brush_off(C);
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, &grease_pencil.id);
}

std::unique_ptr<GreasePencilXRStrokeOperation> new_xr_paint_operation(const bool do_fill_guides)
{
  return std::make_unique<GreasePencilXRPaintOperation>(do_fill_guides);
}

}  // namespace blender::ed::sculpt_paint::greasepencil
