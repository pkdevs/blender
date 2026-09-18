/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_view.bsl.hh"
#include "eevee_defines.hh"
#include "eevee_panoramic_lib.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_math_vector.bsl.hh"

namespace eevee::panoramic_parallax {

struct Resources {
  /** Layer of the parallax cubemap to write into. Matches `ePanoramicFace`. */
  [[push_constant]] const int face_layer;

  [[sampler(0)]] sampler2D combined_tx;
  [[sampler(1)]] sampler2DDepth depth_tx;

  [[image(0, write, PANORAMIC_PARALLAX_FORMAT)]] image2DArray parallax_img;
};

[[compute, local_size(PANORAMIC_PARALLAX_GROUP_SIZE, PANORAMIC_PARALLAX_GROUP_SIZE)]]
void copy([[resource_table]] Resources &srt,
          [[resource_table]] const draw::View &views,
          [[resource_table]] const Uniform &uni,
          [[global_invocation_id]] const uint3 global_id)
{
  const int2 texel = int2(global_id.xy);
  const int2 extent = imageSize(srt.parallax_img).xy;
  if (any(greaterThanEqual(texel, extent))) {
    return;
  }

  const float2 uv = (float2(texel) + 0.5f) / float2(extent);

  const int2 src_extent = textureSize(srt.depth_tx, 0).xy;
  const int2 src_texel = min(int2(uv * float2(src_extent)), src_extent - 1);

  const float3 radiance = textureLod(srt.combined_tx, uv, 0.0f).rgb;
  const float depth = reverse_z::read(texelFetch(srt.depth_tx, src_texel, 0).r);

  /* Scaled off this camera's far clip distance (see `panoramic_parallax_background_distance()`). */
  const float background_distance = eevee::panoramic::panoramic_parallax_background_distance(
      uni.uniform_buf.camera);

  float distance;
  if (depth >= 1.0f) {
    /* World background. */
    distance = background_distance;
  }
  else {
    const ViewMatrices view = views.get(0);
    /* All cubeface subviews share the camera origin so this is the radial distance from the camera.
     * Clamped to avoid being mistaken for the background marker. */
    distance = min(length(view.point_screen_to_view(float3(uv, depth))),
                   background_distance * 0.5f);
    distance = max(distance, 1e-4f);
  }

  imageStoreFast(srt.parallax_img, int3(texel, srt.face_layer), float4(radiance, distance));
}

}  // namespace eevee::panoramic_parallax

PipelineCompute eevee_panoramic_parallax_capture(eevee::panoramic_parallax::copy);
