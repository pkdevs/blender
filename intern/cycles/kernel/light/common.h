/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/types.h"

#include "kernel/sample/mapping.h"

#include "kernel/geom/motion_triangle.h"
#include "kernel/geom/triangle.h"

CCL_NAMESPACE_BEGIN

/* Result of evaluating a light from an intersection. */
struct LightEval {
  float eval_fac = 0.0f; /* Intensity multiplier (normalization, spot falloff) */
  float pdf = 0.0f;      /* PDF for light sampling with next event estimation sampling. */
};

/* Utilities */

ccl_device_inline float3 ellipse_sample(const float3 ru, const float3 rv, const float2 rand)
{
  const float2 uv = sample_uniform_disk(rand);
  return ru * uv.x + rv * uv.y;
}

ccl_device_inline float3 rectangle_sample(const float3 ru, const float3 rv, const float2 rand)
{
  return ru * (2.0f * rand.x - 1.0f) + rv * (2.0f * rand.y - 1.0f);
}

ccl_device float3 disk_light_sample(const float3 n, const float2 rand)
{
  float3 ru;
  float3 rv;

  make_orthonormals(n, &ru, &rv); // !!!
  /*
  Cycles' existing spot sampling disk is not actually a fixed XY disk in the 
  spot's local coordinate system. Its orientation is constructed from:
  lightN = normalize(P - klight->co);
  */

  return ellipse_sample(ru, rv, rand);
}

ccl_device float light_pdf_area_to_solid_angle(const float3 Ng, const float3 I, const float t)
{
  const float cos_pi = dot(Ng, I);

  if (cos_pi <= 0.0f) {
    return 0.0f;
  }

  return t * t / cos_pi;
}

/* Visibility flag om the light shader. */
ccl_device_inline bool is_light_shader_visible_to_path(const int shader,
                                                       const PathRayVisibility path_visibility,
                                                       const uint32_t path_flag)
{
  if ((shader & SHADER_EXCLUDE_ANY) == 0) {
    return true;
  }

  if (((shader & SHADER_EXCLUDE_DIFFUSE) && (path_visibility & PATH_RAY_VISIBILITY_DIFFUSE)) ||
      ((shader & SHADER_EXCLUDE_GLOSSY) &&
       ((path_visibility & PATH_RAY_VISIBILITY_GLOSSY) && (path_flag & PATH_RAY_REFLECT))) ||
      ((shader & SHADER_EXCLUDE_TRANSMIT) && (path_visibility & PATH_RAY_VISIBILITY_TRANSMIT)) ||
      ((shader & SHADER_EXCLUDE_CAMERA) && (path_visibility & PATH_RAY_VISIBILITY_CAMERA)) ||
      ((shader & SHADER_EXCLUDE_SCATTER) &&
       (path_visibility & PATH_RAY_VISIBILITY_VOLUME_SCATTER)))
  {
    return false;
  }

  return true;
}

ccl_device_inline LightSampleReference light_reference_empty()
{
  LightSampleReference reference{};
  reference.object = OBJECT_NONE;
  reference.prim = PRIM_NONE;
  return reference;
}

ccl_device_inline bool light_reference_is_valid(
    const ccl_private LightSampleReference *reference)
{
  return (reference->flags & RESTIR_LIGHT_REFERENCE_VALID) != 0;
}

CCL_NAMESPACE_END
