/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// sample_reference.h
// persistent light sample reference

#pragma once

#include "kernel/light/common.h"

#include "kernel/light/area.h"
#include "kernel/light/background.h"
#include "kernel/light/point.h"
#include "kernel/light/spot.h"
#include "kernel/light/sun.h"
#include "kernel/sample/lcg.h"
#include "kernel/types.h"

#include "kernel/types.h"
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

// MARK: - Producers

/** TODO: Investigate whether light_normal_uv_from_position() can be used
 * as a common producer-side *canonicalization* path for emitter-local references.
 * It currently provides the exact area-light UV we need, but other light types
 * still require explicit per-type encode/decode validation.
 */
ccl_device_inline bool restir_area_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private LightSampleReference *reference)
{
  *reference = light_reference_empty();

  if (ls->type != LIGHT_AREA || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
  if ((LightType)klight->type != LIGHT_AREA) {
    return false;
  }

  reference->type = LIGHT_AREA;
  reference->object = ls->object;
  // reference->object = OBJECT_NONE;
  reference->prim = ls->prim;
// param stores the same barycentric-style emitter coordinates `float2(v + 0.5f, -u - v)`.
  reference->param = area_light_uv(klight, ls->P);
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM; // !!!

  return true;
}

/* MARK: - Consumption */

/**
 * reconstructs exactly the two pieces that matter:
 * 1. Chosen point on emitter geometry
 * 2. Current target-dependent evaluation
 *
 */
ccl_device_inline bool restir_area_light_sample_from_reference(
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
  (void)path_flag;

  if (!light_reference_is_valid(reference) ||
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

  /*
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


// ccl_device_inline void restir_debug_validate_light_reference_roundtrip_from_reference(
//     KernelGlobals kg,
//     const ccl_private LightSample *original,
//     const ccl_private LightSampleReference *reference,
//     const float3 P,
//     const float3 N,
//     const int shader_flags,
//     const uint32_t path_flag)
// {
//   LightSample reconstructed;

//   if (!light_reference_is_valid(reference)) {
//     return;
//   }

//   const float time = 0.0f; // TODO: obtain from sd
//   const bool decoded = light_sample_from_reference(
//       kg, reference, P, N, time, shader_flags, path_flag, &reconstructed);

//   if (!decoded) {
// #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
//     printf("ReSTIR roundtrip decode failed: type=%d prim=%d object=%d\n",
//            (int)original->type, original->prim, original->object);
// #endif
//     return;
//   }

//   const float pos_eps = 1e-4f;
//   const float dir_eps = 1e-4f;
//   const float rel_eps = 1e-4f;

//   bool ok = true;
//   ok &= (original->type == reconstructed.type);
//   ok &= (original->prim == reconstructed.prim);
//   ok &= (original->object == reconstructed.object);
//   ok &= (original->shader == reconstructed.shader);
//   ok &= (original->group == reconstructed.group);

//   ok &= restir_debug_compare_float3(original->P, reconstructed.P, pos_eps);
//   ok &= restir_debug_compare_float3(original->Ng, reconstructed.Ng, dir_eps);
//   ok &= restir_debug_compare_float3(original->D, reconstructed.D, dir_eps);
//   ok &= restir_debug_compare_float(original->t, reconstructed.t, rel_eps);
//   ok &= restir_debug_compare_float(original->pdf, reconstructed.pdf, rel_eps);
//   ok &= restir_debug_compare_float(original->eval_fac, reconstructed.eval_fac, rel_eps);

//   if (!ok) {
// #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
//     printf("ReSTIR roundtrip mismatch\n");

// //     printf("  original: type=%d prim=%d object=%d pdf=%f t=%f\n",
// //            (int)original->type, original->prim, original->object,
// //            (double)original->pdf, (double)original->t);
// //     printf("  recon   : type=%d prim=%d object=%d pdf=%f t=%f\n",
// //            (int)reconstructed.type, reconstructed.prim, reconstructed.object,
// //            (double)reconstructed.pdf, (double)reconstructed.t);
// // #else
// //     assert(false);
// #endif
//   }
// }

// ccl_device_inline void restir_debug_validate_light_reference_roundtrip(
//     KernelGlobals kg,
//     const ccl_private LightSample *original,
//     const float3 P,
//     const float3 N,
//     const int shader_flags,
//     const uint32_t path_flag)
// {
//   LightSampleReference reference = light_reference_empty();

//   const float time = 0.0f;
//   const bool encoded = light_reference_from_sample(kg, original, time, &reference);
//   if (!encoded) {
//     return;
//   }

//   restir_debug_validate_light_reference_roundtrip_from_reference(
//       kg, original, &reference, P, N, shader_flags, path_flag);
// }

CCL_NAMESPACE_END