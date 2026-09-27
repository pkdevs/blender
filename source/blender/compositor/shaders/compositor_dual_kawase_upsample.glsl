/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_dual_kawase_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_dual_kawase_upsample)

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 uv = (float2(texel) + float2(0.5f)) / float2(imageSize(output_img));

  float4 col0 = texture(input_tx, uv + uv_step * float2(0, -1));
  float4 col1 = texture(input_tx, uv + uv_step * float2(0, +1));
  float4 col2 = texture(input_tx, uv + uv_step * float2(-1, 0));
  float4 col3 = texture(input_tx, uv + uv_step * float2(+1, 0));

  float4 col4 = texture(input_tx, uv + uv_step * float2(-0.5f, -0.5f));
  float4 col5 = texture(input_tx, uv + uv_step * float2(-0.5f, +0.5f));
  float4 col6 = texture(input_tx, uv + uv_step * float2(+0.5f, -0.5f));
  float4 col7 = texture(input_tx, uv + uv_step * float2(+0.5f, +0.5f));

  float4 col = (col0 + col1 + col2 + col3 + (col4 + col5 + col6 + col7) * 2.0f) * (1.0f / 12.0f);

  imageStore(output_img, texel, col);
}
