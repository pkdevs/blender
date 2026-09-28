/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_smol_gaussian_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_smol_gaussian_downsample)

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
  float4 color = float4(0.0f);
  /* Integrate pixel areas, including fractional edges on odd-sized images. */
  for (int y = int(floor(lo.y)); y < int(ceil(hi.y)); y++) {
    for (int x = int(floor(lo.x)); x < int(ceil(hi.x)); x++) {
      float2 overlap = max(float2(0.0f), min(hi, float2(x + 1, y + 1)) - max(lo, float2(x, y)));
      color += texelFetch(input_tx, int2(x, y), 0) * overlap.x * overlap.y;
    }
  }
  imageStore(output_img, texel, color / ((hi.x - lo.x) * (hi.y - lo.y)));
}
