/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_dual_kawase_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_dual_kawase_downsample)

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 uv = (float2(texel) + float2(0.5f)) / float2(imageSize(output_img));

  float4 col0 = texture(input_tx, uv);
  float4 col1 = texture(input_tx, uv + uv_step * float2(-0.5f, -0.5f));
  float4 col2 = texture(input_tx, uv + uv_step * float2(-0.5f, +0.5f));
  float4 col3 = texture(input_tx, uv + uv_step * float2(+0.5f, -0.5f));
  float4 col4 = texture(input_tx, uv + uv_step * float2(+0.5f, +0.5f));

  float4 col = (4.0f * col0 + col1 + col2 + col3 + col4) * 0.125f;

  imageStore(output_img, texel, col);
}
