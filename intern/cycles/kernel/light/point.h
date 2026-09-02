/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/geom/object.h"

#include "util/defines.h"
#include "util/math_intersect.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline bool point_light_sample(const ccl_global KernelLight *klight,
                                          const float2 rand,
                                          const float3 P,
                                          const float3 N,
                                          const int shader_flags,
                                          ccl_private LightSample *ls)
{
  const float r_sq = sqr(klight->spot.radius);

  float3 lightN = P - klight->co;
  const float d_sq = len_squared(lightN);
  const float d = sqrtf(d_sq);
  lightN /= d;

  ls->eval_fac = klight->spot.eval_fac;

  if (klight->spot.is_sphere) {
    /* Spherical light geometry. */
    float cos_theta;
    if (d_sq > r_sq) {
      /* Outside sphere. */
      const float one_minus_cos = sin_sqr_to_one_minus_cos(r_sq / d_sq);
      ls->D = sample_uniform_cone(-lightN, one_minus_cos, rand, &cos_theta, &ls->pdf);
    }
    else {
      /* Inside sphere. */
      const bool has_transmission = (shader_flags & SD_BSDF_HAS_TRANSMISSION);
      if (has_transmission) {
        ls->D = sample_uniform_sphere(rand);
        ls->pdf = M_1_2PI_F * 0.5f;
      }
      else {
        sample_cos_hemisphere(N, rand, &ls->D, &ls->pdf);
      }
      cos_theta = -dot(ls->D, lightN);
    }

    /* Law of cosines. */
    ls->t = d * cos_theta -
            copysignf(safe_sqrtf(r_sq - d_sq + d_sq * sqr(cos_theta)), d_sq - r_sq);

    /* Remap sampled point onto the sphere to prevent precision issues with small radius. */
    ls->P = P + ls->D * ls->t;
    ls->Ng = normalize(ls->P - klight->co);
    ls->P = ls->Ng * klight->spot.radius + klight->co;
  }
  else {
    /* Point light with ad-hoc radius based on oriented disk. */
    ls->P = klight->co;
    if (r_sq > 0.0f) {
      ls->P += disk_light_sample(lightN, rand) * klight->spot.radius;
    }

    ls->D = safe_normalize_len(ls->P - P, &ls->t);
    ls->Ng = -ls->D;

    /* PDF. */
    const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;
    ls->pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ls->D, ls->t);
  }

  return true;
}

ccl_device_forceinline float sphere_light_pdf(
    const float d_sq, const float r_sq, const float3 N, const float3 D, const uint32_t path_flag)
{
  if (d_sq > r_sq) {
    return M_1_2PI_F / sin_sqr_to_one_minus_cos(r_sq / d_sq);
  }

  const bool has_transmission = (path_flag & PATH_RAY_MIS_HAD_TRANSMISSION);
  return has_transmission ? M_1_2PI_F * 0.5f : pdf_cos_hemisphere(N, D);
}

ccl_device_forceinline float2 point_light_uv(KernelGlobals kg, // !!!
                                             const ccl_global KernelLight *klight,
                                             const float3 Ng)
{
  /* Texture coordinates. */
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const float2 uv = map_to_sphere(transform_direction(&itfm, Ng));
  /* NOTE: Return barycentric coordinates in the same notation as Embree and OptiX. */
  return make_float2(uv.y, 1.0f - uv.x - uv.y);
}

ccl_device_forceinline void point_light_mnee_sample_update(const ccl_global KernelLight *klight,
                                                           ccl_private LightSample *ls,
                                                           const float3 P,
                                                           const float3 N,
                                                           const uint32_t path_flag)
{
  ls->D = safe_normalize_len(ls->P - P, &ls->t);

  const float radius = klight->spot.radius;

  if (klight->spot.is_sphere) {
    const float d_sq = len_squared(P - klight->co);
    const float r_sq = sqr(radius);
    const float t_sq = sqr(ls->t);

    /* NOTE : preserve pdf in area measure. */
    const float jacobian_solid_angle_to_area = 0.5f * fabsf(d_sq - r_sq - t_sq) /
                                               (radius * ls->t * t_sq);
    ls->pdf = sphere_light_pdf(d_sq, r_sq, N, ls->D, path_flag) * jacobian_solid_angle_to_area;

    ls->Ng = normalize(ls->P - klight->co);
  }
  else {
    /* NOTE : preserve pdf in area measure. */
    ls->pdf = ls->eval_fac * 4.0f * M_PI_F;

    ls->Ng = -ls->D;
  }
}

ccl_device_inline bool point_light_intersect(const ccl_global KernelLight *klight,
                                             const ccl_private Ray *ccl_restrict ray,
                                             ccl_private float *t)
{
  const float radius = klight->spot.radius;
  if (radius == 0.0f) {
    return false;
  }

  if (klight->spot.is_sphere) {
    float3 P;
    return ray_sphere_intersect(ray->P, ray->D, ray->tmin, ray->tmax, klight->co, radius, &P, t);
  }

  float3 P;
  const float3 diskN = normalize(ray->P - klight->co);
  return ray_disk_intersect(
      ray->P, ray->D, ray->tmin, ray->tmax, klight->co, diskN, radius, &P, t);
}

ccl_device_inline bool point_light_sample_from_intersection(KernelGlobals kg,
                                                            const ccl_global KernelLight *klight,
                                                            const float3 ray_P,
                                                            const float3 ray_D,
                                                            const float3 N,
                                                            const uint32_t path_flag,
                                                            ccl_private LightSample *ccl_restrict ls,
                                                            ccl_private LightSampleReference *ref)
{
  const float r_sq = sqr(klight->spot.radius);

  ls->eval_fac = klight->spot.eval_fac;

  if (klight->spot.is_sphere) {
    const float d_sq = len_squared(ray_P - klight->co);
    ls->pdf = sphere_light_pdf(d_sq, r_sq, N, ray_D, path_flag);
    ls->Ng = normalize(ls->P - klight->co);
  }
  else {
    if (ls->t != FLT_MAX) {
      const float3 lightN = normalize(ray_P - klight->co);
      const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;
      ls->pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ray_D, ls->t);
    }
    else {
      ls->pdf = 0.0f;
    }
    ls->Ng = -ray_D;
  }

  /* Texture coordinates saved to reference param. */
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const float2 uv = map_to_sphere(transform_direction(&itfm, ls->Ng));
  ref->param.x = uv.y;
  ref->param.y = 1.0f - uv.x - uv.y;

  return true;
}

ccl_device_inline bool point_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private LightSampleReference *reference)
{
  *reference = light_reference_empty();

  if (ls->type != LIGHT_POINT || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
  if ((LightType)klight->type != LIGHT_POINT) {
    return false;
  }

  reference->type = LIGHT_POINT;
  reference->object = ls->object;
  reference->prim = ls->prim;

  if (klight->spot.is_sphere) {
    const Transform itfm = lamp_get_inverse_transform(kg, klight);
    const float3 local_Ng = normalize(transform_direction(&itfm, ls->Ng));

    reference->param = map_to_sphere(local_Ng);
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                       RESTIR_LIGHT_REFERENCE_HAS_PARAM;
    return true;
  }
  /* None-Spherical Point, i.e. Disk, LA_USE_SOFT_FALLOFF. */
  else {
    /*
     * Non-spherical point light:
     * The persistent identity is simply the Point emitter itself,
     * with no additional geometric parameter.
     */
    reference->type = LIGHT_POINT;
    reference->object = ls->object;
    reference->prim = ls->prim;
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID;

    return true;
  }
}

ccl_device_inline bool point_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private LightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags, // !!!(Tri): Unused
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  if (!light_reference_is_valid(reference) ||
      reference->type != LIGHT_POINT || reference->prim < 0)
  {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_POINT) {
    return false;
  }

/* TODO(Tri): For relocated sphere-light reuse, verify that ls->P is the
   first valid intersection from target P along ls->D. A persisted point on the
   far side of the sphere may not be a valid sample from the new target.
 * Use point_light_intersect() with a scale-aware tolerance and reject on mismatch.
 */
  ls->type = LIGHT_POINT;
  ls->shader = klight->shader_id;
  ls->object = klight->object_id;
  ls->prim = reference->prim;
  ls->group = object_lightgroup(kg, ls->object);
  ls->eval_fac = klight->spot.eval_fac;

  /*
   * These are not available from the current reference contract. Callers must
   * restore them from emitter-selection state before using the sample for MIS.
   */
  ls->pdf_selection = 0.0f;
  ls->emitter_id = EMITTER_NONE;

  const float r_sq = sqr(klight->spot.radius);

  if (klight->spot.is_sphere) {
    if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
      return false;
    }

    const float3 local_Ng = map_to_sphere(reference->param);
    const Transform itfm = lamp_get_inverse_transform(kg, klight);
    const Transform tfm = transform_inverse(itfm);

    ls->Ng = normalize(transform_direction(&tfm, local_Ng));
    ls->P = klight->co + float3(ls->Ng) * klight->spot.radius;
    ls->D = safe_normalize_len(ls->P - P, &ls->t);

    const float d_sq = len_squared(P - klight->co);
    ls->pdf = sphere_light_pdf(d_sq, r_sq, N, ls->D, path_flag); /* Deliberately passed N. */

    return isfinite(ls->pdf) && ls->pdf > 0.0f && ls->t >= 0.0f;
  }
  /* None-Spherical Point, i.e. Disk, LA_USE_SOFT_FALLOFF. */
  else {
    /*
     * Non-spherical point light:
     * The emitter degenerates to a single point at klight->co.
     * There is no persistent geometric parameter to decode.
     * Therefore: ls->P = klight->co
     *
     * and all remaining quantities are reconstructed relative
     * to the current target P.
     */

    ls->P = klight->co;

    /* Target-dependent direction and distance. */
    ls->D = safe_normalize_len(ls->P - P, &ls->t);

    if (!isfinite(ls->t) || ls->t < 0.0f) {
      return false;
    }

    ls->Ng = -ls->D;

    /* Restore the metadata. */
    ls->type = LIGHT_POINT;
    ls->shader = klight->shader_id;
    ls->object = klight->object_id;
    ls->prim = reference->prim;
    ls->group = object_lightgroup(kg, ls->object);

    ls->pdf_selection = 0.0f;
    ls->emitter_id = EMITTER_NONE;

    /*
     * Match point_light_sample(): a non-spherical zero-radius
     * Point light has no finite-area parameterization.
     */
    ls->pdf = 1.0f;

    return isfinite(ls->pdf) && ls->pdf > 0.0f && ls->t >= 0.0f;
  }
}

ccl_device_inline LightEval
point_light_eval_from_intersection(const ccl_global KernelLight *klight,
                                   const float3 ray_P,
                                   const float3 ray_D,
                                   const float t,
                                   const float3 N,
                                   const uint32_t path_flag)
{
  const float r_sq = sqr(klight->spot.radius);

  LightEval light_eval = {klight->spot.eval_fac, 0.0f};

  if (klight->spot.is_sphere) {
    const float d_sq = len_squared(ray_P - klight->co);
    light_eval.pdf = sphere_light_pdf(d_sq, r_sq, N, ray_D, path_flag);
  }
  else {
    if (t != FLT_MAX) {
      const float3 lightN = normalize(ray_P - klight->co);
      const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;
      light_eval.pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ray_D, t);
    }
  }

  return light_eval;
}

template<bool in_volume_segment>
ccl_device_forceinline bool point_light_tree_parameters(const ccl_global KernelLight *klight,
                                                        const float3 centroid,
                                                        const float3 P,
                                                        ccl_private float &cos_theta_u,
                                                        ccl_private float2 &distance,
                                                        ccl_private float3 &point_to_centroid)
{
  float min_distance;
  point_to_centroid = safe_normalize_len(centroid - P, &min_distance);
  distance = min_distance * one_float2();

  if (in_volume_segment) {
    cos_theta_u = 1.0f; /* Any value in [-1, 1], irrelevant since theta = 0 */
    return true;
  }

  const float radius = klight->spot.radius;

  if (klight->spot.is_sphere) {
    if (min_distance > radius) {
      /* Equivalent to a disk light with the same angular span. */
      cos_theta_u = cos_from_sin(radius / min_distance);
      distance.x = min_distance / cos_theta_u;
    }
    else {
      /* Similar to background light. */
      cos_theta_u = -1.0f;
      /* HACK: pack radiance scaling in the distance. */
      distance = one_float2() * radius / M_SQRT2_F;
    }
  }
  else {
    const float hypotenus = sqrtf(sqr(radius) + sqr(min_distance));
    cos_theta_u = min_distance / hypotenus;

    distance.x = hypotenus;
  }

  return true;
}

CCL_NAMESPACE_END
