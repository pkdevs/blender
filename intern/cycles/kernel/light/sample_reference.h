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

ccl_device_inline float2 restir_direction_encode(const float3 direction)
{
  return map_to_sphere(direction);
}

ccl_device_inline float3 restir_direction_decode(const float2 uv)
{
  return map_to_sphere(uv); // !!! 
}

ccl_device_inline float2 restir_area_position_encode(const ccl_global KernelLight *klight,
                                                     const float3 light_P)
{
  return area_light_uv(klight, light_P);
}

ccl_device_inline float3 restir_area_position_decode(const ccl_global KernelLight *klight,
                                                     const float2 uv)
{
  const float v = uv.x - 0.5f;
  const float u = -uv.y - v;

  return klight->co +
         u * klight->area.len_u * klight->area.axis_u +
         v * klight->area.len_v * klight->area.axis_v;
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

    reference->param = restir_direction_encode(local_Ng);
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

ccl_device_inline bool restir_sun_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  *reference = restir_light_reference_empty();

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

  // store light-local cone direction
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const float3 local_Ng = normalize(transform_direction(&itfm, ls->Ng));

  // reference->param = restir_direction_encode(local_Ng);
  reference->param = restir_area_position_encode(klight, ls->P);
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM;

  return true;
}


/** TODO: Investigate whether light_normal_uv_from_position() can be used
 * as a common producer-side *canonicalization* path for emitter-local references.
 * It currently provides the exact area-light UV we need, but other light types
 * still require explicit per-type encode/decode validation.
 */
ccl_device_inline bool restir_area_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  *reference = restir_light_reference_empty();

  if (ls->type != LIGHT_AREA || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
  if ((LightType)klight->type != LIGHT_AREA) {
    return false;
  }

  reference->type = LIGHT_AREA;
  reference->object = ls->object;
  reference->prim = ls->prim;
// param stores the same barycentric-style emitter coordinates `float2(v + 0.5f, -u - v)`.
  reference->param = area_light_uv(klight, ls->P);
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM;

  return true;
}


// TODO: Handle all analytic lights
ccl_device_inline bool restir_light_reference_from_sample( // !!!
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  switch (ls->type) {
    case LIGHT_POINT:
      return restir_point_light_reference_from_sample(kg, ls, reference);

    case LIGHT_SUN:
      return restir_sun_light_reference_from_sample(kg, ls, reference);

    case LIGHT_AREA:
      return restir_area_light_reference_from_sample(kg, ls, reference);

    default:
      *reference = restir_light_reference_empty();
      return false;
  }
}

ccl_device_inline bool restir_light_reference_restore_metadata(
    KernelGlobals kg,
    ccl_private ReSTIRLightSampleReference *reference)
{
  reference->flags = 0;

  if (reference->object != OBJECT_NONE) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);

  reference->type = (LightType)klight->type;
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID;

  if (reference->type == LIGHT_SUN || reference->type == LIGHT_AREA ||
      (reference->type == LIGHT_POINT && klight->spot.is_sphere))
  {
    reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
  }

  switch (reference->type) {
    case LIGHT_POINT:
    case LIGHT_SUN:
    case LIGHT_AREA:
      return true;
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

    const float3 local_Ng = restir_direction_decode(reference->param);
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

ccl_device_inline bool restir_sun_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  // Unused vars
  (void)P;
  (void)N;
  (void)shader_flags;
  (void)path_flag;

  if (!restir_light_reference_is_valid(reference) ||
      reference->type != LIGHT_SUN ||
      reference->prim < 0)
  {
    return false;
  }

  if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
    return false;
  }

  const ccl_global KernelLight *klight =
      &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_SUN) {
    return false;
  }

  const float3 local_Ng = restir_direction_decode(reference->param);

  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const Transform tfm = transform_inverse(itfm);

  const float3 Ng = normalize(transform_direction(&tfm, local_Ng));
  const float3 D = -Ng;

  // /*
  //  * The persisted direction should still lie inside the current sun cone.
  //  * This matters for relocated/reused samples and for animated or changed lights.
  //  */
  // if (klight->sun.angle == 0.0f) {
  //   return false;
  // }

  // if (vector_angle(-klight->co, D) > klight->sun.angle) {
  //   return false;
  // }

  // Allow zero-angle and reconstruct the delta sun direction:
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

  ls->pdf = klight->sun.pdf; // !!!: og sample gets pdf from sample_uniform_cone, should match klight->sun.pdf
  ls->eval_fac = klight->sun.eval_fac;

  ls->pdf_selection = 0.0f;
  ls->emitter_id = EMITTER_NONE; /* Replace if Cycles has a more specific invalid value. */

  return isfinite(ls->pdf) && ls->pdf > 0.0f;
}

/**
 * reconstructs exactly the two pieces that matter:
 * 1. Chosen point on emitter geometry
 * 2. Current target-dependent evaluation
 *
 */
ccl_device_inline bool restir_area_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  (void)N;
  (void)shader_flags;
  (void)path_flag;

  if (!restir_light_reference_is_valid(reference) ||
      reference->type != LIGHT_AREA ||
      reference->prim < 0)
  {
    return false;
  }

  if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_AREA) {
    return false;
  }

  const float2 uv = reference->param;

  /* Invert area_light_uv():
   * uv.x = v + 0.5
   * uv.y = -u - v
   */
  const float v = uv.x - 0.5f;
  const float u = -uv.y - v;

  /**
  * Ellipse Validity: area_light_uv() clamps coordinates to [-0.5, 0.5], 
  * but for ellipse lights not every point in the rectangle is valid. 
  */
  if (area_light_is_ellipse(&klight->area)) {
    const float ellipse_u = (2.0f * u);
    const float ellipse_v = (2.0f * v);
    if (sqr(ellipse_u) + sqr(ellipse_v) > 1.0f + 1e-5f) {
      *ls = {};
      return false;
    }
  }


  /* Reconstruct emitter point from normalized in-plane coordinates. */
  // const float3 light_P = klight->co +
  //                        u * klight->area.len_u * klight->area.axis_u +
  //                        v * klight->area.len_v * klight->area.axis_v;
  const float3 light_P = restir_area_position_decode(klight, uv);

  ls->type = LIGHT_AREA;
  ls->shader = klight->shader_id;
  ls->object = klight->object_id;
  ls->prim = reference->prim;
  ls->group = object_lightgroup(kg, ls->object);

  ls->P = light_P;
  ls->Ng = klight->area.dir;
  ls->D = safe_normalize_len(ls->P - P, &ls->t);

  ls->pdf_selection = 0.0f;
  ls->emitter_id = EMITTER_NONE;

  /* Match area_light_sample(): one-sided surface light rejection. */
  if (dot(ls->P - P, ls->Ng) > 0.0f) {
    *ls = {};
    return false;
  }

// instead of hand-rebuilding pdf & eval_fac, use the existing eval (area_light_eval()) after reconstructing ls->P, ls->D, ls->t, and ls->Ng.
  float3 eval_light_P = ls->P;
  if (!area_light_eval<false>(klight, P, &eval_light_P, ls, zero_float2(), false)) {
    *ls = {};
    return false;
  }

  return isfinite(ls->pdf) && ls->pdf > 0.0f &&
         isfinite(ls->eval_fac) && ls->eval_fac >= 0.0f &&
         ls->t >= 0.0f;
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

    case LIGHT_SUN:
      return restir_sun_light_sample_from_reference(
          kg, reference, P, N, shader_flags, path_flag, ls);

    case LIGHT_AREA:
      return restir_area_light_sample_from_reference(
          kg, reference, P, N, shader_flags, path_flag, ls);

    default:
      *ls = {};
      return false;
  }
}


// MARK: --- Debug ---

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
  if (a == FLT_MAX || b == FLT_MAX) {
    return a == b;
  }

  const float scale = fmaxf(fmaxf(fabsf(a), fabsf(b)), 1.0f);
  return fabsf(a - b) <= rel_eps * scale;
}


ccl_device_inline void restir_debug_validate_light_reference_roundtrip_from_reference(
    KernelGlobals kg,
    const ccl_private LightSample *original,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag)
{
  LightSample reconstructed;

  if (!restir_light_reference_is_valid(reference)) {
    return;
  }

  const bool decoded = restir_light_sample_from_reference(
      kg, reference, P, N, shader_flags, path_flag, &reconstructed);

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
}

ccl_device_inline void restir_debug_validate_light_reference_roundtrip(
    KernelGlobals kg,
    const ccl_private LightSample *original,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag)
{
  ReSTIRLightSampleReference reference = restir_light_reference_empty();

  const bool encoded = restir_light_reference_from_sample(kg, original, &reference);
  if (!encoded) {
    return;
  }

  restir_debug_validate_light_reference_roundtrip_from_reference(
      kg, original, &reference, P, N, shader_flags, path_flag);
}

CCL_NAMESPACE_END