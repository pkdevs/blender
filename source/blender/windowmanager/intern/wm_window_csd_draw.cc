/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Window client-side-decorations (CSD) drawing.
 */

#include <algorithm>
#include <cmath>

#include "DNA_vec_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_bounds_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.hh"

#include "GHOST_IWindow.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh" /* #GLA_PIXEL_OFS */

#include "WM_api.hh"
#include "wm_window.hh"
#include "wm_window_private.hh" /* Own include. */

#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "BLF_api.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Window Title Bar Drawing
 *
 * For systems with client-side-decorations (CSD).
 * \{ */

void WM_window_csd_draw_titlebar_ex(const int win_size[2],
                                    const char win_state,
                                    const GHOST_CSD_Layout *csd_layout,
                                    const bool is_active,
                                    const uint16_t dpi,
                                    const char *title,
                                    const int font_id,
                                    const int font_size,
                                    const uchar border_color[3],
                                    const uchar text_color[3],
                                    const float alpha)
{
  GHOST_CSD_Elem csd_elems_orig[GHOST_kCSDType_NUM];

  const int fractional_scale[2] = {
      GHOST_CSD_DPI_FRACTIONAL_BASE,
      dpi,
  };
  const int csd_elems_num = WM_window_csd_layout_callback(
      win_size, fractional_scale, win_state, csd_layout, csd_elems_orig);

  if (csd_elems_num <= 0) {
    return;
  }

  const rcti window_rect = {
      /*xmin*/ 0,
      /*xmax*/ win_size[0],
      /*ymin*/ 0,
      /*ymax*/ win_size[1],
  };

  if (border_color) {
    wmWindowViewportTitle_ex(window_rect, 0.0f);
    GPU_clear_color(
        border_color[0] / 255.0f, border_color[1] / 255.0f, border_color[2] / 255.0f, 1.0f);
  }

  /* Flip the Y axis. */
  GHOST_CSD_Elem csd_elems[GHOST_kCSDType_NUM];
  for (int i = 0; i < GHOST_kCSDType_NUM; i++) {
    csd_elems[i].type = GHOST_kCSDTypeBody;
    csd_elems[i].bounds[0][0] = 0;
    csd_elems[i].bounds[0][1] = 0;
    csd_elems[i].bounds[1][0] = 0;
    csd_elems[i].bounds[1][1] = 0;
  }
  for (int i = 0; i < csd_elems_num; i++) {
    GHOST_CSD_Elem *elem = &csd_elems[csd_elems_orig[i].type];
    *elem = csd_elems_orig[i];
    elem->bounds[1][0] = win_size[1] - elem->bounds[1][0];
    elem->bounds[1][1] = win_size[1] - elem->bounds[1][1];
    std::swap(elem->bounds[1][0], elem->bounds[1][1]);
  }

  BLI_assert(csd_elems[GHOST_kCSDTypeTitlebar].type == GHOST_kCSDTypeTitlebar);
  const rcti title_rect = {
      /*xmin*/ csd_elems[GHOST_kCSDTypeTitlebar].bounds[0][0],
      /*xmax*/ csd_elems[GHOST_kCSDTypeTitlebar].bounds[0][1],
      /*ymin*/ csd_elems[GHOST_kCSDTypeTitlebar].bounds[1][0],
      /*ymax*/ csd_elems[GHOST_kCSDTypeTitlebar].bounds[1][1],
  };
  const int rect_size_y = BLI_rcti_size_y(&title_rect);

  wmWindowViewportTitle_ex(title_rect, 0.0f);
  if (title) {
    const float px_offset = -GLA_PIXEL_OFS;
    const size_t title_len = strlen(title);
    uchar color[4];
    if (!is_active) {
      if (border_color) {
        color[0] = uchar((int(border_color[0]) + int(text_color[0])) / 2);
        color[1] = uchar((int(border_color[1]) + int(text_color[1])) / 2);
        color[2] = uchar((int(border_color[2]) + int(text_color[2])) / 2);
      }
      else {
        color[0] = text_color[0] / 2;
        color[1] = text_color[1] / 2;
        color[2] = text_color[2] / 2;
      }
    }
    else {
      ARRAY_SET_ITEMS(color, UNPACK3(text_color), uchar(255 * alpha));
    }
    BLF_color4ubv(font_id, color);
    if (border_color == nullptr) {
      const float shadow_color[4] = {0.0f, 0.0f, 0.0f, alpha};
      BLF_enable(font_id, BLF_SHADOW);
      BLF_shadow(font_id, FontShadowType::Outline, shadow_color);
      BLF_shadow_offset(font_id, 0, 0);
    }
    BLF_enable(font_id, BLF_BOLD);
    BLF_size(font_id, WM_window_csd_fracitonal_scale_apply(int(font_size), fractional_scale));

    const int title_width = BLF_width(font_id, title, title_len);
    const int title_decender = -BLF_descender(font_id);

    const int title_height_max = BLF_height_max(font_id);
    const int offset_y = rect_size_y > title_height_max ? (rect_size_y - title_height_max) / 2 : 0;

    BLF_position(font_id,
                 float(title_rect.xmin + (BLI_rcti_cent_x(&title_rect) - (title_width / 2))) +
                     px_offset,
                 float(title_decender + offset_y) + px_offset,
                 0);

    BLF_draw(font_id, title, title_len);
    BLF_disable(font_id, BLF_BOLD);
    if (border_color == nullptr) {
      BLF_disable(font_id, BLF_SHADOW);
    }
  }

  /* Draw buttons (starting at the title region offset). */
  {
    constexpr int circle_segments = 16;
    GPUVertFormat *format = immVertexFormat();
    const uint shdr_pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

    GPU_blend(GPU_BLEND_ALPHA);

    GPU_polygon_smooth(true);

    const GHOST_TCSD_Type button_types[] = {
        GHOST_kCSDTypeButtonClose,
        GHOST_kCSDTypeButtonMaximize,
        GHOST_kCSDTypeButtonMinimize,
        GHOST_kCSDTypeButtonMenu,
    };

    const int button_icons[] = {
        ICON_X,
        (win_state == GHOST_kWindowStateMaximized) ? ICON_AREA_DOCK : ICON_CHECKBOX_DEHLT,
        ICON_DOT,
        ICON_BLENDER,
    };

    {
      const int button_margin = rect_size_y / 12;
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      if (border_color) {
        immUniformColor4f(1.0f, 1.0f, 1.0f, 0.15f);
      }
      else {
        immUniformColor4f(0.25f, 0.25f, 0.25f, 0.5f * alpha);
      }

      for (int i = 0; i < ARRAY_SIZE(button_types); i++) {
        const GHOST_TCSD_Type ty = button_types[i];
        if (csd_elems[ty].type == GHOST_kCSDTypeBody) [[unlikely]] {
          continue;
        }
        const rcti butrect = {
            /*xmin*/ csd_elems[ty].bounds[0][0] - title_rect.xmin,
            /*xmax*/ csd_elems[ty].bounds[0][1] - title_rect.xmin,
            /*ymin*/ csd_elems[ty].bounds[1][0] - title_rect.ymin,
            /*ymax*/ csd_elems[ty].bounds[1][1] - title_rect.ymin,
        };
        const int but_radius = (BLI_rcti_size_x(&butrect) / 2) - button_margin;
        const int center[2] = {
            BLI_rcti_cent_x(&butrect),
            BLI_rcti_cent_y(&butrect),
        };
        imm_draw_circle_fill_2d(shdr_pos, UNPACK2(center), but_radius, circle_segments);
      }
      immUnbindProgram();
    }

    const float button_color[4] = {1.0f, 1.0f, 1.0f, alpha};
    const int icon_size = WM_window_csd_fracitonal_scale_apply(ICON_DEFAULT_HEIGHT,
                                                               fractional_scale);
    for (int i = 0; i < ARRAY_SIZE(button_types); i++) {
      const GHOST_TCSD_Type ty = button_types[i];
      if (csd_elems[ty].type == GHOST_kCSDTypeBody) [[unlikely]] {
        continue;
      }
      const rcti butrect = {
          /*xmin*/ csd_elems[ty].bounds[0][0] - title_rect.xmin,
          /*xmax*/ csd_elems[ty].bounds[0][1] - title_rect.xmin,
          /*ymin*/ csd_elems[ty].bounds[1][0] - title_rect.ymin,
          /*ymax*/ csd_elems[ty].bounds[1][1] - title_rect.ymin,
      };

      const int xy[2] = {
          BLI_rcti_cent_x(&butrect) - (icon_size / 2),
          BLI_rcti_cent_y(&butrect) - (icon_size / 2),
      };
      BLF_draw_svg_icon(button_icons[i], UNPACK2(xy), icon_size, button_color, 0, false, nullptr);
    }

    GPU_polygon_smooth(false);

    GPU_blend(GPU_BLEND_NONE);
  }
}

void WM_window_csd_draw_titlebar(const wmWindow *win)
{
  BLI_assert(WM_window_is_csd(win));
  const int2 win_size = WM_window_native_pixel_size(win);
  const GHOST_CSD_Layout *csd_layout = WM_window_csd_layout_get();
  GHOST_IWindow *ghost_window = static_cast<GHOST_IWindow *>(win->runtime->ghostwin);
  const uint16_t dpi = ghost_window->getDPIHint();
  const char win_state = GHOST_TWindowState(win->windowstate);
  const std::string window_title = ghost_window->getTitle();
  const char *title = window_title.c_str();
  const bool is_active = (win->active != 0);

  const bScreen *screen = WM_window_get_active_screen(win);
  wm_window_titlebar_theme_context_set(win, screen);

  uchar border_color[3], text_color[3];
  ui::theme::get_color_3ubv(TH_BACK, border_color);
  ui::theme::get_color_3ubv(TH_TEXT_HI, text_color);

  const uiStyle *style = ui::style_get_dpi();
  const uiFontStyle &fstyle = style->paneltitle;

  const int font_id = fstyle.uifont_id;
  const int font_size = fstyle.points;

  const float alpha = 1.0f;
  WM_window_csd_draw_titlebar_ex(win_size,
                                 win_state,
                                 csd_layout,
                                 is_active,
                                 dpi,
                                 title,
                                 font_id,
                                 font_size,
                                 border_color,
                                 text_color,
                                 alpha);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Rounded Corner Drawing
 *
 * \{ */

void WM_window_csd_clear_alpha(const wmWindow *win)
{
  auto *ghost_window = static_cast<GHOST_IWindow *>(win->runtime->ghostwin);
  if (!ghost_window->hasAlpha()) {
    return;
  }

  const int2 win_size = WM_window_native_pixel_size(win);
  const rcti window_rect = {
      .xmin = 0,
      .xmax = win_size[0],
      .ymin = 0,
      .ymax = win_size[1],
  };
  /* Clears are scissor tested and the window frame-buffer keeps its scissor between frames. */
  wmWindowViewportTitle_ex(window_rect, 0.0f);
  /* Only the alpha matters, every pixel of the color is drawn over by the screen below.
   * Could be avoided if the blit shader for viewports wrote an alpha of 1 instead. */
  GPU_clear_color(0.0f, 0.0f, 0.0f, 1.0f);
}

void WM_window_csd_draw_corner_mask(const wmWindow *win)
{
  BLI_assert(WM_window_is_csd(win));

  auto *ghost_window = static_cast<GHOST_IWindow *>(win->runtime->ghostwin);
  if (!ghost_window->hasAlpha()) {
    return;
  }

  const int fractional_scale[2] = {
      GHOST_CSD_DPI_FRACTIONAL_BASE,
      ghost_window->getDPIHint(),
  };
  const int radius = (win->windowstate == GHOST_kWindowStateNormal) ?
                         WM_window_csd_fracitonal_scale_apply(WM_WINDOW_CSD_CORNER_RADIUS,
                                                              fractional_scale) :
                         0;

  if (radius <= 0) {
    return;
  }

  /* The Y axis points up here, so the first two are the *bottom* corners of the window. */
  const GHOST_TWindowTiledFlag tiled_edges = ghost_window->getTiledEdges();
  const std::array<int, 4> corner_tiled_edges{
      GHOST_kWindowTiledLeft | GHOST_kWindowTiledBottom,
      GHOST_kWindowTiledRight | GHOST_kWindowTiledBottom,
      GHOST_kWindowTiledRight | GHOST_kWindowTiledTop,
      GHOST_kWindowTiledLeft | GHOST_kWindowTiledTop,
  };

  const bool all_corners_square = std::ranges::all_of(
      corner_tiled_edges,
      [&](const int corner_edges) { return (tiled_edges & corner_edges) != 0; });
  if (all_corners_square) {
    return;
  }

  const int2 win_size = WM_window_native_pixel_size(win);
  const rcti window_rect = {
      .xmin = 0,
      .xmax = win_size[0],
      .ymin = 0,
      .ymax = win_size[1],
  };
  wmWindowViewportTitle_ex(window_rect, 0.0f);

  GPUVertFormat *format = immVertexFormat();
  const uint shdr_pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  const uint shdr_col = GPU_vertformat_attr_add(
      format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);

  /* The bounding square of each corner, ordered to match #corner_tiled_edges above. */
  const std::array<Bounds<float2>, 4> corner_bounds{
      Bounds<float2>{
          float2(0.0f, 0.0f),
          float2(radius, radius),
      },
      Bounds<float2>{
          float2(win_size[0] - radius, 0.0f),
          float2(win_size[0], radius),
      },
      Bounds<float2>{
          float2(win_size[0] - radius, win_size[1] - radius),
          float2(win_size[0], win_size[1]),
      },
      Bounds<float2>{
          float2(0.0f, win_size[1] - radius),
          float2(radius, win_size[1]),
      },
  };

  /* Scaling the color along with the alpha is what keeps the anti-aliased edge correct, the
   * surface is composited pre-multiplied. */
  GPU_blend(GPU_BLEND_OVERLAY_MASK_FROM_ALPHA);
  immBindBuiltinProgram(GPU_SHADER_3D_SMOOTH_COLOR);

  const int segments = std::clamp(radius / 2, 6, 24);
  /* Half a pixel either side of the arc gives a one pixel wide anti-aliased edge. */
  const float feather = 0.5f;

  for (int corner = 0; corner < 4; corner++) {
    if (tiled_edges & corner_tiled_edges[corner]) {
      continue;
    }
    const Bounds<float2> &bounds = corner_bounds[corner];
    const bool is_max_x = ELEM(corner, 1, 2);
    const bool is_max_y = corner >= 2;

    const float2 window_corner{
        is_max_x ? bounds.max.x : bounds.min.x,
        is_max_y ? bounds.max.y : bounds.min.y,
    };
    const float2 arc_center{
        is_max_x ? bounds.min.x : bounds.max.x,
        is_max_y ? bounds.min.y : bounds.max.y,
    };
    const float2 sign = {is_max_x ? 1.0f : -1.0f, is_max_y ? 1.0f : -1.0f};

    /* Sweep from the vertical window edge round to the horizontal one. */
    auto arc_co = [&](const float angle, const float dist) {
      return float2{arc_center[0] + (sign[0] * dist * std::cos(angle)),
                    arc_center[1] + (sign[1] * dist * std::sin(angle))};
    };

    /* Add one triangle filling in to the corner and two for the feathered edge. */
    immBegin(GPU_PRIM_TRIS, segments * 9);
    for (int i = 0; i < segments; i++) {
      const float angle_a = (float(M_PI_2) * float(i)) / float(segments);
      const float angle_b = (float(M_PI_2) * float(i + 1)) / float(segments);

      const float2 outer_a = arc_co(angle_a, float(radius) + feather);
      const float2 outer_b = arc_co(angle_b, float(radius) + feather);
      const float2 inner_a = arc_co(angle_a, float(radius) - feather);
      const float2 inner_b = arc_co(angle_b, float(radius) - feather);

      /* Fully cut away, between the corner and the outside of the arc. */
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 1.0f);
      immVertex2fv(shdr_pos, window_corner);
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 1.0f);
      immVertex2fv(shdr_pos, outer_a);
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 1.0f);
      immVertex2fv(shdr_pos, outer_b);

      /* Fading out across the arc, leaving the window untouched on the inside. */
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 1.0f);
      immVertex2fv(shdr_pos, outer_a);
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 0.0f);
      immVertex2fv(shdr_pos, inner_a);
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 0.0f);
      immVertex2fv(shdr_pos, inner_b);

      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 1.0f);
      immVertex2fv(shdr_pos, outer_a);
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 0.0f);
      immVertex2fv(shdr_pos, inner_b);
      immAttr4f(shdr_col, 0.0f, 0.0f, 0.0f, 1.0f);
      immVertex2fv(shdr_pos, outer_b);
    }
    immEnd();
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

}  // namespace blender
