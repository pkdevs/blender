/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_dual_kawase_infos.hh"

#include "gpu_shader_compositor_texture_utilities.glsl"

COMPUTE_SHADER_CREATE_INFO(compositor_dual_kawase_mix)

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 curr = texture_load(input_tx, texel);
  float4 next = imageLoad(output_img, texel);
  float4 pix = mix(next, curr, input_weight);
  imageStore(output_img, texel, pix);
}
