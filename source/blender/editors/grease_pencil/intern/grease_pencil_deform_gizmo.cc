/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bounds.hh"
#include "BLI_listbase.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_c.hh"

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_key.hh"
#include "BKE_scene.hh"
#include "BKE_tracking.hh"

#include "ED_gizmo_library.hh"
#include "ED_gizmo_utils.hh"
#include "ED_grease_pencil.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

namespace blender {

namespace ed::greasepencil {

enum class DeformMode : int8_t {
  TRANSFORM = 0,
  PERSPECTIVE = 1,
  GRID = 2,
};

static const EnumPropertyItem prop_deform_modes[] = {
    {int(DeformMode::TRANSFORM), "TRANSFORM", 0, "Transform", "TODO"},
    {int(DeformMode::PERSPECTIVE), "PERSPECTIVE", 0, "Perspective", "TODO"},
    {int(DeformMode::GRID), "GRID", 0, "Grid", "TODO"},
    {0, nullptr, 0, nullptr, nullptr},
};

constexpr float gizmo_scale_basis_grid = 0.06f;
constexpr float gizmo_scale_basis_perspective = 0.10f;

static bool WIDGETGROUP_grease_pencil_quad_poll(const bContext *C, wmGizmoGroupType *gzgt)
{
  if (!ED_gizmo_poll_or_unlink_delayed_from_tool(C, gzgt)) {
    return false;
  }
  View3D *v3d = CTX_wm_view3d(C);
  if (v3d->gizmo_flag & (V3D_GIZMO_HIDE | V3D_GIZMO_HIDE_TOOL)) {
    return false;
  }
  return true;
}

struct CornerPinData {
  wmGizmo *gizmos;
  float2 corner_pos;
};

struct QuadWidgetGroup2 {
  wmGizmo *border;

  struct {
    float2 dims;
    float2 offset;
  } state;

  Vector<MutableDrawingInfo> drawings;
  Vector<IndexMask> selections;
  Vector<Array<float3>> src_positions;
  Vector<Array<float>> src_radii;

  GreasePencil *grease_pencil_src;

  IndexMaskMemory memory;

  DeformMode mode;
  int subdivisions;
  int lock_axis;
  bool is_scale_thickness;

  Vector<CornerPinData> corner_data;

  struct {
    PointerRNA ptr;
    PropertyRNA *prop;
    PropertyRNA *gzgt_mode_prop;
    PropertyRNA *gzgt_subdivisions_prop;
    bContext *context;
  } update_data;
};

/* translate callbacks */
static void gizmo_bisect_prop_translate_get(const wmGizmo *gz,
                                            wmGizmoProperty *gz_prop,
                                            void *value_p)
{
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gz->parent_gzgroup->customdata);
  const int index = static_cast<int>(reinterpret_cast<intptr_t>(gz_prop->custom_func.user_data));
  const CornerPinData *corner_pin_data = &mask_group->corner_data[index];

  BLI_assert(gz_prop->type->array_length == 3);
  float *values = static_cast<float *>(value_p);

  values[0] = corner_pin_data->corner_pos[0];
  values[1] = corner_pin_data->corner_pos[1];
  values[2] = 0.0f;
}

static std::optional<float3x3> compute_homography_matrix(const QuadWidgetGroup2 *mask_group)
{
  float2 lower_left = mask_group->corner_data[0].corner_pos;
  float2 lower_right = mask_group->corner_data[1].corner_pos;
  float2 upper_right = mask_group->corner_data[2].corner_pos;
  float2 upper_left = mask_group->corner_data[3].corner_pos;

  if (!is_quad_convex_v2(lower_left, lower_right, upper_right, upper_left)) {
    return std::nullopt;
  }

  float3x3 homography_matrix;
  float corners[4][2] = {{lower_left.x, lower_left.y},
                         {lower_right.x, lower_right.y},
                         {upper_right.x, upper_right.y},
                         {upper_left.x, upper_left.y}};
  float identity_corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
  for (const int i : IndexRange(4)) {
    identity_corners[i][0] *= mask_group->state.dims.x;
    identity_corners[i][1] *= mask_group->state.dims.y;
  }
  BKE_tracking_homography_between_two_quads(identity_corners, corners, homography_matrix.ptr());
  return homography_matrix;
}

static void update_positions_transform(QuadWidgetGroup2 *mask_group, const float4x4 &mat)
{
  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;
  const Vector<Array<float3>> &src_positions = mask_group->src_positions;
  const Vector<Array<float>> &src_radii = mask_group->src_radii;

  const float4x4 transform = float4x4(mask_group->border->matrix_space) * mat *
                             math::invert(float4x4(mask_group->border->matrix_space));

  const float scale_fac = math::sqrt(math::abs(math::determinant(float3x3(transform))));

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];
    const IndexMask &selected_points = selections[drawing_i];
    const Span<float3> src_positions_i = src_positions[drawing_i];
    const Span<float> src_radii_i = src_radii[drawing_i];

    bke::CurvesGeometry &strokes = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = strokes.positions_for_write();
    MutableSpan<float> radii = strokes.radius_for_write();

    selected_points.foreach_index([&](const int point_i) {
      positions[point_i] = math::transform_point(transform, src_positions_i[point_i]);
    });

    if (mask_group->is_scale_thickness) {
      selected_points.foreach_index(
          [&](const int point_i) { radii[point_i] = src_radii_i[point_i] * scale_fac; });
    }

    info.drawing.tag_positions_changed();
  }

  DEG_id_tag_update(&mask_group->grease_pencil_src->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(mask_group->update_data.context,
                        NC_GEOM | ND_DATA | NA_EDITED,
                        mask_group->grease_pencil_src);
}

static void update_positions_prespective(QuadWidgetGroup2 *mask_group)
{
  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;
  const Vector<Array<float3>> &src_positions = mask_group->src_positions;
  const Vector<Array<float>> &src_radii = mask_group->src_radii;

  const float4x4 mat = float4x4(mask_group->border->matrix_space);
  const float4x4 mat_inv = math::invert(mat);

  const std::optional<float3x3> homography_mat = compute_homography_matrix(mask_group);

  /* If the matrix can not be created leave the all points at the last valid position. */
  if (!homography_mat) {
    return;
  }

  const float scale_fac = math::sqrt(math::abs(math::determinant(float2x2(*homography_mat))));

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];
    const IndexMask &selected_points = selections[drawing_i];
    const Span<float3> src_positions_i = src_positions[drawing_i];
    const Span<float> src_radii_i = src_radii[drawing_i];

    bke::CurvesGeometry &strokes = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = strokes.positions_for_write();
    MutableSpan<float> radii = strokes.radius_for_write();

    selected_points.foreach_index([&](const int point_i) {
      const float2 pos_2d = float2(math::transform_point(mat_inv, src_positions_i[point_i]));

      const float3 pos_3d_out = (*homography_mat) * float3(pos_2d, 1.0f);
      const float2 pos_2d_out = float2(pos_3d_out.x, pos_3d_out.y) / pos_3d_out.z;

      positions[point_i] = math::transform_point(mat, float3(pos_2d_out, 0.0f));

      if (mask_group->is_scale_thickness) {
        radii[point_i] = src_radii_i[point_i] * scale_fac / pos_3d_out.z;
      }
    });

    info.drawing.tag_positions_changed();
  }

  DEG_id_tag_update(&mask_group->grease_pencil_src->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(mask_group->update_data.context,
                        NC_GEOM | ND_DATA | NA_EDITED,
                        mask_group->grease_pencil_src);
}

static void update_positions_grid(QuadWidgetGroup2 *mask_group)
{
  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;
  const Vector<Array<float3>> &src_positions = mask_group->src_positions;
  const Vector<Array<float>> &src_radii = mask_group->src_radii;

  const float4x4 mat = float4x4(mask_group->border->matrix_space);
  const float4x4 mat_inv = math::invert(mat);

  const int x_num = 2 + mask_group->subdivisions;
  const int y_num = 2 + mask_group->subdivisions;

  auto xy_to_position = [&](const int2 xy) {
    const int2 xy_clamped = math::clamp(xy, int2(0), int2(x_num - 1, y_num - 1));
    const int index = xy_clamped.x + x_num * xy_clamped.y;
    return mask_group->corner_data[index].corner_pos;
  };

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];
    const IndexMask &selected_points = selections[drawing_i];
    const Span<float3> src_positions_i = src_positions[drawing_i];
    const Span<float> src_radii_i = src_radii[drawing_i];

    bke::CurvesGeometry &strokes = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = strokes.positions_for_write();
    MutableSpan<float> radii = strokes.radius_for_write();

    selected_points.foreach_index([&](const int point_i) {
      const float2 pos_2d = float2(math::transform_point(mat_inv, src_positions_i[point_i])) /
                                mask_group->state.dims +
                            float2(0.5f);

      const float2 local_uv = math::fract(pos_2d * (mask_group->subdivisions + 1));
      const int2 grid_xy = int2(math::floor(pos_2d * (mask_group->subdivisions + 1)));

      float4 data_x;
      float4 data_y;
      key_curve_position_weights(local_uv.x, data_x, KeyInterpolationType::KEY_BSPLINE);
      key_curve_position_weights(local_uv.y, data_y, KeyInterpolationType::KEY_BSPLINE);

      /* Prevent distortion around the edges of the grid. */
      if (grid_xy.x == 0) {
        data_x = float4(0.0f, data_x.x * 2.0f + data_x.y, data_x.z - data_x.x, data_x.w);
      }
      if (grid_xy.y == 0) {
        data_y = float4(0.0f, data_y.x * 2.0f + data_y.y, data_y.z - data_y.x, data_y.w);
      }
      if (grid_xy.x == x_num - 2) {
        data_x = float4(data_x.x, data_x.y - data_x.w, data_x.w * 2.0f + data_x.z, 0.0f);
      }
      if (grid_xy.y == y_num - 2) {
        data_y = float4(data_y.x, data_y.y - data_y.w, data_y.w * 2.0f + data_y.z, 0.0f);
      }

      float2 pos_2d_out = float2(0.0f);

      for (const int x : IndexRange(4)) {
        for (const int y : IndexRange(4)) {
          const int2 xy_offset = int2(x - 1, y - 1);
          const float weight = data_x[x] * data_y[y];
          pos_2d_out += weight * xy_to_position(grid_xy + xy_offset);
        }
      }

      positions[point_i] = math::transform_point(mat, float3(pos_2d_out, 0.0f));

      if (mask_group->is_scale_thickness) {
        /* TODO. */
        radii[point_i] = src_radii_i[point_i];
      }
    });

    info.drawing.tag_positions_changed();
  }

  DEG_id_tag_update(&mask_group->grease_pencil_src->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(mask_group->update_data.context,
                        NC_GEOM | ND_DATA | NA_EDITED,
                        mask_group->grease_pencil_src);
}

static void gizmo_bisect_prop_translate_set(const wmGizmo *gz,
                                            wmGizmoProperty *gz_prop,
                                            const void *value_p)
{
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gz->parent_gzgroup->customdata);
  const int index = static_cast<int>(reinterpret_cast<intptr_t>(gz_prop->custom_func.user_data));
  CornerPinData *corner_pin_data = &mask_group->corner_data[index];

  BLI_assert(gz_prop->type->array_length == 3);

  const float *values = static_cast<const float *>(value_p);
  const float3 position_value = float3(values);

  corner_pin_data->corner_pos = float2(position_value.x, position_value.y);

  if (mask_group->mode == DeformMode::PERSPECTIVE) {
    update_positions_prespective(mask_group);
  }
  else if (mask_group->mode == DeformMode::GRID) {
    update_positions_grid(mask_group);
  }
}

static void gizmo_node_box_mask_prop_matrix_get(const wmGizmo * /*gz*/,
                                                wmGizmoProperty *gz_prop,
                                                void * /*value_p*/)
{
  // float (*matrix)[4] = static_cast<float (*)[4]>(value_p);
  BLI_assert(gz_prop->type->array_length == 16);
}

static void gizmo_node_box_mask_prop_matrix_set(const wmGizmo *gz,
                                                wmGizmoProperty *gz_prop,
                                                const void *value_p)
{
  BLI_assert(gz_prop->type->array_length == 16);
  const float (*matrix)[4] = static_cast<const float (*)[4]>(value_p);
  const float4x4 mat = float4x4(matrix);

  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gz->parent_gzgroup->customdata);

  update_positions_transform(mask_group, mat);
}

static std::optional<Bounds<float2>> compute_screen_space_drawing_bounds(
    const IndexMask selection,
    const bke::greasepencil::Drawing &drawing,
    const FunctionRef<float2(const float3 &position)> project_to_screen)
{
  using bke::greasepencil::Drawing;
  using bke::greasepencil::Layer;

  std::optional<Bounds<float2>> drawing_bounds = std::nullopt;

  const VArray<float> radii = drawing.radii();
  const bke::CurvesGeometry &strokes = drawing.strokes();
  const Span<float3> positions = strokes.positions();

  selection.foreach_index(
      [&](const int point_i) {
        const float2 screen_co = project_to_screen(positions[point_i]);
        if (screen_co.x != V2D_IS_CLIPPED) {
          const float pixels = radii[point_i];

          std::optional<Bounds<float2>> point_bounds = Bounds<float2>(screen_co);
          point_bounds->pad(pixels);
          drawing_bounds = bounds::merge(drawing_bounds, point_bounds);
        }
      },
      exec_mode::grain_size(512));

  return drawing_bounds;
}

static void add_corner(QuadWidgetGroup2 *mask_group, wmGizmoGroup *gzgroup)
{
  const wmGizmoType *gzt_move_3d = WM_gizmotype_find("GIZMO_GT_move_3d", false);
  const int i = mask_group->corner_data.size();

  mask_group->corner_data.append({});
  CornerPinData &corner_pin_data = mask_group->corner_data[i];

  corner_pin_data.gizmos = WM_gizmo_new_ptr(gzt_move_3d, gzgroup, nullptr);

  wmGizmo *gz = corner_pin_data.gizmos;

  WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_VALUE, true);

  wmGizmoPropertyFnParams params{};
  params.value_get_fn = gizmo_bisect_prop_translate_get;
  params.value_set_fn = gizmo_bisect_prop_translate_set;
  params.range_get_fn = nullptr;
  params.user_data = reinterpret_cast<void *>(static_cast<intptr_t>(i));
  WM_gizmo_target_property_def_func(gz, "offset", &params);
}

static void update_things(const bContext *C, wmGizmoGroup *gzgroup)
{
  const Scene &scene = *CTX_data_scene(C);

  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gzgroup->customdata);
  wmGizmo *gz = mask_group->border;

  PointerRNA ptr;
  bToolRef *tref = WM_toolsystem_ref_from_context(const_cast<bContext *>(C));
  WM_toolsystem_ref_properties_ensure_from_gizmo_group(tref, gzgroup->type, &ptr);
  mask_group->mode = DeformMode(
      RNA_property_enum_get(&ptr, mask_group->update_data.gzgt_mode_prop));
  mask_group->subdivisions = RNA_property_int_get(&ptr,
                                                  mask_group->update_data.gzgt_subdivisions_prop);

  unit_m4(gz->matrix_space);
  unit_m4(gz->matrix_offset);

  float4x4 mat = float4x4::identity();

  switch (scene.toolsettings->gp_sculpt.lock_axis) {
    case GP_LOCKAXIS_VIEW:
      mat = float4x4(rv3d->viewinv);

      mat[3] = float4(float3(0.0f), 1.0f);
      break;
    case GP_LOCKAXIS_Y:
      mat = float4x4({1.0f, 0.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 1.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 0.0f, 1.0f});
      break;
    case GP_LOCKAXIS_X:
      mat = float4x4({0.0f, 1.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 1.0f, 0.0f},
                     {1.0f, 0.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 0.0f, 1.0f});
      break;
    case GP_LOCKAXIS_Z:
      mat = float4x4::identity();
      break;
    case GP_LOCKAXIS_CURSOR: {
      mat = scene.cursor.matrix<float4x4>();
      break;
    }
  }

  const float4x4 mat_inv = math::invert(mat);

  auto projection_function = [&](const float3 position) {
    return float2(math::transform_point(mat_inv, position));
  };

  std::optional<Bounds<float2>> box;

  const ToolSettings *ts = scene.toolsettings;
  mask_group->is_scale_thickness = ts->gp_sculpt.flag & GP_SCULPT_SETT_FLAG_SCALE_THICKNESS;

  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object->data);

  mask_group->grease_pencil_src = &grease_pencil;
  mask_group->lock_axis = scene.toolsettings->gp_sculpt.lock_axis;

  mask_group->selections = Vector<IndexMask>();
  mask_group->src_positions = Vector<Array<float3>>();
  mask_group->src_radii = Vector<Array<float>>();

  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);
  for (const MutableDrawingInfo &info : drawings) {
    const IndexMask selected_points = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, mask_group->memory);

    mask_group->selections.append(selected_points);
    mask_group->src_positions.append(Array<float3>(info.drawing.strokes().positions()));

    Array<float> radius(info.drawing.strokes().points_num());
    array_utils::copy(info.drawing.strokes().radius(), radius.as_mutable_span());
    mask_group->src_radii.append(radius);

    box = bounds::merge(
        box,
        compute_screen_space_drawing_bounds(selected_points, info.drawing, projection_function));
  }

  mask_group->drawings = drawings;

  if (box) {
    mask_group->state.dims = box->size();
  }
  else {
    mask_group->state.dims = float2(1.0f);
  }

  if (mask_group->mode == DeformMode::GRID) {
    const int x_num = 2 + mask_group->subdivisions;
    const int y_num = 2 + mask_group->subdivisions;

    if (mask_group->corner_data.size() < x_num * y_num) {
      for (const int i : IndexRange::from_begin_end(mask_group->corner_data.size(), x_num * y_num))
      {
        UNUSED_VARS(i);
        add_corner(mask_group, gzgroup);
      }
    }

    for (const int y_i : IndexRange(y_num)) {
      for (const int x_i : IndexRange(x_num)) {
        CornerPinData &corner_pin_data = mask_group->corner_data[x_i + y_i * x_num];
        corner_pin_data.corner_pos = (float2(x_i / float(x_num - 1), y_i / float(y_num - 1)) -
                                      float2(0.5f)) *
                                     mask_group->state.dims;
        wmGizmo *gz = corner_pin_data.gizmos;

        RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_RING_2D);
        gz->scale_basis = gizmo_scale_basis_grid;
      }
    }
  }
  else if (mask_group->mode == DeformMode::PERSPECTIVE) {
    constexpr std::array<float2, 4> corner_positions(
        {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}});

    for (const int i : IndexRange(4)) {
      CornerPinData &corner_pin_data = mask_group->corner_data[i];
      corner_pin_data.corner_pos = corner_positions[i] * mask_group->state.dims;
      wmGizmo *gz = corner_pin_data.gizmos;

      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_CROSS_2D);
      gz->scale_basis = gizmo_scale_basis_perspective;
    }
  }

  WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  for (const int i : mask_group->corner_data.index_range()) {
    wmGizmo *gz2 = mask_group->corner_data[i].gizmos;
    WM_gizmo_set_flag(gz2, WM_GIZMO_HIDDEN, true);
  }

  if (box) {
    mat[3] += mat[0] * box->center().x;
    mat[3] += mat[1] * box->center().y;

    copy_m4_m4(gz->matrix_space, mat.ptr());
    RNA_float_set_array(gz->ptr, "dimensions", box->size());

    for (const int i : mask_group->corner_data.index_range()) {
      wmGizmo *gz2 = mask_group->corner_data[i].gizmos;
      copy_m4_m4(gz2->matrix_space, mat.ptr());
    }

    if (mask_group->mode == DeformMode::TRANSFORM) {
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    }
    else if (mask_group->mode == DeformMode::PERSPECTIVE) {
      for (const int i : IndexRange(4)) {
        wmGizmo *gz2 = mask_group->corner_data[i].gizmos;
        WM_gizmo_set_flag(gz2, WM_GIZMO_HIDDEN, false);
      }
    }
    else if (mask_group->mode == DeformMode::GRID) {
      const int x_num = 2 + mask_group->subdivisions;
      const int y_num = 2 + mask_group->subdivisions;

      for (const int i : IndexRange(x_num * y_num)) {
        wmGizmo *gz2 = mask_group->corner_data[i].gizmos;
        WM_gizmo_set_flag(gz2, WM_GIZMO_HIDDEN, false);
      }
    }
  }

  DEG_id_tag_update(&mask_group->grease_pencil_src->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(mask_group->update_data.context,
                        NC_GEOM | ND_DATA | NA_EDITED,
                        mask_group->grease_pencil_src);
}

static void box_mask_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  QuadWidgetGroup2 *mask_group = MEM_new<QuadWidgetGroup2>(__func__);
  mask_group->border = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);

  mask_group->update_data.context = const_cast<bContext *>(C);

  RNA_enum_set(mask_group->border->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_ROTATE |
                   ED_GIZMO_CAGE_XFORM_FLAG_SCALE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE_SIGNED);

  RNA_enum_set(mask_group->border->ptr,
               "draw_options",
               ED_GIZMO_CAGE_DRAW_FLAG_XFORM_CENTER_HANDLE |
                   ED_GIZMO_CAGE_DRAW_FLAG_CORNER_HANDLES);

  gzgroup->customdata = mask_group;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<QuadWidgetGroup2 *>(customdata));
  };

  wmGizmoPropertyFnParams params{};
  params.value_get_fn = gizmo_node_box_mask_prop_matrix_get;
  params.value_set_fn = gizmo_node_box_mask_prop_matrix_set;
  params.range_get_fn = nullptr;
  WM_gizmo_target_property_def_func(mask_group->border, "matrix", &params);

  for (const int i : IndexRange(4)) {
    UNUSED_VARS(i);
    add_corner(mask_group, gzgroup);
  }

  mask_group->update_data.gzgt_mode_prop = RNA_struct_type_find_property(gzgroup->type->srna,
                                                                         "mode");
  mask_group->update_data.gzgt_subdivisions_prop = RNA_struct_type_find_property(
      gzgroup->type->srna, "subdivisions");

  update_things(C, gzgroup);
}

static bool should_update(const bContext *C, wmGizmoGroup *gzgroup)
{
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gzgroup->customdata);

  const Scene &scene = *CTX_data_scene(C);
  const ToolSettings *ts = scene.toolsettings;

  if (mask_group->is_scale_thickness !=
      bool(ts->gp_sculpt.flag & GP_SCULPT_SETT_FLAG_SCALE_THICKNESS))
  {
    return true;
  }

  if (mask_group->lock_axis != scene.toolsettings->gp_sculpt.lock_axis) {
    return true;
  }

  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;

  Object *object = CTX_data_active_object(C);

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];

    const IndexMask selected_points_1 = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, mask_group->memory);

    const IndexMask selected_points_2 = selections[drawing_i];

    if (selected_points_1 != selected_points_2) {
      return true;
    }
  }

  PointerRNA ptr;
  bToolRef *tref = WM_toolsystem_ref_from_context(const_cast<bContext *>(C));
  WM_toolsystem_ref_properties_ensure_from_gizmo_group(tref, gzgroup->type, &ptr);
  if (mask_group->mode !=
      DeformMode(RNA_property_enum_get(&ptr, mask_group->update_data.gzgt_mode_prop)))
  {
    return true;
  }
  if (mask_group->subdivisions !=
      RNA_property_int_get(&ptr, mask_group->update_data.gzgt_subdivisions_prop))
  {
    return true;
  }

  return false;
}

static void box_mask_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gzgroup->customdata);

  mask_group->update_data.context = const_cast<bContext *>(C);
}

static void bbox_draw_prepare_space_image(const bContext *C, wmGizmoGroup *gzgroup)
{
  if (should_update(C, gzgroup)) {
    update_things(C, gzgroup);
  }
}

void VIEW3D_GGT_grease_pencil_deform(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Deform Gizmo";
  gzgt->idname = "VIEW3D_GGT_grease_pencil_deform";

  gzgt->flag |= WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_PERSISTENT |
                WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = WIDGETGROUP_grease_pencil_quad_poll;
  gzgt->setup = box_mask_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = box_mask_refresh;
  gzgt->draw_prepare = bbox_draw_prepare_space_image;

  RNA_def_enum(gzgt->srna, "mode", prop_deform_modes, int(DeformMode::TRANSFORM), "Mode", "");

  RNA_def_int(gzgt->srna, "subdivisions", 1, 0, 10, "Subdivisions", "", 0, 5);
}

}  // namespace ed::greasepencil

}  // namespace blender
