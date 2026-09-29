/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

namespace compositor::downsampled_gaussian::downsampling {

struct Resources {
  [[compilation_constant]] const bool use_4x;

  [[push_constant]] int4 source_bounds;
  [[push_constant]] int4 destination_bounds;

  [[sampler(0)]] sampler2D input_tx;
  [[image(0, write, SFLOAT_16_16_16_16)]] image2D output_img;
};

/* Footprint covers at most three texels per axis. Use one linear sample for first
 * two weights. Return two normalized coordinates, and weight for the second sample. */
float3 area_axis(float lo, float hi, float physical_size)
{
  const float first = floor(lo);
  const float w0 = min(hi, first + 1.0f) - lo;
  const float w1 = max(0.0f, min(hi, first + 2.0f) - (first + 1.0f));
  const float w2 = max(0.0f, hi - (first + 2.0f));
  return float3((first + 0.5f + w1 / (w0 + w1)) / physical_size,
                (first + 2.5f) / physical_size,
                w2 / (hi - lo));
}

[[compute, local_size(16, 16)]]
void compute_main([[resource_table]] Resources &srt,
                  [[global_invocation_id]] const uint3 global_id)
{
  const int2 texel = int2(global_id.xy);
  const int2 output_size = imageSize(srt.output_img);
  if (any(greaterThanEqual(texel, output_size))) {
    return;
  }

  if (srt.use_4x) [[static_branch]] {
    /* Exact 4x downsampling code path.
     * Four bilinear reads average a 4x4 footprint, or two reads average a 4x1 footprint.
     * Unchanged axes and preserved borders use one coordinate. */
    const int2 physical_size = textureSize(srt.input_tx, 0);
    const int2 scale = srt.source_bounds.xy / srt.destination_bounds.xy;
    const int2 pixel = texel - srt.destination_bounds.zw;
    float2 center = (float2(pixel) + 0.5f) * float2(scale) + float2(srt.source_bounds.zw);
    float2 offset = float2(scale.x == 4 ? 1.0f : 0.0f, scale.y == 4 ? 1.0f : 0.0f);
    for (int axis = 0; axis < 2; axis++) {
      if (pixel[axis] < 0) {
        center[axis] = 0.5f;
        offset[axis] = 0.0f;
      }
      else if (pixel[axis] >= srt.destination_bounds[axis]) {
        center[axis] = float(physical_size[axis]) - 0.5f;
        offset[axis] = 0.0f;
      }
    }
    const float2 lo = (center - offset) / float2(physical_size);
    const float2 hi = (center + offset) / float2(physical_size);
    float4 color = textureLod(srt.input_tx, lo, 0.0f);
    if (scale.x == 4) {
      color = 0.5f * (color + textureLod(srt.input_tx, float2(hi.x, lo.y), 0.0f));
    }
    if (scale.y == 4) {
      float4 row = textureLod(srt.input_tx, float2(lo.x, hi.y), 0.0f);
      if (scale.x == 4) {
        row = 0.5f * (row + textureLod(srt.input_tx, hi, 0.0f));
      }
      color = 0.5f * (color + row);
    }
    imageStore(srt.output_img, texel, color);
  }
  else {
    const int2 input_size = srt.source_bounds.xy;
    const int2 logical_output_size = srt.destination_bounds.xy;
    const int2 physical_size = textureSize(srt.input_tx, 0);
    const int2 pixel = texel - srt.destination_bounds.zw;
    const float2 scale = float2(input_size) / float2(logical_output_size);
    float2 lo = float2(pixel) * scale + float2(srt.source_bounds.zw);
    float2 hi = min(float2(pixel + int2(1)) * scale, float2(input_size)) +
                float2(srt.source_bounds.zw);
    /* Preserve the outermost source edge in the border. */
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
      imageStore(srt.output_img, texel, textureLod(srt.input_tx, uv, 0.0f));
      return;
    }
    const float3 x_axis = area_axis(lo.x, hi.x, float(physical_size.x));
    const float3 y_axis = area_axis(lo.y, hi.y, float(physical_size.y));
    /* Exact halves and unchanged axes need only the first group, including their preserved
     * borders. */
    const bool second_x = input_size.x != logical_output_size.x &&
                          input_size.x != 2 * logical_output_size.x;
    const bool second_y = input_size.y != logical_output_size.y &&
                          input_size.y != 2 * logical_output_size.y;
    float4 color = textureLod(srt.input_tx, float2(x_axis.x, y_axis.x), 0.0f);
    if (second_x) {
      color = mix(color, textureLod(srt.input_tx, float2(x_axis.y, y_axis.x), 0.0f), x_axis.z);
    }
    if (second_y) {
      float4 row = textureLod(srt.input_tx, float2(x_axis.x, y_axis.y), 0.0f);
      if (second_x) {
        row = mix(row, textureLod(srt.input_tx, float2(x_axis.y, y_axis.y), 0.0f), x_axis.z);
      }
      color = mix(color, row, y_axis.z);
    }
    imageStore(srt.output_img, texel, color);
  }
}

}  // namespace compositor::downsampled_gaussian::downsampling

#ifndef GLSL_CPP_STUBS
PipelineCompute compositor_downsampled_gaussian_downsample(
    compositor::downsampled_gaussian::downsampling::compute_main,
    compositor::downsampled_gaussian::downsampling::Resources{.use_4x = false});
PipelineCompute compositor_downsampled_gaussian_downsample_4x(
    compositor::downsampled_gaussian::downsampling::compute_main,
    compositor::downsampled_gaussian::downsampling::Resources{.use_4x = true});
#endif
