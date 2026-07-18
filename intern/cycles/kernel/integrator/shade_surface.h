/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/path_state.h"
#include "kernel/integrator/surface_shader.h"

#include "kernel/film/data_passes.h"
#include "kernel/film/denoising_passes.h"
#include "kernel/film/light_passes.h"

#include "kernel/light/sample.h"
#include "kernel/light/sample_reference.h"

#include "kernel/geom/motion_triangle.h"
#include "kernel/geom/triangle.h"

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/shadow_linking.h"
#include "kernel/integrator/subsurface.h"

// #include "kernel/integrator/reservoir.h"
#include "kernel/integrator/restir.h" // !!!: Remove!
// #include "kernel/integrator/restir_temporal.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/types.h"
#include "util/math_intersect.h"

CCL_NAMESPACE_BEGIN

ccl_device_forceinline void integrate_surface_shader_setup(KernelGlobals kg,
                                                           ConstIntegratorState state,
                                                           ccl_private ShaderData *sd)
{
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_isect(state, &isect);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_ray(state, &ray);

  shader_setup_from_ray(kg, sd, &ray, &isect);
}

ccl_device_forceinline float3 integrate_surface_ray_offset(KernelGlobals kg,
                                                           const ccl_private ShaderData *sd,
                                                           const float3 ray_P,
                                                           const float3 ray_D)
{
  /* No ray offset needed for other primitive types. */
  if (!(sd->type & PRIMITIVE_TRIANGLE)) {
    return ray_P;
  }

  /* Self intersection tests already account for the case where a ray hits the
   * same primitive. However precision issues can still cause neighboring
   * triangles to be hit. Here we test if the ray-triangle intersection with
   * the same primitive would miss, implying that a neighboring triangle would
   * be hit instead.
   *
   * This relies on triangle intersection to be watertight, and the object inverse
   * object transform to match the one used by ray intersection exactly.
   *
   * Potential improvements:
   * - It appears this happens when either barycentric coordinates are small,
   *   or dot(sd->Ng, ray_D)  is small. Detect such cases and skip test?
   * - Instead of ray offset, can we tweak P to lie within the triangle?
   */

  /* TODO: Investigate if there are better ray offsetting algorithms for each BVH.
   * Cycles and Custom BVH triangle tests aren't numerically identical, meaning
   * this method isn't ideal for them. */

  float3 verts[3];
  if (sd->type == PRIMITIVE_TRIANGLE) {
    triangle_vertices(kg, sd->object, sd->prim, verts);
  }
  else {
    kernel_assert(sd->type == PRIMITIVE_MOTION_TRIANGLE);
    motion_triangle_vertices(kg, sd->object, sd->prim, sd->time, verts);
  }

  float3 local_ray_P = ray_P;
  float3 local_ray_D = ray_D;

  if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    const Transform itfm = object_get_inverse_transform(kg, sd);
    local_ray_P = transform_point(&itfm, local_ray_P);
    local_ray_D = transform_direction(&itfm, local_ray_D);
  }

  if (ray_triangle_intersect_self(local_ray_P, local_ray_D, verts)) {
    return ray_P;
  }
  return ray_offset(ray_P, sd->Ng);
}

ccl_device_forceinline bool integrate_surface_holdout(KernelGlobals kg,
                                                      ConstIntegratorState state,
                                                      ccl_private ShaderData *sd,
                                                      ccl_global float *ccl_restrict render_buffer)
{
  /* Write holdout transparency to render buffer and stop if fully holdout. */
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (((sd->flag & SD_HOLDOUT) || (sd->object_flag & SD_OBJECT_HOLDOUT_MASK)) &&
      (path_flag & PATH_RAY_TRANSPARENT_BACKGROUND))
  {
    const Spectrum holdout_weight = surface_shader_apply_holdout(sd);
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    const float transparent = average(holdout_weight * throughput);
    film_write_holdout(kg, state, path_flag, transparent, render_buffer);
    if (isequal(holdout_weight, one_spectrum())) {
      return false;
    }
  }

  return true;
}

ccl_device_forceinline void integrate_surface_emission(KernelGlobals kg,
                                                       IntegratorState state,
                                                       const ccl_private ShaderData *sd,
                                                       ccl_global float *ccl_restrict
                                                           render_buffer)
{
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

#ifdef __LIGHT_LINKING__
  if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
      !light_link_object_match(kg, light_link_receiver_forward(kg, state), sd->object))
  {
    return;
  }
#endif

#ifdef __SHADOW_LINKING__
  /* Indirect emission of shadow-linked emissive surfaces is done via shadow rays to dedicated
   * light sources. */
  if (kernel_data.kernel_features & KERNEL_FEATURE_SHADOW_LINKING) {
    if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
        kernel_data_fetch(objects, sd->object).shadow_set_membership != LIGHT_LINK_MASK_ALL)
    {
      return;
    }
  }
#endif

  /* Evaluate emissive closure. */
  const Spectrum L = surface_shader_emission(sd);

  const float mis_weight = light_sample_mis_weight_forward_surface(
      kg, state, path_visibility, path_flag, sd);

  guiding_record_surface_emission(kg, state, L, mis_weight);
  film_write_surface_emission( // !!!: Final radiance commit
      kg, state, L, mis_weight, render_buffer, object_lightgroup(kg, sd->object));
}

ccl_device int integrate_surface_ray_portal(KernelGlobals kg,
                                            IntegratorState state,
                                            ccl_private ShaderData *sd,
                                            const ccl_private ShaderClosure *sc)
{
  const ccl_private RayPortalClosure *pc = (const ccl_private RayPortalClosure *)sc;

  float sum_sample_weight = 0.0f;
  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];

    if (CLOSURE_IS_BSDF_OR_BSSRDF(sc->type)) {
      sum_sample_weight += sc->sample_weight;
    }
  }
  if (sum_sample_weight <= 0.0f) {
    return LABEL_NONE;
  }

  if (len_squared(sd->P - pc->P) > 1e-9f) {
    /* if the ray origin is changed, unset the current object,
     * so we can potentially hit the same polygon again */
    INTEGRATOR_STATE_WRITE(state, isect, object) = OBJECT_NONE;
    INTEGRATOR_STATE_WRITE(state, ray, P) = pc->P;
  }
  else {
    INTEGRATOR_STATE_WRITE(state, ray, P) = integrate_surface_ray_offset(kg, sd, pc->P, pc->D);
  }
  INTEGRATOR_STATE_WRITE(state, ray, D) = pc->D;
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
  INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
  INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);
#endif

  const float pick_pdf = pc->sample_weight / sum_sample_weight;
  INTEGRATOR_STATE_WRITE(state, path, throughput) *= pc->weight / pick_pdf;

  const int label = LABEL_TRANSMIT | LABEL_RAY_PORTAL;
  path_state_next(kg, state, label, sd->flag);

  return label;
}

// !!!: where shadow state payload is initialized.
/* Branch off a shadow path and initialize common part of it.
 * THe common is between the surface shading and configuration of a special shadow ray for the
 * shadow linking. */
ccl_device_inline IntegratorShadowState
integrate_direct_light_shadow_init_common(KernelGlobals kg,
                                          IntegratorState state,
                                          const ccl_private Ray *ccl_restrict ray,
                                          const Spectrum bsdf_spectrum,
                                          const int light_group,
                                          const int mnee_vertex_count,
                                          const bool constant_light_shader)
{
  const DeviceKernel next_kernel = (constant_light_shader) ?
                                       DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW :
                                       DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE;

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state;
#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    /* Reuse shadow path that was already allocated by intersect_mnee. */
    shadow_state = integrator_state_get_mnee_shadow_state(state);
    integrator_shadow_path_next(
        shadow_state, DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING, next_kernel);
  }
  else
#endif
  {
    shadow_state = integrator_shadow_path_init(kg, state, next_kernel, false);
  }

#ifdef __VOLUME__
  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);
#endif

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(shadow_state, ray);
  integrator_state_write_shadow_ray_self(shadow_state, ray);

  /* Copy state from main path to shadow path. */
  const Spectrum unlit_throughput = INTEGRATOR_STATE(state, path, throughput);
  const Spectrum throughput = unlit_throughput * bsdf_spectrum;

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bsdf_eval_average) = average(bsdf_spectrum);
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_pixel) = INTEGRATOR_STATE(
      state, path, rng_pixel);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = INTEGRATOR_STATE(
      state, path, transparent_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, volume_bounds_bounce) = INTEGRATOR_STATE(
      state, path, volume_bounds_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, glossy_bounce) = INTEGRATOR_STATE(
      state, path, glossy_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, restir_di) = 0; // !!!(Tri):
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if ((kernel_data.kernel_features & KERNEL_FEATURE_NODE_PORTAL)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, portal_bounce) = INTEGRATOR_STATE(
        state, path, portal_bounce);
  }

#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) =
        INTEGRATOR_STATE(state, path, transmission_bounce) + mnee_vertex_count - 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           diffuse_bounce) = INTEGRATOR_STATE(state, path, diffuse_bounce) + 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           bounce) = INTEGRATOR_STATE(state, path, bounce) + mnee_vertex_count;
  }
  else
#endif
  {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) = INTEGRATOR_STATE(
        state, path, transmission_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, diffuse_bounce) = INTEGRATOR_STATE(
        state, path, diffuse_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = INTEGRATOR_STATE(
        state, path, bounce);
  }

  /* Write Light-group, +1 as light-group is int but we need to encode into a uint8_t. */
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, lightgroup) = light_group + 1;

#if defined(__PATH_GUIDING__)
  if ((kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unlit_throughput) = unlit_throughput;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, path_segment) = INTEGRATOR_STATE(
        state, guiding, path_segment);
    INTEGRATOR_STATE(shadow_state, shadow_path, guiding_mis_weight) = 0.0f;
  }
#endif

  return shadow_state;
}

#ifdef __RESTIR_DI__
ccl_device_forceinline
// ccl_device // !!!: stack size
bool restir_di_select_light(KernelGlobals kg,
                            IntegratorState state,
                            ccl_private ShaderData *sd,
                            const ccl_private RNGState *rng_state,
                            ccl_global ReSTIRDIReservoir *restir_reservoirs,
                            ccl_global const ReSTIRDIReservoir *restir_reservoirs_prev,
                            const int restir_reservoir_count,
                            ccl_global float *ccl_restrict render_buffer,
                            ccl_private LightSample *r_ls,
                            ccl_private BsdfEval *r_radiance,
                            ccl_private float *r_total_weight,
                            int mnee_vertex_count)
{
  kernel_assert(kernel_data.integrator.use_restir);

  /* initialize the output immediately! */
  r_ls->pdf = 0.0f;
  *r_radiance = BsdfEval();
  *r_total_weight = 0.0f;


  const uint candidate_count = kernel_data.integrator.restir_light_samples;

  ReSTIRDIRisState ris = restir_di_ris_empty(); // !!!: Removing restir_.h

  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const uint bounce = INTEGRATOR_STATE(state, path, bounce);

  const uint pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  // !!!:
  if (UNLIKELY(pixel_index >= (uint)restir_reservoir_count)) {
    return false;
  }

  for (uint i = 0; i < candidate_count; i++) {
    // PROFILING_EVENT(PROFILING_RESTIR_LIGHT_RESAMPLING);

    LightSample candidate ccl_optional_struct_init;

    const float3 rand_light = path_branched_rng_3D(kg, rng_state, i, candidate_count, PRNG_SURFACE_RESTIR_LIGHT);

    // // Emitter sample parameters; light sampler should use these values directly as emmiter UVs
    // const float2 candidate_uv = make_float2(rand_light.x, rand_light.y);

    // Selected candidate is generated during the current path trace step:
    if (!light_sample_from_position(kg,
                                    rand_light,
                                    sd->time,
                                    sd->P,
                                    sd->N,
                                    light_link_receiver_nee(kg, sd),
                                    sd->flag,
                                    bounce,
                                    path_flag,
                                    &candidate)) // Relocation is needed only when we evaluate or output a reused reservoir sample
    {
      ris.M++;
      continue;
    }

    if (candidate.pdf <= 0.0f) {
      ris.M++;
      continue;
    }

    Spectrum candidate_light_eval ccl_optional_struct_init;
    light_sample_shader_eval_nee_constant(kg,
                                          candidate.shader,
                                          candidate.prim,
                                          candidate.type != LIGHT_TRIANGLE,
                                          candidate_light_eval);

    // reject !isfinite(candidate_light_eval) lights
    if (!isfinite(spectrum_to_gray(kg, candidate_light_eval))) { // !!!: New
        ris.M++;
        continue;
    }

    BsdfEval candidate_bsdf_eval ccl_optional_struct_init;
    const float bsdf_pdf = surface_shader_bsdf_eval(kg,
                                                    state,
                                                    sd,
                                                    candidate.D,
                                                    &candidate_bsdf_eval,
                                                    candidate.shader);

    // Approximate radiance payload
    BsdfEval candidate_radiance = candidate_bsdf_eval;
    bsdf_eval_mul(&candidate_radiance, candidate_light_eval * candidate.eval_fac);


    /** 
     * !!!: Current RIS baseline is NEE-only. For forward direct-light sampling this must use the
     * corresponding forward MIS formulation. */
    // const float mis_weight = light_sample_mis_weight_nee(kg, candidate.pdf, bsdf_pdf);
    /** 
     * !!!: It may still need a broader audit later depending on:
     * what measure ls.pdf really means for each emitter type
     * whether the light tree changes proposal semantics
     * whether future spatial relocation reconstructs a proposal in a canonical measure
     */

    const float random = path_branched_rng_1D(
        kg, rng_state, i, candidate_count, PRNG_SURFACE_RESTIR_RESAMPLE);


    ReSTIRLightSampleReference candidate_reference = restir_light_reference_empty();

    const bool has_reference = restir_light_reference_from_sample(
        kg, &candidate, &candidate_reference);

    if (has_reference) {
      restir_debug_validate_light_reference_roundtrip_from_reference(
          kg, &candidate, &candidate_reference, sd->P, sd->N, sd->flag, path_flag);
    } else {
      printf("restir_di_select_light(); restir_debug_validate_light_reference_roundtrip() was not executed\n");
      // assert(false);
    }

    restir_di_add_light_sample(
        kg, &ris, &candidate, &candidate_reference, &candidate_radiance, bsdf_pdf, random);
  }

// >>> DEBUG
  kernel_assert(ris.M == candidate_count);
//   kernel_assert(isfinite(ris.weight_sum));
//   kernel_assert(ris.weight_sum >= 0.0f);
kernel_assert(isfinite(ris.total_weight));
kernel_assert(ris.total_weight >= 0.0f);
// <<<

  // ReSTIRReservoir reservoir = restir_finalize(&ris);
  ReSTIRDIReservoir reservoir;
  const bool valid = restir_di_finalize(&reservoir, &ris);

  // const bool use_temporal = kernel_data.integrator.use_temporal_resampling &&
  //                               bounce == 0 && mnee_vertex_count == 0;

  // !!!: For now, keep ReSTIR DI temporal reuse first-hit only.
  if (false) {
    // !!!: Temporal reuse is disabled until it is ported to ReSTIRDIReservoir semantics.
    // ReSTIRReservoir new_res = restir_temporal_reuse( // Temporal reuse stage
    //     kg,
    //     state,
    //     sd,
    //     rng_state,
    //     &reservoir,
    //     // kernel_data_fetch(PathTraceWorkGPU::restir_reservoirs_prev_, 0));
    //     restir_reservoirs_prev,
    //     restir_reservoir_count
    //     // render_buffer
    //   );

    // reservoir = new_res;
  }

  // Shadow test is done after selection.
  // Required before temporal reuse.

  if (!valid || !restir_di_reservoir_is_valid(&reservoir) || reservoir.light.pdf <= 0.0f) {
    if (restir_reservoirs != nullptr && bounce == 0 && mnee_vertex_count == 0) {
      restir_reservoirs[pixel_index] = restir_di_reservoir_empty(); // !!!: Removing restir_.h
    }
    return false; // Invalid reservoir
  }

  if (restir_reservoirs != nullptr && bounce == 0 && mnee_vertex_count == 0) { // storage and shadow validation are matched
      restir_reservoirs[pixel_index] = reservoir;
  }

// >>> Debug
  kernel_assert(isfinite(reservoir.light.pdf));
  kernel_assert(reservoir.light.pdf >= 0.0f);
// <<<

  *r_ls = reservoir.light;
  *r_radiance = reservoir.radiance;
  *r_total_weight = reservoir.total_weight;

  return true;
}
#endif

ccl_device_forceinline 
bool integrate_surface_sample_light_nee(KernelGlobals kg,
                                        IntegratorState state,
                                        ccl_private ShaderData *sd,
                                        ccl_private LightSample *ls,
                                        const uint bounce,
                                        const uint32_t path_flag,
                                        const ccl_private RNGState *rng_state,
                                        ccl_private int *mnee_vertex_count)
{
#ifdef __MNEE__
  if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
      (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
  {
    /* MNEE already sampled a light and caustics casters. */
    integrator_state_read_mnee(state, ls, mnee_vertex_count);
    return true;
  }
  else
#endif
  {
    /* Sample position on a light. */
    const float3 rand_light = path_state_rng_3D(kg, rng_state, PRNG_LIGHT);

    return light_sample_from_position(kg,
                                      rand_light,
                                      sd->time,
                                      sd->P,
                                      sd->N,
                                      light_link_receiver_nee(kg, sd),
                                      sd->flag,
                                      bounce,
                                      path_flag,
                                      ls);
  }
}


// !!!: Holds the Direct-Light Logic Path; wavefront scheduler stage.
/* Path tracing: sample point on light and evaluate light shader, then
 * queue shadow ray to be traced. */
template<uint node_feature_mask>
#if defined(__KERNEL_GPU__)
ccl_device_forceinline
#else
/* MSVC has very long compilation time (x20) if we force inline this function */
ccl_device
#endif
    ShaderEvalResult
    integrate_surface_direct_light(KernelGlobals kg,
                                   IntegratorState state,
                                   ccl_private ShaderData *sd,
                                   const ccl_private RNGState *rng_state,
                                   ccl_global ReSTIRDIReservoir *restir_reservoirs,
                                   ccl_global const ReSTIRDIReservoir *restir_reservoirs_prev,
                                   const int restir_reservoir_count,
                                   ccl_global float *ccl_restrict render_buffer)
{ // integrate_surface_direct_light
  PROFILING_INIT(kg, PROFILING_RESTIR_INITIAL_RESAMPLING);

  /* Test if there is a light or BSDF that needs direct light. */
  if (!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL))) {
    return SHADER_EVAL_EMPTY;
  }

  LightSample ls ccl_optional_struct_init;
  int mnee_vertex_count = 0;  // NOLINT

  const uint bounce = INTEGRATOR_STATE(state, path, bounce);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

/* Select light sample (classic NEE or ReSTIR) */
#ifdef __RESTIR_DI__

  const bool use_restir = kernel_data.integrator.use_restir;

  // float restir_W = 0.0f;
  float restir_total_weight = 0.0f;
  BsdfEval restir_radiance ccl_optional_struct_init;

  // printf("integrate_surface_direct_light(); use_restir:%d\n", use_restir); // DEBUG ONLY

  if (use_restir) {
    PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

    // if (!restir_di_select_light(kg, state, sd, rng_state, 
    //                             restir_reservoirs, restir_reservoirs_prev, restir_reservoir_count, 
    //                             render_buffer,&ls, &restir_W, mnee_vertex_count)) { // !!!
    if (!restir_di_select_light(kg,
                                state,
                                sd,
                                rng_state,
                                restir_reservoirs,
                                restir_reservoirs_prev,
                                restir_reservoir_count,
                                render_buffer,
                                &ls,
                                &restir_radiance,
                                &restir_total_weight,
                                mnee_vertex_count)) {
        return SHADER_EVAL_EMPTY;
    }
  }
  else {
    if (!integrate_surface_sample_light_nee(kg, state, sd, &ls, bounce, path_flag, 
                                            rng_state, &mnee_vertex_count)) {
        return SHADER_EVAL_EMPTY;
    }
  }

#else

  if (!integrate_surface_sample_light_nee(kg, state, sd, &ls, bounce, path_flag, 
                                          rng_state, &mnee_vertex_count)) {
      return SHADER_EVAL_EMPTY;
  }

#endif


  kernel_assert(ls.pdf != 0.0f);

  const bool is_transmission = dot(ls.D, sd->N) < 0.0f;

  if (ls.prim != PRIM_NONE && ls.prim == sd->prim && ls.object == sd->object) {
    /* Skip self intersection if light direction lies in the same hemisphere as the geometric
     * normal. */
    if (dot(ls.D, is_transmission ? -sd->Ng : sd->Ng) > 0.0f) {
      return SHADER_EVAL_EMPTY;
    }
  }

#ifdef __MNEE__
  /* On a caustic caster, a caustic light's contribution is delivered to receivers by
   * MNEE and does not need to be computed again here. */
  if (kernel_data.kernel_features & KERNEL_FEATURE_MNEE) {
    if (mnee_vertex_count == 0 && is_transmission &&
        (sd->object_flag & SD_OBJECT_CAUSTICS_CASTER) && ls.type != LIGHT_TRIANGLE &&
        kernel_data_fetch(lights, ls.prim).use_caustics)
    {
      return SHADER_EVAL_EMPTY;
    }
  }
#endif

  /* Evaluate constant part of light shader, rest will optionally be done in another kernel. */
  Spectrum light_shader_eval ccl_optional_struct_init; // !!!: not necessarily the final emitted radiance (for all lights)
  const bool is_constant_light_shader = light_sample_shader_eval_nee_constant( // !!!
      kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_shader_eval);

  /* Evaluate BSDF. */
  BsdfEval bsdf_eval ccl_optional_struct_init; // Should be the throughput/unshadowed contribution
  const float bsdf_pdf = surface_shader_bsdf_eval(kg, state, sd, ls.D, &bsdf_eval, ls.shader); // !!!

  Ray ray ccl_optional_struct_init;

#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    light_shader_eval *= integrator_state_read_mnee_throughput(state);
    bsdf_eval_mul(&bsdf_eval, light_shader_eval);

    if (bsdf_eval_is_zero(&bsdf_eval)) {
      return SHADER_EVAL_EMPTY;
    }

    integrator_state_read_mnee_ray(state, &ls, &ray);
  }
  else
#endif /* __MNEE__ */
  {
    const float mis_weight = light_sample_mis_weight_nee(kg, ls.pdf, bsdf_pdf);
#ifdef __RESTIR_DI__
    const bool use_restir = kernel_data.integrator.use_restir;

    // if (restir_W > 0.0f) {
    //   if (use_restir) {
    //     bsdf_eval_mul(&bsdf_eval,
    //                   light_shader_eval * ls.eval_fac * restir_W);
    //   }
    // }

    if (use_restir && restir_total_weight > 0.0f) {
      bsdf_eval = restir_radiance;
      bsdf_eval_mul(&bsdf_eval, restir_total_weight);
    }
    else
    /**
     * RIS/temporal path: restir_W already contains the normalization implied by selected target
     * fallback non-ReSTIR path: still uses ordinary direct-light evaluation with / ls.pdf * mis_weight
     */
#endif
    {
      bsdf_eval_mul(&bsdf_eval, light_shader_eval * ls.eval_fac / ls.pdf * mis_weight);
    }


    /* Path termination for constant light shader. */ // !!!
    if (is_constant_light_shader && !(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
      const float terminate = path_state_rng_light_termination(kg, rng_state);
      if (light_sample_terminate(kg, &bsdf_eval, terminate)) {
        return SHADER_EVAL_EMPTY;
      }
    }
    /* For non-constant light shader, probabilistic termination happens in
     * SHADE_LIGHT_NEE when the full contribution is known. */ // !!!
    else if (bsdf_eval_is_zero(&bsdf_eval)) {
      return SHADER_EVAL_EMPTY;
    }

    /* Create shadow ray. */
    light_sample_to_surface_shadow_ray(kg, sd, &ls, &ray); // !!!
  }

  if (ray.self.object != OBJECT_NONE) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrate_direct_light_shadow_init_common(
      kg,
      state,
      &ray,
      bsdf_eval_sum(&bsdf_eval),
      ls.group,
      mnee_vertex_count,
      is_constant_light_shader);

#ifdef __RESTIR_DI__ // Shadow ray visibility
// !!!: MNEE path should not be allowed to mutate the normal per-pixel ReSTIR reservoir unless 
// TODO(Tri): ReSTIR+MNEE is explicitly supported
// TODO(Tri): exclude AO/shadow catcher paths ...
  if (use_restir && bounce == 0 && mnee_vertex_count == 0) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, restir_di) = 1;
  }
#endif

  if (is_transmission) {
#ifdef __VOLUME__
    volume_stack_enter_exit<true>(kg, shadow_state, sd);
#endif
  }

  uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    PackedSpectrum pass_diffuse_weight;
    PackedSpectrum pass_glossy_weight;

    if (shadow_flag & PATH_RAY_ANY_PASS) {
      /* Indirect bounce, use weights from earlier surface or volume bounce. */
      pass_diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      pass_glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
    }
    else {
      /* Direct light, use BSDFs at this bounce. */
      shadow_flag |= PATH_RAY_SURFACE_PASS;
      pass_diffuse_weight = PackedSpectrum(bsdf_eval_pass_diffuse_weight(&bsdf_eval));
      pass_glossy_weight = PackedSpectrum(bsdf_eval_pass_glossy_weight(&bsdf_eval));
    }

    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_diffuse_weight) = pass_diffuse_weight;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_glossy_weight) = pass_glossy_weight;
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
      state, path, visibility);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;

  return SHADER_EVAL_OK;
}

// TODO: For ReSTIR, this needs special handling
/* Path tracing: bounce off or through surface with new direction. */
ccl_device_forceinline int integrate_surface_bsdf_bssrdf_bounce(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    const ccl_private RNGState *rng_state)
{
  /* Sample BSDF or BSSRDF. */
  if (!(sd->flag & (SD_BSDF | SD_BSSRDF))) {
    return LABEL_NONE;
  }

  float3 rand_bsdf = path_state_rng_3D(kg, rng_state, PRNG_SURFACE_BSDF);
  const ccl_private ShaderClosure *sc = surface_shader_bsdf_bssrdf_pick(sd, &rand_bsdf);

#ifdef __SUBSURFACE__
  /* BSSRDF closure, we schedule subsurface intersection kernel. */
  if (CLOSURE_IS_BSSRDF(sc->type)) {
    return subsurface_bounce(kg, state, sd, sc);
  }
#endif
  if (CLOSURE_IS_RAY_PORTAL(sc->type)) {
    return integrate_surface_ray_portal(kg, state, sd, sc);
  }

  /* BSDF closure, sample direction. */
  float bsdf_pdf = 0.0f;
  float unguided_bsdf_pdf = 0.0f;
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float3 bsdf_wo ccl_optional_struct_init;
  int label;

  float2 bsdf_sampled_roughness = make_float2(1.0f, 1.0f);
  float bsdf_eta = 1.0f;
  float mis_pdf = 1.0f;

#if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
  if (kernel_data.integrator.use_surface_guiding &&
      (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING))
  {
    label = surface_shader_bsdf_guided_sample_closure(kg,
                                                      state,
                                                      sd,
                                                      sc,
                                                      rand_bsdf,
                                                      &bsdf_eval,
                                                      &bsdf_wo,
                                                      &bsdf_pdf,
                                                      &mis_pdf,
                                                      &unguided_bsdf_pdf,
                                                      &bsdf_sampled_roughness,
                                                      &bsdf_eta,
                                                      rng_state);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval)) {
      return LABEL_NONE;
    }

    INTEGRATOR_STATE_WRITE(state, path, unguided_throughput) *= bsdf_pdf / unguided_bsdf_pdf;
  }
  else
#endif
  {
    label = surface_shader_bsdf_sample_closure(kg,
                                               sd,
                                               sc,
                                               rand_bsdf,
                                               &bsdf_eval,
                                               &bsdf_wo,
                                               &bsdf_pdf,
                                               &bsdf_sampled_roughness,
                                               &bsdf_eta);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval)) {
      return LABEL_NONE;
    }
    mis_pdf = bsdf_pdf;
    unguided_bsdf_pdf = bsdf_pdf;
  }

  if (label & LABEL_TRANSPARENT) {
    /* Only need to modify start distance for transparent. */
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);
  }
  else {
    /* Setup ray with changed origin and direction. */
    const float3 D = normalize(bsdf_wo);
    INTEGRATOR_STATE_WRITE(state, ray, P) = integrate_surface_ray_offset(kg, sd, sd->P, D);
    INTEGRATOR_STATE_WRITE(state, ray, D) = D;
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
    INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
    INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);
    INTEGRATOR_STATE_WRITE(state, ray, dD) = bsdf_widen_dD(INTEGRATOR_STATE(state, ray, dD),
                                                           bsdf_sampled_roughness);
#endif
  }

  /* Update throughput. */
  const Spectrum bsdf_weight = bsdf_eval_sum(&bsdf_eval) / bsdf_pdf;
  INTEGRATOR_STATE_WRITE(state, path, throughput) *= bsdf_weight;

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    if (INTEGRATOR_STATE(state, path, bounce) == 0) {
      INTEGRATOR_STATE_WRITE(state, path, pass_diffuse_weight) = bsdf_eval_pass_diffuse_weight(
          &bsdf_eval);
      INTEGRATOR_STATE_WRITE(state, path, pass_glossy_weight) = bsdf_eval_pass_glossy_weight(
          &bsdf_eval);
    }
  }

  /* Update path state */
  if (!(label & LABEL_TRANSPARENT)) {
    const float min_ray_pdf = INTEGRATOR_STATE(state, path, min_ray_pdf);
    INTEGRATOR_STATE_WRITE(state, path, mis_ray_pdf) = mis_pdf;
    INTEGRATOR_STATE_WRITE(state, path, mis_origin_n) = sd->N;
    INTEGRATOR_STATE_WRITE(state, path, min_ray_pdf) = fminf(unguided_bsdf_pdf, min_ray_pdf);

#ifdef __LIGHT_LINKING__
    if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING) {
      INTEGRATOR_STATE_WRITE(state, path, mis_ray_object) = sd->object;
    }
#endif
  }

  path_state_next(kg, state, label, sd->flag);

  guiding_record_surface_bounce(kg,
                                state,
                                bsdf_weight,
                                bsdf_pdf,
                                sd->N,
                                normalize(bsdf_wo),
                                bsdf_sampled_roughness,
                                bsdf_eta);

  return label;
}

#ifdef __VOLUME__
ccl_device_forceinline int integrate_surface_volume_only_bounce(IntegratorState state,
                                                                ccl_private ShaderData *sd)
{
  if (!path_state_volume_next(state)) {
    return LABEL_NONE;
  }

  /* Only modify start distance. */
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);

  return LABEL_TRANSMIT | LABEL_TRANSPARENT;
}
#endif

ccl_device_forceinline bool integrate_surface_terminate(IntegratorState state,
                                                        const uint32_t path_flag)
{
  const float continuation_probability = (path_flag & PATH_RAY_TERMINATE_ON_NEXT_SURFACE) ?
                                             0.0f :
                                             INTEGRATOR_STATE(
                                                 state, path, continuation_probability);
  if (continuation_probability == 0.0f) {
    return true;
  }
  if (continuation_probability != 1.0f) {
    INTEGRATOR_STATE_WRITE(state, path, throughput) /= continuation_probability;
  }

  return false;
}

#if defined(__AO__)
ccl_device_forceinline void integrate_surface_ao(KernelGlobals kg,
                                                 IntegratorState state,
                                                 const ccl_private ShaderData *ccl_restrict sd,
                                                 const ccl_private RNGState *ccl_restrict
                                                     rng_state)
{
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) &&
      !(path_visibility & PATH_RAY_VISIBILITY_CAMERA))
  {
    return;
  }

  /* Skip AO for paths that were split off for shadow catchers to avoid double-counting. */
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  const float2 rand_bsdf = path_state_rng_2D(kg, rng_state, PRNG_SURFACE_BSDF);

  float3 ao_N;
  const Spectrum ao_weight = surface_shader_ao(
      sd, kernel_data.integrator.ao_additive_factor, &ao_N);

  float3 ao_D;
  float ao_pdf;
  sample_cos_hemisphere(ao_N, rand_bsdf, &ao_D, &ao_pdf);

  bool skip_self = true;

  Ray ray ccl_optional_struct_init;
  ray.P = shadow_ray_offset(kg, sd, ao_D, &skip_self);
  ray.D = ao_D;
  if (skip_self) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }
  ray.tmin = 0.0f;
  ray.tmax = kernel_data.integrator.ao_bounces_distance;
  ray.time = sd->time;
  ray.self.object = (skip_self) ? sd->object : OBJECT_NONE;
  ray.self.prim = (skip_self) ? sd->prim : PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrator_shadow_path_init(
      kg, state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW, true);

#  ifdef __VOLUME__
  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);
#  endif

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(shadow_state, &ray);
  integrator_state_write_shadow_ray_self(shadow_state, &ray);

  /* Copy state from main path to shadow path. */
  const uint16_t bounce = INTEGRATOR_STATE(state, path, bounce);
  const uint16_t transparent_bounce = INTEGRATOR_STATE(state, path, transparent_bounce);
  const uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag) | PATH_RAY_SHADOW_FOR_AO;
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput) * surface_shader_alpha(sd);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_pixel) = INTEGRATOR_STATE(
      state, path, rng_pixel);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
      state, path, visibility);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = transparent_bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, volume_bounds_bounce) = INTEGRATOR_STATE(
      state, path, volume_bounds_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if (kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unshadowed_throughput) = ao_weight;
  }
}
#endif /* defined(__AO__) */


template<uint node_feature_mask>
ccl_device int integrate_surface(KernelGlobals kg,
                                 IntegratorState state,
                                 ccl_global float *ccl_restrict render_buffer, 
                                 ccl_global ReSTIRDIReservoir *restir_reservoirs,
                                 ccl_global const ReSTIRDIReservoir *restir_reservoirs_prev,
                                 const int restir_reservoir_count)

{
  PROFILING_INIT_FOR_SHADER(kg, PROFILING_SHADE_SURFACE_SETUP);

  /* Setup shader data. */
  ShaderData sd;
  integrate_surface_shader_setup(kg, state, &sd);
  PROFILING_SHADER(sd.object, sd.shader);

  int continue_path_label = 0;

  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  /* Skip most work for volume bounding surface. */
#ifdef __VOLUME__
  if (!(sd.flag & SD_HAS_ONLY_VOLUME)) {
#endif
#ifdef __SUBSURFACE__
    /* Can skip shader evaluation for BSSRDF exit point without bump mapping. */
    if (!(path_flag & PATH_RAY_SUBSURFACE) || ((sd.flag & SD_HAS_BSSRDF_BUMP)))
#endif
    {
      /* Evaluate shader. */
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_EVAL);
      surface_shader_eval<node_feature_mask>(
          kg, state, &sd, render_buffer, path_visibility, path_flag);
    }

    if (sd.flag & SD_CACHE_MISS) {
      return LABEL_CACHE_MISS;
    }

    /* After shader evaluation, in case of texture cache miss. */
    guiding_record_surface_segment(kg, state, &sd);

#ifdef __SUBSURFACE__
    if (path_flag & PATH_RAY_SUBSURFACE) {
      /* When coming from inside subsurface scattering, setup a diffuse
       * closure to perform lighting at the exit point. */
      subsurface_shader_data_setup(kg, &sd);
      INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_SUBSURFACE;
    }
    else
#endif
    {
      /* Filter closures. */
      surface_shader_prepare_closures(kg, state, &sd, path_visibility);

      /* Evaluate holdout. */
      if (!integrate_surface_holdout(kg, state, &sd, render_buffer)) {
        return LABEL_NONE;
      }

      /* Write emission. */
      if (sd.flag & SD_EMISSION) {
        integrate_surface_emission(kg, state, &sd, render_buffer);
      }

      /* Perform path termination. Most paths have already been terminated in
       * the intersect_closest kernel, this is just for emission and for dividing
       * throughput by the probability at the right moment.
       *
       * Also ensure we don't do it twice for SSS at both the entry and exit point. */
      if (integrate_surface_terminate(state, path_flag)) {
        return LABEL_NONE;
      }

      /* Write render passes. */
#ifdef __PASSES__
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_PASSES);
      film_write_data_passes(kg, state, &sd, render_buffer);
#endif

#ifdef __DENOISING_FEATURES__
      film_write_denoising_features_surface(kg, state, &sd, render_buffer);
#endif
    }

    /* Load random number state. */
    RNGState rng_state;
    path_state_rng_load(state, &rng_state);

#if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
    if (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING) {
      surface_shader_prepare_guiding(kg, state, &sd, &rng_state);
      guiding_write_debug_passes(kg, state, &sd, render_buffer);
    }
#endif
    /* Direct light. */
    PROFILING_EVENT(PROFILING_SHADE_SURFACE_DIRECT_LIGHT);
    const ShaderEvalResult result = integrate_surface_direct_light<node_feature_mask>(
        kg, state, &sd, &rng_state, restir_reservoirs, restir_reservoirs_prev, restir_reservoir_count, render_buffer);
    if (result == SHADER_EVAL_CACHE_MISS) {
      return LABEL_CACHE_MISS;
    }

#if defined(__AO__)
    /* Ambient occlusion pass. */
    if (kernel_data.kernel_features & KERNEL_FEATURE_AO) {
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_AO);
      integrate_surface_ao(kg, state, &sd, &rng_state);
    }
#endif

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_bsdf_bssrdf_bounce(kg, state, &sd, &rng_state);
#ifdef __VOLUME__
  }
  else {
    if (integrate_surface_terminate(state, path_flag)) {
      return LABEL_NONE;
    }

#  ifdef __DENOISING_FEATURES__
    film_write_denoising_features_surface_volume(kg, state, &sd, render_buffer);
#  endif

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_volume_only_bounce(state, &sd);
  }

  if (continue_path_label & LABEL_TRANSMIT) {
    /* Enter/Exit volume. */
    volume_stack_enter_exit<false>(kg, state, &sd);
  }
#endif

  return continue_path_label;
}

template<DeviceKernel current_kernel>
ccl_device_forceinline void integrator_shade_surface_next_kernel(IntegratorState state)
{
  if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_SUBSURFACE) {
    integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE);
  }
  else {
    kernel_assert(INTEGRATOR_STATE(state, ray, tmax) != 0.0f);
    integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
  }
}

// TODO: integrator_finalize_reservoir; used or not to be used
/**
  mostly a scheduler/dispatch function (The BSDF evaluation has already happened.)
 */
template<uint node_feature_mask = KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE,
         DeviceKernel current_kernel = DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE>
ccl_device_forceinline void integrator_shade_surface(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer,
                                                     ccl_global ReSTIRDIReservoir *restir_reservoirs,
                                                     ccl_global const ReSTIRDIReservoir *restir_reservoirs_prev,
                                                     const int restir_reservoir_count)
{
  const int continue_path_label = integrate_surface<node_feature_mask>(kg, state, render_buffer, 
      restir_reservoirs, restir_reservoirs_prev, restir_reservoir_count); // !!!: Main Actor
  if (continue_path_label == LABEL_CACHE_MISS) {
    integrator_path_cache_miss_sorted(state, current_kernel);
    return;
  }

#ifdef __MNEE__
  /* Cleanup MNEE flag and shadow path if it was not reused for shadow trace. */
  if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
      (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
  {
    INTEGRATOR_STATE_WRITE(state, path, mnee) &= ~PATH_MNEE_SAMPLED;

    const IntegratorShadowState shadow_state = integrator_state_get_mnee_shadow_state(state);
    if (INTEGRATOR_STATE(shadow_state, shadow_path, queued_kernel) ==
        DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING)
    {
      integrator_shadow_path_terminate(shadow_state,
                                       DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING);
    }
  }
#endif

  if (continue_path_label == LABEL_NONE) {
    integrator_path_terminate(kg, state, render_buffer, current_kernel);
    return;
  }

#ifdef __SHADOW_LINKING__
  /* No need to cast shadow linking rays at a transparent bounce: the lights will be accumulated
   * via the main path in this case. BSSRDF bounces continue with intersect_subsurface. */
  if ((continue_path_label & (LABEL_TRANSPARENT | LABEL_SUBSURFACE_SCATTER)) == 0) {
    if (shadow_linking_schedule_intersection_kernel<current_kernel>(kg, state)) {
      return;
    }
  }
#endif

  // TODO(Tri): instead of letting it decide the next bounce immediately, intercept it? Nah
  // NO

  integrator_shade_surface_next_kernel<current_kernel>(state);
}

ccl_device_forceinline void integrator_shade_surface_raytrace(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  integrator_shade_surface<KERNEL_FEATURE_NODE_MASK_SURFACE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE>(
      kg, state, render_buffer, nullptr, nullptr, 0); // TODO(Tri): restir_reservoirs?
}

CCL_NAMESPACE_END
