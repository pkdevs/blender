/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"
#include "infos/overlay_wireframe_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_edit_uv_edges)

#include "draw_object_infos_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  float4 inner_color = float4(float3(0.0), 1.0);
  float4 outer_color = float4(0.0);

  float2 dd = gpu_fwidth(stipple_pos);
  float line_distance = distance(stipple_pos, stipple_start) / max(dd.x, dd.y);

  float sel_fac = clamp(selected_fac, 0.0, 1.0);

  if (OVERLAY_UVLineStyle(line_style) == OVERLAY_UV_LINE_STYLE_OUTLINE) {
    if (use_edge_select) {
      /* TODO(@ideasman42): The current wire-edit color contrast enough against the selection.
       * Look into changing the default theme color instead of reducing contrast with edge-select.
       */
      inner_color = (sel_fac != 0.0f) ? theme.colors.edge_select : (theme.colors.wire_edit * 0.5f);
    }
    else {
      inner_color = mix(theme.colors.wire_edit, theme.colors.edge_select, sel_fac);
    }
    outer_color = float4(float3(0.0), 1.0);
  }
  else if (OVERLAY_UVLineStyle(line_style) == OVERLAY_UV_LINE_STYLE_DASH) {
    if (fract(line_distance / float(dash_length)) < 0.5) {
      inner_color = mix(float4(float3(0.35), 1.0), theme.colors.edge_select, sel_fac);
    }
  }
  else if (OVERLAY_UVLineStyle(line_style) == OVERLAY_UV_LINE_STYLE_BLACK) {
    float4 base_color = float4(float3(0.0), 1.0);
    inner_color = mix(base_color, theme.colors.edge_select, sel_fac);
  }
  else if (OVERLAY_UVLineStyle(line_style) == OVERLAY_UV_LINE_STYLE_WHITE) {
    float4 base_color = float4(1.0);
    inner_color = mix(base_color, theme.colors.edge_select, sel_fac);
  }
  else if (OVERLAY_UVLineStyle(line_style) == OVERLAY_UV_LINE_STYLE_SHADOW) {
    inner_color = theme.colors.uv_shadow;
  }

  float mirror_fac = clamp(mirrored_fac, 0.0, 1.0) * (1.0 - sel_fac);
  inner_color = mix(inner_color, theme.colors.edge_mirror_selection, mirror_fac);

  float dist = abs(edge_coord) - max(theme.sizes.edge - 0.5, 0.0);
  float dist_outer = dist - max(theme.sizes.edge, 1.0);
  float mix_w;
  float mix_w_outer;

  if (do_smooth_wire) {
    mix_w = smoothstep(LINE_SMOOTH_START, LINE_SMOOTH_END, dist);
    mix_w_outer = smoothstep(LINE_SMOOTH_START, LINE_SMOOTH_END, dist_outer);
  }
  else {
    mix_w = step(0.5, dist);
    mix_w_outer = step(0.5, dist_outer);
  }

  float4 final_color = mix(outer_color, inner_color, 1.0 - mix_w * outer_color.a);
  final_color.a *= 1.0 - (outer_color.a > 0.0 ? mix_w_outer : mix_w);

  eObjectInfoFlag ob_flag = drw_object_infos().flag;
  bool is_active = flag_test(ob_flag, OBJECT_ACTIVE_EDIT_MODE);
  final_color.a *= is_active ? alpha : (alpha * 0.25);

  frag_color = final_color;
}
