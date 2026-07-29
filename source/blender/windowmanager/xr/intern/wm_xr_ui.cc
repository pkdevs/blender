/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * \name Window-Manager XR UI
 *
 * Implements Blender XR world-space UI management and interaction.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BLI_listbase.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_time.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "DNA_view3d_types.h"
#include "ED_screen.hh"
#include "ED_view3d_offscreen.hh"
#include "UI_interface_c.hh"
#include "UI_view2d.hh"
#include "interface_intern.hh"

#include "GHOST_Xr-api.hh"

#include "GPU_batch_presets.hh"
#include "GPU_framebuffer.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "MEM_guardedalloc.h"

#include "wm_xr_intern.hh"

namespace blender {

static CLG_LogRef LOG = {"xr"};

struct wmXrUiRegionHostContextOverride {
  bContext *C;
  wmWindow *prev_win;
  ScrArea *prev_area;
  ARegion *prev_region;

  wmXrUiRegionHostContextOverride(bContext *context, const wmXrUiRegion *ui_region) : C(context)
  {
    prev_win = CTX_wm_window(C);
    prev_area = CTX_wm_area(C);
    prev_region = CTX_wm_region(C);

    CTX_wm_window_set(C, ui_region->ui_region_host_win);
    CTX_wm_area_set(C, ui_region->ui_region_host_area);
    CTX_wm_region_set(C, ui_region->ui_region_host_region);
  }

  ~wmXrUiRegionHostContextOverride()
  {
    CTX_wm_window_set(C, prev_win);
    CTX_wm_area_set(C, prev_area);
    CTX_wm_region_set(C, prev_region);
  }
};

/* Switches the context to a world space temp region for the duration of this object's lifecycle.
 */
struct wmXrTempRegionContextOverride {
  bContext *C;
  wmWindow *prev_win;
  ScrArea *prev_area;
  ARegion *prev_region;
  ARegion *prev_region_popup;

  wmXrTempRegionContextOverride(bContext *context,
                                const wmXrUiRegion *ui_region,
                                ARegion *popup_region)
      : C(context)
  {
    prev_win = CTX_wm_window(C);
    prev_area = CTX_wm_area(C);
    prev_region = CTX_wm_region(C);
    prev_region_popup = CTX_wm_region_popup(C);

    CTX_wm_window_set(C, ui_region->ui_region_host_win);
    CTX_wm_area_set(C, ui_region->ui_region_host_area);
    CTX_wm_region_set(C, popup_region);
    CTX_wm_region_popup_set(C, popup_region);
  }

  ~wmXrTempRegionContextOverride()
  {
    CTX_wm_window_set(C, prev_win);
    CTX_wm_area_set(C, prev_area);
    CTX_wm_region_set(C, prev_region);
    CTX_wm_region_popup_set(C, prev_region_popup);
  }
};

static bool wm_xr_button_uses_selected_draw_override(const ui::Button &but)
{
  return ELEM(but.type,
              ui::ButtonType::Toggle,
              ui::ButtonType::ToggleN,
              ui::ButtonType::IconToggle,
              ui::ButtonType::IconToggleN,
              ui::ButtonType::ButToggle,
              ui::ButtonType::Checkbox,
              ui::ButtonType::CheckboxN);
}

/* Temporarily marks checked buttons as `selected` for the lifetime of this object.
 * This is used when rendering a world space ARegion, in order to ensure that
 * selected buttons appear properly selected. */
struct wmXrSelectedToggleDrawStateScope {
  Vector<ui::Button *> draw_selected_buttons;

  explicit wmXrSelectedToggleDrawStateScope(ARegion *region)
  {
    if (region == nullptr || region->runtime == nullptr) {
      return;
    }

    for (ui::Block &block : region->runtime->uiblocks) {
      for (ui::Button &but : block.buttons()) {
        if (!wm_xr_button_uses_selected_draw_override(but) || !ui::button_is_pushed(&but) ||
            (but.flag & (ui::UI_SELECT | ui::UI_SELECT_DRAW)) != 0)
        {
          continue;
        }

        ui::button_flag_enable(&but, ui::UI_SELECT_DRAW);
        draw_selected_buttons.append(&but);
      }
    }
  }

  ~wmXrSelectedToggleDrawStateScope()
  {
    for (ui::Button *but : draw_selected_buttons) {
      ui::button_flag_disable(but, ui::UI_SELECT_DRAW);
    }
  }
};

static wmXrUiRegion *wm_xr_ui_region_find(wmXrSurfaceData *surface_data,
                                          const wmWindow *win,
                                          const ScrArea *area,
                                          const ARegion *region,
                                          const eWMXrUiRegionMountPoint mount_point)
{
  if (surface_data == nullptr) {
    return nullptr;
  }

  for (wmXrUiRegion *ui_region : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
    if (ui_region->ui_region_host_win == win && ui_region->ui_region_host_area == area &&
        ui_region->ui_region_host_region == region && ui_region->mount_point == mount_point)
    {
      return ui_region;
    }
  }
  return nullptr;
}

static wmXrUiRegion *wm_xr_ui_region_find_by_host(wmXrSurfaceData *surface_data,
                                                  const wmWindow *win,
                                                  const ScrArea *area,
                                                  const ARegion *region)
{
  if (surface_data == nullptr || win == nullptr || area == nullptr || region == nullptr) {
    return nullptr;
  }

  if (surface_data->active_ui_region != nullptr &&
      surface_data->active_ui_region->ui_region_host_win == win &&
      surface_data->active_ui_region->ui_region_host_area == area &&
      surface_data->active_ui_region->ui_region_host_region == region)
  {
    return surface_data->active_ui_region;
  }

  wmXrUiRegion *match = nullptr;
  for (wmXrUiRegion *ui_region : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
    if (ui_region->ui_region_host_win != win || ui_region->ui_region_host_area != area ||
        ui_region->ui_region_host_region != region)
    {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = ui_region;
  }

  return match;
}

static wmXrUiRegion *wm_xr_ui_region_find_by_host_region(wmXrSurfaceData *surface_data,
                                                         const wmWindow *win,
                                                         const ARegion *region)
{
  if (surface_data == nullptr || win == nullptr || region == nullptr) {
    return nullptr;
  }

  if (surface_data->active_ui_region != nullptr &&
      surface_data->active_ui_region->ui_region_host_win == win &&
      surface_data->active_ui_region->ui_region_host_region == region)
  {
    return surface_data->active_ui_region;
  }

  wmXrUiRegion *match = nullptr;
  for (wmXrUiRegion *ui_region : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
    if (ui_region->ui_region_host_win != win || ui_region->ui_region_host_region != region) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = ui_region;
  }

  return match;
}

static wmXrTempRegion *wm_xr_temp_region_find(wmXrUiRegion *ui_region, const ARegion *region)
{
  if (ui_region == nullptr || region == nullptr) {
    return nullptr;
  }

  for (wmXrTempRegion *temp_region : ListBaseWrapper<wmXrTempRegion>(ui_region->child_regions)) {
    if (temp_region->region == region) {
      return temp_region;
    }
  }

  return nullptr;
}

static wmXrTempRegion *wm_xr_temp_region_find_any(wmXrSurfaceData *surface_data,
                                                  const ARegion *region,
                                                  wmXrUiRegion **r_ui_region)
{
  if (r_ui_region != nullptr) {
    *r_ui_region = nullptr;
  }
  if (surface_data == nullptr || region == nullptr) {
    return nullptr;
  }

  for (wmXrUiRegion *ui_region : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
    if (wmXrTempRegion *temp_region = wm_xr_temp_region_find(ui_region, region)) {
      if (r_ui_region != nullptr) {
        *r_ui_region = ui_region;
      }
      return temp_region;
    }
  }

  return nullptr;
}

/* Resolves XR ownership once when a temporary region is created.
 * The source may be the XR host region itself, another already-registered XR temp region,
 * or the currently active XR ui_region for operator-driven popup chains.
 */
static wmXrUiRegion *wm_xr_ui_region_find_by_source_region(wmXrSurfaceData *surface_data,
                                                           wmWindow *win,
                                                           ScrArea *area,
                                                           ARegion *source_region)
{
  if (surface_data == nullptr || source_region == nullptr) {
    return nullptr;
  }

  if (source_region->regiontype == RGN_TYPE_XR) {
    if (area == nullptr) {
      return wm_xr_ui_region_find_by_host_region(surface_data, win, source_region);
    }
    return wm_xr_ui_region_find_by_host(surface_data, win, area, source_region);
  }

  wmXrUiRegion *ui_region = nullptr;
  wm_xr_temp_region_find_any(surface_data, source_region, &ui_region);
  if (ui_region != nullptr) {
    return ui_region;
  }

  if (surface_data->active_ui_region != nullptr &&
      surface_data->active_ui_region->ui_region_host_win == win)
  {
    return surface_data->active_ui_region;
  }

  return nullptr;
}

static bool wm_xr_temp_region_rect_update(const wmWindow *win, wmXrTempRegion *temp_region)
{
  if (win == nullptr || temp_region == nullptr || temp_region->region == nullptr) {
    return false;
  }

  rcti clipped_rect = temp_region->region->winrct;
  if (win->runtime != nullptr && win->runtime->ghostwin == nullptr) {
    if (BLI_rcti_size_x(&clipped_rect) <= 0 || BLI_rcti_size_y(&clipped_rect) <= 0) {
      temp_region->valid = false;
      return false;
    }
    temp_region->region_rect = clipped_rect;
    return true;
  }
  const rcti window_bounds = {
      0, std::max(0, int(win->sizex) - 1), 0, std::max(0, int(win->sizey) - 1)};
  if (!BLI_rcti_isect(&clipped_rect, &window_bounds, &clipped_rect)) {
    temp_region->valid = false;
    return false;
  }

  temp_region->region_rect = clipped_rect;
  return true;
}

static bool wm_xr_temp_region_offscreen_ensure(wmXrTempRegion *temp_region,
                                               const int px_width,
                                               const int px_height)
{
  if (temp_region == nullptr || px_width <= 0 || px_height <= 0) {
    return false;
  }

  if (temp_region->offscreen != nullptr) {
    if (GPU_offscreen_width(temp_region->offscreen) == px_width &&
        GPU_offscreen_height(temp_region->offscreen) == px_height)
    {
      return true;
    }

    GPU_offscreen_free(temp_region->offscreen);
    temp_region->offscreen = nullptr;
  }

  temp_region->offscreen = GPU_offscreen_create(px_width,
                                                px_height,
                                                false,
                                                gpu::TextureFormat::SRGBA_8_8_8_8,
                                                GPU_TEXTURE_USAGE_SHADER_READ,
                                                false,
                                                nullptr);
  return temp_region->offscreen != nullptr;
}

static void wm_xr_temp_region_draw_direct(const bContext *C,
                                          const wmXrUiRegion *ui_region,
                                          ARegion *region)
{
  if (C == nullptr || ui_region == nullptr || region == nullptr || region->runtime == nullptr ||
      region->runtime->type == nullptr || region->runtime->type->draw == nullptr)
  {
    return;
  }

  region->runtime->do_draw |= RGN_DRAWING;
  wmPartialViewport(&region->runtime->drawrct, &region->winrct, &region->runtime->drawrct);
  wmOrtho2_region_pixelspace(region);
  ui::theme::theme_set(ui_region->ui_region_host_area ? ui_region->ui_region_host_area->spacetype :
                                                        0,
                       region->runtime->type->regionid);
  /* Temporary popups depend on their own region draw callback; using the generic region path
   * regressed into uniform background-only output in XR. */
  region->runtime->type->draw(const_cast<bContext *>(C), region);
  ED_region_pixelspace(region);
  region->runtime->drawrct = rcti{};
  region->runtime->do_draw &= ~RGN_DRAWING;
}

static bool wm_xr_temp_region_cache_update(const bContext *C,
                                           wmXrUiRegion *ui_region,
                                           wmXrTempRegion *temp_region)
{
  if (C == nullptr || ui_region == nullptr || temp_region == nullptr ||
      temp_region->region == nullptr || ui_region->ui_region_host_win == nullptr)
  {
    return false;
  }

  ARegion *region = temp_region->region;
  if (region->runtime == nullptr || region->runtime->type == nullptr) {
    return false;
  }
  if (!wm_xr_temp_region_rect_update(ui_region->ui_region_host_win, temp_region)) {
    return false;
  }

  const int px_width = BLI_rcti_size_x(&temp_region->region_rect) + 1;
  const int px_height = BLI_rcti_size_y(&temp_region->region_rect) + 1;
  if (px_width <= 0 || px_height <= 0) {
    return false;
  }

  bContext *mutable_C = const_cast<bContext *>(C);
  wmXrTempRegionContextOverride context_override(mutable_C, ui_region, region);
  rcti winrct_prev = region->winrct;
  const int winx_prev = region->winx;
  const int winy_prev = region->winy;
  const int sizex_prev = region->sizex;
  const int sizey_prev = region->sizey;
  BLI_rcti_init(&region->winrct, 0, px_width - 1, 0, px_height - 1);
  region->winx = px_width;
  region->winy = px_height;
  region->sizex = px_width;
  region->sizey = px_height;
  ED_region_update_rect(region);
  region->runtime->visible = true;

  if (region->runtime->type != nullptr && region->runtime->type->layout != nullptr) {
    wmViewport(&region->winrct);
    region->runtime->type->layout(mutable_C, region);
  }

  if (!wm_xr_temp_region_offscreen_ensure(temp_region, px_width, px_height)) {
    region->winrct = winrct_prev;
    region->winx = winx_prev;
    region->winy = winy_prev;
    region->sizex = sizex_prev;
    region->sizey = sizey_prev;
    ED_region_update_rect(region);
    return false;
  }

  GPU_offscreen_bind(temp_region->offscreen, false);
  gpu::FrameBuffer *framebuffer = nullptr;
  gpu::Texture *color_texture = nullptr;
  gpu::Texture *depth_texture = nullptr;
  GPU_offscreen_viewport_data_get(
      temp_region->offscreen, &framebuffer, &color_texture, &depth_texture);
  if (framebuffer != nullptr) {
    GPU_framebuffer_viewport_reset(framebuffer);
  }
  GPU_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
  GPU_scissor_test(true);
  GPU_scissor(0, 0, px_width, px_height);
  GPU_matrix_push_projection();
  GPU_matrix_push();
  wmOrtho2_region_pixelspace(region);
  ui::blocklist_update_window_matrix(mutable_C, &region->runtime->uiblocks);
  ui::blocklist_update_view_for_buttons(mutable_C, &region->runtime->uiblocks);
  GPU_matrix_pop();
  GPU_matrix_pop_projection();
  wm_xr_temp_region_draw_direct(mutable_C, ui_region, region);
  GPU_scissor_test(false);
  GPU_offscreen_unbind(temp_region->offscreen, false);
  if (color_texture != nullptr) {
    GPU_texture_mipmap_mode(color_texture, false, false);
  }
  region->winrct = winrct_prev;
  region->winx = winx_prev;
  region->winy = winy_prev;
  region->sizex = sizex_prev;
  region->sizey = sizey_prev;
  ED_region_update_rect(region);

  temp_region->valid = true;
  temp_region->z_offset = 1.0f;
  return true;
}

void wm_xr_temp_region_draw_to_world_quad(const float viewmat[4][4],
                                          const float winmat[4][4],
                                          const wmXrUiRegion *ui_region,
                                          const wmXrTempRegion *temp_region)
{
  if (ui_region == nullptr || temp_region == nullptr || !temp_region->valid ||
      temp_region->region == nullptr || ui_region->ui_region_host_region == nullptr)
  {
    return;
  }

  gpu::Texture *color_texture = temp_region->offscreen ?
                                    GPU_offscreen_color_texture(temp_region->offscreen) :
                                    nullptr;
  if (color_texture == nullptr) {
    return;
  }

  const int px_width = BLI_rcti_size_x(&temp_region->region_rect) + 1;
  const int px_height = BLI_rcti_size_y(&temp_region->region_rect) + 1;
  const int offset_x = temp_region->region_rect.xmin -
                       ui_region->ui_region_host_region->winrct.xmin;
  const int offset_y = temp_region->region_rect.ymin -
                       ui_region->ui_region_host_region->winrct.ymin;

  float obmat[4][4];
  copy_m4_m4(obmat, ui_region->ui_region_obmat);
  madd_v3_v3fl(obmat[3], ui_region->ui_region_obmat[0], float(offset_x));
  madd_v3_v3fl(obmat[3], ui_region->ui_region_obmat[1], float(offset_y));
  madd_v3_v3fl(obmat[3], ui_region->ui_region_obmat[2], temp_region->z_offset);

  GPU_color_mask(true, true, true, true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_face_culling(GPU_CULL_NONE);
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);

  GPU_matrix_push_projection();
  GPU_matrix_projection_set(winmat);
  GPU_matrix_push();
  GPU_matrix_set(viewmat);
  GPU_matrix_mul(obmat);

  float q0[3] = {0.0f, 0.0f, 0.0f};
  float q1[3] = {float(px_width), 0.0f, 0.0f};
  float q2[3] = {float(px_width), float(px_height), 0.0f};
  float q3[3] = {0.0f, float(px_height), 0.0f};

  GPUVertFormat *fmt = immVertexFormat();
  uint a_pos = GPU_vertformat_attr_add(fmt, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  uint a_uv = GPU_vertformat_attr_add(fmt, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immBindTexture("image", color_texture);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(a_uv, 0.0f, 0.0f);
  immVertex3fv(a_pos, q0);
  immAttr2f(a_uv, 1.0f, 0.0f);
  immVertex3fv(a_pos, q1);
  immAttr2f(a_uv, 1.0f, 1.0f);
  immVertex3fv(a_pos, q2);
  immAttr2f(a_uv, 0.0f, 1.0f);
  immVertex3fv(a_pos, q3);
  immEnd();

  immUnbindProgram();
  GPU_matrix_pop();
  GPU_matrix_pop_projection();
}

static void wm_xr_temp_regions_clear(wmXrUiRegion *ui_region)
{
  if (ui_region == nullptr) {
    return;
  }

  while (
      wmXrTempRegion *temp_region = static_cast<wmXrTempRegion *>(ui_region->child_regions.first))
  {
    BLI_remlink(&ui_region->child_regions, temp_region);
    if (temp_region->offscreen != nullptr) {
      GPU_offscreen_free(temp_region->offscreen);
    }
    MEM_delete(temp_region);
  }
}

static void wm_xr_ui_region_free(wmXrSurfaceData *surface_data, wmXrUiRegion *ui_region)
{
  if (surface_data == nullptr || ui_region == nullptr) {
    return;
  }
  wm_xr_temp_regions_clear(ui_region);
  if (ui_region->ui_region_offscreen != nullptr) {
    GPU_offscreen_free(ui_region->ui_region_offscreen);
  }
  if (surface_data->active_ui_region == ui_region) {
    surface_data->active_ui_region = nullptr;
  }
  wm_xr_ui_region_host_free(ui_region);
  BLI_remlink(&surface_data->ui_regions, ui_region);
  MEM_delete(ui_region);
}

bool WM_xr_temp_region_register(ARegion *region, wmWindow *win, ScrArea *area, ARegion *xr_region)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (surface_data == nullptr || region == nullptr || win == nullptr || xr_region == nullptr) {
    return false;
  }

  if (wm_xr_temp_region_find_any(surface_data, region, nullptr) != nullptr) {
    return true;
  }

  wmXrUiRegion *ui_region = wm_xr_ui_region_find_by_source_region(
      surface_data, win, area, xr_region);
  if (ui_region == nullptr) {
    return false;
  }

  wmXrTempRegion *temp_region = MEM_new_zeroed<wmXrTempRegion>(__func__);
  temp_region->region = region;
  BLI_addtail(&ui_region->child_regions, temp_region);
  ui_region->ui_region_dirty = true;
  ED_region_tag_redraw(region);
  if (region->runtime != nullptr) {
    region->runtime->do_draw |= RGN_REFRESH_UI;
  }

  return true;
}

void WM_xr_temp_region_unregister(ARegion *region)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (surface_data == nullptr || region == nullptr) {
    return;
  }

  wmXrUiRegion *ui_region = nullptr;
  wmXrTempRegion *temp_region = wm_xr_temp_region_find_any(surface_data, region, &ui_region);
  if (temp_region == nullptr || ui_region == nullptr) {
    return;
  }

  BLI_remlink(&ui_region->child_regions, temp_region);
  ui_region->ui_region_dirty = true;
  if (temp_region->offscreen != nullptr) {
    GPU_offscreen_free(temp_region->offscreen);
  }
  MEM_delete(temp_region);
}

bool WM_xr_temp_region_is_registered(const ARegion *region)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (surface_data == nullptr || region == nullptr) {
    return false;
  }

  return wm_xr_temp_region_find_any(surface_data, region, nullptr) != nullptr;
}

void WM_xr_surface_ui_region_mount_set(wmXrData *xr, eWMXrUiRegionMountPoint mount_point)
{
  if (xr == nullptr || xr->runtime == nullptr) {
    return;
  }
  xr->runtime->ui_region_mount_point = mount_point;
}

static eWMXrUiRegionMountPoint wm_xr_ui_region_mount_point_resolve(const wmXrData *xr,
                                                                   const ARegion *region)
{
  if (xr != nullptr && xr->runtime != nullptr &&
      xr->runtime->ui_region_mount_point != XR_UI_REGION_MOUNT_NONE)
  {
    return xr->runtime->ui_region_mount_point;
  }
  if (region == nullptr || region->runtime == nullptr || region->runtime->type == nullptr) {
    return XR_UI_REGION_MOUNT_NONE;
  }

  eWMXrUiRegionMountPoint mount_point = XR_UI_REGION_MOUNT_NONE;
  for (PanelType *panel_type : ListBaseWrapper<PanelType>(region->runtime->type->paneltypes)) {
    const eWMXrUiRegionMountPoint ui_region_mount = eWMXrUiRegionMountPoint(
        panel_type->xr_mount_point);
    if (ui_region_mount == XR_UI_REGION_MOUNT_NONE) {
      continue;
    }
    if (mount_point != XR_UI_REGION_MOUNT_NONE && mount_point != ui_region_mount) {
      return XR_UI_REGION_MOUNT_NONE;
    }
    mount_point = ui_region_mount;
  }

  return mount_point;
}

static eWMXrUiRegionMountPoint wm_xr_ui_region_type_mount_point_get(const PanelType *panel_type)
{
  for (const PanelType *current = panel_type; current != nullptr; current = current->parent) {
    if (current->xr_mount_point != XR_UI_REGION_MOUNT_NONE) {
      return eWMXrUiRegionMountPoint(current->xr_mount_point);
    }
  }
  return XR_UI_REGION_MOUNT_NONE;
}

static ARegionType *wm_xr_ui_region_region_type_get()
{
  SpaceType *space_type = BKE_spacetype_from_id(SPACE_VIEW3D);
  return (space_type != nullptr) ? BKE_regiontype_from_id(space_type, RGN_TYPE_XR) : nullptr;
}

static bool wm_xr_ui_region_mount_point_visible(const wmXrData *xr,
                                                const eWMXrUiRegionMountPoint mount_point)
{
  switch (mount_point) {
    case XR_UI_REGION_MOUNT_LEFT_HAND:
    case XR_UI_REGION_MOUNT_RIGHT_HAND:
      return xr->session_settings.show_controller_ui;
    case XR_UI_REGION_MOUNT_HEAD_FOLLOW:
    case XR_UI_REGION_MOUNT_WORLD:
      return xr->session_settings.show_world_space_ui;
    default:
      return false;
  }
}

static int wm_xr_ui_region_mount_points_collect(const wmXrData *xr,
                                                const ARegionType *region_type,
                                                eWMXrUiRegionMountPoint r_mount_points[4])
{
  if (xr != nullptr && xr->runtime != nullptr &&
      xr->runtime->ui_region_mount_point != XR_UI_REGION_MOUNT_NONE)
  {
    if (wm_xr_ui_region_mount_point_visible(xr, xr->runtime->ui_region_mount_point)) {
      r_mount_points[0] = xr->runtime->ui_region_mount_point;
      return 1;
    }
    return 0;
  }
  if (region_type == nullptr) {
    return 0;
  }

  int count = 0;
  for (const PanelType *panel_type : ConstListBaseWrapper<PanelType>(region_type->paneltypes)) {
    const eWMXrUiRegionMountPoint mount_point = wm_xr_ui_region_type_mount_point_get(panel_type);
    if (mount_point == XR_UI_REGION_MOUNT_NONE) {
      continue;
    }
    if (!wm_xr_ui_region_mount_point_visible(xr, mount_point)) {
      continue;
    }
    if (!ELEM(mount_point,
              XR_UI_REGION_MOUNT_LEFT_HAND,
              XR_UI_REGION_MOUNT_RIGHT_HAND,
              XR_UI_REGION_MOUNT_HEAD_FOLLOW,
              XR_UI_REGION_MOUNT_WORLD))
    {
      continue;
    }
    bool already_added = false;
    for (int i = 0; i < count; i++) {
      if (r_mount_points[i] == mount_point) {
        already_added = true;
        break;
      }
    }
    if (!already_added && count < 4) {
      r_mount_points[count++] = mount_point;
    }
  }

  return count;
}

class wmXrUiRegionTypeFilterScope {
  struct PanelTypeLink {
    PanelTypeLink *next, *prev;
    PanelType *panel_type;
  };

  ARegion *region_;
  ListBase original_paneltypes_ = {nullptr, nullptr};

 public:
  wmXrUiRegionTypeFilterScope(ARegion *region, eWMXrUiRegionMountPoint mount_point)
      : region_(region)
  {
    if (region_ == nullptr || region_->runtime == nullptr || region_->runtime->type == nullptr) {
      return;
    }

    ListBase &paneltypes = region_->runtime->type->paneltypes;
    for (PanelType *panel_type : ListBaseWrapper<PanelType>(paneltypes)) {
      PanelTypeLink *link = MEM_new<PanelTypeLink>(__func__);
      link->panel_type = panel_type;
      BLI_addtail(&original_paneltypes_, link);
    }

    BLI_listbase_clear(&paneltypes);
    for (PanelTypeLink *link : ListBaseWrapper<PanelTypeLink>(original_paneltypes_)) {
      if (wm_xr_ui_region_type_mount_point_get(link->panel_type) == mount_point) {
        BLI_addtail(&paneltypes, link->panel_type);
      }
    }
  }

  ~wmXrUiRegionTypeFilterScope()
  {
    if (region_ == nullptr || region_->runtime == nullptr || region_->runtime->type == nullptr) {
      return;
    }

    ListBase &paneltypes = region_->runtime->type->paneltypes;
    BLI_listbase_clear(&paneltypes);
    for (PanelTypeLink *link : ListBaseWrapper<PanelTypeLink>(original_paneltypes_)) {
      BLI_addtail(&paneltypes, link->panel_type);
    }
    BLI_freelistN(&original_paneltypes_);
  }
};

static void wm_xr_region_ensure_layout_rect(ARegion *region)
{
  if (region == nullptr || region->runtime == nullptr || region->runtime->type == nullptr) {
    return;
  }
  if (BLI_rcti_size_x(&region->winrct) > 0 && BLI_rcti_size_y(&region->winrct) > 0) {
    return;
  }

  const int width = (region->runtime->type->prefsizex > 0) ? region->runtime->type->prefsizex :
                                                             UI_SIDEBAR_PANEL_WIDTH;
  const int height = (region->sizey > 1) ? region->sizey : 2048;

  BLI_rcti_init(&region->winrct, 0, width - 1, 0, height - 1);
  region->sizex = width;
  region->sizey = height;
  ED_region_update_rect(region);
}

static void wm_xr_ui_region_default_transform_init(wmXrUiRegion *ui_region)
{
  const float half_pi = 3.1415f * 0.5f;
  const float screen_to_world_scale = 1.0f / 1536.0f;
  float pos[3] = {0.0f, 0.0f, 2.0f};
  float rot[3] = {half_pi, 0.0f, 0.0f};
  float size[3] = {screen_to_world_scale, screen_to_world_scale, screen_to_world_scale};
  loc_eul_size_to_mat4(ui_region->ui_region_obmat, pos, rot, size);
}

static float wm_xr_ui_region_scale_get(const wmXrUiRegion *ui_region)
{
  if (ui_region != nullptr && ui_region->mount_point == XR_UI_REGION_MOUNT_HEAD_FOLLOW) {
    return 1.0f / 500.0f;
  }
  return 1.0f / 1536.0f;
}

bool wm_xr_controller_object_mat_calc(const wmXrSessionState *state,
                                      const char *subaction_path,
                                      const bool require_grip_pose,
                                      const float offset[3],
                                      const float rotation[3],
                                      const float scale[3],
                                      float r_mat[4][4])
{
  if (state == nullptr || subaction_path == nullptr || offset == nullptr || rotation == nullptr ||
      scale == nullptr || r_mat == nullptr)
  {
    return false;
  }

  const wmXrController *controller = nullptr;
  bool use_grip_pose = false;
  for (const wmXrController *candidate : ConstListBaseWrapper<wmXrController>(state->controllers))
  {
    if (STREQ(candidate->subaction_path, subaction_path)) {
      if (candidate->grip_active) {
        controller = candidate;
        use_grip_pose = true;
        break;
      }
      if (!require_grip_pose && candidate->aim_active) {
        controller = candidate;
        use_grip_pose = false;
        break;
      }
      return false;
    }
  }

  if (controller == nullptr) {
    return false;
  }

  const float (*controller_mat)[4] = use_grip_pose ? controller->grip_mat : controller->aim_mat;
  float local_mat[4][4];
  loc_eul_size_to_mat4(local_mat, offset, rotation, scale);
  mul_m4_m4m4(r_mat, controller_mat, local_mat);
  return true;
}

static void wm_xr_ui_region_basis_from_back(const float back_in[3],
                                            float r_right[3],
                                            float r_up[3],
                                            float r_back[3])
{
  float back[3];
  copy_v3_v3(back, back_in);
  if (normalize_v3(back) == 0.0f) {
    back[0] = 0.0f;
    back[1] = -1.0f;
    back[2] = 0.0f;
  }

  const float world_up[3] = {0.0f, 0.0f, 1.0f};
  cross_v3_v3v3(r_right, back, world_up);
  if (normalize_v3(r_right) == 0.0f) {
    r_right[0] = 1.0f;
    r_right[1] = 0.0f;
    r_right[2] = 0.0f;
  }

  cross_v3_v3v3(r_up, r_right, back);
  normalize_v3(r_up);
  copy_v3_v3(r_back, back);
}

static void wm_xr_ui_region_transform_apply(wmXrUiRegion *ui_region,
                                            const float pos[3],
                                            const float back[3])
{
  if (ui_region == nullptr) {
    return;
  }

  const float screen_to_world_scale = wm_xr_ui_region_scale_get(ui_region);
  float right[3];
  float up[3];
  float basis_back[3];

  wm_xr_ui_region_basis_from_back(back, right, up, basis_back);

  unit_m4(ui_region->ui_region_obmat);
  copy_v3_v3(ui_region->ui_region_obmat[0], right);
  copy_v3_v3(ui_region->ui_region_obmat[1], up);
  copy_v3_v3(ui_region->ui_region_obmat[2], basis_back);
  mul_v3_fl(ui_region->ui_region_obmat[0], screen_to_world_scale);
  mul_v3_fl(ui_region->ui_region_obmat[1], screen_to_world_scale);
  mul_v3_fl(ui_region->ui_region_obmat[2], screen_to_world_scale);
  copy_v3_v3(ui_region->ui_region_obmat[3], pos);

  const int ui_region_px_width = std::max(BLI_rcti_size_x(&ui_region->ui_region_rect) + 1, 1);
  const int ui_region_px_height = std::max(BLI_rcti_size_y(&ui_region->ui_region_rect) + 1, 1);
  madd_v3_v3fl(
      ui_region->ui_region_obmat[3], ui_region->ui_region_obmat[0], -0.5f * ui_region_px_width);
  madd_v3_v3fl(
      ui_region->ui_region_obmat[3], ui_region->ui_region_obmat[1], -0.5f * ui_region_px_height);
}

static bool wm_xr_ui_region_host_ensure(wmXrUiRegion *ui_region, const wmXrData *xr)
{
  if (ui_region == nullptr || xr == nullptr || xr->runtime == nullptr ||
      xr->runtime->xr_window == nullptr || xr->runtime->xr_screen == nullptr)
  {
    return false;
  }
  if (ui_region->ui_region_host_area != nullptr && ui_region->ui_region_host_region != nullptr) {
    return true;
  }

  ScrArea *area = ED_area_offscreen_create(xr->runtime->xr_window, SPACE_VIEW3D);
  if (area == nullptr) {
    return false;
  }
  BLI_addtail(&xr->runtime->xr_screen->areabase, area);

  ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_UI);
  if (region == nullptr) {
    BLI_remlink(&xr->runtime->xr_screen->areabase, area);
    wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
    if (wm != nullptr) {
      ED_area_offscreen_free(wm, xr->runtime->xr_window, area);
    }
    return false;
  }

  region->regiontype = RGN_TYPE_XR;
  region->runtime->type = BKE_regiontype_from_id(area->type, RGN_TYPE_XR);
  ui::region_handlers_add(&region->runtime->handlers);

  ui_region->ui_region_host_win = xr->runtime->xr_window;
  ui_region->ui_region_host_area = area;
  ui_region->ui_region_host_region = region;
  ui_region->ui_region_host_initialized = false;
  return true;
}

void wm_xr_ui_region_host_free(wmXrUiRegion *ui_region)
{
  if (ui_region == nullptr || ui_region->ui_region_host_area == nullptr) {
    return;
  }

  wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
  if (wm != nullptr && wm->xr.runtime != nullptr && wm->xr.runtime->xr_window != nullptr) {
    bContext *xr_context = wm->xr.runtime->b_context;
    wmWindow *xr_win = wm->xr.runtime->xr_window;
    bScreen *xr_screen = wm->xr.runtime->xr_screen;

    CTX_wm_window_set(xr_context, xr_win);
    CTX_wm_screen_set(xr_context, xr_screen);
    CTX_wm_area_set(xr_context, ui_region->ui_region_host_area);
    for (ARegion *region =
             static_cast<ARegion *>(ui_region->ui_region_host_area->regionbase.first);
         region != nullptr;
         region = region->next)
    {
      CTX_wm_region_set(xr_context, region);
      WM_event_remove_handlers(xr_context, &region->runtime->handlers);
      ui::UI_region_free_active_but_all(xr_context, region);
      ED_region_panels_exit_active_state(xr_context, region);
      ui::blocklist_free(xr_context, region);
      BKE_area_region_panels_free(&region->panels);
    }
    CTX_wm_region_set(xr_context, nullptr);
    WM_event_remove_handlers_by_area(&xr_win->runtime->handlers, ui_region->ui_region_host_area);
    if (xr_screen != nullptr) {
      BLI_remlink(&xr_screen->areabase, ui_region->ui_region_host_area);
    }
    ED_area_offscreen_free(wm, xr_win, ui_region->ui_region_host_area);
    CTX_wm_area_set(xr_context, nullptr);
  }

  ui_region->ui_region_host_win = nullptr;
  ui_region->ui_region_host_area = nullptr;
  ui_region->ui_region_host_region = nullptr;
  ui_region->ui_region_host_initialized = false;
}

static void wm_xr_ui_region_center_position_get(const wmXrUiRegion *ui_region, float r_center[3])
{
  if (ui_region == nullptr) {
    zero_v3(r_center);
    return;
  }

  copy_v3_v3(r_center, ui_region->ui_region_obmat[3]);
  const int ui_region_px_width = std::max(BLI_rcti_size_x(&ui_region->ui_region_rect) + 1, 1);
  const int ui_region_px_height = std::max(BLI_rcti_size_y(&ui_region->ui_region_rect) + 1, 1);
  madd_v3_v3fl(r_center, ui_region->ui_region_obmat[0], 0.5f * ui_region_px_width);
  madd_v3_v3fl(r_center, ui_region->ui_region_obmat[1], 0.5f * ui_region_px_height);
}

static bool wm_xr_ui_region_target_from_viewer(const wmXrData *xr, float r_pos[3], float r_back[3])
{
  if (xr == nullptr || xr->runtime == nullptr || !xr->runtime->session_state.is_view_data_set) {
    return false;
  }

  const GHOST_XrPose *viewer_pose = &xr->runtime->session_state.viewer_pose;
  float viewer_mat[4][4];
  wm_xr_pose_to_mat(viewer_pose, viewer_mat);

  float forward[3];
  normalize_v3_v3(forward, viewer_mat[2]);
  negate_v3(forward);

  float forward_xy[2] = {forward[0], forward[1]};
  if (normalize_v2(forward_xy) == 0.0f) {
    forward_xy[0] = 0.0f;
    forward_xy[1] = -1.0f;
  }

  copy_v3_v3(r_pos, viewer_pose->position);
  r_pos[0] += forward_xy[0] * 1.5f;
  r_pos[1] += forward_xy[1] * 1.5f;
  r_back[0] = forward_xy[0];
  r_back[1] = forward_xy[1];
  r_back[2] = 0.0f;
  return true;
}

static bool wm_xr_ui_region_controller_mount_mat_calc(const wmXrData *xr,
                                                      wmXrUiRegion *ui_region,
                                                      const char *subaction_path,
                                                      float r_mat[4][4])
{
  if (xr == nullptr || xr->runtime == nullptr || ui_region == nullptr || r_mat == nullptr) {
    return false;
  }

  const float scale_fac = wm_xr_ui_region_scale_get(ui_region);
  const int ui_region_px_height = std::max(BLI_rcti_size_y(&ui_region->ui_region_rect) + 1, 1);
  const float ui_region_world_height = float(ui_region_px_height) * scale_fac;
  const float side_sign = STREQ(subaction_path, "/user/hand/right") ? 1.0f : -1.0f;
  const float offset[3] = {0.08f * side_sign, 0.0f, (ui_region_world_height * 0.5f) * -1.0f};
  const float rotation[3] = {-float(M_PI_2), 0.0f, 0.0f};
  const float scale[3] = {scale_fac, scale_fac, scale_fac};
  if (!wm_xr_controller_object_mat_calc(
          &xr->runtime->session_state, subaction_path, true, offset, rotation, scale, r_mat))
  {
    return false;
  }

  return true;
}

static bool wm_xr_ui_region_target_from_head_follow(const wmXrData *xr,
                                                    const wmXrUiRegion *ui_region,
                                                    float r_pos[3],
                                                    float r_back[3])
{
  float target_pos[3];
  float target_back[3];
  if (!wm_xr_ui_region_target_from_viewer(xr, target_pos, target_back)) {
    return false;
  }

  if (ui_region == nullptr || is_zero_m4(ui_region->ui_region_obmat)) {
    copy_v3_v3(r_pos, target_pos);
    copy_v3_v3(r_back, target_back);
    return true;
  }

  float current_back[3];
  if (xr == nullptr || xr->runtime == nullptr || !xr->runtime->session_state.is_view_data_set) {
    copy_v3_v3(r_pos, target_pos);
    copy_v3_v3(r_back, target_back);
    return true;
  }

  const GHOST_XrPose *viewer_pose = &xr->runtime->session_state.viewer_pose;
  float current_center[3];
  wm_xr_ui_region_center_position_get(ui_region, current_center);
  sub_v3_v3v3(current_back, current_center, viewer_pose->position);
  current_back[2] = 0.0f;
  if (normalize_v3(current_back) == 0.0f) {
    copy_v3_v3(current_back, target_back);
  }

  const float fixed_distance = 1.0f;
  const float follow_angle_threshold = DEG2RADF(25.0f);
  const float follow_factor = 0.01f;
  const float angle = angle_normalized_v3v3(current_back, target_back);

  if (angle < follow_angle_threshold) {
    madd_v3_v3v3fl(r_pos, viewer_pose->position, current_back, fixed_distance);
    copy_v3_v3(r_back, current_back);
    return true;
  }

  interp_v3_v3v3(r_back, current_back, target_back, follow_factor);
  normalize_v3(r_back);
  madd_v3_v3v3fl(r_pos, viewer_pose->position, r_back, fixed_distance);
  return true;
}

static void wm_xr_ui_region_mount_update(wmXrUiRegion *ui_region, const wmXrData *xr)
{
  if (ui_region == nullptr) {
    return;
  }

  float pos[3];
  float back[3];
  bool has_target = false;

  switch (ui_region->mount_point) {
    case XR_UI_REGION_MOUNT_NONE:
      return;
    case XR_UI_REGION_MOUNT_LEFT_HAND:
      if (wm_xr_ui_region_controller_mount_mat_calc(
              xr, ui_region, "/user/hand/left", ui_region->ui_region_obmat))
      {
        return;
      }
      has_target = wm_xr_ui_region_target_from_viewer(xr, pos, back);
      break;
    case XR_UI_REGION_MOUNT_RIGHT_HAND:
      if (wm_xr_ui_region_controller_mount_mat_calc(
              xr, ui_region, "/user/hand/right", ui_region->ui_region_obmat))
      {
        return;
      }
      has_target = wm_xr_ui_region_target_from_viewer(xr, pos, back);
      break;
    case XR_UI_REGION_MOUNT_HEAD_FOLLOW:
      has_target = wm_xr_ui_region_target_from_head_follow(xr, ui_region, pos, back);
      break;
    case XR_UI_REGION_MOUNT_WORLD:
      if (is_zero_m4(ui_region->ui_region_obmat)) {
        has_target = wm_xr_ui_region_target_from_viewer(xr, pos, back);
      }
      else {
        return;
      }
      break;
  }

  if (!has_target) {
    wm_xr_ui_region_default_transform_init(ui_region);
    return;
  }

  wm_xr_ui_region_transform_apply(ui_region, pos, back);
}

static wmXrUiRegion *wm_xr_ui_region_register(wmXrSurfaceData *surface_data,
                                              wmWindow *win,
                                              eWMXrUiRegionMountPoint mount_point,
                                              const wmXrData *xr)
{
  if (mount_point == XR_UI_REGION_MOUNT_NONE) {
    return nullptr;
  }

  wmXrUiRegion *ui_region = nullptr;
  for (wmXrUiRegion *candidate : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
    if (candidate->ui_region_host_win == win && candidate->mount_point == mount_point) {
      ui_region = candidate;
      break;
    }
  }
  if (ui_region != nullptr) {
    ui_region->mount_point = mount_point;
    wm_xr_ui_region_mount_update(ui_region, xr);
    return ui_region;
  }

  ui_region = MEM_new_zeroed<wmXrUiRegion>(__func__);
  ui_region->mount_point = mount_point;
  ui_region->ui_region_host_win = win;
  if (!wm_xr_ui_region_host_ensure(ui_region, xr)) {
    MEM_delete(ui_region);
    return nullptr;
  }
  wm_xr_ui_region_mount_update(ui_region, xr);
  ui_region->ui_region_frame_tag = surface_data->ui_regions_frame_tag;
  BLI_addtail(&surface_data->ui_regions, ui_region);
  return ui_region;
}

static void wm_xr_ui_region_pointer_clear(wmXrUiRegion *ui_region)
{
  ui_region->ui_region_hovered = false;
  ui_region->ui_region_hover_region = nullptr;
  ui_region->ui_region_cursor_visible = false;
  zero_v2_int(ui_region->ui_region_window_xy);
  ui_region->ui_region_hover_subaction_path[0] = '\0';
  ui_region->ui_region_pointer.pressed = false;
  ui_region->ui_region_pointer.subaction_path[0] = '\0';
  ui_region->ui_region_pointer.region = nullptr;
}

static const wmXrController *wm_xr_surface_interaction_controller_find(
    const wmXrSessionState *state, char *r_subaction_path)
{
  const wmXrController *fallback = nullptr;
  for (const wmXrController &controller : state->controllers) {
    if (!controller.aim_active) {
      continue;
    }
    if (strstr(controller.subaction_path, "right") != nullptr) {
      BLI_strncpy(r_subaction_path, controller.subaction_path, XR_MAX_USER_PATH_LENGTH);
      return &controller;
    }
    if (fallback == nullptr) {
      fallback = &controller;
    }
  }
  if (fallback != nullptr) {
    BLI_strncpy(r_subaction_path, fallback->subaction_path, XR_MAX_USER_PATH_LENGTH);
  }
  return fallback;
}

static bool wm_xr_surface_action_is_teleport(const wmXrAction *action)
{
  return action != nullptr && action->ot != nullptr && action->ot->idname != nullptr &&
         BLI_strcasestr(action->ot->idname, "xr_navigation_teleport") != nullptr;
}

static bool wm_xr_surface_action_is_ui_region_click_compatible(const wmXrAction *action)
{
  if (action == nullptr || action->name == nullptr || action->ot == nullptr ||
      action->ot->idname == nullptr)
  {
    return false;
  }
  if (action->type == XR_VECTOR2F_INPUT) {
    return false;
  }
  if (wm_xr_surface_action_is_teleport(action) ||
      BLI_strcasestr(action->name, "teleport") != nullptr)
  {
    return ELEM(action->type, XR_BOOLEAN_INPUT, XR_FLOAT_INPUT);
  }
  if (BLI_strcasestr(action->ot->idname, "xr_navigation_") != nullptr) {
    return false;
  }
  if (BLI_strcasestr(action->name, "trigger") == nullptr &&
      BLI_strcasestr(action->name, "select") == nullptr &&
      BLI_strcasestr(action->name, "click") == nullptr)
  {
    return false;
  }
  return ELEM(action->type, XR_BOOLEAN_INPUT, XR_FLOAT_INPUT);
}

static bool wm_xr_surface_controller_teleport_active(const wmXrData *xr,
                                                     const char *subaction_path)
{
  if (xr == nullptr || xr->runtime == nullptr || subaction_path == nullptr) {
    return false;
  }

  const wmXrActionSet *active_action_set = xr->runtime->session_state.active_action_set;
  if (active_action_set == nullptr) {
    return false;
  }

  for (LinkData &ld : active_action_set->active_modal_actions) {
    const wmXrAction *action = static_cast<const wmXrAction *>(ld.data);
    if (wm_xr_surface_action_is_teleport(action) && action->active_modal_path != nullptr &&
        STREQ(action->active_modal_path, subaction_path))
    {
      return true;
    }
  }

  return false;
}

static void wm_xr_surface_interaction_ray_from_pose(const GHOST_XrPose *aim_pose,
                                                    float r_origin[3],
                                                    float r_direction[3])
{
  float aim_mat[4][4];
  wm_xr_pose_to_mat(aim_pose, aim_mat);
  copy_v3_v3(r_origin, aim_pose->position);
  normalize_v3_v3(r_direction, aim_mat[2]);
  negate_v3(r_direction);
}

static bool wm_xr_surface_interaction_raycast_rect(const wmXrUiRegion *ui_region,
                                                   const float ray_origin[3],
                                                   const float ray_direction[3],
                                                   const rcti &local_rect,
                                                   const float plane_z,
                                                   const bool allow_outside_bounds,
                                                   int r_win_xy[2],
                                                   float r_hit_world[3],
                                                   float *r_lambda)
{
  if (ui_region == nullptr || !ui_region->ui_region_valid ||
      ui_region->ui_region_offscreen == nullptr || ui_region->ui_region_host_region == nullptr)
  {
    return false;
  }

  float obimat[4][4];
  if (!invert_m4_m4(obimat, ui_region->ui_region_obmat)) {
    return false;
  }

  float ray_end[3];
  madd_v3_v3v3fl(ray_end, ray_origin, ray_direction, 100.0f);

  float origin_local[3], end_local[3], dir_local[3];
  copy_v3_v3(origin_local, ray_origin);
  copy_v3_v3(end_local, ray_end);
  mul_m4_v3(obimat, origin_local);
  mul_m4_v3(obimat, end_local);
  sub_v3_v3v3(dir_local, end_local, origin_local);

  if (fabsf(dir_local[2]) < 1e-6f) {
    return false;
  }

  const float lambda = (plane_z - origin_local[2]) / dir_local[2];
  if (lambda < 0.0f) {
    return false;
  }

  float hit_local[3];
  madd_v3_v3v3fl(hit_local, origin_local, dir_local, lambda);

  const float rect_xmin = float(local_rect.xmin);
  const float rect_xmax = float(local_rect.xmax + 1);
  const float rect_ymin = float(local_rect.ymin);
  const float rect_ymax = float(local_rect.ymax + 1);
  if (!allow_outside_bounds && (hit_local[0] < rect_xmin || hit_local[1] < rect_ymin ||
                                hit_local[0] > rect_xmax || hit_local[1] > rect_ymax))
  {
    return false;
  }

  r_win_xy[0] = ui_region->ui_region_host_region->winrct.xmin + round_fl_to_int(hit_local[0]);
  r_win_xy[1] = ui_region->ui_region_host_region->winrct.ymin + round_fl_to_int(hit_local[1]);
  copy_v3_v3(r_hit_world, hit_local);
  r_hit_world[2] = plane_z;
  mul_m4_v3(ui_region->ui_region_obmat, r_hit_world);
  if (r_lambda != nullptr) {
    *r_lambda = lambda;
  }
  return true;
}

static bool wm_xr_surface_interaction_raycast_target(const wmXrUiRegion *ui_region,
                                                     const wmXrTempRegion *temp_region,
                                                     const float ray_origin[3],
                                                     const float ray_direction[3],
                                                     const bool allow_outside_bounds,
                                                     ARegion **r_region,
                                                     int r_win_xy[2],
                                                     float r_hit_world[3],
                                                     float *r_lambda)
{
  if (ui_region == nullptr || ui_region->ui_region_host_region == nullptr) {
    return false;
  }

  rcti local_rect = ui_region->ui_region_rect;
  float plane_z = 0.0f;
  ARegion *target_region = ui_region->ui_region_host_region;
  if (temp_region != nullptr) {
    if (!temp_region->valid || temp_region->region == nullptr) {
      return false;
    }
    target_region = temp_region->region;
    local_rect.xmin = temp_region->region_rect.xmin -
                      ui_region->ui_region_host_region->winrct.xmin;
    local_rect.xmax = temp_region->region_rect.xmax -
                      ui_region->ui_region_host_region->winrct.xmin;
    local_rect.ymin = temp_region->region_rect.ymin -
                      ui_region->ui_region_host_region->winrct.ymin;
    local_rect.ymax = temp_region->region_rect.ymax -
                      ui_region->ui_region_host_region->winrct.ymin;
    plane_z = temp_region->z_offset;
  }

  if (!wm_xr_surface_interaction_raycast_rect(ui_region,
                                              ray_origin,
                                              ray_direction,
                                              local_rect,
                                              plane_z,
                                              allow_outside_bounds,
                                              r_win_xy,
                                              r_hit_world,
                                              r_lambda))
  {
    return false;
  }

  if (r_region != nullptr) {
    *r_region = target_region;
  }
  return true;
}

static void wm_xr_surface_interaction_event_add(const bContext * /*C*/,
                                                wmWindow *win,
                                                ScrArea *area,
                                                ARegion *region,
                                                short type,
                                                short val,
                                                const int xy[2])
{
  wmEvent event{};
  wm_event_init_from_window(win, &event);
  copy_v2_v2_int(event.xy, xy);
  copy_v2_v2_int(event.prev_xy, win->runtime->eventstate->xy);
  event.type = static_cast<wmEventType>(type);
  event.val = val;
  event.target_area = area;
  event.target_region = region;

  const int g_flag_prev = G.f;
  G.f |= G_FLAG_EVENT_SIMULATE;
  WM_event_add_simulate(win, &event);
  G.f = g_flag_prev;
}

static bool wm_xr_panels_have_dragging_state(const ListBase &panels)
{
  for (const Panel *panel = static_cast<const Panel *>(panels.first); panel != nullptr;
       panel = panel->next)
  {
    if (ui::panel_is_dragging(panel) || wm_xr_panels_have_dragging_state(panel->children)) {
      return true;
    }
  }
  return false;
}

static bool wm_xr_panels_have_transient_reorder_offset(const ListBase &panels)
{
  for (const Panel *panel = static_cast<const Panel *>(panels.first); panel != nullptr;
       panel = panel->next)
  {
    if ((panel->ofsx != 0 && ui::panel_is_active(panel)) ||
        wm_xr_panels_have_transient_reorder_offset(panel->children))
    {
      return true;
    }
  }
  return false;
}

static int wm_xr_panel_real_ofsy(const Panel *panel)
{
  return ui::panel_is_closed(panel) ? (panel->ofsy + panel->sizey) : panel->ofsy;
}

static void wm_xr_panels_current_size(const ARegion *region,
                                      const int base_width,
                                      int *r_x,
                                      int *r_y)
{
  int max_ofsx = 0;
  int sizey = 0;
  bool has_active_panel = false;

  for (const Panel *panel = static_cast<const Panel *>(region->panels.first); panel != nullptr;
       panel = panel->next)
  {
    if (!ui::panel_is_active(panel)) {
      continue;
    }
    has_active_panel = true;
    max_ofsx = std::max(max_ofsx, panel->ofsx);
    sizey = std::min(sizey, wm_xr_panel_real_ofsy(panel));
  }

  const int sizex = std::max({base_width, int(region->winx), 1});
  if (sizey == 0) {
    sizey = -UI_PANEL_WIDTH;
  }
  if (has_active_panel) {
    sizey -= UI_PANEL_MARGIN_Y;
  }
  *r_x = sizex;
  *r_y = -sizey;
}

static bool wm_xr_ui_region_cache_update(const bContext *C, wmXrUiRegion *ui_region)
{
  ScrArea *area = ui_region ? ui_region->ui_region_host_area : nullptr;
  ARegion *xr_region = ui_region ? ui_region->ui_region_host_region : nullptr;
  if (ui_region == nullptr || area == nullptr || xr_region == nullptr ||
      xr_region->runtime == nullptr || xr_region->runtime->type == nullptr)
  {
    return false;
  }

  const bool needs_post_release_reorder_settle =
      !wm_xr_panels_have_dragging_state(xr_region->panels) &&
      wm_xr_panels_have_transient_reorder_offset(xr_region->panels);
  if (ui_region->ui_region_valid && ui_region->ui_region_offscreen != nullptr &&
      !ui_region->ui_region_dirty &&
      ui_region->ui_region_last_rebuild_tag == ui_region->ui_region_frame_tag &&
      !needs_post_release_reorder_settle)
  {
    return true;
  }

  bContext *mutable_C = const_cast<bContext *>(C);
  eRegion_Alignment prev_alignment = xr_region->alignment;
  ARegion *prev_region = CTX_wm_region(mutable_C);
  const bool prev_visible = xr_region->runtime->visible;
  rcti ui_region_rect = ui_region->ui_region_rect;
  int w = 0;
  int h = 0;
  bool needs_followup_transient_settle = false;
  bool needs_followup_post_resize_settle = false;
  const bool needs_layout = !ui_region->ui_region_valid ||
                            (ui_region->ui_region_last_rebuild_tag !=
                             ui_region->ui_region_frame_tag) ||
                            (xr_region->runtime->do_draw & RGN_REFRESH_UI) ||
                            BLI_listbase_is_empty(&xr_region->runtime->uiblocks) ||
                            needs_post_release_reorder_settle;
  if (needs_layout) {
    wm_xr_region_ensure_layout_rect(xr_region);
  }
  xr_region->runtime->visible = true;
  if (!ui_region->ui_region_host_initialized) {
    if (xr_region->runtime->type->init != nullptr) {
      xr_region->runtime->type->init(CTX_wm_manager(C), xr_region);
    }
    xr_region->flag |= RGN_FLAG_INDICATE_OVERFLOW;
    ui_region->ui_region_host_initialized = true;
  }
  CTX_wm_region_set(mutable_C, xr_region);
  xr_region->alignment = RGN_ALIGN_FLOAT;
  {
    wmXrUiRegionTypeFilterScope ui_region_type_filter(xr_region, ui_region->mount_point);
    const bool has_dragging_panel_state = wm_xr_panels_have_dragging_state(xr_region->panels);
    const bool has_transient_reorder_offset = wm_xr_panels_have_transient_reorder_offset(
        xr_region->panels);
    constexpr int reorder_drag_grace_updates = 2;
    constexpr int reorder_settle_retry_updates = 2;
    const bool within_reorder_drag_grace = ui_region->ui_region_reorder_preserve_active &&
                                           has_transient_reorder_offset &&
                                           !has_dragging_panel_state &&
                                           (ui_region->ui_region_reorder_grace_remaining > 0);
    const bool has_reorder_start = has_dragging_panel_state;
    if (has_reorder_start && !ui_region->ui_region_reorder_preserve_active) {
      ui_region->ui_region_reorder_preserve_active = true;
      ui_region->ui_region_reorder_base_width = std::max(BLI_rcti_size_x(&ui_region_rect) + 1, 1);
      ui_region->ui_region_reorder_grace_remaining = reorder_drag_grace_updates;
      ui_region->ui_region_reorder_settle_retry_remaining = reorder_settle_retry_updates;
    }
    else if (has_reorder_start) {
      ui_region->ui_region_reorder_grace_remaining = reorder_drag_grace_updates;
      ui_region->ui_region_reorder_settle_retry_remaining = reorder_settle_retry_updates;
    }
    else if (ui_region->ui_region_reorder_preserve_active && !has_dragging_panel_state &&
             !has_transient_reorder_offset)
    {
      ui_region->ui_region_reorder_preserve_active = false;
      ui_region->ui_region_reorder_base_width = 0;
      ui_region->ui_region_reorder_grace_remaining = 0;
      ui_region->ui_region_reorder_settle_retry_remaining = 0;
    }
    else if (within_reorder_drag_grace) {
      ui_region->ui_region_reorder_grace_remaining--;
    }
    else if (ui_region->ui_region_reorder_preserve_active && !has_dragging_panel_state) {
      ui_region->ui_region_reorder_preserve_active = false;
      ui_region->ui_region_reorder_grace_remaining = 0;
    }
    const bool has_reorder_transition = ui_region->ui_region_reorder_preserve_active;
    const bool clamp_width_during_transient_settle =
        !has_reorder_transition && has_transient_reorder_offset &&
        (ui_region->ui_region_reorder_base_width > 0) &&
        (ui_region->ui_region_reorder_settle_retry_remaining > 0);
    const bool force_final_transient_settle = !has_reorder_transition &&
                                              has_transient_reorder_offset &&
                                              (ui_region->ui_region_reorder_base_width > 0) &&
                                              !clamp_width_during_transient_settle;
    needs_followup_transient_settle = clamp_width_during_transient_settle &&
                                      has_transient_reorder_offset;
    const bool defer_second_layout_for_reorder = has_reorder_transition ||
                                                 clamp_width_during_transient_settle;
    const bool preserve_active_panel_state = has_reorder_transition;
    const bool preserve_existing_layout_for_reorder = preserve_active_panel_state &&
                                                      !BLI_listbase_is_empty(
                                                          &xr_region->runtime->uiblocks);
    if (needs_layout) {
      int content_width;
      int content_height;
      if (preserve_existing_layout_for_reorder) {
        /* Preserve the current transient drag geometry instead of rebuilding the panel stack,
         * which snaps the reorder offsets back to their settled positions. */
        wm_xr_panels_current_size(xr_region,
                                  std::max(ui_region->ui_region_reorder_base_width, 1),
                                  &content_width,
                                  &content_height);
        ui::view2d_totRect_set(&xr_region->v2d, content_width, content_height);
        ui::blocklist_update_window_matrix(mutable_C, &xr_region->runtime->uiblocks);
        ui::blocklist_update_view_for_buttons(mutable_C, &xr_region->runtime->uiblocks);
      }
      else {
        if (!preserve_active_panel_state) {
          ED_region_panels_exit_active_state(mutable_C, xr_region);
        }
        ui::blocklist_free(mutable_C, xr_region);
        /* Keep the ui_region list alive across XR relayouts so runtime state like collapse and
         * drag-reorder order persists, just like it does for regular screen regions. */
        ED_region_panels_layout(mutable_C, xr_region);

        content_width = std::max(1, int(std::ceil(BLI_rctf_size_x(&xr_region->v2d.tot))));
        content_height = std::max(1, int(std::ceil(BLI_rctf_size_y(&xr_region->v2d.tot))));
        if (clamp_width_during_transient_settle) {
          content_width = ui_region->ui_region_reorder_base_width;
        }
        if (force_final_transient_settle) {
          ED_region_panels_settle_active_state(mutable_C, xr_region);
          ui::blocklist_free(mutable_C, xr_region);
          ED_region_panels_layout(mutable_C, xr_region);
          wm_xr_panels_current_size(xr_region,
                                    std::max(ui_region->ui_region_reorder_base_width, 1),
                                    &content_width,
                                    &content_height);
        }
        if ((force_final_transient_settle || !has_transient_reorder_offset) &&
            ui_region->ui_region_reorder_base_width > 0)
        {
          ui_region->ui_region_reorder_base_width = 0;
          ui_region->ui_region_reorder_settle_retry_remaining = 0;
        }
      }

      if (content_width != xr_region->winx || content_height != xr_region->winy) {
        const bool settled_resize = !has_transient_reorder_offset;
        BLI_rcti_init(&xr_region->winrct, 0, content_width - 1, 0, content_height - 1);
        xr_region->sizex = content_width;
        xr_region->sizey = content_height;
        ED_region_update_rect(xr_region);
        view2d_region_reinit(
            &xr_region->v2d, ui::V2D_COMMONVIEW_PANELS_UI, xr_region->winx, xr_region->winy);
        if (!defer_second_layout_for_reorder) {
          ED_region_panels_exit_active_state(mutable_C, xr_region);
          ui::blocklist_free(mutable_C, xr_region);
          ED_region_panels_layout(mutable_C, xr_region);
          content_width = std::max(1, int(std::ceil(BLI_rctf_size_x(&xr_region->v2d.tot))));
          content_height = std::max(1, int(std::ceil(BLI_rctf_size_y(&xr_region->v2d.tot))));
          if (force_final_transient_settle &&
              (content_width != xr_region->winx || content_height != xr_region->winy))
          {
            BLI_rcti_init(&xr_region->winrct, 0, content_width - 1, 0, content_height - 1);
            xr_region->sizex = content_width;
            xr_region->sizey = content_height;
            ED_region_update_rect(xr_region);
            view2d_region_reinit(
                &xr_region->v2d, ui::V2D_COMMONVIEW_PANELS_UI, xr_region->winx, xr_region->winy);
            ED_region_panels_settle_active_state(mutable_C, xr_region);
            ui::blocklist_free(mutable_C, xr_region);
            ED_region_panels_layout(mutable_C, xr_region);
            wm_xr_panels_current_size(xr_region,
                                      std::max(ui_region->ui_region_reorder_base_width, 1),
                                      &content_width,
                                      &content_height);
          }
        }
        needs_followup_post_resize_settle = settled_resize;
      }
      BLI_rcti_init(&ui_region_rect, 0, content_width - 1, 0, content_height - 1);
    }

    w = BLI_rcti_size_x(&ui_region_rect) + 1;
    h = BLI_rcti_size_y(&ui_region_rect) + 1;
    bool create_new = true;
    if (ui_region->ui_region_offscreen) {
      if (GPU_offscreen_width(ui_region->ui_region_offscreen) == w &&
          GPU_offscreen_height(ui_region->ui_region_offscreen) == h)
      {
        create_new = false;
      }
      else {
        GPU_offscreen_free(ui_region->ui_region_offscreen);
        ui_region->ui_region_offscreen = nullptr;
      }
    }
    if (create_new) {
      ui_region->ui_region_offscreen = GPU_offscreen_create(w,
                                                            h,
                                                            false,
                                                            gpu::TextureFormat::SRGBA_8_8_8_8,
                                                            GPU_TEXTURE_USAGE_SHADER_READ,
                                                            false,
                                                            nullptr);
    }
    if (!ui_region->ui_region_offscreen) {
      CLOG_ERROR(&LOG, "offscreen create failed");
      xr_region->runtime->visible = prev_visible;
      ED_region_panels_world_layout_end(mutable_C, xr_region, prev_alignment, prev_region);
      return false;
    }

    /* Ensure semantically checked XR toggles draw as selected in the cached world-space UI
     * even when their runtime button flags only carry hover state at this point in the draw
     * pipeline.
     */
    wmXrSelectedToggleDrawStateScope selected_toggle_draw_state_scope(xr_region);
    ED_region_panels_draw_offscreen(C, xr_region, &ui_region_rect, ui_region->ui_region_offscreen);
  }
  ui_region->ui_region_rect = ui_region_rect;
  BLI_rcti_init(&area->totrct,
                ui_region_rect.xmin,
                ui_region_rect.xmax,
                ui_region_rect.ymin,
                ui_region_rect.ymax);
  ui_region->ui_region_valid = true;
  ui_region->ui_region_dirty = needs_followup_transient_settle ||
                               needs_followup_post_resize_settle;
  ui_region->ui_region_last_rebuild_tag = ui_region->ui_region_frame_tag;
  ui_region->ui_region_host_win = CTX_wm_window(C);
  ui_region->ui_region_host_area = area;
  ui_region->ui_region_host_region = xr_region;
  xr_region->runtime->do_draw &= ~RGN_REFRESH_UI;
  if (!wm_xr_panels_have_transient_reorder_offset(xr_region->panels)) {
    ui_region->ui_region_suppress_hover_refresh_while_settling = false;
  }
  if (needs_followup_transient_settle || needs_followup_post_resize_settle) {
    ui_region->ui_region_reorder_settle_retry_remaining = std::max(
        ui_region->ui_region_reorder_settle_retry_remaining - 1, 0);
    ED_region_tag_redraw(xr_region);
  }

  xr_region->runtime->visible = prev_visible;
  ED_region_panels_world_layout_end(mutable_C, xr_region, prev_alignment, prev_region);
  return true;
}

static void wm_xr_ui_region_cache_refresh_host(const bContext *C, wmXrUiRegion *ui_region)
{
  if (C == nullptr || ui_region == nullptr || ui_region->ui_region_host_win == nullptr ||
      ui_region->ui_region_host_area == nullptr || ui_region->ui_region_host_region == nullptr)
  {
    return;
  }

  bContext *mutable_C = const_cast<bContext *>(C);
  wmXrUiRegionHostContextOverride context_override(mutable_C, ui_region);
  wm_xr_ui_region_cache_update(mutable_C, ui_region);
  ED_region_tag_redraw(ui_region->ui_region_host_region);
}

void WM_xr_surface_ui_regions_register(const bContext *C)
{
  if (C == nullptr) {
    return;
  }

  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmWindow *win = CTX_wm_window(C);
  const wmWindowManager *wm = CTX_wm_manager(C);
  const wmXrData *xr = wm ? &wm->xr : nullptr;
  if (surface_data == nullptr || win == nullptr || xr == nullptr || xr->runtime == nullptr) {
    return;
  }

  ARegionType *xr_region_type = wm_xr_ui_region_region_type_get();
  eWMXrUiRegionMountPoint mount_points[4];
  const int mount_count = wm_xr_ui_region_mount_points_collect(xr, xr_region_type, mount_points);
  if (mount_count == 0) {
    return;
  }
  for (int i = 0; i < mount_count; i++) {
    wm_xr_ui_region_register(surface_data, win, mount_points[i], xr);
  }
}

void WM_xr_surface_ui_regions_update(const bContext *C, const wmXrData *xr)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmWindow *win = C ? CTX_wm_window(C) : nullptr;
  if (surface_data == nullptr || xr == nullptr || xr->runtime == nullptr || win == nullptr) {
    return;
  }

  ARegionType *xr_region_type = wm_xr_ui_region_region_type_get();
  eWMXrUiRegionMountPoint mount_points[4];
  const int mount_count = wm_xr_ui_region_mount_points_collect(xr, xr_region_type, mount_points);
  for (int i = 0; i < mount_count; i++) {
    wm_xr_ui_region_register(surface_data, win, mount_points[i], xr);
  }

  wmXrUiRegion *ui_region = static_cast<wmXrUiRegion *>(surface_data->ui_regions.first);
  while (ui_region != nullptr) {
    wmXrUiRegion *ui_region_next = ui_region->next;
    bool mount_found = false;
    for (int i = 0; i < mount_count; i++) {
      if (ui_region->mount_point == mount_points[i]) {
        mount_found = true;
        break;
      }
    }
    if (ui_region->ui_region_host_win != win || !mount_found) {
      wm_xr_ui_region_free(surface_data, ui_region);
      ui_region = ui_region_next;
      continue;
    }
    wm_xr_ui_region_mount_update(ui_region, xr);
    ui_region = ui_region_next;
  }
}

void WM_xr_surface_ui_regions_tag_refresh(const bContext *C, const wmXrData *xr)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmWindow *win = C ? CTX_wm_window(C) : nullptr;
  if (surface_data == nullptr || xr == nullptr || xr->runtime == nullptr || win == nullptr) {
    return;
  }

  bContext *mutable_C = const_cast<bContext *>(C);
  for (wmXrUiRegion *ui_region = static_cast<wmXrUiRegion *>(surface_data->ui_regions.first);
       ui_region != nullptr;
       ui_region = ui_region->next)
  {
    if (ui_region->ui_region_host_win != win) {
      continue;
    }

    wmXrUiRegionHostContextOverride context_override(mutable_C, ui_region);
    ui::UI_region_free_active_but_all(mutable_C, ui_region->ui_region_host_region);
    ED_region_panels_exit_active_state(mutable_C, ui_region->ui_region_host_region);
    ui::blocklist_free(mutable_C, ui_region->ui_region_host_region);
    BKE_area_region_panels_free(&ui_region->ui_region_host_region->panels);

    ui_region->ui_region_dirty = true;
    ui_region->ui_region_valid = false;
    ED_region_tag_redraw(ui_region->ui_region_host_region);
    if (ui_region->ui_region_host_region->runtime != nullptr) {
      ui_region->ui_region_host_region->runtime->do_draw |= RGN_REFRESH_UI;
    }
  }
}

void wm_xr_surface_interaction_update(const bContext *C, wmXrData *xr)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (C == nullptr || xr == nullptr || surface_data == nullptr) {
    return;
  }

  char subaction_path[64] = "";
  const wmXrController *controller = wm_xr_surface_interaction_controller_find(
      &xr->runtime->session_state, subaction_path);
  if (controller == nullptr) {
    if (surface_data->active_ui_region != nullptr &&
        !surface_data->active_ui_region->ui_region_pointer.pressed)
    {
      wm_xr_ui_region_pointer_clear(surface_data->active_ui_region);
      surface_data->active_ui_region = nullptr;
    }
    return;
  }

  const bool is_captured_ui_region_drag =
      surface_data->active_ui_region != nullptr &&
      surface_data->active_ui_region->ui_region_pointer.pressed &&
      STREQ(surface_data->active_ui_region->ui_region_pointer.subaction_path, subaction_path);
  if (!is_captured_ui_region_drag && wm_xr_surface_controller_teleport_active(xr, subaction_path))
  {
    if (surface_data->active_ui_region != nullptr &&
        !surface_data->active_ui_region->ui_region_pointer.pressed)
    {
      wm_xr_ui_region_pointer_clear(surface_data->active_ui_region);
      surface_data->active_ui_region = nullptr;
    }
    return;
  }

  float ray_origin[3], ray_direction[3];
  wm_xr_surface_interaction_ray_from_pose(&controller->aim_pose, ray_origin, ray_direction);
  wmXrUiRegion *hit_ui_region = nullptr;
  ARegion *hit_region = nullptr;
  int hit_win_xy[2] = {0, 0};
  float hit_world[3] = {0.0f, 0.0f, 0.0f};
  float hit_lambda = FLT_MAX;

  if (is_captured_ui_region_drag) {
    hit_ui_region = surface_data->active_ui_region;
    ARegion *capture_region = hit_ui_region->ui_region_pointer.region ?
                                  hit_ui_region->ui_region_pointer.region :
                                  hit_ui_region->ui_region_host_region;
    wmXrTempRegion *capture_temp_region = (capture_region ==
                                           hit_ui_region->ui_region_host_region) ?
                                              nullptr :
                                              wm_xr_temp_region_find(hit_ui_region,
                                                                     capture_region);
    if (!wm_xr_surface_interaction_raycast_target(hit_ui_region,
                                                  capture_temp_region,
                                                  ray_origin,
                                                  ray_direction,
                                                  true,
                                                  &hit_region,
                                                  hit_win_xy,
                                                  hit_world,
                                                  &hit_lambda))
    {
      return;
    }
  }
  else {
    for (wmXrUiRegion *ui_region : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
      for (wmXrTempRegion *temp_region : ListBaseWrapper<wmXrTempRegion>(ui_region->child_regions))
      {
        ARegion *region = nullptr;
        int win_xy[2];
        float ui_region_hit_world[3];
        float lambda;
        if (!wm_xr_surface_interaction_raycast_target(ui_region,
                                                      temp_region,
                                                      ray_origin,
                                                      ray_direction,
                                                      false,
                                                      &region,
                                                      win_xy,
                                                      ui_region_hit_world,
                                                      &lambda))
        {
          continue;
        }
        if (lambda < hit_lambda) {
          hit_ui_region = ui_region;
          hit_region = region;
          hit_lambda = lambda;
          copy_v2_v2_int(hit_win_xy, win_xy);
          copy_v3_v3(hit_world, ui_region_hit_world);
        }
      }

      ARegion *region = nullptr;
      int win_xy[2];
      float ui_region_hit_world[3];
      float lambda;
      if (wm_xr_surface_interaction_raycast_target(ui_region,
                                                   nullptr,
                                                   ray_origin,
                                                   ray_direction,
                                                   false,
                                                   &region,
                                                   win_xy,
                                                   ui_region_hit_world,
                                                   &lambda))
      {
        if (lambda < hit_lambda) {
          hit_ui_region = ui_region;
          hit_region = region;
          hit_lambda = lambda;
          copy_v2_v2_int(hit_win_xy, win_xy);
          copy_v3_v3(hit_world, ui_region_hit_world);
        }
      }
    }
  }

  if (hit_ui_region == nullptr || hit_region == nullptr) {
    if (surface_data->active_ui_region != nullptr &&
        !surface_data->active_ui_region->ui_region_pointer.pressed)
    {
      wm_xr_ui_region_pointer_clear(surface_data->active_ui_region);
      surface_data->active_ui_region = nullptr;
    }
    return;
  }

  hit_ui_region->ui_region_cursor_visible = true;
  copy_v3_v3(hit_ui_region->ui_region_cursor_world, hit_world);

  if (surface_data->active_ui_region != nullptr &&
      surface_data->active_ui_region != hit_ui_region &&
      !surface_data->active_ui_region->ui_region_pointer.pressed)
  {
    wm_xr_ui_region_pointer_clear(surface_data->active_ui_region);
  }
  surface_data->active_ui_region = hit_ui_region;

  if (!hit_ui_region->ui_region_hovered || hit_ui_region->ui_region_hover_region != hit_region ||
      hit_ui_region->ui_region_window_xy[0] != hit_win_xy[0] ||
      hit_ui_region->ui_region_window_xy[1] != hit_win_xy[1] ||
      !STREQ(hit_ui_region->ui_region_hover_subaction_path, subaction_path))
  {
    const bool suppress_hover_refresh =
        hit_ui_region->ui_region_suppress_hover_refresh_while_settling &&
        wm_xr_panels_have_transient_reorder_offset(hit_ui_region->ui_region_host_region->panels);
    if (suppress_hover_refresh) {
      copy_v2_v2_int(hit_ui_region->ui_region_window_xy, hit_win_xy);
      hit_ui_region->ui_region_hover_region = hit_region;
      hit_ui_region->ui_region_hovered = true;
      BLI_strncpy(
          hit_ui_region->ui_region_hover_subaction_path, subaction_path, XR_MAX_USER_PATH_LENGTH);
      return;
    }
    hit_ui_region->ui_region_suppress_hover_refresh_while_settling = false;
    wm_xr_ui_region_cache_refresh_host(C, hit_ui_region);
    wm_xr_surface_interaction_event_add(C,
                                        hit_ui_region->ui_region_host_win,
                                        hit_ui_region->ui_region_host_area,
                                        hit_region,
                                        MOUSEMOVE,
                                        KM_NOTHING,
                                        hit_win_xy);
    ED_region_tag_redraw(hit_region);
    hit_ui_region->ui_region_dirty = true;
  }

  hit_ui_region->ui_region_hover_region = hit_region;
  copy_v2_v2_int(hit_ui_region->ui_region_window_xy, hit_win_xy);
  hit_ui_region->ui_region_hovered = true;
  BLI_strncpy(
      hit_ui_region->ui_region_hover_subaction_path, subaction_path, XR_MAX_USER_PATH_LENGTH);
}

bool wm_xr_surface_interaction_owns_subaction(const wmXrData *xr,
                                              const wmXrAction *action,
                                              const char *subaction_path,
                                              short event_val)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmXrUiRegion *ui_region = surface_data ? surface_data->active_ui_region : nullptr;
  if (xr == nullptr || action == nullptr || subaction_path == nullptr || surface_data == nullptr ||
      ui_region == nullptr || ui_region->ui_region_host_win == nullptr)
  {
    return false;
  }

  if (!ELEM(event_val, KM_PRESS, KM_RELEASE)) {
    return false;
  }

  if (!wm_xr_surface_action_is_ui_region_click_compatible(action)) {
    return false;
  }

  const bool ui_region_hover_owns_subaction = ui_region->ui_region_hovered &&
                                              ui_region->ui_region_hover_region != nullptr;
  const bool ui_region_pointer_owns_subaction = ui_region->ui_region_pointer.pressed &&
                                                STREQ(ui_region->ui_region_pointer.subaction_path,
                                                      subaction_path);

  if (event_val == KM_PRESS) {
    return ui_region_pointer_owns_subaction || ui_region_hover_owns_subaction;
  }
  return ui_region_pointer_owns_subaction;
}

bool wm_xr_surface_interaction_apply_action(const bContext *C,
                                            wmXrData *xr,
                                            const wmXrAction *action,
                                            const char *subaction_path,
                                            short event_val)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmXrUiRegion *ui_region = surface_data ? surface_data->active_ui_region : nullptr;
  if (C == nullptr || action == nullptr ||
      !wm_xr_surface_interaction_owns_subaction(xr, action, subaction_path, event_val) ||
      surface_data == nullptr || ui_region == nullptr)
  {
    return false;
  }

  /* A press that begins over the ui_region may start capture. After that, only the matching
   * release for that captured action/subaction is rerouted to the ui_region. */
  if (event_val == KM_PRESS) {
    const bool action_is_ui_region_click = wm_xr_surface_action_is_ui_region_click_compatible(
        action);
    if (ui_region->ui_region_pointer.pressed &&
        STREQ(ui_region->ui_region_pointer.subaction_path, subaction_path))
    {
      /* Keep consuming held press events for an active ui_region drag so the ui_region
       * interaction keeps ownership of this controller until release. */
      return true;
    }
    if (!action_is_ui_region_click) {
      return false;
    }
    if (!ui_region->ui_region_hovered || ui_region->ui_region_hover_region == nullptr) {
      return false;
    }
    ARegion *target_region = ui_region->ui_region_hover_region;
    wm_xr_ui_region_cache_refresh_host(C, ui_region);
    wm_xr_surface_interaction_event_add(C,
                                        ui_region->ui_region_host_win,
                                        ui_region->ui_region_host_area,
                                        target_region,
                                        LEFTMOUSE,
                                        KM_PRESS,
                                        ui_region->ui_region_window_xy);
    ui_region->ui_region_pointer.pressed = true;
    ui_region->ui_region_pointer.region = target_region;
    BLI_strncpy(
        ui_region->ui_region_pointer.subaction_path, subaction_path, XR_MAX_USER_PATH_LENGTH);
    ED_region_tag_redraw(ui_region->ui_region_host_region);
    ui_region->ui_region_dirty = true;
    return true;
  }

  if (event_val == KM_RELEASE && ui_region->ui_region_pointer.pressed &&
      STREQ(ui_region->ui_region_pointer.subaction_path, subaction_path))
  {
    ARegion *target_region = ui_region->ui_region_pointer.region ?
                                 ui_region->ui_region_pointer.region :
                                 ui_region->ui_region_hover_region;
    if (target_region == nullptr) {
      return false;
    }
    wm_xr_ui_region_cache_refresh_host(C, ui_region);
    wm_xr_surface_interaction_event_add(C,
                                        ui_region->ui_region_host_win,
                                        ui_region->ui_region_host_area,
                                        target_region,
                                        LEFTMOUSE,
                                        KM_RELEASE,
                                        ui_region->ui_region_window_xy);
    /* Releasing while still hovering the same panel can immediately trigger a hover-refresh
     * relayout that reintroduces transient reorder geometry. Delay that hover-refresh until
     * transient settle completes. */
    ui_region->ui_region_suppress_hover_refresh_while_settling = true;
    wm_xr_ui_region_pointer_clear(ui_region);
    /* Keep the active XR ui_region alive through release handling so any popup opened by the
     * dispatched mouse-release event can still inherit XR ownership. Hover updates clear it
     * naturally once the controller leaves the ui_region. */
    ED_region_tag_redraw(ui_region->ui_region_host_region);
    ui_region->ui_region_frame_tag++;
    ui_region->ui_region_dirty = true;
    return true;
  }

  return false;
}

void wm_xr_draw_ui_regions_world_space(const bContext *C, ARegion * /*region*/, void *customdata)
{
  if (C == nullptr) {
    return;
  }
  BLI_assert(customdata != nullptr);

  wmXrData *xr = static_cast<wmXrData *>(customdata);
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (!surface_data) {
    return;
  }

  gpu::FrameBuffer *prev_framebuffer = GPU_framebuffer_active_get();
  int prev_viewport[4];
  GPU_viewport_size_get_i(prev_viewport);
  float prev_model_view[4][4];
  float prev_projection[4][4];
  GPU_matrix_model_view_get(prev_model_view);
  GPU_matrix_projection_get(prev_projection);

  wmWindow *target_win = (xr->runtime != nullptr && xr->runtime->xr_window != nullptr) ?
                             xr->runtime->xr_window :
                             CTX_wm_window(C);
  bool found_host = false;
  for (wmXrUiRegion *ui_region : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
    if (ui_region->ui_region_host_win != target_win) {
      continue;
    }
    found_host = true;
    if (ui_region->ui_region_last_update_tag == surface_data->ui_regions_frame_tag &&
        !ui_region->ui_region_dirty)
    {
      continue;
    }
    const rcti prev_rect = ui_region->ui_region_rect;
    wm_xr_ui_region_mount_update(ui_region, xr);
    wm_xr_ui_region_cache_refresh_host(C, ui_region);
    if (!BLI_rcti_compare(&prev_rect, &ui_region->ui_region_rect)) {
      /* If layout changes the rect this frame, refresh the mount transform so placement
       * matches the newly sized offscreen content immediately. */
      wm_xr_ui_region_mount_update(ui_region, xr);
    }

    if (!BLI_listbase_is_empty(&ui_region->child_regions)) {
      for (wmXrTempRegion *temp_region : ListBaseWrapper<wmXrTempRegion>(ui_region->child_regions))
      {
        wm_xr_temp_region_cache_update(C, ui_region, temp_region);
      }
    }
    ui_region->ui_region_last_update_tag = surface_data->ui_regions_frame_tag;
  }
  if (!found_host) {
    if (xr->runtime != nullptr && xr->runtime->b_context != nullptr &&
        xr->runtime->xr_window != nullptr)
    {
      bContext *xr_context = xr->runtime->b_context;
      CTX_wm_window_set(xr_context, xr->runtime->xr_window);
      CTX_wm_screen_set(xr_context, xr->runtime->xr_screen);
      CTX_wm_area_set(xr_context, xr->runtime->xr_operator_area);
      CTX_wm_region_set(xr_context, xr->runtime->xr_operator_region);
      WM_xr_surface_ui_regions_update(xr_context, xr);

      for (wmXrUiRegion *ui_region : ListBaseWrapper<wmXrUiRegion>(surface_data->ui_regions)) {
        if (ui_region->ui_region_host_win != target_win) {
          continue;
        }
        found_host = true;
        break;
      }
    }
    if (!found_host) {
      CLOG_ERROR(&LOG, "XR ui_region host not registered");
    }
  }

  if (prev_framebuffer != nullptr) {
    GPU_framebuffer_bind(prev_framebuffer);
  }
  else {
    GPU_framebuffer_restore();
  }
  GPU_viewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
  GPU_scissor_test(false);
  GPU_matrix_projection_set(prev_projection);
  GPU_matrix_set(prev_model_view);
}

}  // namespace blender
