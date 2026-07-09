/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// "kernel/integrator/restir.h"

#pragma once

#include "kernel/types.h"
#include "kernel/light/sample.h" // light_sample_mis_weight_nee
#include "kernel/integrator/state.h"
#include "kernel/integrator/state_flow.h" // integrator_path_next

#include "kernel/sample/pattern.h"
#include "util/math_float2.h"

CCL_NAMESPACE_BEGIN

// ccl_device_inline ReSTIRReservoir restir_reservoir_empty()
// {
//   ReSTIRReservoir r{};      // Optional

//   r.W = 0.0f;
//   r.M = 0;

//   return r;
// }

ccl_device_inline ReSTIRDIReservoir restir_di_reservoir_empty()
{
  ReSTIRDIReservoir r{};
  r.luminance = 0.0f;
  r.total_weight = 0.0f;
  r.M = 0;
  return r;
}

ccl_device_inline bool restir_di_reservoir_is_empty(
    const ccl_private ReSTIRDIReservoir *r)
{
  return !(r->total_weight > 0.0f);
}

ccl_device_inline bool restir_di_reservoir_is_valid(
    const ccl_private ReSTIRDIReservoir *r)
{
  return (r->M > 0) &&
         isfinite(r->total_weight) &&
         (r->total_weight > 0.0f) &&
         isfinite(r->luminance) &&
         (r->luminance > 0.0f);
}

// MARK: --- RIS State ---

struct ReSTIRRisState {
    LightSample light;

    float weight_sum;           // Sum of (target/source); only exists while streaming candidates.
    float selected_target_pdf;  // selected RIS weight (target)

    uint M;                     // Sample count
};

// To replace ReSTIRRisState
/**
 * @brief Streaming RIS state used while selecting the initial light candidate.
 * Better Rename:
  ```
  ReSTIRDIRisState.total_weight = raw accumulated RIS sum.
  ReSTIRDIReservoir.total_weight = finalized multiplier.
  ```
 */
struct ReSTIRDIRisState {
  LightSample light;
  // !!!: light_sample_shader_eval_nee_constant only captures the constant light shader part for some emitters
  BsdfEval radiance;          // Currently selected candidate payload.
  float luminance;            // Luminance of the selected payload.
  float total_weight;         // Accumulated proposal weights before finalization.
  uint M;                     // Number of processed candidate slots.
};

ccl_device_inline ReSTIRRisState restir_ris_empty()
{
  ReSTIRRisState s{}; // Initialize s.light

  s.weight_sum = 0.0f;
  s.selected_target_pdf = 0.0f;
  s.M = 0;

  return s;
}

ccl_device_inline ReSTIRDIRisState restir_di_ris_empty()
{
  ReSTIRDIRisState s{};
  s.luminance = 0.0f;
  s.total_weight = 0.0f;
  s.M = 0;
  return s;
}

// compute_luminance()
ccl_device_inline float restir_di_compute_luminance(KernelGlobals kg,
                                                    const ccl_private BsdfEval *radiance)
{
  const float3 rgb_to_y = make_float3(kernel_data.film.rgb_to_y);
  return dot(spectrum_to_rgb(radiance->sum), rgb_to_y);
}

// MARK: -

// // TODO: Utilize restir_candidate_weight
// ccl_device_inline void restir_streaming_resample_step(
//     ccl_private ReSTIRRisState *ris,
//     ccl_private const LightSample *candidate,
//     const float target_pdf,
//     const float source_pdf,
//     const float random)
// {
//   // restir_streaming_resample_step(&ris, &candidate, target, source, random);
//   // Candidate weight is target_pdf / source_pdf.
//   if (source_pdf <= 0.0f || target_pdf <= 0.0f) {
//     ris->M++;
//     return;
//   }

//   // NaN / Inf
//   if (!(isfinite(source_pdf) && source_pdf > 0.0f &&
//         isfinite(target_pdf) && target_pdf > 0.0f)) {
//     ris->M++;
//     return;
//   }

//   const float sample_weight = target_pdf / source_pdf;

//   // See if necessary!
//   if (!(isfinite(sample_weight) && sample_weight > 0.0f)) {
//     ris->M++;
//     return;
//   }

//   ris->weight_sum += sample_weight;
//   ris->M++;

//   if (random * ris->weight_sum < sample_weight) {
//     ris->light = *candidate;
//     ris->selected_target_pdf = target_pdf;
//   }
// }

// add_sample()
ccl_device_inline void restir_di_add_sample(
    KernelGlobals kg,
    ccl_private ReSTIRDIRisState *ris,
    const ccl_private LightSample *ls,
    const ccl_private BsdfEval *radiance,
    const float mis_weight,
    const float random)
{
  const float luminance = restir_di_compute_luminance(kg, radiance);
  const float weight = luminance * mis_weight; // !!!

  if (!(isfinite(luminance) && luminance > 0.0f)) {
    ris->M++;
    return;
  }

  if (!(isfinite(weight) && weight > 0.0f)) {
    ris->M++;
    return;
  }

  ris->total_weight += weight;
  ris->M++;

  if (random * ris->total_weight <= weight) {
    ris->light = *ls;
    ris->radiance = *radiance;
    ris->luminance = luminance;
  }
}

/** TODO: Current RIS bridge follows Cycles NEE semantics:
 * Use ls->pdf directly for now; revisit counted MIS and area-measure PDFs when
 * the full ReSTIR DI estimator matches the old reservoir pipeline.
 */
ccl_device_inline void restir_di_add_light_sample(
    KernelGlobals kg,
    ccl_private ReSTIRDIRisState *ris,
    const ccl_private LightSample *ls,
    const ccl_private BsdfEval *radiance,
    const float bsdf_pdf,
    const float random)
{
  // TODO: use power_heuristics...
  const float mis_weight = light_sample_mis_weight_nee(kg, ls->pdf, bsdf_pdf);

  // float ls_pdf_in_area = ls->pdf;
  // const float jacobian = ls->jacobian_solid_angle_to_area();
  // if (jacobian > 0.0f) {
  //   ls_pdf_in_area *= jacobian;
  // }

  // if (!(isfinite(ls_pdf_in_area) && ls_pdf_in_area > 0.0f)) {
  //   ris->M++;
  //   return;
  // }
  // printf(ls_pdf_in_area);

  // restir_di_add_sample(kg, ris, ls, radiance, mis_weight / ls_pdf_in_area, random);

  if (!(isfinite(ls->pdf) && ls->pdf > 0.0f)) {
    ris->M++;
    return;
  }

  restir_di_add_sample(kg, ris, ls, radiance, mis_weight / ls->pdf, random);
}

// MARK: - 

// /* Matches Falcor: toReservoir() logic and Finalize weight calculation */
// ccl_device_inline ReSTIRReservoir restir_finalize(
//     const ccl_private ReSTIRRisState *ris)
// {
//   ReSTIRReservoir reservoir = restir_reservoir_empty();

//   if (ris->selected_target_pdf <= 0.0f || ris->M == 0) {
//     return reservoir;
//   }

//   reservoir.light = ris->light;

//   reservoir.M = ris->M;
//   reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;

//   if (!(isfinite(reservoir.W) && reservoir.W > 0.0f)) {
//     return restir_reservoir_empty();
//   }

//   return reservoir;
// }

// !!!: total_weight /= luminance;
ccl_device_inline bool restir_di_finalize(
    ccl_private ReSTIRDIReservoir *reservoir,
    const ccl_private ReSTIRDIRisState *ris)
{
  if (!(ris->total_weight > 0.0f) || !(ris->luminance > 0.0f) || ris->M == 0) {
    *reservoir = restir_di_reservoir_empty();
    return false;
  }

  reservoir->light = ris->light;
  reservoir->radiance = ris->radiance;
  reservoir->luminance = ris->luminance;
// RIS final weight uses the mean proposal weight, not the raw sum.
  reservoir->total_weight = (ris->total_weight / float(ris->M)) / ris->luminance; // !!!
  reservoir->M = ris->M;

  if (!(isfinite(reservoir->total_weight) && reservoir->total_weight > 0.0f)) {
    *reservoir = restir_di_reservoir_empty();
    return false;
  }

  return true;
}

// MARK: -

// ccl_device_inline void restir_di_shadow_set_visible(IntegratorShadowState state,
//                                                     ccl_global ReSTIRReservoir *restir_reservoirs,
//                                                     const int restir_reservoir_count,
//                                                     const bool visible)
// {
//   // TODO: visibility payload returns in spatial phase...
// }

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

// ccl_device_inline ReSTIRReservoir restir_finalize_temporal(const ccl_private ReSTIRRisState *ris)
// {
//   ReSTIRReservoir reservoir = restir_reservoir_empty();

//   if (ris->selected_target_pdf <= 0.0f || ris->M == 0) {
//     return reservoir;
//   }

//   reservoir.light = ris->light;

//   reservoir.M = ris->M;
//   reservoir.W = (ris->weight_sum / float(ris->M)) / ris->selected_target_pdf;

//   // should mirror restir_finalize()
//   if (!(isfinite(reservoir.W) && reservoir.W > 0.0f)) {
//     return restir_reservoir_empty();
//   }

//   return reservoir;
// }

CCL_NAMESPACE_END