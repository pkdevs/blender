/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// "kernel/integrator/restir.h"

#pragma once

#include "kernel/types.h"
#include "kernel/integrator/state.h"
#include "kernel/integrator/state_flow.h" // integrator_path_next

#include "kernel/sample/pattern.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline ReSTIRReservoir restir_reservoir_empty()
{
  ReSTIRReservoir r{};      // Optional

  r.W = 0.0f;
  r.M = 0;
  r.visible = false; // this used to be true; Old comment: necessarily optimistic here; visibility is known only after the shadow ray. Later

  return r;
}

// MARK: --- RIS State ---

struct ReSTIRRisState {
    LightSample light;

    float weight_sum;           // Sum of (target/source); only exists while streaming candidates.
    float selected_target_pdf;  // selected RIS weight (target)

    uint M;                     // Sample count
};

ccl_device_inline ReSTIRRisState restir_ris_empty()
{
  ReSTIRRisState s;

  s.weight_sum = 0.0f;
  s.selected_target_pdf = 0.0f;
  s.M = 0;

  return s;
}

// TODO: Utilize restir_candidate_weight
ccl_device_inline void restir_streaming_resample_step(
    ccl_private ReSTIRRisState *ris,
    ccl_private const LightSample *candidate,
    const float target_pdf,
    const float source_pdf,
    const float random)
{
  // restir_streaming_resample_step(&ris, &candidate, target, source, random);
  // candidate weights
  if (source_pdf <= 0.0f || target_pdf <= 0.0f) {
    ris->M++;
    return;
  }

  // NaN / Inf
  if (!(isfinite(source_pdf) && source_pdf > 0.0f &&
        isfinite(target_pdf) && target_pdf > 0.0f)) {
    ris->M++;
    return;
  }

  const float sample_weight = target_pdf / source_pdf;

  // See if necessary!
  if (!(isfinite(sample_weight) && sample_weight > 0.0f)) {
    ris->M++;
    return;
  }

  ris->weight_sum += sample_weight;
  ris->M++;

  if (random * ris->weight_sum < sample_weight) {
    ris->light = *candidate;
    ris->selected_target_pdf = target_pdf;
  }
}

// /* Matches Falcor: toReservoir() logic and Finalize weight calculation */
// ccl_device_inline ReSTIRReservoir restir_finalize(
//     const ccl_private ReSTIRRisState *ris)
// {
//   ReSTIRReservoir reservoir;

//   reservoir.light = ris->light;
//   reservoir.M = ris->M;
//   reservoir.visible = true;

//   /* Falcor InitialResampling.cs.slang: 
//    * risState.weight = risState.weight > 0.f ? (risState.weightSum / risState.M) / risState.weight : 0.f; */
//   if (ris->selected_target_pdf > 0.0f && ris->M > 0)
//   {
//     reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;
//   }
//   else {
//     reservoir.W = 0.0f;
//   }

//   return reservoir;
// }
ccl_device_inline ReSTIRReservoir restir_finalize( // !!!: Migth be unnecessary
    const ccl_private ReSTIRRisState *ris)
{
  ReSTIRReservoir reservoir = restir_reservoir_empty();

  if (ris->selected_target_pdf <= 0.0f || ris->M == 0) {
    return reservoir;
  }

  reservoir.light = ris->light;
  reservoir.M = ris->M;
  reservoir.visible = true;
  reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;

  if (!(isfinite(reservoir.W) && reservoir.W > 0.0f)) {
    return restir_reservoir_empty();
  }

  return reservoir;
}

ccl_device_inline void restir_di_shadow_set_visible(IntegratorShadowState state, // New
                                                    ccl_global ReSTIRReservoir *restir_reservoirs,
                                                    const bool visible)
{
#ifdef __RESTIR_DI__
  if (restir_reservoirs == nullptr) {
    return;
  }

  if (INTEGRATOR_STATE(state, shadow_path, restir_di) == 0) {
    return;
  }

  const uint pixel_index = INTEGRATOR_STATE(state, shadow_path, render_pixel_index);
  restir_reservoirs[pixel_index].visible = visible;
#else
  (void)state;
  (void)restir_reservoirs;
  (void)visible;
#endif
}

// TODO: Should become the kernel?:
ccl_device void integrator_restir_test(KernelGlobals kg,
                                       IntegratorState state,
                                       ccl_global float *ccl_restrict render_buffer)
{
  // Simply transition the path to the next stage
  integrator_path_next(state, 
                       DEVICE_KERNEL_INTEGRATOR_RESTIR_TEST, 
                       DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
}

ccl_device_inline bool restir_reservoir_is_valid(const ccl_private ReSTIRReservoir *r) // New
{
  return r->M > 0 && r->W > 0.0f && r->visible; // rejects invisible reservoirs
}

// This is the math that actually chooses which light to keep (streamingResampleStep).
ccl_device_inline bool restir_streaming_resample_reservoir( // New
    ccl_private ReSTIRRisState *ris,
    const ccl_private ReSTIRReservoir *reservoir,
    const float target_pdf,
    const float random)
{
  if (!restir_reservoir_is_valid(reservoir) || target_pdf <= 0.0f) {
    return false;
  }

  const float sample_weight = target_pdf * reservoir->W * float(reservoir->M);

  if (sample_weight <= 0.0f || !isfinite(sample_weight)) {
    return false;
  }

  ris->weight_sum += sample_weight;
  ris->M += reservoir->M;

  if (random * ris->weight_sum < sample_weight) {
    ris->light = reservoir->light;
    ris->selected_target_pdf = target_pdf;
    return true;
  }

  return false;
}

ccl_device_inline ReSTIRReservoir restir_finalize_temporal(const ccl_private ReSTIRRisState *ris) // New
{
  ReSTIRReservoir reservoir = restir_reservoir_empty();

  if (ris->selected_target_pdf <= 0.0f || ris->M == 0) {
    return reservoir;
  }

  reservoir.light = ris->light;
  reservoir.M = ris->M;
  reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;
  reservoir.visible = true;

  if (!isfinite(reservoir.W)) {
    return restir_reservoir_empty();
  }

  return reservoir;
}

CCL_NAMESPACE_END