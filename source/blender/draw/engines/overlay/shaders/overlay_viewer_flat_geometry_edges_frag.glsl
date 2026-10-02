/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_viewer_attribute_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_viewer_flat_geometry_edges)

#include "overlay_common_lib.glsl"

void main()
{
  float width = edge_width * theme.sizes.pixel;
  /* Distance in pixels from the edge of the line, anti-aliased with a smooth falloff. */
  float dist = abs(geometry_noperspective_out.edge_coord) - (width * 0.5f - 0.5f);
  float coverage = 1.0f - smoothstep(LINE_SMOOTH_START, LINE_SMOOTH_END, dist);
  /* Fade out lines that are thinner than a pixel instead of making them thinner. */
  coverage *= min(width, 1.0f);

  out_color = geometry_out.final_color;
  out_color.a *= coverage;
  if (out_color.a <= 0.0f) {
    /* Avoid writing depth for the fully transparent anti-aliasing margin. */
    gpu_discard_fragment();
    return;
  }
  line_output = pack_line_data_no_aa();
}
