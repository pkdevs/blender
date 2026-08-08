/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_test_infos.hh"

COMPUTE_SHADER_CREATE_INFO(gpu_texture_bandwidth_test)

/* Sample mip 0 of a texture in a pattern that defeats the texture cache, so the measured time is
 * dominated by memory bandwidth. Driver internal compression, when it is not disabled by the
 * texture usage flags, shows up as a higher throughput here.
 *
 * The texture this reads is deliberately much larger than the last level cache, otherwise it
 * stays resident and the measurement is bound by texel fetch rate instead of memory bandwidth.
 * Watch the Gtex/s column: if it is flat across formats while GB/s scales with the bytes per
 * texel, the reads are not reaching memory.
 *
 * Each invocation strides far enough per iteration to land on a different page, so consecutive
 * iterations of one invocation never hit the same cache line, while neighboring invocations
 * still read neighboring texels. */
void main()
{
  int2 size = textureSize(source_tx, 0);

  /* The dispatch is a fixed size while the texture is as large as it has to be to escape the
   * cache, so spread the invocations out over the whole texture. */
  int2 dispatch_size = int2(gl_NumWorkGroups.xy * gl_WorkGroupSize.xy);
  int2 stride = max(size / dispatch_size, int2(1));
  int2 coord = int2(gl_GlobalInvocationID.xy) * stride;

  float4 sum = float4(0.0f);
  for (int i = 0; i < sample_count; i++) {
    int2 offset = int2(i * 4093, i * 2039);
    sum += texelFetch(source_tx, (coord + offset) % size, 0);
  }

  /* Store unconditionally so the loads can't be optimized out. The output buffer is much smaller
   * than the texture and written modulo its size, so it stays cache resident and its own traffic
   * does not show up in the measurement. */
  int index = coord.y * size.x + coord.x;
  data_out[index & output_mask] = sum.x + sum.y + sum.z + sum.w;
}
