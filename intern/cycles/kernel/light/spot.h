/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/light/point.h"

#include "util/math_fast.h"
#include "util/math_intersect.h"

CCL_NAMESPACE_BEGIN

// TODO(Tri): Cleanup your comments

/* Transform vector to spot light's local coordinate system. */
ccl_device float3 spot_light_to_local(KernelGlobals kg,
                                      const ccl_global KernelLight *klight,
                                      const float3 ray)
{
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  float3 transformed_ray = safe_normalize(transform_direction(&itfm, ray));
  transformed_ray.z = -transformed_ray.z;

  return transformed_ray;
}

/* Compute spot light attenuation of a ray given in local coordinate system. */
ccl_device float spot_light_attenuation(const ccl_global KernelSpotLight *spot, const float3 ray)
{
  return smoothstepf((ray.z - spot->cos_half_spot_angle) * spot->spot_smooth);
}

ccl_device float2 spot_light_uv(const float3 ray, const float half_cot_half_spot_angle)
{
  /* Ensures that the spot light projects the full image regardless of the spot angle. */
  const float factor = half_cot_half_spot_angle / ray.z;

  /* NOTE: Return barycentric coordinates in the same notation as Embree and OptiX. */
  return make_float2(ray.y * factor + 0.5f, -(ray.x + ray.y) * factor);
}

template<bool in_volume_segment>
ccl_device_inline bool spot_light_sample(KernelGlobals kg,
                                         const ccl_global KernelLight *klight,
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
    // ccl_debug_printf("\n=== SPOT CANONICAL SAMPLE DEBUG (Sphere) ===\n");
    /* Spherical light geometry. */
    float cos_theta;
    ls->t = FLT_MAX;

    if (d_sq > r_sq) {
      /* Outside sphere. */
      const float one_minus_cos_half_spot_spread = 1.0f - klight->spot.cos_half_larger_spread;

      const float one_minus_cos_half_angle = sin_sqr_to_one_minus_cos(r_sq / d_sq);

      if (in_volume_segment || one_minus_cos_half_angle < one_minus_cos_half_spot_spread) {
        /* Sample visible part of the sphere. */
        ls->D = sample_uniform_cone(-lightN, one_minus_cos_half_angle, rand, &cos_theta, &ls->pdf);
      }
      else {
        /* Sample spread cone. */
        ls->D = sample_uniform_cone(
            -klight->spot.dir, one_minus_cos_half_spot_spread, rand, &cos_theta, &ls->pdf);

        if (!ray_sphere_intersect(
                P, ls->D, 0.0f, FLT_MAX, klight->co, klight->spot.radius, &ls->P, &ls->t))
        {
          /* Sampled direction does not intersect with the light. */
          return false;
        }
      }
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

    /* Attenuation. */
    const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);

    if (d_sq > r_sq) {
      ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);
    }

    if (!in_volume_segment && ls->eval_fac == 0.0f) {
      return false;
    }

    if (ls->t == FLT_MAX) {
      /* Law of cosines. */
      ls->t = d * cos_theta -
              copysignf(safe_sqrtf(r_sq - d_sq + d_sq * sqr(cos_theta)), d_sq - r_sq);
      ls->P = P + ls->D * ls->t;
    }

    /*
     * Establish the canonical spherical emitter point.
     *
     * The persistent ReSTIR representation will be the local-space
     * sphere normal, not this target-dependent intersection point.
     */
    ls->Ng = normalize(ls->P - klight->co);

    /*
     * Reconstruct P from the canonical sphere normal. This guarantees
     * that P lies exactly on the emitter sphere even when the initial
     * intersection suffered numerical error.
     */
    ls->P = klight->co + ls->Ng * klight->spot.radius;
  }

// DISK SPOT: will replace its target-dependent disk construction with the canonical local XY disk. NO NEED!
// !!!: Sampling distribution may depend on the target. Persistent geometry identity must not.
  else {
    // ccl_debug_printf("\n=== SPOT CANONICAL SAMPLE DEBUG (Disk) ===\n");
    /* Point light with ad-hoc radius based on oriented disk. */
    ls->P = klight->co;
    if (r_sq > 0.0f) {
      ls->P += disk_light_sample(lightN, rand) * klight->spot.radius;
    }

    ls->D = safe_normalize_len(ls->P - P, &ls->t);
    ls->Ng = -ls->D;

    /* Attenuation. */
    const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);
    ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);
    if (!in_volume_segment && ls->eval_fac == 0.0f) {
      return false;
    }

    /* PDF. */
    const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;
    ls->pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ls->D, ls->t);
  }

  return true;
}

ccl_device_forceinline float spot_light_pdf(const ccl_global KernelSpotLight *spot,
                                            const float d_sq,
                                            const float r_sq,
                                            const float3 N,
                                            const float3 D,
                                            const uint32_t path_flag)
{
  if (d_sq > r_sq) {
    return M_1_2PI_F /
           min(sin_sqr_to_one_minus_cos(r_sq / d_sq), 1.0f - spot->cos_half_larger_spread);
  }

  const bool has_transmission = (path_flag & PATH_RAY_MIS_HAD_TRANSMISSION);
  return has_transmission ? M_1_2PI_F * 0.5f : pdf_cos_hemisphere(N, D);
}

ccl_device_forceinline void spot_light_mnee_sample_update(KernelGlobals kg,
                                                          const ccl_global KernelLight *klight,
                                                          ccl_private LightSample *ls,
                                                          const float3 P,
                                                          const float3 N,
                                                          const uint32_t path_flag)
{
  ls->D = safe_normalize_len(ls->P - P, &ls->t);

  ls->eval_fac = klight->spot.eval_fac;

  const float radius = klight->spot.radius;
  bool use_attenuation = true;

  if (klight->spot.is_sphere) {
    const float d_sq = len_squared(P - klight->co);
    const float r_sq = sqr(radius);
    const float t_sq = sqr(ls->t);

    /* NOTE : preserve pdf in area measure. */
    const float jacobian_solid_angle_to_area = 0.5f * fabsf(d_sq - r_sq - t_sq) /
                                               (radius * ls->t * t_sq);
    ls->pdf = spot_light_pdf(&klight->spot, d_sq, r_sq, N, ls->D, path_flag) *
              jacobian_solid_angle_to_area;

    ls->Ng = normalize(ls->P - klight->co);

    use_attenuation = (d_sq > r_sq);
  }
  else {
    /* NOTE : preserve pdf in area measure. */
    ls->pdf = ls->eval_fac * 4.0f * M_PI_F;

    ls->Ng = -ls->D;
  }

  /* Attenuation. */
  const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);
  if (use_attenuation) {
    ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);
  }
}

ccl_device_inline bool spot_light_intersect(const ccl_global KernelLight *klight,
                                            const ccl_private Ray *ccl_restrict ray,
                                            ccl_private float *t)
{
  /* One sided. */
  if (dot(ray->D, ray->P - klight->co) >= 0.0f) {
    return false;
  }

  return point_light_intersect(klight, ray, t);
}

ccl_device_inline LightEval spot_light_eval_from_intersection(KernelGlobals kg,
                                                              const ccl_global KernelLight *klight,
                                                              const float3 ray_P,
                                                              const float3 ray_D,
                                                              const float t,
                                                              const float3 N,
                                                              const uint32_t path_flag)
{
  const float r_sq = sqr(klight->spot.radius);
  const float d_sq = len_squared(ray_P - klight->co);

  LightEval light_eval = {klight->spot.eval_fac, 0.0f};

  if (klight->spot.is_sphere) {
    light_eval.pdf = spot_light_pdf(&klight->spot, d_sq, r_sq, N, ray_D, path_flag);
  }
  else {
    if (t != FLT_MAX) {
      const float3 lightN = normalize(ray_P - klight->co);

      const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;
      light_eval.pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ray_D, t);
    }
  }

  /* Attenuation. */
  const float3 local_ray = spot_light_to_local(kg, klight, -ray_D);
  if (!klight->spot.is_sphere || d_sq > r_sq) {
    light_eval.eval_fac *= spot_light_attenuation(&klight->spot, local_ray);
  }

  return light_eval;
}

ccl_device_inline bool spot_light_sample_from_intersection(KernelGlobals kg,
                                                           const ccl_global KernelLight *klight,
                                                           const float3 ray_P,
                                                           const float3 ray_D,
                                                           const float3 N,
                                                           const uint32_t path_flag,
                                                           ccl_private LightSample *ccl_restrict ls,
                                                           ccl_private LightSampleReference *ref)
{
  const float r_sq = sqr(klight->spot.radius);
  const float d_sq = len_squared(ray_P - klight->co);

  ls->eval_fac = klight->spot.eval_fac;

  if (klight->spot.is_sphere) {
    ls->pdf = spot_light_pdf(&klight->spot, d_sq, r_sq, N, ray_D, path_flag);
    ls->Ng = normalize(ls->P - klight->co);

    const Transform itfm = lamp_get_inverse_transform(kg, klight);
    ref->param = map_to_sphere(normalize(transform_direction(&itfm, ls->Ng)));
  }
  else {
    if (ls->t != FLT_MAX) {
      const float3 lightN = normalize(ray_P - klight->co);
      const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;
      ls->pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ray_D, ls->t);

      if (r_sq > 0.0f) {
        const float3 hit_P = ray_P + ray_D * ls->t;
        ref->param = disk_light_offset_to_uv(lightN, hit_P - klight->co, klight->spot.radius);
      }
    }
    else {
      ls->pdf = 0.0f;
    }
    ls->Ng = -ray_D;
  }

  /* Attenuation. */
  const float3 local_ray = spot_light_to_local(kg, klight, -ray_D);
  if (!klight->spot.is_sphere || d_sq > r_sq) {
    ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);
  }
  if (ls->eval_fac == 0) {
    return false;
  }

  return true;
}

/* Find the ray segment lit by the spot light. */
ccl_device_inline bool spot_light_valid_ray_segment(KernelGlobals kg,
                                                    const ccl_global KernelLight *klight,
                                                    const float3 P,
                                                    const float3 D,
                                                    ccl_private Interval<float> *t_range)
{
  /* Convert to local space of the spot light. */
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  float3 local_P = P + klight->spot.dir * klight->spot.ray_segment_dp;
  local_P = transform_point(&itfm, local_P);
  const float3 local_D = transform_direction(&itfm, D);
  const float3 axis = make_float3(0.0f, 0.0f, -1.0f);

  /* Intersect the ray with the smallest enclosing cone of the light spread. */
  return ray_cone_intersect(
      axis, local_P, local_D, sqr(klight->spot.cos_half_spot_angle), t_range);
}

/* Resampling. */

ccl_device_inline bool spot_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private LightSampleReference *reference)
{
  *reference = light_reference_empty();

  if (ls->type != LIGHT_SPOT || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);

  if ((LightType)klight->type != LIGHT_SPOT) {
    return false;
  }

  if (klight->spot.radius < 0.0f) {
    return false;
  }

  if (klight->spot.is_sphere) {
    /* The sphere's persistent identity is its surface normal.
     *
     * ls->Ng is already the geometric normal of the sampled sphere
     * point in world space.
     */
    const float3 world_Ng = normalize(ls->Ng);

    if (!isfinite(world_Ng.x) ||
        !isfinite(world_Ng.y) ||
        !isfinite(world_Ng.z))
    {
      return false;
    }

    /* Convert the sphere normal into the emitter's local space.
     *
     * This is the canonical parameterization of a spherical Spot.
     */
    const Transform itfm = lamp_get_inverse_transform(kg, klight);

    const float3 local_Ng = normalize(transform_direction(&itfm, world_Ng));

    if (!isfinite(local_Ng.x) ||
        !isfinite(local_Ng.y) ||
        !isfinite(local_Ng.z))
    {
      return false;
    }

    const float2 param = map_to_sphere(local_Ng);

    if (!isfinite(param.x) || !isfinite(param.y)) {
      return false;
    }

    /* Persistent light identity. */
    reference->type = LIGHT_SPOT;
    reference->object = ls->object;
    reference->prim = ls->prim;
    reference->param = param;

    reference->flags =
        RESTIR_LIGHT_REFERENCE_VALID |
        RESTIR_LIGHT_REFERENCE_HAS_PARAM;

    return true;
  }
  else {
    reference->type = LIGHT_SPOT;
    reference->object = ls->object;
    reference->prim = ls->prim;
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID;

    const float radius = klight->spot.radius;
    if (radius > 0.0f) {
      /* Receiver that produced this sample: ls->D | ls->t point from it to ls->P. */
      const float3 source_P = ls->P - ls->D * ls->t;
      const float3 to_source = source_P - klight->co;
      if (!(len_squared(to_source) > 0.0f)) {
        return false;
      }
      reference->param = disk_light_offset_to_uv(
          normalize(to_source), ls->P - klight->co, radius);
      reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
    }
    return true;
  }
}

ccl_device_inline bool spot_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private LightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  (void)N;
  (void)shader_flags;

  if (!light_reference_is_valid(reference) ||
      reference->type != LIGHT_SPOT || reference->prim < 0)
  {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);
  if ((LightType)klight->type != LIGHT_SPOT) {
    return false;
  }

  if (klight->spot.radius < 0.0f) {
    return false;
  }

  if (klight->spot.is_sphere) {
    if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
      return false;
    }

    /* Decode the canonical sphere-surface normal in emitter-local space. */
    const float3 local_Ng = map_to_sphere(reference->param);

    if (!isfinite(local_Ng.x) ||
        !isfinite(local_Ng.y) ||
        !isfinite(local_Ng.z))
    {
      return false;
    }

    /* Transform the canonical normal into world space. */
    const Transform itfm = lamp_get_inverse_transform(kg, klight);

    const Transform tfm = transform_inverse(itfm);

    const float3 world_Ng = normalize(transform_direction(&tfm, local_Ng));

    if (!isfinite(world_Ng.x) ||
        !isfinite(world_Ng.y) ||
        !isfinite(world_Ng.z))
    {
      return false;
    }

    /* Reconstruct the exact point on the spherical emitter. */
    ls->P = klight->co + world_Ng * klight->spot.radius;

    ls->Ng = world_Ng;

    /* Target-dependent direction and distance. */
    ls->D = safe_normalize_len(ls->P - P, &ls->t);

    if (!isfinite(ls->t) || ls->t < 0.0f) {
      return false;
    }

    /* Restore the ordinary LightSample metadata. */
    ls->type = LIGHT_SPOT;
    ls->shader = klight->shader_id;
    ls->object = klight->object_id;
    ls->prim = reference->prim;
    ls->group = object_lightgroup(kg, ls->object);

    ls->pdf_selection = 0.0f;
    ls->emitter_id = EMITTER_NONE;

    /* Spot attenuation is target-dependent and therefore must be
     * recomputed after reconstruction. */
    ls->eval_fac = klight->spot.eval_fac;

    const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);

    ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);

    if (!isfinite(ls->eval_fac)) {
      return false;
    }

    if (ls->eval_fac == 0.0f) {
      return false;
    }

    /* Recompute the spherical sampling PDF for the new target (target dependent). */
    const float r_sq = sqr(klight->spot.radius);

    const float d_sq = len_squared(P - klight->co);

    ls->pdf = sphere_light_pdf(d_sq, r_sq, N, ls->D, path_flag);

    return isfinite(ls->pdf) &&
          ls->pdf > 0.0f &&
          isfinite(ls->eval_fac) &&
          ls->eval_fac >= 0.0f;
  }
  else {
    const float radius = klight->spot.radius;
    const float r_sq = sqr(radius);

    /* Same billboard normal spot_light_sample() uses, from the NEW receiver. */
    float3 lightN = P - klight->co;
    const float d_sq = len_squared(lightN);
    if (!(d_sq > 0.0f)) {
      return false;
    }
    lightN /= sqrtf(d_sq);

    ls->P = klight->co;
    if (r_sq > 0.0f) {
      if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
        return false;
      }
      ls->P += disk_light_uv_to_offset(lightN, reference->param, radius);
    }

    ls->D = safe_normalize_len(ls->P - P, &ls->t);
    if (!isfinite(ls->t) || ls->t < 0.0f) {
      return false;
    }
    ls->Ng = -ls->D;

    ls->type = LIGHT_SPOT;
    ls->shader = klight->shader_id;
    ls->object = klight->object_id;
    ls->prim = reference->prim;
    ls->group = object_lightgroup(kg, ls->object);
    ls->pdf_selection = 0.0f;
    ls->emitter_id = EMITTER_NONE;

    /* Attenuation is target-dependent. */
    ls->eval_fac = klight->spot.eval_fac;
    const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);
    ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);
    if (!isfinite(ls->eval_fac) || ls->eval_fac == 0.0f) {
      return false;
    }

    /* Identical to spot_light_sample(). */
    const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;
    ls->pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ls->D, ls->t);

    return isfinite(ls->pdf) && ls->pdf > 0.0f;
  }
}

template<bool in_volume_segment>
ccl_device_forceinline bool spot_light_tree_parameters(const ccl_global KernelLight *klight,
                                                       const float3 centroid,
                                                       const float3 P,
                                                       const ccl_private KernelBoundingCone &bcone,
                                                       ccl_private float &cos_theta_u,
                                                       ccl_private float2 &distance,
                                                       ccl_private float3 &point_to_centroid,
                                                       ccl_private float &energy)
{
  float min_distance;
  point_to_centroid = safe_normalize_len(centroid - P, &min_distance);
  distance = min_distance * one_float2();

  const float radius = klight->spot.radius;

  if (klight->spot.is_sphere) {
    cos_theta_u = (min_distance > radius) ? cos_from_sin(radius / min_distance) : -1.0f;

    if (in_volume_segment) {
      return true;
    }

    distance = (min_distance > radius) ? min_distance * make_float2(1.0f / cos_theta_u, 1.0f) :
                                         one_float2() * radius / M_SQRT2_F;
  }
  else {
    const float hypotenus = sqrtf(sqr(radius) + sqr(min_distance));
    cos_theta_u = min_distance / hypotenus;

    if (in_volume_segment) {
      return true;
    }

    distance.x = hypotenus;
  }

  /* Apply a similar scaling as in `spot_light_attenuation()` to account for spot blend. */
  {
    /* Minimum angle formed by the emitter axis and the direction to the shading point,
     * cos(theta') in the paper. */
    const float cos_min_outgoing_angle = cosf(
        fmaxf(0.0f, fast_acosf(dot(bcone.axis, -point_to_centroid)) - fast_acosf(cos_theta_u)));
    /* Use `cos(bcone.theta_e)` instead of `klight->spot.cos_half_spot_angle` to account for
     * non-uniform scaling. */
    energy *= smoothstepf((cos_min_outgoing_angle - cosf(bcone.theta_e)) *
                          klight->spot.spot_smooth);
  }

  return true;
}

CCL_NAMESPACE_END
