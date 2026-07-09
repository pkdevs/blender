/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// restir_temporal.h

// For the first implementation, temporal reuse should run inside `integrate_surface_direct_light()`
/**
RIS candidates
→ initial reservoir
→ temporal merge
→ selected light sample
→ shadow ray
 */

#pragma once

#include "kernel/integrator/restir.h"
#include "kernel/integrator/path_state.h" // RNGState
#include "kernel/geom/shader_data.h"
#include "kernel/light/sample.h"
#include "util/defines.h"

CCL_NAMESPACE_BEGIN

// TODO: Rename to restir_evaluate_target
ccl_device_inline float restir_evaluate_target_pdf(KernelGlobals kg,
                                                   IntegratorState state,
                                                   ccl_private ShaderData *sd,
                                                   const ccl_private ReSTIRReservoir *reservoir)
{
  Spectrum light_eval ccl_optional_struct_init;
  light_sample_shader_eval_nee_constant(kg,
                                        reservoir->light.shader,
                                        reservoir->light.prim,
                                        reservoir->light.type != LIGHT_TRIANGLE,
                                        light_eval);

  BsdfEval bsdf_eval ccl_optional_struct_init;
  const float bsdf_pdf = surface_shader_bsdf_eval(kg,
                                                  state,
                                                  sd,
                                                  reservoir->light.D,
                                                  &bsdf_eval,
                                                  reservoir->light.shader);

  const float mis_weight = light_sample_mis_weight_nee(kg, reservoir->light.pdf, bsdf_pdf);

  const Spectrum radiance = bsdf_eval_sum(&bsdf_eval) *
                            light_eval *
                            reservoir->light.eval_fac;

  const float jacobian = reservoir->light.jacobian_solid_angle_to_area();
  float pdf_in_area = reservoir->light.pdf;
  if (jacobian > 0.0f) {
    pdf_in_area *= jacobian;
  }

  if (!(isfinite(pdf_in_area) && pdf_in_area > 0.0f)) {
    return 0.0f;
  }

  return spectrum_to_gray(kg, radiance) * mis_weight / pdf_in_area;
}

ccl_device_inline ReSTIRReservoir restir_temporal_reuse(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    const ccl_private RNGState *rng_state,
    const ccl_private ReSTIRReservoir *current,
    ccl_global const ReSTIRReservoir *prev_reservoirs,
    const int restir_reservoir_count)
{
  if (prev_reservoirs == nullptr || !restir_reservoir_is_valid(current)) {
    return *current;
  }

  const uint pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  if (pixel_index >= (uint)restir_reservoir_count) {
    return *current;
  }

  const ReSTIRReservoir prev = prev_reservoirs[pixel_index];

  if (!restir_reservoir_is_valid(&prev)) {
    return *current;
  }

  ReSTIRRisState ris = restir_ris_empty();

  const float current_target = restir_evaluate_target_pdf(kg, state, sd, current);
/**
 * Current `prev_target` is not correct & temporal reuse is false...
 * Because it evaluates: prev.light.D, .eval_fac, .shader, .prim
 * at the current shading point without reconstructing the sample.
 */
  const float prev_target = restir_evaluate_target_pdf(kg, state, sd, &prev); // !!!

  const float rand_current = path_state_rng_1D(kg, rng_state, PRNG_SURFACE_RESTIR_TEMPORAL);
  const float rand_prev = path_state_rng_1D(kg, rng_state, PRNG_SURFACE_RESTIR_TEMPORAL + 1);

  restir_streaming_resample_reservoir(&ris, current, current_target, rand_current);
  restir_streaming_resample_reservoir(&ris, &prev, prev_target, rand_prev);

  // return restir_finalize_temporal(&ris);
}

// ccl_device_inline float restir_evaluate_target_pdf(KernelGlobals kg,
//                                                    IntegratorState state,
//                                                    ccl_private ShaderData *sd,
//                                                    const uint32_t path_flag,
//                                                    const ccl_private ReSTIRReservoir *reservoir)
// {
//   if (!restir_reservoir_is_valid(reservoir)) {
//     return 0.0f;
//   }

//   // !!!: Reconstruct the sample relative to the target shading point context
//   LightSample ls = reservoir->light;
//   // restir_light_sample_from_uv(kg, sd, path_flag, &ls, reservoir->uv); // Not implemented

//   if (ls.pdf <= 0.0f) {
//     return 0.0f;
//   }

//   Spectrum light_eval ccl_optional_struct_init;
//   light_sample_shader_eval_nee_constant(
//       kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_eval);

//   if (!isfinite(spectrum_to_gray(kg, light_eval))) {
//     return 0.0f;
//   }

//   BsdfEval bsdf_eval ccl_optional_struct_init;
//   surface_shader_bsdf_eval(kg, state, sd, ls.D, &bsdf_eval, ls.shader);

//   const Spectrum contribution = bsdf_eval_sum(&bsdf_eval) * light_eval * ls.eval_fac;
//   return spectrum_to_gray(kg, contribution);
// }

// /**
//  * Temporal ReSTIR, merges `current frame reservoir + previous frame reservoir`
//  */
// ccl_device_inline ReSTIRReservoir restir_temporal_reuse(
//     KernelGlobals kg,
//     IntegratorState state,
//     ccl_private ShaderData *sd,
//     const ccl_private RNGState *rng_state,
//     const ccl_private ReSTIRReservoir *current,
//     ccl_global const ReSTIRReservoir *prev_reservoirs,
//     const int restir_reservoir_count,
//     ccl_global float *ccl_restrict render_buffer)
// {
//   if (prev_reservoirs == nullptr || !restir_reservoir_is_valid(current)) {
//     return *current;
//   }

//   const uint pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
//   if (pixel_index >= (uint)restir_reservoir_count) {
//     return *current;
//   }

//   const ReSTIRReservoir prev = prev_reservoirs[pixel_index];

//   if (!restir_reservoir_is_valid(&prev)) {
//     return *current;
//   }

//   const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

//   /* 1. Reconstruct the previous shading point context (prev_sd) from the surface pass */
//   ShaderData prev_sd;
//   uint32_t prev_path_flag = 0;
//   /* Needed to reconstruct the previous-frame shading point from the stored ReSTIR
//   * surface pass before evaluating temporal reuse candidates. */
//   if (!integrator_restir_unpack_shader(kg, &prev_sd, pixel_index, render_buffer, &prev_path_flag)) {
//     return *current;
//   }

//   /* Setup Ray and state for the previous shading point */
//   Ray prev_ray;
//   // !!!: should be the camera origin, not the current shading point
//   // TODO(Tri): reconstruct geometry from primitive + barycentrics, not from a ray; `triangle_interpolate()`
// /**
//  * !!!: This does not reconstruct the previous shading point.
//  * position = current shading point
//  * normal is garbage
//  * u/v are garbage
//  * BSDF is from the wrong primitive
//  * and then evaluates prev light under this broken shading context
//   */
//   prev_ray.P = sd->P;       // Assuming camera position or local origin // !!!
//   prev_ray.D = -sd->wi;     // Incoming direction // !!!
//   prev_ray.time = sd->time;

//   shader_setup_from_restir(kg, &prev_ray, &prev_sd);


  
//   /* Run surface shader evaluation to populate BSDF parameters on prev_sd */
//   // const PathRayVisibility prev_visibility = path_flag_to_visibility(prev_path_flag); // TODO ?
//   // !!!: visibility mask still comes from the current integrator path state, because it describes the current ray type (camera / diffuse / glossy etc.)
//   const PathRayVisibility prev_visibility = INTEGRATOR_STATE(state, path, visibility);

//   surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>(
//       kg, state, &prev_sd, render_buffer, prev_visibility, prev_path_flag); // !!!

//   ReSTIRRisState ris = restir_ris_empty();

//   /* 2. Evaluate target PDFs
//    * NOTE: In ReSTIR, when merging reservoirs, both candidates must compete under the same 
//    * target distribution (the current shading point context `sd`). We evaluate both using `sd`.
//    * Evaluating `prev` on `prev_sd` is still useful for Jacobians/spatial shift, but for basic DI 
//    * temporal blending we evaluate `prev_target` on `sd` to test if the light is viable here. */
//   const float current_target = restir_evaluate_target_pdf(kg, state, sd, path_flag, current);
//   const float prev_target = restir_evaluate_target_pdf(kg, state, sd, path_flag, &prev);

//   /* 3. Stream sample */
//   const float rand_current = path_state_rng_1D(kg, rng_state, PRNG_SURFACE_RESTIR_TEMPORAL);
//   const float rand_prev = path_state_rng_1D(kg, rng_state, PRNG_SURFACE_RESTIR_TEMPORAL + 1);

//   restir_streaming_resample_reservoir(&ris, current, current_target, rand_current);
//   restir_streaming_resample_reservoir(&ris, &prev, prev_target, rand_prev);

//   return restir_finalize_temporal(&ris);
// }

CCL_NAMESPACE_END