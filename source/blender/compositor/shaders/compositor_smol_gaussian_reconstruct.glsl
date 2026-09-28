/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_smol_gaussian_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_smol_gaussian_reconstruct)

float3 cubic_axis(float uv, int size, int output_size)
{
  if (2 * size >= output_size) {
    return float3(uv, uv, 0.0f);
  }
  float p = uv * float(size) - 0.5f;
  float base = floor(p);
  float f = p - base;
  float w0 = (1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f;
  float w1 = (3.0f * f * f * f - 6.0f * f * f + 4.0f) / 6.0f;
  float w2 = (-3.0f * f * f * f + 3.0f * f * f + 3.0f * f + 1.0f) / 6.0f;
  float w3 = f * f * f / 6.0f;
  return float3((base - 0.5f + w1 / (w0 + w1)) / float(size),
                (base + 1.5f + w3 / (w2 + w3)) / float(size),
                w2 + w3);
}

/* Map logical coordinates into the texture, including its preserved border. */
float2 padded_uv(float2 uv, int4 bounds)
{
  return (uv * float2(bounds.xy) + float2(bounds.zw)) / float2(bounds.xy + 2 * bounds.zw);
}

float4 reconstruct(sampler2D image, float2 uv, int2 output_size, int4 bounds)
{
  int2 size = bounds.xy;
  float3 x = cubic_axis(uv.x, size.x, output_size.x);
  float3 y = cubic_axis(uv.y, size.y, output_size.y);
  float4 a = textureLod(image, padded_uv(float2(x.x, y.x), bounds), 0.0f);
  if (2 * size.x < output_size.x) {
    a = mix(a, textureLod(image, padded_uv(float2(x.y, y.x), bounds), 0.0f), x.z);
  }
  if (2 * size.y < output_size.y) {
    float4 b = textureLod(image, padded_uv(float2(x.x, y.y), bounds), 0.0f);
    if (2 * size.x < output_size.x) {
      b = mix(b, textureLod(image, padded_uv(float2(x.y, y.y), bounds), 0.0f), x.z);
    }
    a = mix(a, b, y.z);
  }
  return a;
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 size = imageSize(output_img);
  if (any(greaterThanEqual(texel, size))) {
    return;
  }
  float2 uv = (float2(texel) + float2(0.5f)) / float2(size);
  float4 color = reconstruct(input_tx, uv, size, base_bounds) * (1.0f - weights.x - weights.y);
  if (weights.x > 0.0f) {
    color += reconstruct(neighbor_tx, uv, size, neighbor_bounds) * weights.x;
  }
  if (weights.y > 0.0f) {
    color += reconstruct(diagonal_tx, uv, size, diagonal_bounds) * weights.y;
  }
  imageStore(output_img, texel, color);
}
