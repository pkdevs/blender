/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// sample_reference.h
// persistent light sample reference

#pragma once

#include "kernel/types.h"
#include "kernel/integrator/restir.h"
#include "util/math_float2.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline float2 restir_point_direction_encode(const float3 direction)
{
  return map_to_sphere(direction);
}

// TODO: Verify correctness
ccl_device_inline float3 restir_point_direction_decode(const float2 uv)
{
  // /* inverse of map_to_sphere(). */
  // const float phi = M_2PI_F * uv.x;
  // const float theta = M_PI_F * uv.y;
  // const float sin_theta = sinf(theta);

  // return make_float3(
  //     sin_theta * cosf(phi), sin_theta * sinf(phi), cosf(theta));

  // !!!
  return map_to_sphere(uv);
}

/**
 * Producer
 */
ccl_device_inline bool restir_point_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  *reference = restir_light_reference_empty();

  if (ls->type != LIGHT_POINT || ls->prim < 0) {
    // targets must be reconstructed from LightSample
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

    reference->param = restir_point_direction_encode(local_Ng);
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                       RESTIR_LIGHT_REFERENCE_HAS_PARAM;
    return true;
  }
/**
At the target point, reconstruct:

ls->P = light_P;
ls->Ng = world_Ng;
ls->D = safe_normalize_len(ls->P - target_P, &ls->t);
 */

  if (klight->spot.radius == 0.0f) {
    reference->param = zero_float2();
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID;
    return true;
  }
/**
At the target:

ls->P = klight->co;
ls->D = safe_normalize_len(ls->P - target_P, &ls->t);
ls->Ng = -ls->D;
ls->eval_fac = klight->spot.eval_fac;
 */

  /* A finite non-sphere point light is a target-facing sampling disk. */
  *reference = restir_light_reference_empty();
  return false;
}


// TODO: Handle all analytic lights
ccl_device_inline bool restir_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  switch (ls->type) {
    case LIGHT_POINT:
      return restir_point_light_reference_from_sample(kg, ls, reference);
    default:
      *reference = restir_light_reference_empty();
      return false;
  }
}

// MARK: - Consumption

/*
To Check:
Confirm the forward transform helper name corresponding to lamp_get_inverse_transform().
Confirm the invalid emitter_id constant.
Confirm whether packed_float3 accepts direct assignment/conversion as shown.
Confirm whether zero-radius point lights are expected to use the area-to-solid-angle expression exactly as current sampling does.
shader_flags is currently unused by reconstruction because the selected sphere point is already fixed. Keep it only if the generic API will need it, or remove it from the point-specific helper.
path_flag is needed by sphere_light_pdf().
*/
ccl_device_inline bool restir_point_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags, // !!!: Unused
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  if (!restir_light_reference_is_valid(reference) ||
      reference->type != LIGHT_POINT ||
      reference->prim < 0)
  {
    return false;
  }

  const ccl_global KernelLight *klight =
      &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_POINT) {
    return false;
  }

/** TODO: For relocated sphere-light reuse, verify that ls->P is the
 * first valid intersection from target P along ls->D. A persisted point on the
 * far side of the sphere may not be a valid sample from the new target.
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
  ls->emitter_id = EMITTER_NONE; // !!!: Use the actual Cycles invalid constant.

  const float r_sq = sqr(klight->spot.radius);

  if (klight->spot.is_sphere) {
    if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
      return false;
    }

    const float3 local_Ng = restir_point_direction_decode(reference->param);
    // const Transform tfm = lamp_get_transform(kg, klight); // lamp_get_transform deprecated
    const Transform itfm = lamp_get_inverse_transform(kg, klight);
    const Transform tfm = transform_inverse(itfm);

    ls->Ng = normalize(transform_direction(&tfm, local_Ng));
    ls->P = klight->co + float3(ls->Ng) * klight->spot.radius;
    ls->D = safe_normalize_len(ls->P - P, &ls->t);

    const float d_sq = len_squared(P - klight->co);
    ls->pdf = sphere_light_pdf(d_sq, r_sq, N, ls->D, path_flag);
    return isfinite(ls->pdf) && ls->pdf > 0.0f && ls->t >= 0.0f;
  }

  if (klight->spot.radius != 0.0f ||
      (reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM))
  {
    return false;
  }

  ls->P = klight->co;
  ls->D = safe_normalize_len(ls->P - P, &ls->t);
  ls->Ng = -ls->D;

  /*
   * Match point_light_sample() rather than labeling this as a generic
   * measure-one delta PDF.
   */
  const float3 lightN = safe_normalize(P - klight->co);
  ls->pdf = light_pdf_area_to_solid_angle(lightN, -ls->D, ls->t);

  return isfinite(ls->pdf) && ls->pdf > 0.0f && ls->t >= 0.0f;
}

// Dispatcher
ccl_device_inline bool restir_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  if (!restir_light_reference_is_valid(reference)) {
    *ls = {};
    return false;
  }

  // TODO: Rest of the light types
  switch (reference->type) {
    case LIGHT_POINT:
      return restir_point_light_sample_from_reference(
          kg, reference, P, N, shader_flags, path_flag, ls);
    default:
      *ls = {};
      return false;
  }
}


// MARK: - DEBUG

ccl_device_inline bool restir_debug_compare_float3(const float3 a,
                                                   const float3 b,
                                                   const float eps)
{
  return len(a - b) <= eps;
}

ccl_device_inline bool restir_debug_compare_float(const float a,
                                                  const float b,
                                                  const float rel_eps)
{
  const float scale = fmaxf(fmaxf(fabsf(a), fabsf(b)), 1.0f);
  return fabsf(a - b) <= rel_eps * scale;
}

// MARK: - Debug

ccl_device_inline void restir_debug_validate_light_reference_roundtrip(
    KernelGlobals kg,
    const ccl_private LightSample *original,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag)
{
// #ifdef WITH_CYCLES_DEBUG
  ReSTIRLightSampleReference reference = restir_light_reference_empty();
  LightSample reconstructed;

  const bool encoded = restir_light_reference_from_sample(kg, original, &reference);
  if (!encoded) {
    return;
  }

  const bool decoded = restir_light_sample_from_reference(
      kg, &reference, P, N, shader_flags, path_flag, &reconstructed);

  if (!decoded) {
    printf("ReSTIR roundtrip decode failed: type=%d prim=%d object=%d\n",
           (int)original->type, original->prim, original->object);
    return;
  }

  const float pos_eps = 1e-4f;
  const float dir_eps = 1e-4f;
  const float rel_eps = 1e-4f;

  bool ok = true;
  ok &= (original->type == reconstructed.type);
  ok &= (original->prim == reconstructed.prim);
  ok &= (original->object == reconstructed.object);
  ok &= (original->shader == reconstructed.shader);
  ok &= (original->group == reconstructed.group);

  ok &= restir_debug_compare_float3(original->P, reconstructed.P, pos_eps);
  ok &= restir_debug_compare_float3(original->Ng, reconstructed.Ng, dir_eps);
  ok &= restir_debug_compare_float3(original->D, reconstructed.D, dir_eps);
  ok &= restir_debug_compare_float(original->t, reconstructed.t, rel_eps);
  ok &= restir_debug_compare_float(original->pdf, reconstructed.pdf, rel_eps);
  ok &= restir_debug_compare_float(original->eval_fac, reconstructed.eval_fac, rel_eps);

  if (!ok) {
    printf("ReSTIR roundtrip mismatch\n");
#if defined(__KERNEL_CPU__)
    printf("  original: type=%d prim=%d object=%d pdf=%f t=%f\n",
           (int)original->type, original->prim, original->object,
           (double)original->pdf, (double)original->t);
    printf("  recon   : type=%d prim=%d object=%d pdf=%f t=%f\n",
           (int)reconstructed.type, reconstructed.prim, reconstructed.object,
           (double)reconstructed.pdf, (double)reconstructed.t);
#else
    assert(false);
#endif
  }
// #endif
}

CCL_NAMESPACE_END