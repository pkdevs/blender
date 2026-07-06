/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// "kernel/integrator/restir.h"

#pragma once

#include "kernel/types.h"
#include "kernel/integrator/state.h"
#include "kernel/integrator/state_flow.h" // integrator_path_next

#include "kernel/sample/pattern.h"
#include "util/math_float2.h"

CCL_NAMESPACE_BEGIN

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

    float weight_sum;           // Sum of (target/source); only exists while streaming candidates.
    float selected_target_pdf;  // selected RIS weight (target)

    uint M;                     // Sample count
};

ccl_device_inline ReSTIRRisState restir_ris_empty()
{
  ReSTIRRisState s{}; // Initialize s.light

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
  // Candidate weight is target_pdf / source_pdf.
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
ccl_device_inline ReSTIRReservoir restir_finalize(
    const ccl_private ReSTIRRisState *ris)
{
  ReSTIRReservoir reservoir = restir_reservoir_empty();

  if (ris->selected_target_pdf <= 0.0f || ris->M == 0) {
    return reservoir;
  }

  reservoir.light = ris->light;

  reservoir.M = ris->M;
  reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;

  if (!(isfinite(reservoir.W) && reservoir.W > 0.0f)) {
    return restir_reservoir_empty();
  }

  return reservoir;
}

ccl_device_inline void restir_di_shadow_set_visible(IntegratorShadowState state,
                                                    ccl_global ReSTIRReservoir *restir_reservoirs,
                                                    const int restir_reservoir_count,
                                                    const bool visible)
{
  // TODO: visibility payload returns in spatial phase...
}

ccl_device_inline bool restir_reservoir_is_valid(const ccl_private ReSTIRReservoir *r) // !!!: Fix visibility
{
  // return r->M > 0 && r->W > 0.0f && r->visible; // rejects invisible reservoirs // !!!: But visibility is not known during reservoir construction.
  /**
1. restir_di_select_light builds reservoir
2. reservoir stored
3. shadow ray traced
4. integrator_shade_shadow sets visible
5. film write
   */

   return r->M > 0 && r->W > 0.0f;
}

// !!!: Not needed until relocation is deployed. 
// ccl_device_inline bool integrator_restir_unpack_shader(ccl_private ShaderData *sd,
//                                                        ccl_private uint32_t *path_flag,
//                                                        const ccl_global float *buffer)
// {
//   int i = 0;

//   *path_flag = (uint32_t)buffer[i++];

//   sd->u = buffer[i++];
//   sd->v = buffer[i++];
//   sd->type = (uint)buffer[i++];
//   sd->object = (int)buffer[i++];
//   sd->prim = (int)buffer[i++];
//   sd->lcg_state = (uint)buffer[i];

//   /* Returns true if we have a valid intersection at the shading point */
//   return sd->type != 0;
// }

ccl_device_forceinline ccl_global float *pixel_render_buffer( // !!!: See if correct!
    KernelGlobals kg, uint32_t render_pixel_index, ccl_global float *ccl_restrict render_buffer)
{
  const uint64_t render_buffer_offset = (uint64_t)render_pixel_index *
                                        kernel_data.film.pass_stride;
  return render_buffer + render_buffer_offset;
}

// ccl_device_inline bool integrator_restir_unpack_shader(KernelGlobals kg,
//                                                        ccl_private ShaderData *sd,
//                                                        uint32_t render_pixel_index,
//                                                        ccl_global float *ccl_restrict render_buffer,
//                                                        ccl_private uint32_t *path_flag)
// {
//   PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);
//   ccl_global const float *buffer = pixel_render_buffer(kg, render_pixel_index, render_buffer) +
//                                    kernel_data.film.pass_surface_data;

//   return integrator_restir_unpack_shader(sd, path_flag, buffer);
// }

// MARK: - 

// This is the math that actually chooses which light to keep (streamingResampleStep).
ccl_device_inline bool restir_streaming_resample_reservoir(
    ccl_private ReSTIRRisState *ris,
    const ccl_private ReSTIRReservoir *reservoir,
    const float target_pdf,
    const float random)
{
  if (!restir_reservoir_is_valid(reservoir) ||
      !(isfinite(target_pdf) && target_pdf > 0.0f)) {
    return false;
  }

  const float sample_weight = target_pdf * reservoir->W * float(reservoir->M);

  if (!(isfinite(sample_weight) && sample_weight > 0.0f)) {
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

ccl_device_inline ReSTIRReservoir restir_finalize_temporal(const ccl_private ReSTIRRisState *ris)
{
  ReSTIRReservoir reservoir = restir_reservoir_empty();

  if (ris->selected_target_pdf <= 0.0f || ris->M == 0) {
    return reservoir;
  }

  reservoir.light = ris->light;

  reservoir.M = ris->M;
  reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;

  // should mirror restir_finalize()
  if (!(isfinite(reservoir.W) && reservoir.W > 0.0f)) {
    return restir_reservoir_empty();
  }

  return reservoir;
}

CCL_NAMESPACE_END