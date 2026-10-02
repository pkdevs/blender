/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Expand each line into a screen space quad with a configurable width. The vertex data is read
 * from storage buffers, and every invocation computes a single corner of the quad.
 */

#include "infos/overlay_viewer_attribute_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_viewer_flat_geometry_edges)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_attribute_load_lib.glsl"
#include "gpu_shader_index_load_lib.glsl"
#include "gpu_shader_utildefines.bsl.hh"

struct Vert {
  float4 hs_P;
  float3 world_pos;
  float4 color;
};

Vert load_vertex(uint in_vertex_id)
{
  uint v_i = gpu_index_load(in_vertex_id);
  uint color_i = gpu_attr_load_index(v_i, gpu_attr_1);

  Vert vert;
  vert.world_pos = drw_point_object_to_world(gpu_attr_load_float3(pos, gpu_attr_0, v_i));
  vert.hs_P = drw_point_world_to_homogenous(vert.world_pos);
  vert.color = float4(attribute_value[color_i + 0u],
                      attribute_value[color_i + 1u],
                      attribute_value[color_i + 2u],
                      attribute_value[color_i + 3u]);
  return vert;
}

/** Move a vertex behind the near plane onto it, along the line towards the other vertex. */
Vert clip_to_near_plane(Vert vert, Vert other)
{
  float dist = vert.hs_P.z + vert.hs_P.w;
  if (dist >= 0.0f) {
    return vert;
  }
  float t = dist / (dist - (other.hs_P.z + other.hs_P.w));
  vert.hs_P = mix(vert.hs_P, other.hs_P, t);
  vert.world_pos = mix(vert.world_pos, other.world_pos, t);
  vert.color = mix(vert.color, other.color, t);
  return vert;
}

void main()
{
  /* Each line is drawn as two triangles, with the quad corners in triangle strip order:
   * (0, 1, 2) and (3, 2, 1). Corners 0 and 1 are at the line start, 2 and 3 at the end. */
  uint line_id = uint(gl_VertexID) / 6u;
  uint triangle_vert = uint(gl_VertexID) % 3u;
  bool is_second_triangle = ((uint(gl_VertexID) / 3u) % 2u) == 1u;
  uint corner = is_second_triangle ? 3u - triangle_vert : triangle_vert;
  bool is_end = corner >= 2u;
  float side = ((corner & 1u) == 0u) ? 1.0f : -1.0f;

  Vert v0 = load_vertex(line_id * 2u + 0u);
  Vert v1 = load_vertex(line_id * 2u + 1u);
  if (v0.hs_P.z + v0.hs_P.w < 0.0f && v1.hs_P.z + v1.hs_P.w < 0.0f) {
    /* The line is completely behind the near plane. */
    gl_Position = float4(NAN_FLT);
    return;
  }
  Vert clipped_v0 = clip_to_near_plane(v0, v1);
  Vert clipped_v1 = clip_to_near_plane(v1, v0);

  /* Line direction in pixels. */
  float2 line_vec = (clipped_v1.hs_P.xy / clipped_v1.hs_P.w -
                     clipped_v0.hs_P.xy / clipped_v0.hs_P.w) *
                    uniform_buf.size_viewport;
  float line_len = length(line_vec);
  float2 line_dir = (line_len > 1e-6f) ? line_vec / line_len : float2(1.0f, 0.0f);
  float2 line_perp = float2(-line_dir.y, line_dir.x);

  float half_size = edge_width * 0.5f * theme.sizes.pixel;
  /* Add 1px for anti-aliasing. */
  float half_size_aa = half_size + 1.0f;

  /* Offset in pixels. The ends are extended by half the width (square caps) to fill gaps between
   * connected edges. */
  float2 offset = line_perp * side * half_size_aa +
                  line_dir * (is_end ? half_size : -half_size);

  Vert vert = is_end ? clipped_v1 : clipped_v0;
  gl_Position = vert.hs_P;
  /* Convert from pixels to NDC, where the range is [-1..1]. */
  gl_Position.xy += offset * 2.0f * uniform_buf.size_viewport_inv * vert.hs_P.w;
  geometry_out.final_color = vert.color;
  geometry_noperspective_out.edge_coord = side * half_size_aa;
  view_clipping_distances(vert.world_pos);
}
