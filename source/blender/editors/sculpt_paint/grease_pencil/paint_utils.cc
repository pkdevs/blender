/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_action.hh"
#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_grease_pencil_vertex_groups.hh"

#include "BLI_listbase.hh"
#include "BLI_math_matrix.hh"

#include "DNA_action_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"

#include "GEO_fit_curves.hh"
#include "GEO_join_geometries.hh"
#include "GEO_set_curve_type.hh"
#include "GEO_smooth_curves.hh"
#include "GEO_subdivide_curves.hh"

#include "grease_pencil_intern.hh"

#include <array>
#include <utility>

namespace blender::ed::sculpt_paint::greasepencil {

void smooth_stroke(bke::greasepencil::Drawing &drawing,
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

void subdivide_stroke(bke::greasepencil::Drawing &drawing,
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

void add_strokes_to_drawing(const bool on_back,
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

void outline_stroke(bke::greasepencil::Drawing &drawing,
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

int trim_end_points(bke::greasepencil::Drawing &drawing,
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

void deselect_stroke(Scene *scene, bke::greasepencil::Drawing &drawing, const int active_curve)
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

void process_stroke_weights(const Scene &scene,
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

void append_stroke_to_multiframe_drawings(
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

void convert_stroke_type(bke::greasepencil::Drawing &drawing,
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

}  // namespace blender::ed::sculpt_paint::greasepencil
