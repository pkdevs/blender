/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_test_infos.hh"

COMPUTE_SHADER_CREATE_INFO(gpu_texture_bandwidth_test)

/* Sample mip 0 of a texture in a pattern that defeats the texture cache, so the measured time is
 * dominated by memory bandwidth. Driver internal compression, when it is not disabled by the
 * texture usage flags, shows up as a higher throughput here.
 *
 * Each invocation reads from a different 512 texel wide band per iteration, so consecutive
 * iterations of one invocation never hit the same cache line, while neighboring invocations
 * still read neighboring texels. */
void main()
{
  int2 size = textureSize(source_tx, 0);
  int2 coord = int2(gl_GlobalInvocationID.xy);

  float4 sum = float4(0.0f);
  for (int i = 0; i < sample_count; i++) {
    int2 offset = int2(i * 509, i * 251);
    sum += texelFetch(source_tx, (coord + offset) % size, 0);
  }

  /* Store unconditionally so the loads can't be optimized out. */
  int index = coord.y * size.x + coord.x;
  data_out[index] = sum.x + sum.y + sum.z + sum.w;
}
