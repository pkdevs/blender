/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// "kernel/integrator/restir.h"

#pragma once

#include "kernel/integrator/state.h"
#include "kernel/light/common.h" // LightSample
#include "kernel/integrator/state_flow.h" // integrator_path_next

#include "kernel/sample/pattern.h"

CCL_NAMESPACE_BEGIN

struct ReSTIRReservoir {
    LightSample light;

    float W;       // Final Probabilistic reservoir weight; W = (ris.weight_sum / ris.M) / ris.selected_target_pdf;
    uint M;
};

ccl_device_inline ReSTIRReservoir restir_reservoir_empty()
{
  ReSTIRReservoir r{};      // Optional

  r.W = 0.0f;
  r.M = 0;

  return r;
}

// MARK: --- RIS State ---

struct ReSTIRRisState {
    LightSample light;

    float weight_sum;       // Sum of (target/source); only exists while streaming candidates.
    float selected_target_pdf;  // selected RIS weight (target)

    uint M;                 // Sample count
};

ccl_device_inline ReSTIRRisState restir_ris_empty()
{
  ReSTIRRisState s;

  s.weight_sum = 0.0f;
  s.selected_target_pdf = 0.0f;
  s.M = 0;

  return s;
}

// !!!: 1 candidate, no shadows, validating the math path, for now...
// TODO: Utilize restir_candidate_weight
ccl_device_inline void restir_streaming_resample_step(
    ccl_private ReSTIRRisState *ris,
    const LightSample &candidate,
    const float target_pdf,
    const float source_pdf,
    const float random)
{
  // candidate weights
  if (source_pdf <= 0.0f || target_pdf <= 0.0f) {
    ris->M++;
    return;
  }

  const float sample_weight = target_pdf / source_pdf;

  ris->weight_sum += sample_weight;
  ris->M++;

  if (random * ris->weight_sum < sample_weight) {
    ris->light = candidate;
    ris->selected_target_pdf = target_pdf;
  }
}

/* Matches Falcor: toReservoir() logic and Finalize weight calculation */
ccl_device_inline ReSTIRReservoir restir_finalize(
    const ccl_private ReSTIRRisState *ris)
{
  ReSTIRReservoir reservoir;

  reservoir.light = ris->light;
  reservoir.M = ris->M;

  /* Falcor InitialResampling.cs.slang: 
   * risState.weight = risState.weight > 0.f ? (risState.weightSum / risState.M) / risState.weight : 0.f; */
  if (ris->selected_target_pdf > 0.0f && ris->M > 0)
  {
    reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;
  }
  else {
    reservoir.W = 0.0f;
  }

  return reservoir;
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


// // This is the math that actually chooses which light to keep (streamingResampleStep).
// ccl_device bool reservoir_update(ReSTIRRisState &res, 
//                                  LightSample sample, 
//                                  float target_pdf, 
//                                  float source_pdf, 
//                                  float random_val) 
// {
//     // Falcor: float sampleWeight = targetPdf / sourcePdf;
//     float weight = (source_pdf > 0.0f) ? (target_pdf / source_pdf) : 0.0f;
    
//     res.w_sum += weight;
//     res.M += 1;

//     // Falcor: bool selectSample = sampleNext1D(sg) * state.weightSum < sampleWeight;
//     bool select = (random_val * res.w_sum) < weight;
//     if (select) {
//         res.light = sample;
//         res.target_pdf = target_pdf;
//     }
//     return select;
// }

CCL_NAMESPACE_END