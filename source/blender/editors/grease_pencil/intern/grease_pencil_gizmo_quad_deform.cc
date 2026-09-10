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
#include "BKE_scene.hh"
#include "BKE_tracking.hh"

#include "ED_gizmo_library.hh"
#include "ED_gizmo_utils.hh"
#include "ED_grease_pencil.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
// #include "WM_gizmo_types.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

// /* Local module include. */
// #include "transform.hh"
// #include "transform_gizmo.hh"

namespace blender {

namespace ed::greasepencil {

enum class DeformMode : int8_t {
  FREE = 0,
  PERSPECTIVE = 1,
  GRID = 2,
};

static const EnumPropertyItem prop_deform_modes[] = {
    {int(DeformMode::FREE), "FREE", 0, "Free", "TODO"},
    {int(DeformMode::PERSPECTIVE), "PERSPECTIVE", 0, "Perspective", "TODO"},
    {int(DeformMode::GRID), "GRID", 0, "Grid", "TODO"},
    {0, nullptr, 0, nullptr, nullptr},
};

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
  int index;
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

  GreasePencil *grease_pencil_src;

  IndexMaskMemory memory;
  DeformMode mode;

  struct {
    wmGizmo *gizmos[4];
    CornerPinData *corner_data[4];
  } corner_pin;

  struct {
    PointerRNA ptr;
    PropertyRNA *prop;
    PropertyRNA *gzgt_mode_prop;
    bContext *context;
  } update_data;
};

static std::optional<Bounds<float2>> compute_screen_space_drawing_bounds(
    const RegionView3D & /*rv3d*/,
    Object &object,
    const int /*layer_index*/,
    const IndexMask selection,
    const bke::greasepencil::Drawing &drawing,
    const FunctionRef<float2(const float3 &position)> project_to_screen)
{
  using bke::greasepencil::Drawing;
  using bke::greasepencil::Layer;

  std::optional<Bounds<float2>> drawing_bounds = std::nullopt;

  BLI_assert(object.type == OB_GREASE_PENCIL);
  // GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object.data);

  // const Layer &layer = *grease_pencil.layers()[layer_index];
  // const float4x4 layer_to_world = layer.to_world_space(object);
  const VArray<float> radii = drawing.radii();
  const bke::CurvesGeometry &strokes = drawing.strokes();
  const Span<float3> positions = strokes.positions();

  selection.foreach_index(
      [&](const int point_i) {
        const float2 screen_co = project_to_screen(positions[point_i]);
        // const float2 screen_co = float2(positions[point_i].x, positions[point_i].z);
        // const float2 screen_co = this->project_to_screen(layer_to_world, positions[point_i]);

        if (screen_co.x != V2D_IS_CLIPPED) {
          // const float3 world_pos = math::transform_point(layer_to_world, positions[point_i]);
          // const float pixels = radii[point_i] / ED_view3d_pixel_size(&rv3d, world_pos);
          const float pixels = radii[point_i];

          std::optional<Bounds<float2>> point_bounds = Bounds<float2>(screen_co);
          point_bounds->pad(pixels);
          drawing_bounds = bounds::merge(drawing_bounds, point_bounds);
        }
      },
      exec_mode::grain_size(512));

  return drawing_bounds;
}

static void update_things(const bContext *C, wmGizmoGroup *gzgroup)
{
  const Scene &scene = *CTX_data_scene(C);

  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
  // wmGizmo *gz = gzgroup->gizmos.first();
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gzgroup->customdata);
  wmGizmo *gz = mask_group->border;

  // SpaceImage *sima = CTX_wm_space_image(C);
  // const float2 offset = float2{sima->xof, sima->yof} * sima->zoom;

  // node_gizmo_calc_matrix_space(region, sima->zoom, offset, gz->matrix_space);

  PointerRNA ptr;
  bToolRef *tref = WM_toolsystem_ref_from_context(const_cast<bContext *>(C));
  WM_toolsystem_ref_properties_ensure_from_gizmo_group(tref, gzgroup->type, &ptr);
  mask_group->mode = DeformMode(
      RNA_property_enum_get(&ptr, mask_group->update_data.gzgt_mode_prop));

  float3 placement_normal_;

  unit_m4(gz->matrix_space);
  unit_m4(gz->matrix_offset);

  switch (scene.toolsettings->gp_sculpt.lock_axis) {
    case GP_LOCKAXIS_VIEW:
      // plane_ = DrawingPlacementPlane::View;
      break;
    case GP_LOCKAXIS_Y:
      // plane_ = DrawingPlacementPlane::Front;
      placement_normal_ = float3(0, 1, 0);

      rotate_m4(gz->matrix_space, 'X', M_PI / 2.0f);
      break;
    case GP_LOCKAXIS_X:
      // plane_ = DrawingPlacementPlane::Side;
      placement_normal_ = float3(1, 0, 0);

      rotate_m4(gz->matrix_space, 'Y', M_PI / 2.0f);
      rotate_m4(gz->matrix_space, 'Z', M_PI / 2.0f);
      break;
    case GP_LOCKAXIS_Z:
      // plane_ = DrawingPlacementPlane::Top;
      placement_normal_ = float3(0, 0, 1);
      break;
    case GP_LOCKAXIS_CURSOR: {
      // plane_ = DrawingPlacementPlane::Cursor;

      // rotate_m4(gz->matrix_space, 'X', M_PI / 2.0f);

      placement_normal_ = scene.cursor.matrix<float3x3>() * float3(0, 0, 1);
      break;
    }
  }

  auto projection_function = [&](const float3 position) {
    switch (scene.toolsettings->gp_sculpt.lock_axis) {
      case GP_LOCKAXIS_VIEW:
        // plane_ = DrawingPlacementPlane::View;
        break;
      case GP_LOCKAXIS_Y:
        // plane_ = DrawingPlacementPlane::Front;

        return float2(position.x, position.z);

        break;
      case GP_LOCKAXIS_X:
        // plane_ = DrawingPlacementPlane::Side;

        return float2(position.y, position.z);

        break;
      case GP_LOCKAXIS_Z:
        // plane_ = DrawingPlacementPlane::Top;
        return float2(position.x, position.y);
        break;
      case GP_LOCKAXIS_CURSOR: {
        // plane_ = DrawingPlacementPlane::Cursor;

        // placement_normal_ = scene.cursor.matrix<float3x3>() * float3(0, 0, 1);
        break;
      }
    }

    return float2(position.x, position.y);
  };

  std::optional<Bounds<float2>> box;

  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object->data);

  mask_group->grease_pencil_src = &grease_pencil;

  mask_group->selections = Vector<IndexMask>();
  mask_group->src_positions = Vector<Array<float3>>();
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);
  for (const MutableDrawingInfo &info : drawings) {
    const IndexMask selected_points = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, mask_group->memory);

    mask_group->selections.append(selected_points);
    mask_group->src_positions.append(Array<float3>(info.drawing.strokes().positions()));

    box = bounds::merge(
        box,
        compute_screen_space_drawing_bounds(
            *rv3d, *object, info.layer_index, selected_points, info.drawing, projection_function));
  }

  mask_group->drawings = drawings;

  if (box) {
    printf("box %f, %f\n", box->size().x, box->size().y);

    mul_v3_fl(gz->matrix_space[0], box->size().x);
    mul_v3_fl(gz->matrix_space[1], box->size().y);

    gz->matrix_space[3][0] = box->center().x;
    gz->matrix_space[3][2] = box->center().y;

    for (int i = 0; i < 4; i++) {
      wmGizmo *gz2 = mask_group->corner_pin.gizmos[i];
      copy_m4_m4(gz2->matrix_space, gz->matrix_space);
    }

    if (mask_group->mode == DeformMode::FREE) {
      // RNA_float_set_array(gz->ptr, "dimensions", box->size());
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
      for (int i = 0; i < 4; i++) {
        wmGizmo *gz2 = mask_group->corner_pin.gizmos[i];
        WM_gizmo_set_flag(gz2, WM_GIZMO_HIDDEN, true);
      }
    }
    else if (mask_group->mode == DeformMode::PERSPECTIVE || mask_group->mode == DeformMode::GRID) {
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
      for (int i = 0; i < 4; i++) {
        wmGizmo *gz2 = mask_group->corner_pin.gizmos[i];
        WM_gizmo_set_flag(gz2, WM_GIZMO_HIDDEN, false);
      }
    }
  }
  else {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);

    for (int i = 0; i < 4; i++) {
      wmGizmo *gz2 = mask_group->corner_pin.gizmos[i];
      WM_gizmo_set_flag(gz2, WM_GIZMO_HIDDEN, true);
    }
  }

  // box.min = float2(-1.0f, -1.0f);
  // box.max = float2(1.0f, 1.0f);

  // mul_v3_fl(gz->matrix_space[0], box.size().x);
  // mul_v3_fl(gz->matrix_space[1], box.size().y);

  // gz->matrix_space[3][0] = box.center().x;
  // gz->matrix_space[3][1] = box.center().y;
}

/* translate callbacks */
static void gizmo_bisect_prop_translate_get(const wmGizmo * /*gz*/,
                                            wmGizmoProperty *gz_prop,
                                            void *value_p)
{
  BLI_assert(gz_prop->type->array_length == 3);
  float *values = static_cast<float *>(value_p);
  // GizmoGroup *ggd = static_cast<GizmoGroup *>(gz->parent_gzgroup->customdata);
  // wmOperator *op = ggd->data.op;

  const CornerPinData *corner_pin_data = static_cast<CornerPinData *>(
      gz_prop->custom_func.user_data);

  values[0] = corner_pin_data->corner_pos[0];
  values[1] = corner_pin_data->corner_pos[1];
  values[2] = 0.0f;
  // UNUSED_VARS_NDEBUG(gz_prop);

  // corner_pin_data->corner_pos

  // RNA_property_float_get_array(op->ptr, ggd->data.prop_plane_co, static_cast<float *>(value_p));
}

static std::optional<float3x3> compute_homography_matrix(const QuadWidgetGroup2 *mask_group)
{
  float2 lower_left = mask_group->corner_pin.corner_data[0]->corner_pos + float2(0.5f);
  float2 lower_right = mask_group->corner_pin.corner_data[1]->corner_pos + float2(0.5f);
  float2 upper_right = mask_group->corner_pin.corner_data[2]->corner_pos + float2(0.5f);
  float2 upper_left = mask_group->corner_pin.corner_data[3]->corner_pos + float2(0.5f);

  if (!is_quad_convex_v2(lower_left, lower_right, upper_right, upper_left)) {
    return std::nullopt;
  }

  float3x3 homography_matrix;
  float corners[4][2] = {{lower_left.x, lower_left.y},
                         {lower_right.x, lower_right.y},
                         {upper_right.x, upper_right.y},
                         {upper_left.x, upper_left.y}};
  float identity_corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
  BKE_tracking_homography_between_two_quads(identity_corners, corners, homography_matrix.ptr());
  return homography_matrix;
}

static void update_positions_transform(QuadWidgetGroup2 *mask_group, const float4x4 &mat)
{
  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;
  const Vector<Array<float3>> &src_positions = mask_group->src_positions;

  const float4x4 transform = float4x4(mask_group->border->matrix_space) * mat *
                             math::invert(float4x4(mask_group->border->matrix_space));

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];
    const IndexMask &selected_points = selections[drawing_i];
    const Span<float3> src_positions_i = src_positions[drawing_i];

    bke::CurvesGeometry &strokes = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = strokes.positions_for_write();

    selected_points.foreach_index([&](const int point_i) {
      positions[point_i] = math::transform_point(transform, src_positions_i[point_i]);
    });

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

  const float4x4 mat = float4x4(mask_group->border->matrix_space);
  const float4x4 mat_inv = math::invert(mat);

  const std::optional<float3x3> homography_mat = compute_homography_matrix(mask_group);

  /* If the matrix can not be created leave the all points at the last valid position. */
  if (!homography_mat) {
    return;
  }

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];
    const IndexMask &selected_points = selections[drawing_i];
    const Span<float3> src_positions_i = src_positions[drawing_i];

    bke::CurvesGeometry &strokes = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = strokes.positions_for_write();

    selected_points.foreach_index([&](const int point_i) {
      const float2 pos_2d = float2(math::transform_point(mat_inv, src_positions_i[point_i])) +
                            float2(0.5f);

      const float3 pos_3d_out = (*homography_mat) * float3(pos_2d, 1.0f);
      const float2 pos_2d_out = float2(pos_3d_out.x, pos_3d_out.y) / pos_3d_out.z;

      positions[point_i] = math::transform_point(mat, float3(pos_2d_out - float2(0.5f), 0.0f));
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

  const float4x4 mat = float4x4(mask_group->border->matrix_space);
  const float4x4 mat_inv = math::invert(mat);

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];
    const IndexMask &selected_points = selections[drawing_i];
    const Span<float3> src_positions_i = src_positions[drawing_i];

    bke::CurvesGeometry &strokes = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = strokes.positions_for_write();
    selected_points.foreach_index([&](const int point_i) {
      const float2 lower_left = mask_group->corner_pin.corner_data[0]->corner_pos;
      const float2 lower_right = mask_group->corner_pin.corner_data[1]->corner_pos;
      const float2 upper_right = mask_group->corner_pin.corner_data[2]->corner_pos;
      const float2 upper_left = mask_group->corner_pin.corner_data[3]->corner_pos;

      const float2 pos_2d = float2(math::transform_point(mat_inv, src_positions_i[point_i])) +
                            float2(0.5f);

      const float2 pos_2d_out = math::interpolate(
          math::interpolate(lower_left, lower_right, pos_2d.x),
          math::interpolate(upper_left, upper_right, pos_2d.x),
          pos_2d.y);

      positions[point_i] = math::transform_point(mat, float3(pos_2d_out, 0.0f));
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
  BLI_assert(gz_prop->type->array_length == 3);
  // GizmoGroup *ggd = static_cast<GizmoGroup *>(gz->parent_gzgroup->customdata);
  // wmOperator *op = ggd->data.op;

  const float *values = static_cast<const float *>(value_p);
  const float3 position_value = float3(values);

  CornerPinData *corner_pin_data = static_cast<CornerPinData *>(gz_prop->custom_func.user_data);

  corner_pin_data->corner_pos = float2(position_value.x, position_value.y);

  // UNUSED_VARS_NDEBUG(gz_prop);

  // RNA_property_float_set_array(
  //     op->ptr, ggd->data.prop_plane_co, static_cast<const float *>(value_p));

  // gizmo_bisect_exec(ggd);

  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gz->parent_gzgroup->customdata);

  if (mask_group->mode == DeformMode::PERSPECTIVE) {
    update_positions_prespective(mask_group);
  }
  else if (mask_group->mode == DeformMode::GRID) {
    update_positions_grid(mask_group);
  }
}

static void box_mask_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  QuadWidgetGroup2 *mask_group = MEM_new<QuadWidgetGroup2>(__func__);
  mask_group->border = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);
  const wmGizmoType *gzt_move_3d = WM_gizmotype_find("GIZMO_GT_move_3d", false);

  RNA_enum_set(mask_group->border->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_ROTATE |
                   ED_GIZMO_CAGE_XFORM_FLAG_SCALE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE_SIGNED);

  RNA_enum_set(mask_group->border->ptr,
               "draw_options",
               ED_GIZMO_CAGE_DRAW_FLAG_XFORM_CENTER_HANDLE |
                   ED_GIZMO_CAGE_DRAW_FLAG_CORNER_HANDLES);

  for (int i = 0; i < 4; i++) {
    mask_group->corner_pin.gizmos[i] = WM_gizmo_new_ptr(gzt_move_3d, gzgroup, nullptr);
    wmGizmo *gz = mask_group->corner_pin.gizmos[i];
    // RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_CROSS_2D);
    RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_CROSS_2D);

    WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_VALUE, true);
    // WM_gizmo_set_flag(gz2, WM_GIZMO_DRAW_MODAL, true);

    gz->scale_basis = 0.1f;
  }

  gzgroup->customdata = mask_group;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<QuadWidgetGroup2 *>(customdata));
  };

  constexpr std::array<float2, 4> corner_positions(
      {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}});

  for (int i = 0; i < 4; i++) {
    mask_group->corner_pin.corner_data[i] = MEM_new<CornerPinData>(__func__);

    wmGizmo *gz = mask_group->corner_pin.gizmos[i];
    CornerPinData *corner_pin_data = mask_group->corner_pin.corner_data[i];

    corner_pin_data->index = i;
    corner_pin_data->corner_pos = corner_positions[i];

    wmGizmoPropertyFnParams params{};
    params.value_get_fn = gizmo_bisect_prop_translate_get;
    params.value_set_fn = gizmo_bisect_prop_translate_set;
    params.range_get_fn = nullptr;
    params.user_data = corner_pin_data;
    WM_gizmo_target_property_def_func(gz, "offset", &params);
  }

  mask_group->update_data.gzgt_mode_prop = RNA_struct_type_find_property(gzgroup->type->srna,
                                                                         "mode");

  update_things(C, gzgroup);
}

static void gizmo_node_box_mask_prop_matrix_get(const wmGizmo * /*gz*/,
                                                wmGizmoProperty *gz_prop,
                                                void * /*value_p*/)
{
  // float (*matrix)[4] = static_cast<float (*)[4]>(value_p);
  BLI_assert(gz_prop->type->array_length == 16);

  printf("gizmo_node_box_mask_prop_matrix_get\n");
  // NodeBBoxWidgetGroup *mask_group = static_cast<NodeBBoxWidgetGroup *>(
  //     gz->parent_gzgroup->customdata);
  // const float2 dims = mask_group->state.dims;
  // const float2 offset = mask_group->state.offset;
  // const bNode *node = static_cast<const bNode *>(gz_prop->custom_func.user_data);
  // const float aspect = dims.x / dims.y;

  // float loc[3], rot[3][3], size[3];
  // mat4_to_loc_rot_size(loc, rot, size, matrix);

  // const bNodeSocket *rotation_input = bke::node_find_socket(*node, SOCK_IN, "Rotation"_ustr);
  // const float rotation = rotation_input->default_value_typed<bNodeSocketValueFloat>()->value;
  // axis_angle_to_mat3_single(rot, 'Z', rotation);

  // const bNodeSocket *position_input = bke::node_find_socket(*node, SOCK_IN, "Position"_ustr);
  // const float2 position = position_input->default_value_typed<bNodeSocketValueVector>()->value;
  // loc[0] = (position.x - 0.5) * dims.x + offset.x;
  // loc[1] = (position.y - 0.5) * dims.y + offset.y;
  // loc[2] = 0;

  // const bNodeSocket *size_input = bke::node_find_socket(*node, SOCK_IN, "Size"_ustr);
  // const float2 size_value = size_input->default_value_typed<bNodeSocketValueVector>()->value;
  // size[0] = size_value.x;
  // size[1] = size_value.y * aspect;
  // size[2] = 1;

  // loc_rot_size_to_mat4(matrix, loc, rot, size);
}

// static void gizmo_node_bbox_update(NodeBBoxWidgetGroup *bbox_group)
// {
//   RNA_property_update(
//       bbox_group->update_data.context, &bbox_group->update_data.ptr,
//       bbox_group->update_data.prop);
// }

// struct Data

static void gizmo_node_box_mask_prop_matrix_set(const wmGizmo *gz,
                                                wmGizmoProperty *gz_prop,
                                                const void *value_p)
{
  const float (*matrix)[4] = static_cast<const float (*)[4]>(value_p);
  const float4x4 mat = float4x4(matrix);
  BLI_assert(gz_prop->type->array_length == 16);

  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gz->parent_gzgroup->customdata);

  update_positions_transform(mask_group, mat);

  // const float2 dims = mask_group->state.dims;
  // const float2 offset = mask_group->state.offset;
  // bNode *node = static_cast<bNode *>(gz_prop->custom_func.user_data);

  // bNodeSocket *position_input = bke::node_find_socket(*node, SOCK_IN, "Position"_ustr);
  // const float2 position = position_input->default_value_typed<bNodeSocketValueVector>()->value;

  // bNodeSocket *size_input = bke::node_find_socket(*node, SOCK_IN, "Size"_ustr);
  // const float2 size_value = size_input->default_value_typed<bNodeSocketValueVector>()->value;

  // const float aspect = dims.x / dims.y;
  // rctf rct;
  // rct.xmin = position.x - size_value.x / 2;
  // rct.xmax = position.x + size_value.x / 2;
  // rct.ymin = position.y - size_value.y / 2;
  // rct.ymax = position.y + size_value.y / 2;

  // float loc[3];
  // float rot[3][3];
  // float size[3];
  // mat4_to_loc_rot_size(loc, rot, size, matrix);

  // float eul[3];

  // /* Rotation can't be extracted from matrix when the gizmo width or height is zero. */
  // if (size[0] != 0 and size[1] != 0) {
  //   mat4_to_eul(eul, matrix);
  //   bNodeSocket *rotation_input = bke::node_find_socket(*node, SOCK_IN, "Rotation"_ustr);
  //   rotation_input->default_value_typed<bNodeSocketValueFloat>()->value = eul[2];
  // }

  // BLI_rctf_resize(&rct, fabsf(size[0]), fabsf(size[1]) / aspect);
  // BLI_rctf_recenter(
  //     &rct, ((loc[0] - offset.x) / dims.x) + 0.5, ((loc[1] - offset.y) / dims.y) + 0.5);

  // size_input->default_value_typed<bNodeSocketValueVector>()->value[0] = size[0];
  // size_input->default_value_typed<bNodeSocketValueVector>()->value[1] = size[1] / aspect;
  // position_input->default_value_typed<bNodeSocketValueVector>()->value[0] = rct.xmin + size[0] /
  // 2; position_input->default_value_typed<bNodeSocketValueVector>()->value[1] = rct.ymin +
  //                                                                           size[1] / aspect /
  //                                                                           2;

  // gizmo_node_bbox_update(mask_group);
}

static void box_mask_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  // Main *bmain = CTX_data_main(C);
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gzgroup->customdata);
  wmGizmo *gz = mask_group->border;

  bool update = false;

  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;

  Object *object = CTX_data_active_object(C);

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];

    const IndexMask selected_points_1 = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, mask_group->memory);

    const IndexMask selected_points_2 = selections[drawing_i];

    if (selected_points_1 != selected_points_2) {
      update = true;
    }
  }

  PointerRNA ptr;
  bToolRef *tref = WM_toolsystem_ref_from_context(const_cast<bContext *>(C));
  WM_toolsystem_ref_properties_ensure_from_gizmo_group(tref, gzgroup->type, &ptr);
  if (mask_group->mode !=
      DeformMode(RNA_property_enum_get(&ptr, mask_group->update_data.gzgt_mode_prop)))
  {
    update = true;
  }

  if (update) {
    update_things(C, gzgroup);
  }

  // void *lock;
  // Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Render Result");
  // ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  // if (ibuf == nullptr) [[unlikely]] {
  //   WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  //   BKE_image_release_ibuf(ima, ibuf, lock);
  //   return;
  // }

  // mask_group->state.dims = node_gizmo_safe_calc_dims(ibuf, GIZMO_NODE_DEFAULT_DIMS);
  // mask_group->state.offset = flag_is_set(ibuf->flags, ImBufFlags::HasDisplayWindow) ?
  //                                float2(ibuf->display_offset) :
  //                                float2(0.0f);

  // RNA_float_set_array(gz->ptr, "dimensions", mask_group->state.dims);
  // WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);

  // const SpaceNode *snode = find_active_node_editor(C);
  // BLI_assert(snode != nullptr);

  // bNode *node = bke::node_get_active(*snode->edittree);

  mask_group->update_data.context = const_cast<bContext *>(C);
  // bNodeSocket *source_input = bke::node_find_socket(*node, SOCK_IN, "Mask"_ustr);
  // mask_group->update_data.ptr = RNA_pointer_create_discrete(
  //     reinterpret_cast<ID *>(snode->edittree), RNA_NodeSocket, source_input);
  // mask_group->update_data.prop = RNA_struct_find_property(&mask_group->update_data.ptr,
  // "enabled"); BLI_assert(mask_group->update_data.prop != nullptr);

  wmGizmoPropertyFnParams params{};
  params.value_get_fn = gizmo_node_box_mask_prop_matrix_get;
  params.value_set_fn = gizmo_node_box_mask_prop_matrix_set;
  params.range_get_fn = nullptr;
  // params.user_data = node;
  // params.foreach_rna_prop_fn = gizmo_node_box_mask_foreach_rna_prop;
  WM_gizmo_target_property_def_func(gz, "matrix", &params);

  // BKE_image_release_ibuf(ima, ibuf, lock);
}

static void bbox_draw_prepare_space_image(const bContext * /*C*/, wmGizmoGroup * /*gzgroup*/)
{
  // update_things(C, gzgroup);
}

void VIEW3D_GGT_grease_pencil_quad(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Quad Deform";
  gzgt->idname = "VIEW3D_GGT_grease_pencil_quad";

  // gzgt->flag |= WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_DRAW_MODAL_EXCLUDE |
  //               WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP |
  //               WM_GIZMOGROUPTYPE_DELAY_REFRESH_FOR_TWEAK;

  // gzgt->flag |= WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP |
  //               WM_GIZMOGROUPTYPE_DELAY_REFRESH_FOR_TWEAK;

  gzgt->flag |= WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_PERSISTENT |
                WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = WIDGETGROUP_grease_pencil_quad_poll;
  gzgt->setup = box_mask_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = box_mask_refresh;
  // gzgt->message_subscribe = WIDGETGROUP_xform_shear_message_subscribe;
  gzgt->draw_prepare = bbox_draw_prepare_space_image;

  RNA_def_enum(gzgt->srna, "mode", prop_deform_modes, int(DeformMode::FREE), "Mode", "");

  // RNA_def_enum_flag(gzgt->srna, "axis", rna_enum_axis_flag_xyz_items, (1 << 2), "Axis", "");
}

}  // namespace ed::greasepencil

}  // namespace blender
