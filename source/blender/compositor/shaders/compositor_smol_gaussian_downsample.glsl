/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_smol_gaussian_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_smol_gaussian_downsample)

/* Each area footprint covers at most three texels per axis. Pair the first two weights
 * into one linear sample; the third texel forms the second group. Return two normalized
 * coordinates and the second group's normalized weight. */
float3 area_axis(float lo, float hi, float physical_size)
{
  float first = floor(lo);
  float w0 = min(hi, first + 1.0f) - lo;
  float w1 = max(0.0f, min(hi, first + 2.0f) - (first + 1.0f));
  float w2 = max(0.0f, hi - (first + 2.0f));
  return float3((first + 0.5f + w1 / (w0 + w1)) / physical_size,
                (first + 2.5f) / physical_size,
                w2 / (hi - lo));
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 output_size = imageSize(output_img);
  if (any(greaterThanEqual(texel, output_size))) {
    return;
  }
  const int2 input_size = source_bounds.xy;
  const int2 logical_output_size = destination_bounds.xy;
  const int2 physical_size = textureSize(input_tx, 0);
  const int2 pixel = texel - destination_bounds.zw;
  const float2 scale = float2(input_size) / float2(logical_output_size);
  float2 lo = float2(pixel) * scale + float2(source_bounds.zw);
  float2 hi = min(float2(pixel + int2(1)) * scale, float2(input_size)) + float2(source_bounds.zw);
  /* Preserve the outermost source edge in the border, integrating along the other axis. */
  for (int axis = 0; axis < 2; axis++) {
    if (pixel[axis] < 0) {
      lo[axis] = 0.0f;
      hi[axis] = 1.0f;
    }
    else if (pixel[axis] >= logical_output_size[axis]) {
      lo[axis] = float(physical_size[axis] - 1);
      hi[axis] = float(physical_size[axis]);
    }
  }
  /* Exact halves, unchanged axes, and their borders need one bilinear read. */
  if ((input_size.x == logical_output_size.x || input_size.x == 2 * logical_output_size.x) &&
      (input_size.y == logical_output_size.y || input_size.y == 2 * logical_output_size.y))
  {
    const float2 uv = (lo + hi) * 0.5f / float2(physical_size);
    imageStore(output_img, texel, textureLod(input_tx, uv, 0.0f));
    return;
  }
  float3 x = area_axis(lo.x, hi.x, float(physical_size.x));
  float3 y = area_axis(lo.y, hi.y, float(physical_size.y));
  /* Uniform decisions: exact halves and unchanged axes need only the first group,
   * including their preserved borders. Pairing preserves area weights apart from
   * hardware filtering precision. */
  bool second_x = input_size.x != logical_output_size.x &&
                  input_size.x != 2 * logical_output_size.x;
  bool second_y = input_size.y != logical_output_size.y &&
                  input_size.y != 2 * logical_output_size.y;
  float4 a = textureLod(input_tx, float2(x.x, y.x), 0.0f);
  if (second_x) {
    a = mix(a, textureLod(input_tx, float2(x.y, y.x), 0.0f), x.z);
  }
  if (second_y) {
    float4 b = textureLod(input_tx, float2(x.x, y.y), 0.0f);
    if (second_x) {
      b = mix(b, textureLod(input_tx, float2(x.y, y.y), 0.0f), x.z);
    }
    a = mix(a, b, y.z);
  }
  imageStore(output_img, texel, a);
}
