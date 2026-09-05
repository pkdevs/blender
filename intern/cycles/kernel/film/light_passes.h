/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/path_state.h" // RNGState
#include "kernel/film/write.h"

#include "kernel/integrator/reservoir.h"
#include "kernel/integrator/shadow_catcher.h"

#include "kernel/sample/pattern.h"
#include "kernel/light/common.h" // light_reference helpers
#include "kernel/types.h"
#include "util/atomic.h"

CCL_NAMESPACE_BEGIN

/* --------------------------------------------------------------------
 * BSDF Evaluation
 *
 * BSDF evaluation result, split between diffuse and glossy. This is used to
 * accumulate render passes separately. Note that reflection, transmission
 * and volume scattering are written to different render passes, but we assume
 * that only one of those can happen at a bounce, and so do not need to accumulate
 * them separately. */

ccl_device_inline void bsdf_eval_init(ccl_private BsdfEval *eval,
                                      const ccl_private ShaderClosure *sc,
                                      const float3 wo,
                                      Spectrum value)
{
  eval->diffuse = zero_spectrum();
  eval->glossy = zero_spectrum();

  if (CLOSURE_IS_BSDF_DIFFUSE(sc->type)) {
    eval->diffuse = value;
  }
  else if (CLOSURE_IS_BSDF_GLOSSY(sc->type)) {
    eval->glossy = value;
  }
  else if (CLOSURE_IS_GLASS(sc->type)) {
    /* Glass can count as glossy or transmission, depending on which side we end up on. */
    if (dot(sc->N, wo) > 0.0f) {
      eval->glossy = value;
    }
  }

  eval->sum = value;
}

ccl_device_inline void bsdf_eval_init(ccl_private BsdfEval *eval, Spectrum value)
{
  eval->diffuse = zero_spectrum();
  eval->glossy = zero_spectrum();
  eval->sum = value;
}

ccl_device_inline void bsdf_eval_accum(ccl_private BsdfEval *eval,
                                       const ccl_private ShaderClosure *sc,
                                       const float3 wo,
                                       Spectrum value)
{
  if (CLOSURE_IS_BSDF_DIFFUSE(sc->type)) {
    eval->diffuse += value;
  }
  else if (CLOSURE_IS_BSDF_GLOSSY(sc->type)) {
    eval->glossy += value;
  }
  else if (CLOSURE_IS_GLASS(sc->type)) {
    if (dot(sc->N, wo) > 0.0f) {
      eval->glossy += value;
    }
  }

  eval->sum += value;
}

ccl_device_inline void bsdf_eval_accum(ccl_private BsdfEval *eval, Spectrum value)
{
  eval->sum += value;
}

ccl_device_inline bool bsdf_eval_is_zero(ccl_private BsdfEval *eval)
{
  return is_zero(eval->sum);
}

ccl_device_inline void bsdf_eval_mul(ccl_private BsdfEval *eval, const float value)
{
  eval->diffuse *= value;
  eval->glossy *= value;
  eval->sum *= value;
}

ccl_device_inline void bsdf_eval_mul(ccl_private BsdfEval *eval, Spectrum value)
{
  eval->diffuse *= value;
  eval->glossy *= value;
  eval->sum *= value;
}

ccl_device_inline Spectrum bsdf_eval_sum(const ccl_private BsdfEval *eval)
{
  return eval->sum;
}

ccl_device_inline Spectrum bsdf_eval_pass_diffuse_weight(const ccl_private BsdfEval *eval)
{
  /* Ratio of diffuse weight to recover proportions for writing to render pass.
   * We assume reflection, transmission and volume scatter to be exclusive. */
  return safe_divide(eval->diffuse, eval->sum);
}

ccl_device_inline Spectrum bsdf_eval_pass_glossy_weight(const ccl_private BsdfEval *eval)
{
  /* Ratio of glossy weight to recover proportions for writing to render pass.
   * We assume reflection, transmission and volume scatter to be exclusive. */
  return safe_divide(eval->glossy, eval->sum);
}

/* --------------------------------------------------------------------
 * Clamping
 *
 * Clamping is done on a per-contribution basis so that we can write directly
 * to render buffers instead of using per-thread memory, and to avoid the
 * impact of clamping on other contributions. */

ccl_device_forceinline void film_clamp_light(KernelGlobals kg,
                                             ccl_private Spectrum *L,
                                             const int bounce)
{
#ifdef __KERNEL_DEBUG_NAN__
  if (!isfinite_safe(*L)) {
    kernel_assert(!"Cycles sample with non-finite value detected");
  }
#endif
  /* Make sure all components are finite, allowing the contribution to be usable by adaptive
   * sampling convergence check, but also to make it so render result never causes issues with
   * post-processing. */
  *L = ensure_finite(*L);

#ifdef __CLAMP_SAMPLE__
  const float limit = (bounce > 0) ? kernel_data.integrator.sample_clamp_indirect :
                                     kernel_data.integrator.sample_clamp_direct;
  const float sum = reduce_add(fabs(*L));
  if (sum > limit) {
    *L *= limit / sum;
  }
#endif
}

/* --------------------------------------------------------------------
 * Pass accumulation utilities.
 */

/* --------------------------------------------------------------------
 * Adaptive sampling.
 */

// This is where the sample count is incremented in the buffer using an atomic add. This is technically the “commit” of the sample’s existence, rather than its color.
ccl_device_inline int film_write_sample(KernelGlobals kg,
                                        ConstIntegratorState state,
                                        ccl_global float *ccl_restrict render_buffer,
                                        const int sample,
                                        const int sample_offset)
{
  if (kernel_data.film.pass_sample_count == PASS_UNUSED) {
    return sample;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  return atomic_fetch_and_add_uint32(
             (ccl_global uint *)(buffer) + kernel_data.film.pass_sample_count, 1) +
         sample_offset;
}

// This is a secondary commit used for adaptive sampling. It writes the contribution to a separate buffer to help Cycles determine if the pixel has converged.
ccl_device void film_write_adaptive_buffer(KernelGlobals kg,
                                           const int sample,
                                           const Spectrum contribution,
                                           ccl_global float *ccl_restrict buffer)
{
  /* Adaptive Sampling. Fill the additional buffer with only one half of the samples and
   * calculate our stopping criteria. This is the heuristic from "A hierarchical automatic
   * stopping condition for Monte Carlo global illumination" except that here it is applied
   * per pixel and not in hierarchical tiles. */

  if (kernel_data.film.pass_adaptive_aux_buffer == PASS_UNUSED) {
    return;
  }

  if (sample_is_class_A(kernel_data.integrator.sampling_pattern, sample)) {
    const float3 contribution_rgb = spectrum_to_rgb(contribution);

    film_write_pass_float4(buffer + kernel_data.film.pass_adaptive_aux_buffer,
                           make_float4(contribution_rgb.x * 2.0f,
                                       contribution_rgb.y * 2.0f,
                                       contribution_rgb.z * 2.0f,
                                       0.0f));
  }
}

/* Write the volume and surface contribution for volume scattering probability guiding. */
ccl_device_inline void film_write_volume_scattering_guiding_pass(
    KernelGlobals kg,
    ccl_global float *ccl_restrict buffer,
    const PathRayVisibility path_visibility,
    const uint32_t path_flag,
    const Spectrum contribution)
{
  int pass_offset = PASS_UNUSED;
  if (path_flag & PATH_RAY_VOLUME_PRIMARY_TRANSMIT) {
    pass_offset = kernel_data.film.pass_volume_transmit;
  }
  else if (path_visibility & PATH_RAY_VISIBILITY_VOLUME_SCATTER) {
    pass_offset = kernel_data.film.pass_volume_scatter;
  }

  if (pass_offset != PASS_UNUSED) {
    film_write_pass_spectrum(buffer + pass_offset, contribution);
  }
}

/* --------------------------------------------------------------------
 * Shadow catcher.
 */

#ifdef __SHADOW_CATCHER__

/* Accumulate contribution to the Shadow Catcher pass.
 *
 * Returns truth if the contribution is fully handled here and is not to be added to the other
 * passes (like combined, adaptive sampling). */

ccl_device bool film_write_shadow_catcher(KernelGlobals kg,
                                          const uint32_t path_flag,
                                          const Spectrum contribution,
                                          ccl_global float *ccl_restrict buffer)
{
  if (!kernel_data.integrator.has_shadow_catcher) {
    return false;
  }

  kernel_assert(kernel_data.film.pass_shadow_catcher != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  /* Matte pass. */
  if (kernel_shadow_catcher_is_matte_path(path_flag)) {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_shadow_catcher_matte, contribution);
    /* NOTE: Accumulate the combined pass and to the samples count pass, so that the adaptive
     * sampling is based on how noisy the combined pass is as if there were no catchers in the
     * scene. */
  }

  /* Shadow catcher pass. */
  if (kernel_shadow_catcher_is_object_pass(path_flag)) {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_shadow_catcher, contribution);
    return true;
  }

  return false;
}

ccl_device bool film_write_shadow_catcher_transparent(KernelGlobals kg,
                                                      const uint32_t path_flag,
                                                      const Spectrum contribution,
                                                      const float transparent,
                                                      ccl_global float *ccl_restrict buffer)
{
  if (!kernel_data.integrator.has_shadow_catcher) {
    return false;
  }

  kernel_assert(kernel_data.film.pass_shadow_catcher != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  if (path_flag & PATH_RAY_SHADOW_CATCHER_BACKGROUND) {
    return true;
  }

  /* Matte pass. */
  if (kernel_shadow_catcher_is_matte_path(path_flag)) {
    const float3 contribution_rgb = spectrum_to_rgb(contribution);

    film_write_pass_float4(buffer + kernel_data.film.pass_shadow_catcher_matte,
                           make_float4(contribution_rgb, transparent));
    /* NOTE: Accumulate the combined pass and to the samples count pass, so that the adaptive
     * sampling is based on how noisy the combined pass is as if there were no catchers in the
     * scene. */
  }

  /* Shadow catcher pass. */
  if (kernel_shadow_catcher_is_object_pass(path_flag)) {
    /* NOTE: The transparency of the shadow catcher pass is ignored. It is not needed for the
     * calculation and the alpha channel of the pass contains numbers of samples contributed to a
     * pixel of the pass. */
    film_write_pass_spectrum(buffer + kernel_data.film.pass_shadow_catcher, contribution);
    return true;
  }

  return false;
}

ccl_device void film_write_shadow_catcher_transparent_only(KernelGlobals kg,
                                                           const uint32_t path_flag,
                                                           const float transparent,
                                                           ccl_global float *ccl_restrict buffer)
{
  if (!kernel_data.integrator.has_shadow_catcher) {
    return;
  }

  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  /* Matte pass. */
  if (kernel_shadow_catcher_is_matte_path(path_flag)) {
    film_write_pass_float(buffer + kernel_data.film.pass_shadow_catcher_matte + 3, transparent);
  }
}

/* Write shadow catcher passes on a bounce from the shadow catcher object. */
ccl_device_forceinline void film_write_shadow_catcher_bounce_data(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  kernel_assert(kernel_data.film.pass_shadow_catcher_sample_count != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  /* Count sample for the shadow catcher object. */
  film_write_pass_float(buffer + kernel_data.film.pass_shadow_catcher_sample_count, 1.0f);

  /* Since the split is done, the sample does not contribute to the matte, so accumulate it as
   * transparency to the matte. */
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
  film_write_pass_float(buffer + kernel_data.film.pass_shadow_catcher_matte + 3,
                        average(throughput));
}

#endif /* __SHADOW_CATCHER__ */

/* --------------------------------------------------------------------
 * Render passes.
 */

/* Write combined pass. */
ccl_device_inline void film_write_combined_pass(KernelGlobals kg, // !!!
                                                const PathRayVisibility path_visibility, // !!!
                                                const uint32_t path_flag,
                                                const int sample,
                                                const Spectrum contribution,
                                                ccl_global float *ccl_restrict buffer)
{
#ifdef __SHADOW_CATCHER__
  if (film_write_shadow_catcher(kg, path_flag, contribution, buffer)) {
    return;
  }
#endif

  if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_combined, contribution);
  }

  film_write_adaptive_buffer(kg, sample, contribution, buffer);
  film_write_volume_scattering_guiding_pass(kg, buffer, path_visibility, path_flag, contribution);
}

/* Write combined pass with transparency. */
ccl_device_inline void film_write_combined_transparent_pass(
    KernelGlobals kg,
    const PathRayVisibility path_visibility,
    const uint32_t path_flag,
    const int sample,
    const Spectrum contribution,
    const float transparent,
    ccl_global float *ccl_restrict buffer)
{
#ifdef __SHADOW_CATCHER__
  if (film_write_shadow_catcher_transparent(kg, path_flag, contribution, transparent, buffer)) {
    return;
  }
#endif

  if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
    const float3 contribution_rgb = spectrum_to_rgb(contribution);

    film_write_pass_float4(buffer + kernel_data.film.pass_combined,
                           make_float4(contribution_rgb, transparent));
  }

  film_write_adaptive_buffer(kg, sample, contribution, buffer);
  film_write_volume_scattering_guiding_pass(kg, buffer, path_visibility, path_flag, contribution);
}

/* Write background or emission to appropriate pass. */
ccl_device_inline void film_write_emission_or_background_pass(
    KernelGlobals kg,
    ConstIntegratorState state,
    Spectrum contribution,
    ccl_global float *ccl_restrict buffer,
    const int pass,
    const int lightgroup = LIGHTGROUP_NONE)
{
  if (!(kernel_data.film.light_pass_flag & PASS_ANY)) {
    return;
  }

#ifdef __PASSES__
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  int pass_offset = PASS_UNUSED;

  /* Denoising albedo. */
#  ifdef __DENOISING_FEATURES__
  if (path_flag & PATH_RAY_DENOISING_FEATURES) {
    if (kernel_data.film.pass_denoising_albedo != PASS_UNUSED) {
      const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
          state, path, denoising_feature_throughput);
      const Spectrum denoising_albedo = denoising_feature_throughput * contribution;
      film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_albedo, denoising_albedo);
    }
  }
#  endif /* __DENOISING_FEATURES__ */

  const bool is_shadowcatcher = (path_flag & PATH_RAY_SHADOW_CATCHER_HIT) != 0;
  if (!is_shadowcatcher && lightgroup != LIGHTGROUP_NONE &&
      kernel_data.film.pass_lightgroup != PASS_UNUSED)
  {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_lightgroup + 3 * lightgroup,
                             contribution);
  }

  if (!(path_flag & PATH_RAY_ANY_PASS)) {
    /* Directly visible, write to emission or background pass. */
    pass_offset = pass;
  }
  else if (is_shadowcatcher) {
    /* Don't write any light passes for shadow catcher, for easier
     * compositing back together of the combined pass. */
    return;
  }
  else if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    if (path_flag & PATH_RAY_SURFACE_PASS) {
      /* Indirectly visible through reflection. */
      const Spectrum diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      const Spectrum glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);

      /* Glossy */
      const int glossy_pass_offset = ((INTEGRATOR_STATE(state, path, bounce) == 1) ?
                                          kernel_data.film.pass_glossy_direct :
                                          kernel_data.film.pass_glossy_indirect);
      if (glossy_pass_offset != PASS_UNUSED) {
        film_write_pass_spectrum(buffer + glossy_pass_offset, glossy_weight * contribution);
      }

      /* Transmission */
      const int transmission_pass_offset = ((INTEGRATOR_STATE(state, path, bounce) == 1) ?
                                                kernel_data.film.pass_transmission_direct :
                                                kernel_data.film.pass_transmission_indirect);

      if (transmission_pass_offset != PASS_UNUSED) {
        /* Transmission is what remains if not diffuse and glossy, not stored explicitly to save
         * GPU memory. */
        const Spectrum transmission_weight = one_spectrum() - diffuse_weight - glossy_weight;
        film_write_pass_spectrum(buffer + transmission_pass_offset,
                                 transmission_weight * contribution);
      }

      /* Reconstruct diffuse subset of throughput. */
      pass_offset = (INTEGRATOR_STATE(state, path, bounce) == 1) ?
                        kernel_data.film.pass_diffuse_direct :
                        kernel_data.film.pass_diffuse_indirect;
      if (pass_offset != PASS_UNUSED) {
        contribution *= diffuse_weight;
      }
    }
    else if (path_flag & PATH_RAY_VOLUME_PASS) {
      /* Indirectly visible through volume. */
      pass_offset = (INTEGRATOR_STATE(state, path, bounce) == 1) ?
                        kernel_data.film.pass_volume_direct :
                        kernel_data.film.pass_volume_indirect;
    }
  }

  /* Single write call for GPU coherence. */
  if (pass_offset != PASS_UNUSED) {
    film_write_pass_spectrum(buffer + pass_offset, contribution);
  }
#endif /* __PASSES__ */
}

// MARK: - 

/* GI/PT counterpart of film_add_reservoir_di().
 * Merges a newly produced path throughput into the persistent PT reservoir. */
// !!!: Logged and seems to be fine...
// !!!: Doesn't require the geometry_term...
ccl_device_inline void film_add_reservoir_pt(KernelGlobals kg,
                                             const uint32_t path_flag,
                                             const ccl_private Spectrum &throughput,
                                             ccl_private const RNGState *rng_state,
                                             ccl_global float *ccl_restrict buffer,
                                             const bool write_prev = false)
{
printf("\n=== film_add_reservoir_pt ENTER ===\n");
// printf("PT ADD ENTER pixel=%u\n",
//        INTEGRATOR_STATE(state, path, render_pixel_index)); // No state

  const bool use_restir = kernel_data.integrator.use_restir;
  kernel_assert(use_restir);

  ccl_global float *ptr = buffer + (write_prev ?
                                        kernel_data.film.pass_restir_pt_previous_reservoir :
                                        kernel_data.film.pass_restir_pt_reservoir);

printf("film_add_reservoir_pt PT WRITE ptr=%p weight=%f\n",
       ptr, (double)ptr[0]);
// Relevant offset:
printf("film_add_reservoir_pt PT WRITE pixel_buffer=%p ptr=%p offset=%d prev_offset=%d\n",
       buffer, ptr,
       kernel_data.film.pass_restir_pt_reservoir,
       kernel_data.film.pass_restir_pt_previous_reservoir);

printf("film_add_reservoir_pt throughput=(%f,%f,%f)\n",
       (double)throughput.x,
       (double)throughput.y,
       (double)throughput.z);

  GlobalReservoir reservoir;
  global_reservoir_init(kg, &reservoir);

printf("before unpack\n");
  restir_unpack_reservoir_pt(kg, &reservoir, ptr);
printf("after unpack\n");

  const float rand = path_state_rng_1D(kg, rng_state, PRNG_PICK);

printf("before add_sample\n");

const bool selected =
    global_reservoir_add_sample(&reservoir, throughput, rand);

printf("film_add_reservoir_pt after add_sample selected=%d total=%f, reservoir.rcv_index=%d\n",
       (int)selected,
       (double)reservoir.total_weight,
       reservoir.rcv_index);

  /* throughput arriving here is already the appropriately weighted PT candidate. */
  // if (global_reservoir_add_sample(&reservoir, throughput, rand)) { // TODO(Tri): Uncomment
  if (selected) {
    film_overwrite_pass_float (ptr + 1, (float)path_flag);
    film_overwrite_pass_float (ptr + 2, (float)reservoir.rcv_index); // New
    film_overwrite_pass_float3(ptr + 3, reservoir.radiance); // !!!: Not serializing sd
  }

  film_overwrite_pass_float(ptr, reservoir.total_weight);

printf("film_add_reservoir_pt returned: PT AFTER FILM WRITE ptr=%p stored=%f\n",
      //  INTEGRATOR_STATE(state, path, render_pixel_index), // No state here
       ptr,(double)*ptr); // film_add_reservoir_pt returned: PT AFTER FILM WRITE ptr=0x9c0f53358 stored=0.000027
}

// !!!: LOGGED This method's totally fine 
ccl_device_inline void film_write_pass_reservoir_pt(KernelGlobals kg,
                                                    ConstIntegratorState state,
                                                    ccl_private GlobalReservoir *reservoir,
                                                    ccl_global float *ccl_restrict render_buffer,
                                                    const bool write_prev = false)
{
  kernel_assert(kernel_data.integrator.use_restir);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  ccl_global float *ptr = buffer + (write_prev ?
                                        kernel_data.film.pass_restir_pt_previous_reservoir :
                                        kernel_data.film.pass_restir_pt_reservoir);

  film_overwrite_pass_float(ptr, reservoir->total_weight);
  film_overwrite_pass_float(ptr + 1, (float)reservoir->path_flag);
  film_overwrite_pass_float(ptr + 2, (float)reservoir->rcv_index);
  film_overwrite_pass_float3(ptr + 3, reservoir->radiance);

// // This tells us whether spatial resampling itself is replacing the valid initial reservoir with an empty one.
// printf("film_write_pass_reservoir_pt: PT SPATIAL WRITE pixel=%u write_prev=%d ptr=%p weight=%f\n",
//        INTEGRATOR_STATE(state, path, render_pixel_index),
//        (int)write_prev,
//        ptr,
//        (double)reservoir->total_weight);

}

// !!!: GI written into combine pass here? probably not
/* Write light contribution to render buffer. */
ccl_device_inline void film_write_direct_light(KernelGlobals kg,
                                               ConstIntegratorShadowState state,
                                               ccl_global float *ccl_restrict render_buffer)
{
  /* The throughput for shadow paths already contains the light shader evaluation. */
  Spectrum contribution = INTEGRATOR_STATE(state, shadow_path, throughput);

  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, shadow_path, bounce)); // TODO: Remove when ReSTIR's integrated

  /* Explicitly derived from the shadow state. */
  ccl_global float *buffer = film_pass_pixel_render_buffer_shadow(kg, state, render_buffer);

  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, shadow_path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, shadow_path, flag);
  const int sample = INTEGRATOR_STATE(state, shadow_path, sample);

printf("\n\n--- film_write_direct_light() prior to GI/PT insertion ---\n\n");
// // ???: Wrong? GI/PT reservoir insertion point? Do inside `film_write_background()` instead?
// // !!!: >>> TEMPORARILY DISABLED
//   // TODO: Move before AO for benchmark parity?
//   // TODO(weizhen): bounce number probably doesn't work for volume.
//   if (kernel_data.integrator.use_restir && INTEGRATOR_STATE(state, shadow_path, bounce) > 0)
//   {
//     printf("film_write_direct_light() Entered ReSTIR Path\n");

//     RNGState rng_state;
//     shadow_path_state_rng_load(state, &rng_state);

//     // ???: Don't need film_clamp_light() here?

//     /* TODO(weizhen): does this result in correlated samples in ReSTIR PT if branched version is
//      * used in DI? */
// // !!!: We've got another hook in integrate_light_forward() as well!!!
//     film_add_reservoir_pt(kg, path_flag, contribution, &rng_state, buffer); // !!!: Internally calls restir_unpack_reservoir_pt()
//   }
//   else {
//     printf("film_write_direct_light() Entered None-ReSTIR Path\n");
//     // !!!: would call `integrate_light_forward()`:

//     film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, shadow_path, bounce));
//     /* Direct light shadow. */
//     film_write_combined_pass(kg, path_visibility, path_flag, sample, contribution, buffer);
//   }
// !!!: <<<

  /* Ambient occlusion. */
  if (path_flag & PATH_RAY_SHADOW_FOR_AO) {
    if ((kernel_data.kernel_features & KERNEL_FEATURE_AO_PASS) &&
        (path_visibility & PATH_RAY_VISIBILITY_CAMERA))
    {
      film_write_pass_spectrum(buffer + kernel_data.film.pass_ao, contribution);
    }
    if (kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) {
      const Spectrum ao_weight = INTEGRATOR_STATE(state, shadow_path, unshadowed_throughput);
      film_write_combined_pass(
          kg, path_visibility, path_flag, sample, contribution * ao_weight, buffer);
    }
    return;
  }

  film_write_combined_pass(kg, path_visibility, path_flag, sample, contribution, buffer); // TODO: Remove when ReSTIR's integrated

#ifdef __PASSES__
  if (kernel_data.film.light_pass_flag & PASS_ANY) {
    const uint32_t path_flag = INTEGRATOR_STATE(state, shadow_path, flag);

    /* Don't write any light passes for shadow catcher, for easier
     * compositing back together of the combined pass. */
    if (path_flag & PATH_RAY_SHADOW_CATCHER_HIT) {
      return;
    }

    /* Write lightgroup pass. LIGHTGROUP_NONE is ~0 so decode from unsigned to signed */
    const int lightgroup = (int)(INTEGRATOR_STATE(state, shadow_path, lightgroup)) - 1;
    if (lightgroup != LIGHTGROUP_NONE && kernel_data.film.pass_lightgroup != PASS_UNUSED) {
      film_write_pass_spectrum(buffer + kernel_data.film.pass_lightgroup + 3 * lightgroup,
                               contribution);
    }

    if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
      int pass_offset = PASS_UNUSED;

      if (path_flag & PATH_RAY_SURFACE_PASS) {
        /* Indirectly visible through reflection. */
        const Spectrum diffuse_weight = INTEGRATOR_STATE(state, shadow_path, pass_diffuse_weight);
        const Spectrum glossy_weight = INTEGRATOR_STATE(state, shadow_path, pass_glossy_weight);

        /* Glossy */
        const int glossy_pass_offset = ((INTEGRATOR_STATE(state, shadow_path, bounce) == 0) ?
                                            kernel_data.film.pass_glossy_direct :
                                            kernel_data.film.pass_glossy_indirect);
        if (glossy_pass_offset != PASS_UNUSED) {
          film_write_pass_spectrum(buffer + glossy_pass_offset, glossy_weight * contribution);
        }

        /* Transmission */
        const int transmission_pass_offset = ((INTEGRATOR_STATE(state, shadow_path, bounce) == 0) ?
                                                  kernel_data.film.pass_transmission_direct :
                                                  kernel_data.film.pass_transmission_indirect);

        if (transmission_pass_offset != PASS_UNUSED) {
          /* Transmission is what remains if not diffuse and glossy, not stored explicitly to save
           * GPU memory. */
          const Spectrum transmission_weight = one_spectrum() - diffuse_weight - glossy_weight;
          film_write_pass_spectrum(buffer + transmission_pass_offset,
                                   transmission_weight * contribution);
        }

        /* Reconstruct diffuse subset of throughput. */
        pass_offset = (INTEGRATOR_STATE(state, shadow_path, bounce) == 0) ?
                          kernel_data.film.pass_diffuse_direct :
                          kernel_data.film.pass_diffuse_indirect;
        if (pass_offset != PASS_UNUSED) {
          contribution *= diffuse_weight;
        }
      }
      else if (path_flag & PATH_RAY_VOLUME_PASS) {
        /* Indirectly visible through volume. */
        pass_offset = (INTEGRATOR_STATE(state, shadow_path, bounce) == 0) ?
                          kernel_data.film.pass_volume_direct :
                          kernel_data.film.pass_volume_indirect;
      }

      /* Single write call for GPU coherence. */
      if (pass_offset != PASS_UNUSED) {
        film_write_pass_spectrum(buffer + pass_offset, contribution);
      }
    }
  }
#endif
}

/* Write transparency to render buffer.
 *
 * Note that we accumulate transparency = 1 - alpha in the render buffer.
 * Otherwise we'd have to write alpha on path termination, which happens
 * in many places. */
ccl_device_inline void film_write_transparent(KernelGlobals kg,
                                              const uint32_t path_flag,
                                              const float transparent,
                                              ccl_global float *ccl_restrict buffer)
{
  if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
    film_write_pass_float(buffer + kernel_data.film.pass_combined + 3, transparent);
  }

#ifdef __SHADOW_CATCHER__
  film_write_shadow_catcher_transparent_only(kg, path_flag, transparent, buffer);
#endif

  if (path_flag & PATH_RAY_VOLUME_PRIMARY_TRANSMIT) {
    kernel_assert(kernel_data.film.pass_volume_transmit != PASS_UNUSED);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_volume_transmit,
                             make_spectrum(transparent));
  }
}

/* Write holdout to render buffer. */
ccl_device_inline void film_write_holdout(KernelGlobals kg,
                                          ConstIntegratorState state,
                                          const uint32_t path_flag,
                                          const float transparent,
                                          ccl_global float *ccl_restrict render_buffer)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  film_write_transparent(kg, path_flag, transparent, buffer);
}

/* Write background contribution to render buffer.
 *
 * Includes transparency, matching film_write_transparent. */
ccl_device_inline void film_write_background(KernelGlobals kg,
                                             ConstIntegratorState state,
                                             const Spectrum L,
                                             const float transparent,
                                             const bool is_transparent_background_ray,
                                             ccl_global float *ccl_restrict render_buffer)
{
  Spectrum contribution = INTEGRATOR_STATE(state, path, throughput) * L;
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (is_transparent_background_ray) {
    film_write_transparent(kg, path_flag, transparent, buffer);
  }
  else {
    const int sample = INTEGRATOR_STATE(state, path, sample);
    film_write_combined_transparent_pass(
        kg, path_visibility, path_flag, sample, contribution, transparent, buffer);
  }

  film_write_emission_or_background_pass(kg,
                                         state,
                                         contribution,
                                         buffer,
                                         kernel_data.film.pass_background,
                                         kernel_data.background.lightgroup);
}

// MARK: - 

/* Shared serialization for both the spatial ping-pong reservoir passes and
 * the temporal history pass below. 
 * Layout is identical, only the destination offset differs. */
ccl_device_inline void film_write_pass_reservoir_at(ccl_private const Reservoir *reservoir,
                                                    ccl_global float *ccl_restrict ptr)
{
  // TODO(weizhen): it is possible to compress the light sample reference.

  film_overwrite_pass_float(ptr++, (float)reservoir->reference.type);
  film_overwrite_pass_float(ptr++, (float)reservoir->reference.flags);

  film_overwrite_pass_float(ptr++, (float)reservoir->reference.object);
  film_overwrite_pass_float(ptr++, (float)reservoir->reference.prim);

  film_overwrite_pass_float(ptr++, reservoir->reference.param.x);
  film_overwrite_pass_float(ptr++, reservoir->reference.param.y);

  film_overwrite_pass_float(ptr++, reservoir->total_weight);
  film_overwrite_pass_float(ptr++, reservoir->luminance);
  film_overwrite_pass_float(ptr,   (float)reservoir->M);
}

ccl_device_inline void film_write_pass_reservoir(
    KernelGlobals kg,
    ConstIntegratorState state,
    ccl_private const Reservoir *reservoir,
    ccl_global float *ccl_restrict render_buffer,
    const bool write_prev = false)
{
  kernel_assert(kernel_data.integrator.use_restir);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  ccl_global float *ptr = buffer + (write_prev ?
                                        kernel_data.film.pass_restir_previous_reservoir :
                                        kernel_data.film.pass_restir_reservoir);

  film_write_pass_reservoir_at(reservoir, ptr);
}

/* Persists this render sample's finalized reservoir for the next sample's
 * streaming_temporal_sample() to read as history. Distinct from the two
 * pass_restir_reservoir / pass_restir_previous_reservoir slots above: those
 * ping-pong *within* a sample across spatial-resampling iterations and are
 * unconditionally overwritten at the start of every new sample's initial
 * candidate generation, so they cannot double as cross-sample history —
 * see the pass_restir_previous_reservoir discussion. This pass is written
 * exactly once per sample, after spatial resampling has finished, and the
 * spatial ping-pong logic never touches it. */
ccl_device_inline void film_write_pass_reservoir_temporal(
    KernelGlobals kg,
    ConstIntegratorState state,
    ccl_private const Reservoir *reservoir,
    ccl_global float *ccl_restrict render_buffer)
{
  kernel_assert(kernel_data.integrator.use_restir);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  ccl_global float *ptr = buffer + kernel_data.film.pass_restir_temporal_history_reservoir;

  film_write_pass_reservoir_at(reservoir, ptr);
}

// TODO(Tri): film_clear_pass_reservoir() zeroes temporal_history_reservoir unconditionally on every integrator_init_from_camera(): Fixed!
// !!!: Called in init_from_camera; Don't we need a `film_clear_pass_reservoir_pt()` as well?
// !!!: It does not clear object prim, u, v, luminance
/* TODO(weizhen): clear data properly. */
ccl_device_inline void film_clear_pass_reservoir(KernelGlobals kg,
                                                 ConstIntegratorState state,
                                                 ccl_global float *ccl_restrict render_buffer)
{
  if (!kernel_data.integrator.use_restir) {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  /* Set DI reservoir weights to zero. */
  ccl_global float *ptr = buffer + kernel_data.film.pass_restir_reservoir + 6;
  film_overwrite_pass_float(ptr, 0.0f);

  ptr = buffer + kernel_data.film.pass_restir_previous_reservoir + 6;
  film_overwrite_pass_float(ptr, 0.0f);

  // ptr = buffer + kernel_data.film.pass_restir_temporal_history_reservoir + 6;
  // film_overwrite_pass_float(ptr, 0.0f);
  /* pass_restir_temporal_history_reservoir is deliberately NOT cleared here.
   * Unlike the DI/PT reservoirs above, this pass is write-once-per-sample
   * and it must survive across every call to integrator_init_from_camera().
   * film_write_pass_reservoir_temporal() fully overwrites all fields.
   */

  /* Set PT reservoir weights to zero. */
  ptr = buffer + kernel_data.film.pass_restir_pt_reservoir;
  film_overwrite_pass_float(ptr, 0.0f);
  film_overwrite_pass_float(ptr + 2, 0.0f);

  ptr = buffer + kernel_data.film.pass_restir_pt_previous_reservoir;
  film_overwrite_pass_float(ptr, 0.0f);

  /* Set sd.type to zero. */
  ptr = buffer + kernel_data.film.pass_surface_data + 3;
  film_overwrite_pass_float(ptr, 0.0f);
}

// MARK: - 

/* TODO(weizhen): this function is called before terminating the path. Better to move this function
   inside of `integrator_path_terminate()`. And what if multiple threads render the same pixel? */
ccl_device_inline void integrator_finalize_reservoir(KernelGlobals kg, // !!!: DI only
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer)
{
  const bool use_restir = kernel_data.integrator.use_restir;

  if (!use_restir) {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer) +
                             kernel_data.film.pass_restir_reservoir;

  Reservoir reservoir;
  reservoir_init(kg, &reservoir);

  const bool unpack_ok = restir_unpack_reservoir(kg, &reservoir, buffer); // !!!: DI; 

  // printf(
  //     "\n=== DI integrator_finalize_reservoir(); FINALIZE: AFTER UNPACK ===\n"
  //     "unpack_ok=%d\n"
  //     "total_weight=%f\n"
  //     "luminance=%f\n"
  //     "prim=%d object=%d flags=%u type=%d\n",
  //     (int)unpack_ok,
  //     (double)reservoir.total_weight,
  //     (double)reservoir.luminance,
  //     reservoir.reference.prim,
  //     reservoir.reference.object,
  //     reservoir.reference.flags,
  //     reservoir.reference.type);

  const bool finalize_ok = reservoir_finalize(&reservoir, use_restir);

  printf(
      "\n=== DI integrator_finalize_reservoir(); FINALIZE: AFTER FINALIZE ===\n\n"
      "finalize_ok=%d\n"
      "total_weight=%f\n"
      "luminance=%f\n"
      "prim=%d object=%d flags=%u type=%d\n",
      (int)finalize_ok,
      (double)reservoir.total_weight,
      (double)reservoir.luminance,
      reservoir.reference.prim,
      reservoir.reference.object,
      reservoir.reference.flags,
      reservoir.reference.type);

  film_write_pass_reservoir(kg, state, &reservoir, render_buffer);

  /* buffer[4] = safe_divide(buffer[4], buffer[5]); */
}

// MARK: - 

/* Write emission to render buffer. */
ccl_device_inline void film_write_volume_emission(KernelGlobals kg,
                                                  ConstIntegratorState state,
                                                  const Spectrum L,
                                                  ccl_global float *ccl_restrict render_buffer,
                                                  const int lightgroup = LIGHTGROUP_NONE)
{
  Spectrum contribution = L;
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const int sample = INTEGRATOR_STATE(state, path, sample);

  film_write_combined_pass(kg, path_visibility, path_flag, sample, contribution, buffer);
  film_write_emission_or_background_pass(
      kg, state, contribution, buffer, kernel_data.film.pass_emission, lightgroup);
}

ccl_device_inline void film_write_surface_emission(KernelGlobals kg, // !!!: GI
                                                   ConstIntegratorState state,
                                                   const Spectrum L,
                                                   const float mis_weight,
                                                   ccl_global float *ccl_restrict render_buffer,
                                                   const int lightgroup = LIGHTGROUP_NONE)
{
  Spectrum contribution = INTEGRATOR_STATE(state, path, throughput) * L * mis_weight;
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const int sample = INTEGRATOR_STATE(state, path, sample);

printf("film_write_surface_emission: pixel=%u buffer=%p\n",
       INTEGRATOR_STATE(state, path, render_pixel_index), buffer);

  film_write_combined_pass(kg, path_visibility, path_flag, sample, contribution, buffer);
  film_write_emission_or_background_pass(
      kg, state, contribution, buffer, kernel_data.film.pass_emission, lightgroup);
}

ccl_device_inline void film_add_reservoir_di(
    KernelGlobals kg,
    ConstIntegratorState state,
    const Spectrum L,
    ccl_private const LightSample &ls,
    ccl_private const LightSampleReference &candidate_reference,
    ccl_private const RNGState *rng_state,
    ccl_global float *ccl_restrict render_buffer)
{
  const bool use_restir = kernel_data.integrator.use_restir;

  /* TODO(weizhen): This might not work correctly with path guiding because
   * the value was assigned using `mis_pdf` instead of `bsdf_pdf`. */
  const float bsdf_pdf = INTEGRATOR_STATE(state, path, mis_ray_pdf);

  const bool is_direct_illumination = (INTEGRATOR_STATE(state, path, bounce) == 1);

  Reservoir reservoir;
  reservoir_init(kg, &reservoir, is_direct_illumination);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer) +
                             kernel_data.film.pass_restir_reservoir;

  restir_unpack_reservoir(kg, &reservoir, buffer);

  /* The caller supplies the persistent reference together with LightSample.
   * Forward light intersections may contain canonical emitter parameters that
   * cannot be recovered exactly from LightSample after its u/v fields were
   * removed. In particular, reconstructing the reference here using 
   * `light_reference_from_sample()` would duplicate per-light 
   * parameterization logic and would reject light types not supported
   * by `light_reference_from_sample()`. */
  if (!light_reference_is_valid(&candidate_reference)) {
    return;
  }

  const float geometry_term = ls.jacobian_solid_angle_to_area();
  const Spectrum contribution = INTEGRATOR_STATE(state, path, throughput) *
                                bsdf_pdf * L * geometry_term;

  /* TODO(weizhen): Handle diffuse/glossy pass decomposition properly. */
  BsdfEval radiance;
  bsdf_eval_init(&radiance, contribution);

  const float rand_pick = path_branched_rng_1D(
      kg, rng_state, reservoir.num_light_samples, reservoir.max_samples, PRNG_PICK);

  reservoir_add_bsdf_sample(
      &reservoir, ls, candidate_reference, radiance, bsdf_pdf, rand_pick, use_restir);

/*
 * because film_write_pass_reservoir() writes only the serialized reference, weight, and luminance, 
 * the Reservoir must contain the candidate reference selected by reservoir_add_sample(). 
 * The function must therefore receive the candidate reference explicitly
 */
  film_write_pass_reservoir(kg, state, &reservoir, render_buffer);
}

CCL_NAMESPACE_END
