/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_sculpt_debug)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

/* Per-leaf debug palette. In the sculpt (draw_multi) path each PBVH leaf is one
 * instance, so `gpu_InstanceIndex` identifies the leaf. */
static const float3 debug_palette[9] = {
    float3(1.0, 0.2, 0.2),
    float3(0.2, 1.0, 0.2),
    float3(0.2, 0.2, 1.0),
    float3(1.0, 1.0, 0.2),
    float3(0.2, 1.0, 1.0),
    float3(1.0, 0.2, 1.0),
    float3(1.0, 0.7, 0.2),
    float3(0.2, 1.0, 0.7),
    float3(0.7, 0.2, 1.0),
};

void main()
{
  debug_color = float4(debug_palette[gpu_InstanceIndex % 9], debug_opacity);
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);
  view_clipping_distances(world_pos);
}
