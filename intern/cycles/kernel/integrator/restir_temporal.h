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
#include "util/defines.h"

CCL_NAMESPACE_BEGIN

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
  surface_shader_bsdf_eval(kg,
                           state,
                           sd,
                           reservoir->light.D,
                           &bsdf_eval,
                           reservoir->light.shader);

  const Spectrum contribution = bsdf_eval_sum(&bsdf_eval) *
                                light_eval *
                                reservoir->light.eval_fac;

  return spectrum_to_gray(kg, contribution);
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
  const float prev_target = restir_evaluate_target_pdf(kg, state, sd, &prev);

  const float rand_current = path_state_rng_1D(kg, rng_state, PRNG_SURFACE_RESTIR_TEMPORAL);
  const float rand_prev = path_state_rng_1D(kg, rng_state, PRNG_SURFACE_RESTIR_TEMPORAL + 1);

  restir_streaming_resample_reservoir(&ris, current, current_target, rand_current);
  restir_streaming_resample_reservoir(&ris, &prev, prev_target, rand_prev);

  return restir_finalize_temporal(&ris);
}

CCL_NAMESPACE_END