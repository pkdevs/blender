/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_smol_gaussian_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_smol_gaussian_reconstruct)

/* Return coordinates in the padded texture directly, avoiding a logical-UV round trip
 * and repeated padding transforms for each sample. */
float3 cubic_axis(float uv, int size, int output_size, int padding)
{
  float inverse_physical_size = 1.0f / float(size + 2 * padding);
  if (2 * size >= output_size) {
    float p = (uv * float(size) + float(padding)) * inverse_physical_size;
    return float3(p, p, 0.0f);
  }
  float p = uv * float(size) - 0.5f;
  float base = floor(p);
  float f = p - base;
  float w0 = (1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f;
  float w1 = (3.0f * f * f * f - 6.0f * f * f + 4.0f) / 6.0f;
  float w2 = (-3.0f * f * f * f + 3.0f * f * f + 3.0f * f + 1.0f) / 6.0f;
  float w3 = f * f * f / 6.0f;
  return float3((base - 0.5f + w1 / (w0 + w1) + float(padding)) * inverse_physical_size,
                (base + 1.5f + w3 / (w2 + w3) + float(padding)) * inverse_physical_size,
                w2 + w3);
}

float4 reconstruct(sampler2D image, float2 uv, int2 output_size, int4 bounds)
{
  int2 size = bounds.xy;
  float3 x = cubic_axis(uv.x, size.x, output_size.x, bounds.z);
  float3 y = cubic_axis(uv.y, size.y, output_size.y, bounds.w);
  float4 a = textureLod(image, float2(x.x, y.x), 0.0f);
  if (2 * size.x < output_size.x) {
    a = mix(a, textureLod(image, float2(x.y, y.x), 0.0f), x.z);
  }
  if (2 * size.y < output_size.y) {
    float4 b = textureLod(image, float2(x.x, y.y), 0.0f);
    if (2 * size.x < output_size.x) {
      b = mix(b, textureLod(image, float2(x.y, y.y), 0.0f), x.z);
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
