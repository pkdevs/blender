/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_smol_gaussian_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_smol_gaussian_filter)

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 size = imageSize(output_img);
  if (any(greaterThanEqual(texel, size))) {
    return;
  }
  float2 uv = (float2(texel) + float2(0.5f)) / float2(size);
  float4 color = texelFetch(input_tx, texel, 0) * center_weight;
  for (int i = 0; i < tap_count; i++) {
    float2 offset = uv_step * taps[i].x;
    color += (textureLod(input_tx, uv - offset, 0.0f) + textureLod(input_tx, uv + offset, 0.0f)) *
             taps[i].y;
  }
  imageStore(output_img, texel, color);
}
