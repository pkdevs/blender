/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bounds.hh"
#include "BLI_listbase.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_c.hh"

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_scene.hh"

#include "ED_gizmo_library.hh"
#include "ED_gizmo_utils.hh"
#include "ED_grease_pencil.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
// #include "WM_gizmo_types.hh"
#include "WM_types.hh"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"

// /* Local module include. */
// #include "transform.hh"
// #include "transform_gizmo.hh"

namespace blender {

namespace ed::greasepencil {

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

struct QuadWidgetGroup {
  struct {
    wmGizmo *xyz_view[4];
    wmGizmo *icon_button[3][2];
  } gizmos;

  /* Only for view orientation. */
  struct {
    float viewinv_m3[3][3];
  } prev;

  /* We could store more vars here! */
  struct {
    wmOperatorType *ot_spin;
    PropertyRNA *gzgt_axis_prop;
    float orient_mat[3][3];
#ifdef USE_SELECT_CENTER
    float select_center[3];
    float select_center_ortho_axis[3][3];
    bool use_select_center;
#endif
  } data;

  /* Store data for invoke. */
  struct {
    int ortho_axis_active;
  } invoke;
};

static const float dial_angle_partial = M_PI_2;
static const float dial_angle_partial_margin = 0.92f;

/* Use dials only as a visualization when hovering over the icons. */
#define USE_DIAL_HOVER

#define INIT_SCALE_BASE 2.3f
#define INIT_SCALE_BUTTON 0.15f

static const uchar shape_plus[] = {
    0x73, 0x73, 0x73, 0x36, 0x8c, 0x36, 0x8c, 0x73, 0xc9, 0x73, 0xc9, 0x8c, 0x8c,
    0x8c, 0x8c, 0xc9, 0x73, 0xc9, 0x73, 0x8c, 0x36, 0x8c, 0x36, 0x73, 0x36, 0x73,
};

static void WIDGETGROUP_grease_pencil_quad_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  /* alpha values for normal/highlighted states */
  const float alpha = 0.6f;
  const float alpha_hi = 1.0f;
  const float scale_base = INIT_SCALE_BASE;
  const float scale_button = INIT_SCALE_BUTTON;

  QuadWidgetGroup *ggd = MEM_new_zeroed<QuadWidgetGroup>(__func__);
  gzgroup->customdata = ggd;
  const wmGizmoType *gzt_dial = WM_gizmotype_find("GIZMO_GT_dial_3d", true);
  const wmGizmoType *gzt_button = WM_gizmotype_find("GIZMO_GT_button_2d", true);

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 2; j++) {
      wmGizmo *gz = WM_gizmo_new_ptr(gzt_button, gzgroup, nullptr);
      PropertyRNA *prop = RNA_struct_find_property(gz->ptr, "shape");
      RNA_property_string_set_bytes(
          gz->ptr, prop, reinterpret_cast<const char *>(shape_plus), ARRAY_SIZE(shape_plus));

      RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_BACKDROP);

      float color[4];
      ui::theme::get_color_3fv(TH_AXIS_X + i, color);
      color[3] = alpha;
      WM_gizmo_set_color(gz, color);

      WM_gizmo_set_scale(gz, scale_button);
      gz->color[3] = 0.6f;

      gz->flag |= WM_GIZMO_DRAW_OFFSET_SCALE | WM_GIZMO_OPERATOR_TOOL_INIT;

      ggd->gizmos.icon_button[i][j] = gz;
    }
  }

  for (int i = 0; i < ARRAY_SIZE(ggd->gizmos.xyz_view); i++) {
    wmGizmo *gz = WM_gizmo_new_ptr(gzt_dial, gzgroup, nullptr);
    ui::theme::get_color_3fv(TH_GIZMO_PRIMARY, gz->color);
    WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_VALUE | WM_GIZMO_HIDDEN_SELECT, true);
    ggd->gizmos.xyz_view[i] = gz;
  }

  for (int i = 0; i < 3; i++) {
    wmGizmo *gz = ggd->gizmos.xyz_view[i];
#ifndef USE_DIAL_HOVER
    RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_CLIP);
#endif
    WM_gizmo_set_line_width(gz, 2.0f);
    float color[4];
    ui::theme::get_color_3fv(TH_AXIS_X + i, color);
    color[3] = alpha;
    WM_gizmo_set_color(gz, color);
    color[3] = alpha_hi;
    WM_gizmo_set_color_highlight(gz, color);
    WM_gizmo_set_scale(gz, INIT_SCALE_BASE);
    RNA_float_set(gz->ptr,
                  "arc_partial_angle",
                  (M_PI * 2) - (dial_angle_partial * dial_angle_partial_margin));
  }

  {
    wmGizmo *gz = ggd->gizmos.xyz_view[3];
    WM_gizmo_set_line_width(gz, 2.0f);
    float color[4];
    copy_v3_fl(color, 1.0f);
    color[3] = alpha;
    WM_gizmo_set_color(gz, color);
    color[3] = alpha_hi;
    WM_gizmo_set_color_highlight(gz, color);
    WM_gizmo_set_scale(gz, scale_base);
  }

#ifdef USE_DIAL_HOVER
  for (int i = 0; i < 4; i++) {
    wmGizmo *gz = ggd->gizmos.xyz_view[i];
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }
#endif

  ggd->data.ot_spin = WM_operatortype_find("MESH_OT_spin", true);
  ggd->data.gzgt_axis_prop = RNA_struct_type_find_property(gzgroup->type->srna, "axis");
}

// struct QuadWidgetGroup {
//   wmGizmo *gizmo[3][2];
//   /** View aligned gizmos. */
//   wmGizmo *gizmo_view[4];

//   /* Only for view orientation. */
//   struct {
//     float viewinv_m3[3][3];
//   } prev;
// };

// static void WIDGETGROUP_grease_pencil_quad_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
// {
//   QuadWidgetGroup *xgzgroup = MEM_new_uninitialized<QuadWidgetGroup>(__func__);
//   const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_3d", true);
//   wmOperatorType *ot_shear = WM_operatortype_find("TRANSFORM_OT_shear", true);

//   float axis_color[3][3];
//   for (int i = 0; i < 3; i++) {
//     ui::theme::get_color_3fv(TH_AXIS_X + i, axis_color[i]);
//   }

//   for (int i = 0; i < 3; i++) {
//     for (int j = 0; j < 2; j++) {
//       wmGizmo *gz = WM_gizmo_new_ptr(gzt_arrow, gzgroup, nullptr);
//       RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_BOX);
//       const int i_ortho_a = (i + j + 1) % 3;
//       const int i_ortho_b = (i + (1 - j) + 1) % 3;
//       interp_v3_v3v3(gz->color, axis_color[i_ortho_a], axis_color[i_ortho_b], 0.75f);
//       gz->color[3] = 0.5f;
//       PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_shear, nullptr);
//       RNA_boolean_set(ptr, "release_confirm", true);
//       xgzgroup->gizmo[i][j] = gz;
//     }
//   }

//   for (int i = 0; i < 4; i++) {
//     wmGizmo *gz = WM_gizmo_new_ptr(gzt_arrow, gzgroup, nullptr);
//     RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_BOX);
//     RNA_enum_set(gz->ptr, "draw_options", 0); /* No stem. */
//     copy_v3_fl(gz->color, 1.0f);
//     gz->color[3] = 0.5f;
//     WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_OFFSET_SCALE, true);
//     PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_shear, nullptr);
//     RNA_boolean_set(ptr, "release_confirm", true);
//     xgzgroup->gizmo_view[i] = gz;

//     /* Unlike the other gizmos, this never changes so can be set on setup. */
//     wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
//     RNA_enum_set(&gzop->ptr, "orient_type", V3D_ORIENT_VIEW);

//     RNA_enum_set(&gzop->ptr, "orient_axis", 2);
//     RNA_enum_set(&gzop->ptr, "orient_axis_ortho", ((i % 2) ? 0 : 1));
//   }

//   gzgroup->customdata = xgzgroup;
// }

static void WIDGETGROUP_grease_pencil_quad_refresh(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  // Scene *scene = CTX_data_scene(C);
  // ARegion *region = CTX_wm_region(C);
  // RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  QuadWidgetGroup *ggd = static_cast<QuadWidgetGroup *>(gzgroup->customdata);
  // TransformBounds tbounds;

  // /* Needed to test view orientation changes. */
  // copy_m3_m4(xgzgroup->prev.viewinv_m3, rv3d->viewinv);

  // TransformOrientationSlot *orient_slot = BKE_scene_orientation_slot_get_from_flag(
  //     scene, SCE_ORIENT_ROTATE);
  // const int orient_index = BKE_scene_orientation_slot_get_index(orient_slot);

  // TransformCalcParams calc_params{};
  // calc_params.use_local_axis = false;
  // calc_params.orientation_index = orient_index + 1;
  // if (calc_gizmo_stats(C, &calc_params, &tbounds, rv3d) == 0) {
  //   for (int i = 0; i < 3; i++) {
  //     for (int j = 0; j < 2; j++) {
  //       wmGizmo *gz = xgzgroup->gizmo[i][j];
  //       WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  //     }
  //   }

  //   for (int i = 0; i < 4; i++) {
  //     wmGizmo *gz = xgzgroup->gizmo_view[i];
  //     WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  //   }
  // }
  // else {
  //   gizmo_prepare_mat(C, rv3d, &tbounds);
  //   for (int i = 0; i < 3; i++) {
  //     for (int j = 0; j < 2; j++) {
  //       wmGizmo *gz = xgzgroup->gizmo[i][j];
  //       WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
  //       WM_gizmo_set_flag(gz, WM_GIZMO_MOVE_CURSOR, true);

  //       wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
  //       const int i_ortho_a = (i + j + 1) % 3;
  //       const int i_ortho_b = (i + (1 - j) + 1) % 3;
  //       WM_gizmo_set_matrix_rotation_from_yz_axis(gz, rv3d->twmat[i_ortho_a], rv3d->twmat[i]);
  //       WM_gizmo_set_matrix_location(gz, rv3d->twmat[3]);

  //       RNA_float_set_array(&gzop->ptr, "orient_matrix", &tbounds.axis[0][0]);
  //       RNA_enum_set(&gzop->ptr, "orient_type", orient_slot->type);

  //       RNA_enum_set(&gzop->ptr, "orient_axis", i_ortho_b);
  //       RNA_enum_set(&gzop->ptr, "orient_axis_ortho", i_ortho_a);

  //       mul_v3_fl(gz->matrix_basis[0], 0.5f);
  //       mul_v3_fl(gz->matrix_basis[1], 6.0f);
  //     }
  //   }

  //   for (int i = 0; i < 4; i++) {
  //     wmGizmo *gz = xgzgroup->gizmo_view[i];
  //     WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
  //   }
  // }

  // RegionView3D *rv3d = ED_view3d_context_rv3d(const_cast<bContext *>(C));

  for (int i = 0; i < ARRAY_SIZE(ggd->gizmos.icon_button); i++) {
    for (int j = 0; j < 2; j++) {
      wmGizmo *gz = ggd->gizmos.icon_button[i][j];

      const float3 gizmo_center = float3(i, j, 0.0f);

      WM_gizmo_set_matrix_location(gz, gizmo_center);
    }
  }

  //   ed::transform::calc_orientation_from_type(C, ggd->data.orient_mat);
  //   for (int i = 0; i < 3; i++) {
  //     const int axis_ortho = (i + ORTHO_AXIS_OFFSET) % 3;
  //     const float *axis_ortho_vec = ggd->data.orient_mat[axis_ortho];
  // #ifdef USE_SELECT_CENTER
  //     if (ggd->data.use_select_center) {
  //       float delta[3];
  //       sub_v3_v3v3(delta, ggd->data.select_center, gizmo_center);
  //       project_plane_normalized_v3_v3v3(
  //           ggd->data.select_center_ortho_axis[i], delta, ggd->data.orient_mat[i]);
  //       if (normalize_v3(ggd->data.select_center_ortho_axis[i]) != 0.0f) {
  //         axis_ortho_vec = ggd->data.select_center_ortho_axis[i];
  //       }
  //     }
  // #endif
  //     gizmo_mesh_spin_init_refresh_axis_orientation(
  //         gzgroup, i, ggd->data.orient_mat[i], axis_ortho_vec);
  //   }

  //   {
  //     gizmo_mesh_spin_init_refresh_axis_orientation(gzgroup, 3, rv3d->viewinv[2], nullptr);
  //   }

  // #ifdef USE_SELECT_CENTER
  //   {
  //     Object *obedit = CTX_data_edit_object(C);
  //     BMEditMesh *em = BKE_editmesh_from_object(obedit);
  //     float select_center[3] = {0};
  //     int totsel = 0;

  //     BMesh *bm = em->bm;
  //     BMVert *eve;
  //     BMIter iter;

  //     BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
  //       if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
  //         if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
  //           totsel++;
  //           add_v3_v3(select_center, eve->co);
  //         }
  //       }
  //     }
  //     if (totsel) {
  //       mul_v3_fl(select_center, 1.0f / totsel);
  //       mul_m4_v3(obedit->object_to_world().ptr(), select_center);
  //       copy_v3_v3(ggd->data.select_center, select_center);
  //       ggd->data.use_select_center = true;
  //     }
  //     else {
  //       ggd->data.use_select_center = false;
  //     }
  //   }
  // #endif

  //   for (int i = 0; i < ARRAY_SIZE(ggd->gizmos.icon_button); i++) {
  //     const int axis_ortho = (i + ORTHO_AXIS_OFFSET) % 3;
  //     const float *axis_ortho_vec = ggd->data.orient_mat[axis_ortho];
  //     float offset = INIT_SCALE_BASE / INIT_SCALE_BUTTON;
  //     float offset_vec[3];

  // #ifdef USE_SELECT_CENTER
  //     if (ggd->data.use_select_center && !is_zero_v3(ggd->data.select_center_ortho_axis[i])) {
  //       axis_ortho_vec = ggd->data.select_center_ortho_axis[i];
  //     }
  // #endif

  //     mul_v3_v3fl(offset_vec, axis_ortho_vec, offset);
  //     for (int j = 0; j < 2; j++) {
  //       wmGizmo *gz = ggd->gizmos.icon_button[i][j];
  //       float mat3[3][3];
  //       axis_angle_to_mat3(mat3, ggd->data.orient_mat[i], dial_angle_partial * (j ? -0.5f :
  //       0.5f)); mul_v3_m3v3(gz->matrix_offset[3], mat3, offset_vec);
  //     }
  //   }

  //   {
  //     PointerRNA ptr;
  //     bToolRef *tref = WM_toolsystem_ref_from_context(const_cast<bContext *>(C));
  //     WM_toolsystem_ref_properties_ensure_from_gizmo_group(tref, gzgroup->type, &ptr);
  //     const int axis_flag = RNA_property_enum_get(&ptr, ggd->data.gzgt_axis_prop);
  //     for (int i = 0; i < ARRAY_SIZE(ggd->gizmos.icon_button); i++) {
  //       for (int j = 0; j < 2; j++) {
  //         wmGizmo *gz = ggd->gizmos.icon_button[i][j];
  //         WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, (axis_flag & (1 << i)) == 0);
  //       }
  //     }
  //   }

  //   /* Needed to test view orientation changes. */
  //   copy_m3_m4(ggd->prev.viewinv_m3, rv3d->viewinv);
}

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

  struct {
    PointerRNA ptr;
    PropertyRNA *prop;
    bContext *context;
  } update_data;
};

static std::optional<Bounds<float2>> compute_screen_space_drawing_bounds(
    const RegionView3D &rv3d,
    Object &object,
    const int layer_index,
    const IndexMask selection,
    const bke::greasepencil::Drawing &drawing)
{
  using bke::greasepencil::Drawing;
  using bke::greasepencil::Layer;

  std::optional<Bounds<float2>> drawing_bounds = std::nullopt;

  BLI_assert(object.type == OB_GREASE_PENCIL);
  GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object.data);

  const Layer &layer = *grease_pencil.layers()[layer_index];
  const float4x4 layer_to_world = layer.to_world_space(object);
  const VArray<float> radii = drawing.radii();
  const bke::CurvesGeometry &strokes = drawing.strokes();
  const Span<float3> positions = strokes.positions();

  selection.foreach_index(
      [&](const int point_i) {
        const float2 screen_co = float2(positions[point_i].x, positions[point_i].z);
        // const float2 screen_co = this->project_to_screen(layer_to_world, positions[point_i]);

        if (screen_co.x != V2D_IS_CLIPPED) {
          const float3 world_pos = math::transform_point(layer_to_world, positions[point_i]);
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

  float3 placement_normal_;

  unit_m4(gz->matrix_space);

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

  std::optional<Bounds<float2>> box;

  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object->data);

  mask_group->grease_pencil_src = &grease_pencil;

  mask_group->selections = Vector<IndexMask>();
  mask_group->src_positions = Vector<Array<float3>>();
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);
  for (const MutableDrawingInfo &info : drawings) {
    IndexMaskMemory memory;
    const IndexMask selected_points = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, memory);

    mask_group->selections.append(selected_points);
    mask_group->src_positions.append(Array<float3>(info.drawing.strokes().positions()));

    box = bounds::merge(box,
                        compute_screen_space_drawing_bounds(
                            *rv3d, *object, info.layer_index, selected_points, info.drawing));
  }

  mask_group->drawings = drawings;

  if (box) {
    printf("box %f, %f\n", box->size().x, box->size().y);

    mul_v3_fl(gz->matrix_space[0], box->size().x);
    mul_v3_fl(gz->matrix_space[1], box->size().y);

    gz->matrix_space[3][0] = box->center().x;
    gz->matrix_space[3][2] = box->center().y;

    // RNA_float_set_array(gz->ptr, "dimensions", box->size());
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
  }
  else {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }

  // box.min = float2(-1.0f, -1.0f);
  // box.max = float2(1.0f, 1.0f);

  // mul_v3_fl(gz->matrix_space[0], box.size().x);
  // mul_v3_fl(gz->matrix_space[1], box.size().y);

  // gz->matrix_space[3][0] = box.center().x;
  // gz->matrix_space[3][1] = box.center().y;
}

static void box_mask_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  QuadWidgetGroup2 *mask_group = MEM_new<QuadWidgetGroup2>(__func__);
  mask_group->border = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);

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

  update_things(C, gzgroup);
}

static void gizmo_node_box_mask_prop_matrix_get(const wmGizmo *gz,
                                                wmGizmoProperty *gz_prop,
                                                void *value_p)
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
  float4x4 mat = float4x4(matrix);
  BLI_assert(gz_prop->type->array_length == 16);

  printf("gizmo_node_box_mask_prop_matrix_set\n");

  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gz->parent_gzgroup->customdata);

  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;
  const Vector<Array<float3>> &src_positions = mask_group->src_positions;

  const float4x4 transform = float4x4(gz->matrix_space) * mat *
                             math::invert(float4x4(gz->matrix_space));

  printf("transform: \n");
  printf("%f %f %f %f\n", transform[0][0], transform[1][0], transform[2][0], transform[3][0]);
  printf("%f %f %f %f\n", transform[0][1], transform[1][1], transform[2][1], transform[3][1]);
  printf("%f %f %f %f\n", transform[0][2], transform[1][2], transform[2][2], transform[3][2]);
  printf("%f %f %f %f\n", transform[0][3], transform[1][3], transform[2][3], transform[3][3]);
  printf("\n");

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];
    const IndexMask selected_points = selections[drawing_i];
    const Span<float3> src_positions_i = src_positions[drawing_i];

    bke::CurvesGeometry &strokes = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = strokes.positions_for_write();

    selected_points.foreach_index([&](const int point_i) {
      positions[point_i] = math::transform_point(transform, src_positions_i[point_i]);
    });

    info.drawing.tag_positions_changed();

    // box = bounds::merge(box,
    //                     compute_screen_space_drawing_bounds(
    //                         *rv3d, *object, info.layer_index, selected_points, info.drawing));
  }

  DEG_id_tag_update(&mask_group->grease_pencil_src->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(mask_group->update_data.context,
                        NC_GEOM | ND_DATA | NA_EDITED,
                        mask_group->grease_pencil_src);

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
  Main *bmain = CTX_data_main(C);
  QuadWidgetGroup2 *mask_group = static_cast<QuadWidgetGroup2 *>(gzgroup->customdata);
  wmGizmo *gz = mask_group->border;

  bool update = false;

  const Vector<MutableDrawingInfo> drawings = mask_group->drawings;
  const Vector<IndexMask> selections = mask_group->selections;

  Object *object = CTX_data_active_object(C);

  for (const int drawing_i : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[drawing_i];

    IndexMaskMemory memory;
    const IndexMask selected_points_1 = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, memory);

    const IndexMask selected_points_2 = selections[drawing_i];

    if (selected_points_1 != selected_points_2) {
      update = true;
    }
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

void bbox_draw_prepare_space_image(const bContext *C, wmGizmoGroup *gzgroup)
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
}

}  // namespace ed::greasepencil

}  // namespace blender
