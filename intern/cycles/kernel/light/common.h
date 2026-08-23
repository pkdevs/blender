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

// MARK: - Triangle

/* returns true if the triangle is has motion blur or an instancing transform applied */
ccl_device_inline bool triangle_world_space_vertices(
    KernelGlobals kg, const int object, const int prim, const float time, float3 V[3])
{
  bool has_motion = false;
  const uint object_flag = kernel_data_fetch(object_flag, object);

  if (object_flag & SD_OBJECT_HAS_VERTEX_MOTION && time >= 0.0f) {
    motion_triangle_vertices(kg, object, prim, time, V);
    has_motion = true;
  }
  else {
    triangle_vertices(kg, object, prim, V);
  }

  if (!(object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
#ifdef __OBJECT_MOTION__
    const float object_time = (time >= 0.0f) ? time : 0.5f;
    const Transform tfm = object_fetch_transform_motion_test(kg, object, object_time, nullptr);
#else
    Transform tfm = object_fetch_transform(kg, object, OBJECT_TRANSFORM);
#endif
    V[0] = transform_point(&tfm, V[0]);
    V[1] = transform_point(&tfm, V[1]);
    V[2] = transform_point(&tfm, V[2]);
    has_motion = true;
  }
  return has_motion;
}

ccl_device_inline float triangle_light_pdf_area_sampling(const float3 Ng,
                                                         const float3 I,
                                                         const float t)
{
  const float cos_pi = fabsf(dot(Ng, I));

  if (cos_pi == 0.0f) {
    return 0.0f;
  }

  return t * t / cos_pi;
}

ccl_device_forceinline float triangle_light_pdf(KernelGlobals kg,
                                                const ccl_private ShaderData *sd,
                                                const float t)
{
  /* A naive heuristic to decide between costly solid angle sampling
   * and simple area sampling, comparing the distance to the triangle plane
   * to the length of the edges of the triangle. */

  float3 V[3];
  const bool has_motion = triangle_world_space_vertices(kg, sd->object, sd->prim, sd->time, V);

  const float3 e0 = V[1] - V[0];
  const float3 e1 = V[2] - V[0];
  const float3 e2 = V[2] - V[1];
  const float longest_edge_squared = max(len_squared(e0), max(len_squared(e1), len_squared(e2)));
  const float3 N = cross(e0, e1);
  const float distance_to_plane = fabsf(dot(N, sd->wi * t)) / dot(N, N);
  const float area = 0.5f * len(N);

  float pdf;

  if (longest_edge_squared > distance_to_plane * distance_to_plane) {
    /* sd contains the point on the light source
     * calculate Px, the point that we're shading */
    const float3 Px = sd->P + sd->wi * t;

    const float3 A = safe_normalize(V[0] - Px);
    const float3 B = safe_normalize(V[1] - Px);
    const float3 C = safe_normalize(V[2] - Px);

    const float solid_angle = 2.0f * fast_atan2f(fabsf(dot(A, cross(B, C))),
                                                 (1.0f + dot(B, C) + dot(A, C) + dot(A, B)));

    /* distribution_pdf_triangles is calculated over triangle area, but we're not sampling over
     * its area */
    if (UNLIKELY(solid_angle == 0.0f)) {
      return 0.0f;
    }
    pdf = 1.0f / solid_angle;
  }
  else {
    if (UNLIKELY(area == 0.0f)) {
      return 0.0f;
    }

    pdf = triangle_light_pdf_area_sampling(sd->Ng, sd->wi, t) / area;
  }

  /* Belongs in distribution.h but can reuse computations here. */
  if (!kernel_data.integrator.use_light_tree) {
    float distribution_area = area;

    if (has_motion && area != 0.0f) {
      /* For motion blur need area of triangle at fixed time as used in the CDF. */
      triangle_world_space_vertices(kg, sd->object, sd->prim, -1.0f, V);
      distribution_area = triangle_area(V[0], V[1], V[2]);
    }

    pdf *= distribution_area * kernel_data.integrator.distribution_pdf_triangles;
  }

  return pdf;
}

CCL_NAMESPACE_END
