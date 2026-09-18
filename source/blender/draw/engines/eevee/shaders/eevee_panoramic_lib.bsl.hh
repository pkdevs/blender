/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_camera_lib.bsl.hh"
#include "eevee_defines.hh"
#include "gpu_shader_math_base.bsl.hh"
#include "gpu_shader_math_matrix_transform.bsl.hh"
#include "gpu_shader_math_safe.bsl.hh"
#include "gpu_shader_math_vector.bsl.hh"

namespace eevee::panoramic {

/* -------------------------------------------------------------------- */
/** \name Cubeface mapping
 * \{ */

ePanoramicFace face_index(float3 direction)
{
  float3 abs_direction = abs(direction);
  if (abs_direction.x >= abs_direction.y && abs_direction.x >= abs_direction.z) {
    return (direction.x >= 0.0f) ? PANORAMIC_FACE_POS_X : PANORAMIC_FACE_NEG_X;
  }
  if (abs_direction.y >= abs_direction.z) {
    return (direction.y >= 0.0f) ? PANORAMIC_FACE_POS_Y : PANORAMIC_FACE_NEG_Y;
  }
  return (direction.z >= 0.0f) ? PANORAMIC_FACE_POS_Z : PANORAMIC_FACE_NEG_Z;
}

float2 face_uv_from_direction(float3 direction, ePanoramicFace face_id)
{
  float2 uv;
  switch (face_id) {
    default:
    case PANORAMIC_FACE_POS_X:
      uv = float2(-direction.z, -direction.y) * safe_rcp(direction.x);
      break;
    case PANORAMIC_FACE_NEG_X:
      uv = float2(-direction.z, direction.y) * safe_rcp(direction.x);
      break;
    case PANORAMIC_FACE_POS_Y:
      uv = float2(direction.x, direction.z) * safe_rcp(direction.y);
      break;
    case PANORAMIC_FACE_NEG_Y:
      uv = float2(-direction.x, direction.z) * safe_rcp(direction.y);
      break;
    case PANORAMIC_FACE_POS_Z:
      uv = float2(direction.x, -direction.y) * safe_rcp(direction.z);
      break;
    case PANORAMIC_FACE_NEG_Z:
      uv = float2(direction.x, direction.y) * safe_rcp(direction.z);
      break;
  }
  return uv * 0.5f + 0.5f;
}

/**
 * Convert a non-overscanned cubeface UV into the UV of the subview render target.
 * Because subviews are rendered with a slightly wider frustum.
 */
float2 render_uv_from_face_uv(const CameraData cam, float2 face_uv)
{
  const float overscan = max(cam.panoramic_view_overscan, 1.0f);
  return (face_uv - 0.5f) / overscan + 0.5f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parallax cubemap tracing
 * \{ */

/**
 * Scaled off the camera's far clip distance.
 */
float panoramic_parallax_background_distance(const CameraData cam)
{
  return cam.clip_far * PANORAMIC_PARALLAX_BACKGROUND_MARGIN;
}

struct ParallaxSample {
  /** Shaded radiance recorded in this direction. */
  float3 radiance;
  /**
   * Radial distance from the camera to the recorded surface.
   * `PANORAMIC_PARALLAX_DIST_INVALID` if this direction holds no information.
   */
  float distance;
};

/* Sample the parallax cubemap in world space direction. */
ParallaxSample parallax_sample(const CameraData cam,
                               sampler2DArray parallax_tx,
                               float3 direction_world)
{
  ParallaxSample samp;
  samp.radiance = float3(0.0f);
  samp.distance = PANORAMIC_PARALLAX_DIST_INVALID;

  const float3 direction_view = transform_direction(cam.viewmat, direction_world);
  const ePanoramicFace face = face_index(direction_view);
  if ((cam.panoramic_view_mask & (1u << uint(face))) == 0u) {
    /* This face is not part of the render. */
    return samp;
  }
  /* On the edge between two faces, rounding error can happen. */
  const float2 face_uv = clamp(
      face_uv_from_direction(direction_view, face), float2(0.0f), float2(0.999999f));
  const float2 render_uv = render_uv_from_face_uv(cam, face_uv);
  const float4 data = textureLod(
      parallax_tx, float3(render_uv, float(uint(face))), 0.0f);

  samp.radiance = data.rgb;
  samp.distance = data.a;
  return samp;
}

struct ParallaxTraceResult {
  /** True if the ray was found to intersect the recorded geometry. */
  bool valid;
  /** Radiance at the intersection. */
  float3 radiance;
  /** Distance from the ray origin to the intersection. */
  float time;
};

/**
 * March the ray (`P`, `R`) against the camera radiance + distance cubemap.
 *
 * Depth Texture Parallax Correction (DTPC).
 * Based on "Fast, accurate reflections with parallax-corrected cubemaps" by Alastair Toft
 * https://www.atoft.dev/files/dissertation-redacted.pdf
 * (section 3.2.3)
 *
 * Steps through angle instead of world space, with the distance from the camera at each step
 * obtained by law of sines rather than by marching. Many other modifications were made.
 */
ParallaxTraceResult parallax_trace(
    const CameraData cam, sampler2DArray parallax_tx, float3 P, float3 R, float rand)
{
  ParallaxTraceResult result;
  result.valid = false;
  result.radiance = float3(0.0f);
  result.time = 0.0f;

  const float3 camera_P = cam.viewinv[3].xyz;
  const float3 to_P = P - camera_P;
  const float distance_P = length(to_P);
  if (distance_P < 1e-5f) {
    /* Shading point is at the camera origin. */
    return result;
  }
  /* Arc start direction. */
  const float3 arc_start = to_P / distance_P;
  const float3 ray_dir = normalize(R);

  const float cos_beta = clamp(dot(arc_start, ray_dir), -1.0f, 1.0f);
  /* Arc tangent, orthogonal to `arc_start`, inside the (arc_start, ray_dir) plane. */
  float3 arc_tangent = ray_dir - arc_start * cos_beta;
  const float arc_tangent_len = length(arc_tangent);
  if (arc_tangent_len < 1e-4f) {
    /* Every point projects onto the same texel. */
    return result;
  }
  arc_tangent /= arc_tangent_len;

  const float beta = acos(cos_beta);
  const float sin_beta = arc_tangent_len;

  const float step_angle = beta / float(PANORAMIC_PARALLAX_TRACE_STEP);
  const float max_distance = cam.clip_far;
  const float background_distance = panoramic_parallax_background_distance(cam);

  /* Distance from the camera to the ray point at the previous step. Used to size the contact
   * tolerance from the local step size. */
  float prev_distance = distance_P;
  /* Set once the ray is nearer than the sampled surface or past it in background. Avoids matching the contact test against the ray's starting surface. */
  bool in_front = false;

  for (int i = 1; i <= PANORAMIC_PARALLAX_TRACE_STEP; i++) {
    /* Jittered angle. */
    const float alpha = step_angle * (float(i) - rand);
    /* Law of sines in the camera-P-hit triangle. */
    const float sin_remaining = sin(beta - alpha);
    if (sin_remaining < 1e-5f) {
      break;
    }
    const float ray_distance = distance_P * sin_beta / sin_remaining;
    if (ray_distance > max_distance) {
      break;
    }

    const float3 direction = arc_start * cos(alpha) + arc_tangent * sin(alpha);
    const ParallaxSample samp = parallax_sample(cam, parallax_tx, direction);

    if (samp.distance <= PANORAMIC_PARALLAX_DIST_INVALID) {
      /* No data in this direction (a masked-out face) - the arc can still cross back into a
       * recorded face further along, so keep marching instead of giving up. */
      prev_distance = ray_distance;
      continue;
    }

    if (samp.distance >= background_distance) {
      /* The camera sees the world background in this direction. */
      in_front = true;
      prev_distance = ray_distance;
      continue;
    }

    /* Tolerance of the contact test. Widened by how far the ray moves between steps. */
    const float thickness = abs(ray_distance - prev_distance) +
                            samp.distance * PANORAMIC_PARALLAX_TRACE_BIAS;
    if (in_front && abs(ray_distance - samp.distance) < thickness) {
      result.valid = true;
      result.radiance = samp.radiance;
      result.time = ray_distance * sin(alpha) / max(sin_beta, 1e-5f);
      break;
    }

    if (ray_distance < samp.distance) {
      in_front = true;
    }
    prev_distance = ray_distance;
  }

  return result;
}

/** \} */

}  // namespace eevee::panoramic
