/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_uv_verts)

#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  /* TODO: Theme? */
  constexpr float4 pinned_col = float4(1.0f, 0.0f, 0.0f, 1.0f);

  bool is_selected = (flag.x & (VERT_UV_SELECT | FACE_UV_SELECT)) != 0u;
  bool is_pinned = (flag.x & VERT_UV_PINNED) != 0u;
  bool is_mirrored = (flag.x & (VERT_MIRRORED_UV_SELECT | FACE_MIRRORED_UV_SELECT)) != 0u;

  // Priority order: Selected > Mirrored > Pinned > Default
  if (is_selected) {
    fill_color = theme.colors.vert_select;
  }
  else if (is_mirrored) {
    fill_color = theme.colors.vertex_mirror_selection;
  }
  else if (is_pinned) {
    fill_color = pinned_col;
  }
  else {
    fill_color = float4(color.rgb, 1.0f);
  }

  outline_color = (is_pinned) ? pinned_col : float4(fill_color.rgb, 0.0f);

  float3 world_pos = float3(au, 0.0f);
  float depth = (is_selected || is_mirrored) ? (is_pinned ? 0.05f : 0.10f) : 0.15f;
  gl_Position = float4(drw_point_world_to_homogenous(world_pos).xy, depth, 1.0f);
  gl_PointSize = dot_size;

  /* calculate concentric radii in pixels */
  float radius = 0.5f * dot_size;

  /* start at the outside and progress toward the center */
  radii[0] = radius;
  radii[1] = radius - 1.0f;
  radii[2] = radius - outline_width;
  radii[3] = radius - outline_width - 1.0f;

  /* convert to PointCoord units */
  radii /= dot_size;
}
