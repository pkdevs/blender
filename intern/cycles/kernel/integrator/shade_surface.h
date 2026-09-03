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

#include "kernel/geom/motion_triangle.h"
#include "kernel/geom/triangle.h"

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/shadow_linking.h"
#include "kernel/integrator/subsurface.h"
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

// ccl_device_forceinline void integrate_surface_emission(KernelGlobals kg,
//                                                        IntegratorState state,
//                                                        const ccl_private ShaderData *sd,
//                                                        ccl_global float *ccl_restrict
//                                                            render_buffer)
// {
//   const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
//   const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

// #ifdef __LIGHT_LINKING__
//   if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
//       !light_link_object_match(kg, light_link_receiver_forward(kg, state), sd->object))
//   {
//     return;
//   }
// #endif

// #ifdef __SHADOW_LINKING__
//   /* Indirect emission of shadow-linked emissive surfaces is done via shadow rays to dedicated
//    * light sources. */
//   if (kernel_data.kernel_features & KERNEL_FEATURE_SHADOW_LINKING) {
//     if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
//         kernel_data_fetch(objects, sd->object).shadow_set_membership != LIGHT_LINK_MASK_ALL)
//     {
//       return;
//     }
//   }
// #endif

//   /* Evaluate emissive closure. */
//   const Spectrum L = surface_shader_emission(sd);

//   const float mis_weight = light_sample_mis_weight_forward_surface(
//       kg, state, path_visibility, path_flag, sd);

//   guiding_record_surface_emission(kg, state, L, mis_weight);
//   film_write_surface_emission(
//       kg, state, L, mis_weight, render_buffer, object_lightgroup(kg, sd->object));
// }

// TODO(Tri): Debug maybe
ccl_device_forceinline void integrate_surface_emission(KernelGlobals kg,
                                                       IntegratorState state,
                                                       const ccl_private ShaderData *sd,
                                                       const ccl_private RNGState *rng_state,
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

  const float mis_weight = light_sample_mis_weight_forward_surface( // !!!: Wraps forward_pdf, nee_pdf, mis_weight
      kg, state, path_visibility, path_flag, sd);

  guiding_record_surface_emission(kg, state, L, mis_weight);

  if (path_use_restir(kg, state)) {
    /* Evaluate the pdf to sample this light from the previous shading point. */
    LightSample ls ccl_optional_struct_init;
    LightSampleReference ref = light_reference_empty(); // !!!: triangle_light_sample_from_intersection() should fully populate the ref

    triangle_light_sample_from_intersection(kg, state, sd, &ls, &ref);

    /*
     * Weizhen explicitly assigned:
     *   ls.pdf = nee_pdf;
     *
     * Modern Cycles hides this inside light_sample_mis_weight_forward_surface().
     * For ReSTIR reservoir insertion we still need the actual NEE pdf for this
     * surface light sample, so keep this explicit calculation.
     */
    ls.pdf = light_sample_pdf_surface(kg, state, path_visibility, path_flag, sd);

    if (INTEGRATOR_STATE(state, path, bounce) == 1) {
      film_add_reservoir_di(kg, state, L, ls, ref, rng_state, render_buffer);
    }
    else {
      // !!!: Compare against the GI
      // !!!: ReSTIR PT lifecycle must be active, otherwise, dark multipass shadings will occur
      const Spectrum contribution = INTEGRATOR_STATE(state, path, throughput) * L * mis_weight;
      ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
      film_add_reservoir_pt(kg, path_flag, contribution, rng_state, buffer); // ???: GI or basically for emitters?
    }
  }
  else {
    film_write_surface_emission( // ???: GI or basically for emitters? 
        kg, state, L, mis_weight, render_buffer, object_lightgroup(kg, sd->object));
  }
}

/** Evaluate the integrand of the rendering equation in area measure:
 *   BSDF * L * cos_NO * V * G
 *
 * Writes the spectral result to `radiance`.
 * Returns the BSDF pdf for sampling this direction.
 */
ccl_device_forceinline float radiance_eval(KernelGlobals kg,
                                           IntegratorState state,
                                           ccl_private ShaderData *sd,
                                           ccl_private LightSample *ls,
                                           ccl_private BsdfEval *radiance,
                                           ccl_private const LightSampleReference *reference = nullptr, // !!! See if needed here...
                                           const bool check_visibility = false)
{
  if (!(ls->pdf > 0.0f)) {
    bsdf_eval_init(radiance, zero_spectrum());
    return 0.0f;
  }

  ShaderDataCausticsStorage emission_sd_storage;
  ccl_private ShaderData *emission_sd = AS_SHADER_DATA(&emission_sd_storage);

  PROFILING_INIT(kg, PROFILING_RESTIR_LIGHT_EVAL);
  Spectrum light_eval = zero_spectrum();
  const ShaderEvalResult shader_result = light_sample_shader_eval(
      kg, state, emission_sd, ls, sd->time, light_eval, sd, reference, check_visibility);

    // if (shader_result == SHADER_EVAL_CACHE_MISS) {
    //   /* TODO: Propagate or reschedule the cache miss according to the kernel containing
    //    * this ReSTIR candidate evaluation.
    //    */
    //   // return SHADER_EVAL_CACHE_MISS;
    //   return 0.0f; // !!!
    // }
  if (shader_result != SHADER_EVAL_OK) {
    bsdf_eval_init(radiance, zero_spectrum());
    return 0.0f;
  }

  PROFILING_EVENT(PROFILING_RESTIR_BSDF_EVAL);
  const float bsdf_pdf = surface_shader_bsdf_eval(kg, state, sd, ls->D, radiance, ls->shader);

  /* Sun is already sampled in solid angle. */
  float geometry_term = 1.0f;
  if (ls->type != LIGHT_SUN) {
    geometry_term = ls->jacobian_solid_angle_to_area();
  }

  const Spectrum scale = light_eval * geometry_term;
  bsdf_eval_mul(radiance, scale);

  return bsdf_pdf;
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

/* Branch off a shadow path and initialize common part of it.
 * THe common is between the surface shading and configuration of a special shadow ray for the
 * shadow linking. */
ccl_device_inline IntegratorShadowState
integrate_direct_light_shadow_init_common(KernelGlobals kg, // !!!: integrate_direct_light_shadow_init_surface()
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

  // !!!: Not needed since we've got `INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(state, path, visibility);` in integrate_surface_nee()
  // uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);
  // if (light_is_direct_illumination(state)) {
  //   shadow_flag |= PATH_RAY_SURFACE_PASS;
  // }
  // INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;


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


// --- EO SEG 1

template<bool is_direct_light>
ccl_device_inline void integrate_shadow_write_pass_diffuse_glossy(
    KernelGlobals kg,
    ConstIntegratorState state,
    IntegratorShadowState shadow_state,
    const ccl_private BsdfEval *radiance)
{
  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    PackedSpectrum pass_diffuse_weight;
    PackedSpectrum pass_glossy_weight;

    if (is_direct_light) {
      /* Direct light, use BSDFs at this bounce. */
      pass_diffuse_weight = PackedSpectrum(bsdf_eval_pass_diffuse_weight(radiance));
      pass_glossy_weight = PackedSpectrum(bsdf_eval_pass_glossy_weight(radiance));
    }
    else {
      /* Indirect bounce, use weights from earlier surface or volume bounce. */
      pass_diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      pass_glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
    }

    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_diffuse_weight) =
        pass_diffuse_weight;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_glossy_weight) =
        pass_glossy_weight;
  }
}

ccl_device_inline void integrate_write_pass_diffuse_glossy(
    KernelGlobals kg,
    IntegratorState state,
    const ccl_private BsdfEval *radiance)
{
  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    if (INTEGRATOR_STATE(state, path, bounce) == 0) {
      INTEGRATOR_STATE_WRITE(state, path, pass_diffuse_weight) = PackedSpectrum(
          bsdf_eval_pass_diffuse_weight(radiance));
      INTEGRATOR_STATE_WRITE(state, path, pass_glossy_weight) = PackedSpectrum(
          bsdf_eval_pass_glossy_weight(radiance));
    }
  }
}

// !!!: Must preserve the modern MNEE-ray behavior when mnee_vertex_count > 0
/* Create and initialize an ordinary surface direct-light shadow path.
 * In addition to common shadow-state initialization, this helper owns
 * direct-light visibility, flags, pass weights, and transmission volume handling.
 *
 * Specialized callers such as shadow_linking_shade() call the common initializer
 * directly and must initialize their specialized shadow-path fields themselves.
 */
template<bool is_direct_light>
ccl_device_inline void integrate_direct_light_create_shadow_path(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private const RNGState *rng_state,
    ccl_private const ShaderData *sd,
    ccl_private const LightSample *ls,
    ccl_private BsdfEval *radiance,
    const int mnee_vertex_count)
{
  /* Path termination. */
  const float terminate = path_state_rng_light_termination(kg, rng_state);
  if (light_sample_terminate(kg, radiance, terminate)) {
    return;
  }

  Ray ray ccl_optional_struct_init;
  /* Create shadow ray. */
  light_sample_to_surface_shadow_ray(kg, sd, ls, &ray);

  if (ray.self.object != OBJECT_NONE) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }

  /* The light shader has already been evaluated into `radiance`.
   * Not to queue SHADE_LIGHT_NEE; the full emission is already included in the shadow throughput. */
  constexpr bool light_shader_fully_evaluated = true; // !!!: constant_light_shader could be appropriate

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrate_direct_light_shadow_init_common(
      kg,
      state,
      &ray,
      bsdf_eval_sum(radiance),
      ls->group,
      mnee_vertex_count,
      light_shader_fully_evaluated);

  const bool is_transmission = dot(ls->D, sd->N) < 0.0f;
  if (is_transmission) {
#ifdef __VOLUME__
    volume_stack_enter_exit<true>(kg, shadow_state, sd); // !!!: shadow_volume_stack_enter_exit
#endif
  }

  uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);

  if (is_direct_light && (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES))
  {
    shadow_flag |= PATH_RAY_SURFACE_PASS;
  }

  integrate_shadow_write_pass_diffuse_glossy<is_direct_light>(
      kg, state, shadow_state, radiance);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
      state, path, visibility);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;
}




// --- EO SEG 2

// /* Path tracing: sample point on light and evaluate light shader, then
//  * queue shadow ray to be traced. */
// template<uint node_feature_mask>
// #if defined(__KERNEL_GPU__)
// ccl_device_forceinline
// #else
// /* MSVC has very long compilation time (x20) if we force inline this function */
// ccl_device
// #endif
//     ShaderEvalResult
//     integrate_surface_direct_light(KernelGlobals kg,
//                                    IntegratorState state,
//                                    ccl_private ShaderData *sd,
//                                    const ccl_private RNGState *rng_state,
//                                    ccl_global float *ccl_restrict render_buffer)
// {
//   /* Test if there is a light or BSDF that needs direct light. */
//   if (!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL))) {
//     return SHADER_EVAL_EMPTY;
//   }

//   LightSample ls ccl_optional_struct_init;
//   int mnee_vertex_count = 0;  // NOLINT

// #ifdef __MNEE__
//   if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
//       (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
//   {
//     /* MNEE already sampled a light and caustics casters. */
//     integrator_state_read_mnee(state, &ls, &mnee_vertex_count);
//   }
//   else
// #endif
//   {
//     /* Sample position on a light. */
//     const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
//     const uint bounce = INTEGRATOR_STATE(state, path, bounce);
//     const float3 rand_light = path_state_rng_3D(kg, rng_state, PRNG_LIGHT);

//     if (!light_sample_from_position(kg,
//                                     rand_light,
//                                     sd->time,
//                                     sd->P,
//                                     sd->N,
//                                     light_link_receiver_nee(kg, sd),
//                                     sd->flag,
//                                     bounce,
//                                     path_flag,
//                                     &ls))
//     {
//       return SHADER_EVAL_EMPTY;
//     }
//   }

//   kernel_assert(ls.pdf != 0.0f);

//   const bool is_transmission = dot(ls.D, sd->N) < 0.0f;

//   if (ls.prim != PRIM_NONE && ls.prim == sd->prim && ls.object == sd->object) {
//     /* Skip self intersection if light direction lies in the same hemisphere as the geometric
//      * normal. */
//     if (dot(ls.D, is_transmission ? -sd->Ng : sd->Ng) > 0.0f) {
//       return SHADER_EVAL_EMPTY;
//     }
//   }

// #ifdef __MNEE__
//   /* On a caustic caster, a caustic light's contribution is delivered to receivers by
//    * MNEE and does not need to be computed again here. */
//   if (kernel_data.kernel_features & KERNEL_FEATURE_MNEE) {
//     if (mnee_vertex_count == 0 && is_transmission &&
//         (sd->object_flag & SD_OBJECT_CAUSTICS_CASTER) && ls.type != LIGHT_TRIANGLE &&
//         kernel_data_fetch(lights, ls.prim).use_caustics)
//     {
//       return SHADER_EVAL_EMPTY;
//     }
//   }
// #endif

//   /* Evaluate constant part of light shader, rest will optionally be done in another kernel. */
//   Spectrum light_shader_eval ccl_optional_struct_init;
//   const bool is_constant_light_shader = light_sample_shader_eval_nee_constant(
//       kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_shader_eval);

//   /* Evaluate BSDF. */
//   BsdfEval bsdf_eval ccl_optional_struct_init;
//   const float bsdf_pdf = surface_shader_bsdf_eval(kg, state, sd, ls.D, &bsdf_eval, ls.shader);

//   Ray ray ccl_optional_struct_init;

// #ifdef __MNEE__
//   if (mnee_vertex_count > 0) {
//     light_shader_eval *= integrator_state_read_mnee_throughput(state);
//     bsdf_eval_mul(&bsdf_eval, light_shader_eval);

//     if (bsdf_eval_is_zero(&bsdf_eval)) {
//       return SHADER_EVAL_EMPTY;
//     }

//     integrator_state_read_mnee_ray(state, &ls, &ray);
//   }
//   else
// #endif /* __MNEE__ */
//   {
//     const float mis_weight = light_sample_mis_weight_nee(kg, ls.pdf, bsdf_pdf);
//     bsdf_eval_mul(&bsdf_eval, light_shader_eval * ls.eval_fac / ls.pdf * mis_weight);

//     /* Path termination for constant light shader. */
//     if (is_constant_light_shader && !(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
//       const float terminate = path_state_rng_light_termination(kg, rng_state);
//       if (light_sample_terminate(kg, &bsdf_eval, terminate)) {
//         return SHADER_EVAL_EMPTY;
//       }
//     }
//     /* For non-constant light shader, probabilistic termination happens in
//      * SHADE_LIGHT_NEE when the full contribution is known. */
//     else if (bsdf_eval_is_zero(&bsdf_eval)) {
//       return SHADER_EVAL_EMPTY;
//     }

//     /* Create shadow ray. */
//     light_sample_to_surface_shadow_ray(kg, sd, &ls, &ray); // !!!
//   }

//   if (ray.self.object != OBJECT_NONE) {
//     ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
//   }

//   /* Branch off shadow kernel. */
//   IntegratorShadowState shadow_state = integrate_direct_light_shadow_init_common(
//       kg,
//       state,
//       &ray,
//       bsdf_eval_sum(&bsdf_eval),
//       ls.group,
//       mnee_vertex_count,
//       is_constant_light_shader);

//   if (is_transmission) {
// #ifdef __VOLUME__
//     volume_stack_enter_exit<true>(kg, shadow_state, sd);
// #endif
//   }

//   uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);

//   if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
//     // ???: Why is modern pass-weight separated into only 2 passes of glossy and diffuse?
//     PackedSpectrum pass_diffuse_weight;
//     PackedSpectrum pass_glossy_weight;

//     if (shadow_flag & PATH_RAY_ANY_PASS) {
//       /* Indirect bounce, use weights from earlier surface or volume bounce. */
//       pass_diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
//       pass_glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
//     }
//     else {
//       /* Direct light, use BSDFs at this bounce. */
//       shadow_flag |= PATH_RAY_SURFACE_PASS;
//       pass_diffuse_weight = PackedSpectrum(bsdf_eval_pass_diffuse_weight(&bsdf_eval)); // !!!: See if changes are needed here
//       pass_glossy_weight = PackedSpectrum(bsdf_eval_pass_glossy_weight(&bsdf_eval)); // !!!
//     }

//     INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_diffuse_weight) = pass_diffuse_weight;
//     INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_glossy_weight) = pass_glossy_weight;
//   }

//   INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
//       state, path, visibility); // !!!
//   INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag; // !!!

//   return SHADER_EVAL_OK;
// }


/* ReSTIR DI epilogue: the winning candidate is written to the persistent
 * reservoir and its shadow ray is deferred to a later kernel, where it will
 * be temporally/spatially resampled before being traced. */
ccl_device_forceinline ShaderEvalResult integrate_surface_nee_write_restir_di(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private Reservoir *reservoir,
    ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);
// !!!: to write the fresh initial-candidates reservoir, unconditionally clobbering whichever slot is "current" for this iteration parity.
// !!!: Nothing about this write path checks or preserves what was there from the previous sample
// !!!: not designed to survive across samples, only across spatial iterations within one sample.
  /* Write to reservoir and trace shadow ray later. */
  film_write_pass_reservoir(kg, state, reservoir, render_buffer);
  return SHADER_EVAL_OK;
}

/* Ordinary NEE epilogue: used for every bounce that is not the ReSTIR-DI
 * primary connection — i.e. Indirect Ullumination, or Direct Illumination
 * when ReSTIR is disabled entirely. RIS over the sampled candidates is still
 * used to pick a good light, but the reservoir is finalized and shaded
 * immediately: no persistent state, no deferred shadow ray, no reuse. */
ccl_device_forceinline ShaderEvalResult integrate_surface_nee_shade_indirect(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    ccl_private const RNGState *rng_state,
    const bool use_restir,
    ccl_private Reservoir *reservoir)
{
  reservoir_finalize(reservoir, use_restir);
  bsdf_eval_mul(&reservoir->radiance, reservoir->total_weight);

  // if (bsdf_eval_is_zero(&reservoir->radiance)) {
  //   return SHADER_EVAL_EMPTY;
  // }

  int mnee_vertex_count = 0;
  integrate_direct_light_create_shadow_path<false>(kg,
                                                   state,
                                                   rng_state,
                                                   sd,
                                                   &reservoir->light,
                                                   &reservoir->radiance,
                                                   mnee_vertex_count);
  return SHADER_EVAL_OK;
}

/*
 * existing MNEE sample:
    retain MNEE evaluation and shadow setup

 * ordinary NEE:
    generate Reservoir candidates
    create a LightSampleReference for every candidate
    evaluate through radiance_eval()
    add candidate to Reservoir

   * direct ReSTIR DI:
        write Reservoir for later resampling/shadowing

   * non-ReSTIR:
        finalize Reservoir
        apply total_weight
        call integrate_direct_light_create_shadow_path()

 * Next Event Estimation: sample points on lights and evaluate their
 * contributions via Resampled Importance Sampling. At the primary
 * (bounce == 1) connection with ReSTIR enabled, the winning candidate is
 * stored as a persistent ReSTIR DI reservoir for later resampling. At every
 * other bounce, RIS is used purely as an NEE quality improvement and the
 * result is shaded immediately.
 */
template<uint node_feature_mask>
#if defined(__KERNEL_GPU__)
ccl_device_forceinline
#else
/* MSVC has very long compilation time (x20) if we force inline this function. */
ccl_device
#endif
    ShaderEvalResult
    integrate_surface_nee(KernelGlobals kg,
                          IntegratorState state,
                          ccl_private ShaderData *sd,
                          ccl_private const RNGState *rng_state,
                          ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_INITIAL_RESAMPLING);

  /* Test if there is a light or BSDF that needs direct light. */
  if (!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL))) {
    return SHADER_EVAL_EMPTY;
  }

  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const uint bounce = INTEGRATOR_STATE(state, path, bounce);

  /* Use Resampled Importance Sampling for direct illumination based on Talbot, Justin F.
   * Importance resampling for global illumination. Brigham Young University, 2005. */
  /* TODO(weizhen): add MNEE back? */
  const bool is_direct_light = light_is_direct_illumination(state);
  const bool use_restir = kernel_data.integrator.use_restir;

  const bool use_restir_di = is_direct_light && use_restir;

// TODO(Tri): Handle MNEE
// #if 0
#  ifdef __MNEE__
  if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
      (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
  {
    LightSample ls ccl_optional_struct_init;
    int mnee_vertex_count = 0; // NOLINT

    integrator_state_read_mnee(state, &ls, &mnee_vertex_count);

    kernel_assert(ls.pdf != 0.0f);

    const bool is_transmission = dot(ls.D, sd->N) < 0.0f;

    if (ls.prim != PRIM_NONE && ls.prim == sd->prim && ls.object == sd->object) {
      if (dot(ls.D, is_transmission ? -sd->Ng : sd->Ng) > 0.0f) {
        return SHADER_EVAL_EMPTY;
      }
    }

    Spectrum light_shader_eval ccl_optional_struct_init;
    /* Evaluate constant part of light shader, rest will optionally be done in another kernel. */
    light_sample_shader_eval_nee_constant(
        kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_shader_eval);

    BsdfEval radiance ccl_optional_struct_init;
    surface_shader_bsdf_eval(kg, state, sd, ls.D, &radiance, ls.shader);

    light_shader_eval *= integrator_state_read_mnee_throughput(state);
    bsdf_eval_mul(&radiance, light_shader_eval);

    if (bsdf_eval_is_zero(&radiance)) {
      return SHADER_EVAL_EMPTY;
    }

    /*
     * ReSTIR DI does not replace an already-constructed MNEE path.
     * The helper reads the MNEE ray when mnee_vertex_count is nonzero.
     */
    integrate_direct_light_create_shadow_path<false>(
        kg, state, rng_state, sd, &ls, &radiance, mnee_vertex_count);

    return SHADER_EVAL_OK;
  }
#  endif
// #endif

  /* Sample position on a light. */
  Reservoir reservoir;
  reservoir_init(kg, &reservoir, use_restir_di);

  const int max_samples = reservoir.max_samples;

  for (int i = 0; i < reservoir.num_light_samples; i++) {
    PROFILING_EVENT(PROFILING_RESTIR_LIGHT_RESAMPLING);

    LightSample ls ccl_optional_struct_init;
    BsdfEval radiance ccl_optional_struct_init;

    /* TODO(weizhen): does this result in correlated samples in ReSTIR PT? */
    const float3 rand_light = path_branched_rng_3D( // !!!
        kg, rng_state, i, reservoir.num_light_samples, PRNG_LIGHT);

    const float rand_pick = use_restir ?
                                path_branched_rng_1D(kg, rng_state, i, max_samples, PRNG_PICK) :
                                0.0f;

    /* TODO(weizhen): use higher-level nodes in light tree, or use light tile as in the ReSTIR PT
     * paper. */
    if (!light_sample_from_position(kg,
                                    rand_light,
                                    sd->time,
                                    sd->P,
                                    sd->N,
                                    light_link_receiver_nee(kg, sd),
                                    sd->flag,
                                    bounce,
                                    path_flag,
                                    &ls))
    {
      continue;
    }

    kernel_assert(ls.pdf != 0.0f);

    const bool is_transmission = dot(ls.D, sd->N) < 0.0f;

    if (ls.prim != PRIM_NONE && ls.prim == sd->prim && ls.object == sd->object) {
      /* Skip self intersection if light direction lies in the same hemisphere as the geometric
       * normal. */
      if (dot(ls.D, is_transmission ? -sd->Ng : sd->Ng) > 0.0f) {
        continue;
      }
    }

// #if 0
#  ifdef __MNEE__
    /*
     * Temporarily disabled for tests
     *
     * Contributions from caustic lights on a caustic caster were excluded
     * from ordinary NEE by this block. The Old PR did not include this
     * rejection.
     */
    if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) && is_transmission &&
        (sd->object_flag & SD_OBJECT_CAUSTICS_CASTER) && ls.type != LIGHT_TRIANGLE &&
        kernel_data_fetch(lights, ls.prim).use_caustics)
    {
      continue;
    }
#  endif
// #endif

    LightSampleReference reference;
    reference = light_reference_empty();

    // TODO(Tri): Fix the comments!
    if (use_restir) {
      /* Construct the reference from the light sample in ordinary NEE. */
      // !!!: One contributor of the darkness in DI (not all types are supported; current support might have issues)
      // !!!: light_reference_from_sample() may have problem populating reference
      if (!light_reference_from_sample(kg, &ls, sd->time, &reference)) {      
        printf("integrate_surface_nee() returned!; !restir_light_reference_from_sample()=%u\n\n\n\n",
               (!light_reference_from_sample(kg, &ls, sd->time, &reference)));
        continue;
        // break; // !!!: Break or continue?
      }
    }

    // !!!(Tri): Crashes when ReSTIR's off for surface_emission
    // kernel_assert(light_reference_is_valid(&reference));

    /* Evaluate light shader. */
    const bool check_visibility = use_restir && kernel_data.integrator.restir_unbiased; // !!!

    /* Evaluates light emission, BSDF response, and the geometry conversion. */
    const float bsdf_pdf = radiance_eval( // !!!:
        kg, state, sd, &ls, &radiance, &reference, check_visibility);

    PROFILING_EVENT(PROFILING_RESTIR_RESERVOIR);


// printf(
//     "\n=== BEFORE RESERVOIR ADD ===\n"
//     "radiance.sum = (%e, %e, %e)\n"
//     "rgb_to_y     = (%e, %e, %e)\n"
//     // "luminance    = %e\n"
//     "eval_fac     = %e\n"
//     "ls.pdf       = %e\n"
//     "============================\n",
//     (double)radiance.sum.x,
//     (double)radiance.sum.y,
//     (double)radiance.sum.z,
//     (double)reservoir.rgb_to_y.x,
//     (double)reservoir.rgb_to_y.y,
//     (double)reservoir.rgb_to_y.z,
//     // (double)reservoir_compute_luminance(reservoir, radiance),
//     (double)ls.eval_fac,
//     (double)ls.pdf);

// TODO(Tri): Seperate ReSTIR path properly!
    /* Compute the NEE MIS weight and divide by the light PDF in the appropriate measure. */
    reservoir_add_light_sample(&reservoir, ls, reference, radiance, bsdf_pdf, rand_pick, use_restir); // !!!
  }

  if (reservoir_is_empty(&reservoir)) {
    return SHADER_EVAL_EMPTY;
  }

  /* TODO(weizhen): write reconnection vertex. Treat as rough if at least one closure is rough. */

  /*
   * The selected light and reference remain paired inside Reservoir.
   * Persistent ReSTIR DI paths store the reservoir and trace it later;
   * everything else is ordinary NEE, shaded immediately.
   */
  if (use_restir_di) {
    return integrate_surface_nee_write_restir_di(kg, state, &reservoir, render_buffer);
  }
  else {
    return integrate_surface_nee_shade_indirect(kg, state, sd, rng_state, use_restir, &reservoir);
  }
}

/* Path tracing: bounce off or through surface with new direction. */
ccl_device_forceinline int integrate_surface_bsdf_bssrdf_bounce(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    const ccl_private RNGState *rng_state, 
    ccl_global float *ccl_restrict render_buffer)
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

  integrate_write_pass_diffuse_glossy(kg, state, &bsdf_eval);

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

  film_write_reconnection_vertex(kg, state, bsdf_sampled_roughness, sd->ray_length, render_buffer);

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
      state, path, visibility); // !!!
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag; // !!!
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
                                 ccl_global float *ccl_restrict render_buffer)

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

    /* Load random number state. */
    RNGState rng_state;
    path_state_rng_load(state, &rng_state);

#ifdef __SUBSURFACE__
    if (path_flag & PATH_RAY_SUBSURFACE) {
      /* When coming from inside subsurface scattering, setup a diffuse
       * closure to perform lighting at the exit point. */
      subsurface_shader_data_setup(kg, &sd); // !!!
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
        integrate_surface_emission(kg, state, &sd, &rng_state, render_buffer);
        // integrate_surface_emission(kg, state, &sd, render_buffer); // !!!: OG

// TODO(Tri): Remove
        // /* TODO(weizhen): `min_bounce` is only for debugging, revert after finishing the branch. */
        // if (INTEGRATOR_STATE(state, path, bounce) >= kernel_data.integrator.min_bounce) {
        //   integrate_surface_emission(kg, state, &sd, &rng_state, render_buffer);
        // }
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

#if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
    if (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING) {
      surface_shader_prepare_guiding(kg, state, &sd, &rng_state);
      guiding_write_debug_passes(kg, state, &sd, render_buffer);
    }
#endif
    /* Direct light. */
    PROFILING_EVENT(PROFILING_SHADE_SURFACE_DIRECT_LIGHT);
    // !!!: reference for triangle emitters is populated in integrate_surface_emission()
    const ShaderEvalResult result = integrate_surface_nee<node_feature_mask>(
        kg, state, &sd, &rng_state, render_buffer);

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
    continue_path_label = integrate_surface_bsdf_bssrdf_bounce(
        kg, state, &sd, &rng_state, render_buffer);
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
    volume_stack_enter_exit<false>(kg, state, &sd); // !!!
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

template<uint node_feature_mask = KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE,
         DeviceKernel current_kernel = DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE>
ccl_device_forceinline void integrator_shade_surface(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer)
{
  const int continue_path_label = integrate_surface<node_feature_mask>(kg, state, render_buffer);
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
    integrator_finalize_reservoir(kg, state, render_buffer);
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

  integrator_shade_surface_next_kernel<current_kernel>(state);
}


ccl_device_forceinline void integrator_shade_surface_raytrace(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  integrator_shade_surface<KERNEL_FEATURE_NODE_MASK_SURFACE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE>(
      kg, state, render_buffer);
}

CCL_NAMESPACE_END
