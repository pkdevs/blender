/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/geom/object.h"

#include "util/math_fast.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline float2 sun_light_uv(KernelGlobals kg,
                                      const ccl_global KernelLight *klight,
                                      const float3 D)
{
  /* Map direction (x, y, z) to disk [-0.5, 0.5]^2:
   * r^2 = (1 - z) / (1 - cos(klight->sun.angle))
   * u_ = 0.5 * x * r / sin_angle(D, -klight->co)
   * v_ = 0.5 * y * r / sin_angle(D, -klight->co) */
  const float fac = klight->sun.half_inv_sin_half_angle / len(D - klight->co);

  /* Get u axis and v axis. */
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const float u_ = dot(D, make_float3(itfm.x)) * fac;
  const float v_ = dot(D, make_float3(itfm.y)) * fac;

  /* NOTE: Return barycentric coordinates in the same notation as Embree and OptiX. */
  return make_float2(v_ + 0.5f, -u_ - v_);
}

ccl_device_inline bool sun_light_sample(const ccl_global KernelLight *klight,
                                        const float2 rand,
                                        ccl_private LightSample *ls)
{
  float unused;
  ls->Ng = sample_uniform_cone(
      klight->co, klight->sun.one_minus_cosangle, rand, &unused, &ls->pdf);

  ls->P = ls->Ng;
  ls->D = -ls->Ng;
  ls->t = FLT_MAX;

  ls->eval_fac = klight->sun.eval_fac;

  return true;
}

/* Special intersection check.
 * Returns true if the sun_light_eval_from_intersection() for this light would return true.
 *
 * The intersection parameters t, u, v are optimized for the shadow ray towards a dedicated light:
 * u = v = 0, t = FLT_MAX.
 */
ccl_device bool sun_light_intersect(const ccl_global KernelLight *klight,
                                    const ccl_private Ray *ccl_restrict ray,
                                    ccl_private float *t)
{
  kernel_assert(klight->type == LIGHT_SUN);

  if (klight->sun.angle == 0.0f) {
    return false;
  }

  if (vector_angle(-klight->co, ray->D) > klight->sun.angle) {
    return false;
  }

  *t = FLT_MAX;

  return true;
}

ccl_device LightEval sun_light_eval_from_intersection(const ccl_global KernelLight *klight,
                                                      const float3 ray_D)
{
  if (klight->sun.angle == 0.0f) {
    return LightEval{};
  }

  if (vector_angle(-klight->co, ray_D) > klight->sun.angle) {
    return LightEval{};
  }

  return LightEval{klight->sun.eval_fac, klight->sun.pdf};
}

ccl_device_inline bool sun_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private LightSampleReference *reference)
{
  *reference = light_reference_empty();

  if (ls->type != LIGHT_SUN || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
  if ((LightType)klight->type != LIGHT_SUN) {
    return false;
  }

  reference->type = LIGHT_SUN;
  reference->object = ls->object;
  reference->prim = ls->prim;

  /* Store light-local cone direction. */
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const float3 local_Ng = normalize(transform_direction(&itfm, ls->Ng));

  reference->param = map_to_sphere(local_Ng);
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM;

  return true;
}

ccl_device_inline bool sun_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private LightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  /* Unused vars. */
  (void)P;
  (void)N;
  (void)shader_flags;
  (void)path_flag;

  if (!light_reference_is_valid(reference) ||
      reference->type != LIGHT_SUN ||
      reference->prim < 0)
  {
    return false;
  }

  if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);
  if ((LightType)klight->type != LIGHT_SUN) {
    return false;
  }

  const float3 local_Ng = map_to_sphere(reference->param);

  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const Transform tfm = transform_inverse(itfm);

  const float3 Ng = normalize(transform_direction(&tfm, local_Ng));
  const float3 D = -Ng;

  /* Allow zero-angle and reconstruct the delta sun direction:
   * The persisted direction should still lie inside the current sun cone.
   * This matters for relocated/reused samples and for animated or changed lights.
   */
  if (klight->sun.angle != 0.0f &&
      vector_angle(-klight->co, D) > klight->sun.angle)
  {
    return false;
  }


  ls->type = LIGHT_SUN;
  ls->shader = klight->shader_id;
  ls->object = klight->object_id;
  ls->prim = reference->prim;
  ls->group = object_lightgroup(kg, ls->object);

  ls->Ng = Ng;
  ls->P = Ng;
  ls->D = D;
  ls->t = FLT_MAX;

  ls->pdf = klight->sun.pdf;
  ls->eval_fac = klight->sun.eval_fac;

  ls->pdf_selection = 0.0f;
  ls->emitter_id = EMITTER_NONE;

  return isfinite(ls->pdf) && ls->pdf > 0.0f;
}

template<bool in_volume_segment>
ccl_device_forceinline bool sun_light_tree_parameters(const float3 centroid,
                                                      const float theta_e,
                                                      const float t,
                                                      ccl_private float &cos_theta_u,
                                                      ccl_private float2 &distance,
                                                      ccl_private float3 &point_to_centroid,
                                                      ccl_private float &theta_d)
{
  if (in_volume_segment) {
    if (t == FLT_MAX) {
      /* In world volumes, distant lights can contribute to the lighting of the volume with
       * specific configurations of procedurally generated volumes. Use a ray length of 1.0 in this
       * case to give the distant light some weight, but one that isn't too high for a typical
       * world volume use case. */
      theta_d = 1.0f;
    }
    else {
      theta_d = t;
    }
  }

  /* Treating it as a disk light 1 unit away */
  cos_theta_u = fast_cosf(theta_e);

  distance = make_float2(1.0f / cos_theta_u, 1.0f);

  point_to_centroid = -centroid;

  return true;
}

CCL_NAMESPACE_END
