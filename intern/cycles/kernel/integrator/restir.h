/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/reservoir.h"
#include "kernel/integrator/state.h"

CCL_NAMESPACE_BEGIN

struct SpatialResampling {
  ccl_global const KernelWorkTile *tile;
  int radius;

  SpatialResampling(ccl_global const KernelWorkTile *ccl_restrict tile, const int radius)
  {
    this->tile = tile;
    this->radius = radius;
  }

  uint get_render_pixel_index(const int x, const int y) const
  {
    return (uint)tile->offset + x + y * tile->stride;
  }

  /* Get neighbors in a square. */
  uint3 get_neighbor(const float2 rand) const
  {
    const float2 offset = floor(rand * radius + 0.5f);

    const int x = tile->x + (int)offset.x;
    const int y = tile->y + (int)offset.y;

    if (x < tile->min_x || x > tile->max_x || y < tile->min_y || y > tile->max_y) {
      /* TODO(weizhen): this is wasting samples, but can we do better? For example overscan? */
      /* Out of bound. */
      return make_uint3(x, y, -1);
    }

    return make_uint3(x, y, get_render_pixel_index(x, y));
  }

  static bool is_valid_neighbor(const ccl_private ShaderData *sd,
                                const ccl_private ShaderData *neighbor_sd)
  {
    /* TODO(weizhen): find a good criterion, for example the distance to the normal plane of sd.
     * Also check condition in `SpatialReuse.cs.slang`. */
    return (sd->object == neighbor_sd->object) && (sd->type == neighbor_sd->type);
  }
};

/* Reconstruct the minimal ShaderData state required to rebuild the
 * complete surface using shader_setup_from_restir().
 *
 * Fields such as P, N, Ng, shader IDs, object flags and differential
 * geometry are intentionally regenerated from (object, prim, u, v)
 * rather than stored in the render buffer in `shader_setup_from_restir()`.
 */
/* TODO(weizhen): better to pack in another pass. */
ccl_device_inline bool integrator_restir_unpack_shader(ccl_private ShaderData *sd, 
                                                       ccl_private uint32_t *path_flag,
                                                       const ccl_global float *buffer)
{
  int i = 0;

  *path_flag = (uint32_t)buffer[i++];

  /* TODO(weizhen): compress the data. */
  sd->u = buffer[i++];
  sd->v = buffer[i++];
  sd->type = (uint)buffer[i++]; // !!!
  sd->object = (int)buffer[i++];
  sd->prim = (int)buffer[i++];
  /* TODO(weizhen): only Huang Hair BSDF needs LCG now. Can we find a way to remove the usage? */
  /* TODO(weizhen): check if we can compute `lcg_state` after we support curve. */
  sd->lcg_state = (uint)buffer[i];

  /* TODO(weizhen): do I need to write to this? */
  // sd->closure_transparent_extinction;

  // printf("\nintegrator_restir_unpack_shader():\n"
  //        "u=%f v=%f type=%d object=%d prim=%d lcg_state=%d\n\n",
  //        (double)sd->u, (double)sd->v, sd->type, sd->object, sd->prim, sd->lcg_state);

  /* Whether there is valid interesction at the current shading point. */
  // TODO(Tri):
  /* TODO(weizhen): revisit this condition when we support background and distant lights. */ // !!!
  return sd->type; // !!!: a 0 type can mean that there was no valid surface intersection stored for this pixel.
}

ccl_device_forceinline ccl_global float *pixel_render_buffer(
    KernelGlobals kg, uint32_t render_pixel_index, ccl_global float *ccl_restrict render_buffer)
{
  const uint64_t render_buffer_offset = (uint64_t)render_pixel_index *
                                        kernel_data.film.pass_stride;
  return render_buffer + render_buffer_offset;
}

ccl_device_inline bool integrator_restir_unpack_shader(KernelGlobals kg,
                                                       ccl_private SpatialReservoir *reservoir,
                                                       uint32_t render_pixel_index,
                                                       ccl_global float *ccl_restrict
                                                           render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);
  ccl_global const float *buffer = pixel_render_buffer(kg, render_pixel_index, render_buffer) +
                                   kernel_data.film.pass_surface_data;
  return integrator_restir_unpack_shader(&reservoir->sd, &reservoir->path_flag, buffer);
}

ccl_device_inline bool integrator_restir_unpack_shader(KernelGlobals kg,
                                                       ccl_private ShaderData *sd,
                                                       uint32_t render_pixel_index,
                                                       ccl_global float *ccl_restrict
                                                           render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);
  ccl_global const float *buffer = pixel_render_buffer(kg, render_pixel_index, render_buffer) +
                                   kernel_data.film.pass_surface_data;

  /* TODO(weizhen): maybe `path_flag` does not belong in this pass. */
  uint32_t discard;
  return integrator_restir_unpack_shader(sd, &discard, buffer);
}

ccl_device_inline bool integrator_restir_unpack_reservoir( // !!!: DI
    KernelGlobals kg,
    ccl_private Reservoir *reservoir,
    uint32_t render_pixel_index,
    ccl_global float *ccl_restrict render_buffer,
    const bool read_prev)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

  ccl_global const float *buffer =
      pixel_render_buffer(kg, render_pixel_index, render_buffer) +
      (read_prev ? kernel_data.film.pass_restir_previous_reservoir :
                   kernel_data.film.pass_restir_reservoir);

  return restir_unpack_reservoir(kg, reservoir, buffer);
}

// TODO(Tri): Complete
ccl_device_inline bool integrator_restir_unpack_reservoir(KernelGlobals kg, // !!!: GI
                                                          ccl_private GlobalReservoir *reservoir,
                                                          uint32_t render_pixel_index,
                                                          ccl_global float *ccl_restrict
                                                              render_buffer,
                                                          const bool read_prev)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);
  ccl_global float *buffer = pixel_render_buffer(kg, render_pixel_index, render_buffer) +
                             (read_prev ? kernel_data.film.pass_restir_pt_previous_reservoir :
                                          kernel_data.film.pass_restir_pt_reservoir);

printf("integrator_restir_unpack_reservoir() GI called restir_unpack_reservoir_pt()\n");
// Write is correct in film_add_reservoir_pt(); This is to ensure the read!
printf("PT READ pixel=%u read_prev=%d ptr=%p weight=%f\n",
       render_pixel_index,
       (int)read_prev,
       buffer,
       (double)buffer[0]);

  return restir_unpack_reservoir_pt(kg, reservoir, buffer);
}

ccl_device_inline void ray_setup(KernelGlobals kg,
                                 IntegratorState state,
                                 ccl_private Ray *ccl_restrict ray,
                                 const uint3 render_pixel_index)
{
  if (render_pixel_index.z == UINT_MAX) {
    integrator_state_read_ray(state, ray);
  }
  else {
    /* Setup neighboring ray from the camera. */
    const int x = render_pixel_index.x;
    const int y = render_pixel_index.y;

    /* TODO(weizhen): for adaptive sampling maybe the sample is not the same? */
    const int sample = INTEGRATOR_STATE(state, path, sample);
    const uint rng_pixel = path_rng_pixel_init(kg, sample, x, y);

    // !!!: cache_miss
    int cache_miss = 0;
    Spectrum T = integrate_camera_sample(kg, sample, x, y, rng_pixel, ray, cache_miss);
    if (cache_miss) {
      // !!!
    }
  }
}

ccl_device_forceinline void shader_data_setup_from_restir(KernelGlobals kg,
                                                          IntegratorState state,
                                                          ccl_private SpatialReservoir *reservoir,
                                                          ccl_global float *ccl_restrict
                                                              render_buffer)
{
  Ray ray;
  ray_setup(kg, state, &ray, reservoir->pixel_index);

  PROFILING_INIT(kg, PROFILING_RESTIR_SURFACE_DATA_SETUP);
  shader_setup_from_restir(kg, &ray, &reservoir->sd);

  PROFILING_EVENT(PROFILING_RESTIR_SHADER_SETUP);

  // !!!: PathRayVisibility path_visibility
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);

  /* TODO(weizhen): what features are needed here? Is this the right state? */
  surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>( // !!!: The code path that pulls in OSL evaluation.
      kg, state, &reservoir->sd, render_buffer, path_visibility, reservoir->path_flag);
  /* TODO(weizhen): do we call `surface_shader_prepare_closures()` here? */
}

ccl_device_inline bool restir_unpack_neighbor(KernelGlobals kg,
                                              const ccl_private SpatialResampling *resampling,
                                              ccl_private SpatialReservoir *current,
                                              ccl_private SpatialReservoir *neighbor,
                                              const int id,
                                              const float3 rand,
                                              ccl_global float *ccl_restrict render_buffer,
                                              const bool read_prev)
{
  /* TODO(weizhen): probably no need to put the current sample anymore for pairwise. */
  /* Always put the current sample in the reservoir. */
  // const float2 rand_index = (id == 0) ? zero_float2() : float3_to_float2(rand) * 2.0f - 1.0f; // !!!: undeclared id float3_to_float2
  const float2 rand_index = (id == 0) ? zero_float2() : float2(rand.x, rand.y) * 2.0f - 1.0f;
  const uint3 pixel_index = resampling->get_neighbor(rand_index);

  if (pixel_index.z == -1) {
    return false;
  }

  neighbor->pixel_index = pixel_index;
  if (integrator_restir_unpack_shader(kg, neighbor, pixel_index.z, render_buffer)) {
    integrator_restir_unpack_reservoir(
        kg, &neighbor->reservoir, pixel_index.z, render_buffer, read_prev);
    return resampling->is_valid_neighbor(&current->sd, &neighbor->sd);
  }

  return false;
}

ccl_device_inline bool restir_unpack_neighbor(KernelGlobals kg, // !!!: GI
                                              const ccl_private SpatialResampling *resampling,
                                              ccl_private GlobalReservoir *current,
                                              ccl_private GlobalReservoir *neighbor,
                                              const int id,
                                              const float3 rand,
                                              ccl_global float *ccl_restrict render_buffer,
                                              const bool read_prev)
{
  /* TODO(weizhen): probably not needed anymore for pairwise. */
  /* Always put the current sample in the reservoir. */
  // const float2 rand_index = (id == 0) ? zero_float2() : float3_to_float2(rand) * 2.0f - 1.0f; // !!!: float3_to_float2
  const float2 rand_index = (id == 0) ? zero_float2() : float2(rand.x, rand.y) * 2.0f - 1.0f;
  /* !!!(weizhen): this is the same neighboring pixel as GI. */
  const uint3 pixel_index = resampling->get_neighbor(rand_index);

  if (pixel_index.z == -1) {
    return false;
  }

  if (integrator_restir_unpack_shader(kg, &neighbor->sd, pixel_index.z, render_buffer)) {
    integrator_restir_unpack_reservoir(kg, neighbor, pixel_index.z, render_buffer, read_prev);
    return resampling->is_valid_neighbor(&current->sd, &neighbor->sd);
  }

  return false;
}

/* TODO(weizhen): move these functions to a more appropriate place. */
float mis_weight_pairwise(float pdf_a, float pdf_b, float num)
{
  return (pdf_b == 0.0f) ? 0.0f : (pdf_a * pdf_a * num) / (pdf_a * pdf_a * num + pdf_b * pdf_b);
}

float compute_luminance(KernelGlobals kg, const ccl_private BsdfEval &radiance)
{
  return dot(spectrum_to_rgb(radiance.sum), float4_to_float3(kernel_data.film.rgb_to_y));
}

/* Unpack the canonical (this-pixel, non-neighbor) DI reservoir. Shared by
 * streaming_di_samples() and integrator_restir_temporal() — both need the
 * same "read whatever initial candidates or prior-stage result currently
 * sits in this pixel's slot" operation, just at different pipeline stages. */
ccl_device_inline Reservoir restir_unpack_canonical_reservoir(KernelGlobals kg,
                                                              IntegratorState state,
                                                              ccl_private const SpatialReservoir *current,
                                                              ccl_global float *ccl_restrict render_buffer,
                                                              const bool read_prev)
{
  Reservoir canonical;
  reservoir_init(kg, &canonical);

  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  if (integrator_restir_unpack_reservoir(kg, &canonical, render_pixel_index, render_buffer, read_prev)) {
    light_sample_from_reference(kg,
                                &canonical.reference,
                                current->sd.P,
                                current->sd.N,
                                current->sd.time,
                                current->sd.shader,
                                current->path_flag,
                                &canonical.light);
  }

  return canonical;
}

/* Add neighboring reservoirs to the current reservoir using pairewise MIS weight. */
ccl_device_inline bool streaming_di_samples(KernelGlobals kg,
                                            IntegratorState state,
                                            const ccl_private SpatialResampling *resampling,
                                            ccl_private SpatialReservoir *current,
                                            const ccl_private RNGState *rng_state,
                                            const int samples,
                                            const bool visibility,
                                            ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);

  /* Ping-pong between two reservoirs. */
  const bool read_prev = INTEGRATOR_STATE(state, path, spatial_iteration) % 2;

  Reservoir canonical = restir_unpack_canonical_reservoir(
      kg, state, current, render_buffer, read_prev);

  /* Give the canonical sample a defensive constant in the weight. */
  float canonical_mis_weight = 1.0f;
  LightSample canonical_ls = canonical.light;
  int valid_neighbors = 1;
  const int neighbors = samples - 1;

  for (int i = 1; i < samples; i++) {
    const float3 rand = path_branched_rng_3D(
        kg,
        rng_state,
        INTEGRATOR_STATE(state, path, spatial_iteration) * samples + i,
        kernel_data.integrator.restir_spatial_iterations * samples,
        PRNG_SPATIAL_RESAMPLING);

    SpatialReservoir neighbor;
    spatial_reservoir_init(kg, &neighbor, true); // !!!: require_valid_reference

    if (!restir_unpack_neighbor(
            kg, resampling, current, &neighbor, i, rand, render_buffer, read_prev))
    {
      continue;
    }

    valid_neighbors++;
    shader_data_setup_from_restir(kg, state, &neighbor, render_buffer);

    /* Calculate canonical MIS weight. */
    if (!reservoir_is_empty(&canonical)) {
      /* TODO(weizhen): verify if branching has influence on performance on GPU. This branching and
       * the `neighbor.is_empty()` check is unnecessary, because if `total_weight` is zero then the
       * sample would not picked by the reservoir regardless of the `mis_weight`. */
      BsdfEval radiance;

      /* Evaluate current sample from the neighbor shading point. */
      // light_sample_from_uv(kg, &neighbor.sd, neighbor.path_flag, &canonical_ls); // !!!
      light_sample_from_reference( // !!!: Re-Constructs DI analytic lights only
          kg,
          &canonical.reference,
          neighbor.sd.P,
          neighbor.sd.N,
          neighbor.sd.time,
          neighbor.sd.shader,
          neighbor.path_flag,
          &canonical_ls);

      radiance_eval(kg, state, &neighbor.sd, &canonical_ls, &radiance, nullptr, visibility);

      const float canonical_at_neighbor = compute_luminance(kg, radiance);

      canonical_mis_weight += (1.0f - mis_weight_pairwise(
                                          canonical_at_neighbor, canonical.luminance, neighbors));
    }

    if (spatial_reservoir_is_empty(&neighbor)) {
      continue;
    }

    // light_sample_from_uv(kg, &neighbor.sd, neighbor.path_flag, &neighbor.reservoir.ls); // !!!
    light_sample_from_reference(kg, // !!!: Re-Constructs DI analytic lights only
                                &neighbor.reservoir.reference,
                                neighbor.sd.P,
                                neighbor.sd.N,
                                neighbor.sd.time,
                                neighbor.sd.shader,
                                neighbor.path_flag,
                                &neighbor.reservoir.light);

    /* Jacobian. */
    const bool copy_direction = sample_copy_direction(kg, neighbor.reservoir);
    if (copy_direction) { // !!!
      neighbor.reservoir.total_weight *= neighbor.reservoir.light.jacobian_area_to_solid_angle();
    }

    /* Evaluate neighbor sample from the current shading point. */
    // light_sample_from_uv(kg, &current->sd, current->path_flag, &neighbor.reservoir.light); // !!!
    light_sample_from_reference( // !!!: Re-Constructs DI analytic lights only
        kg,
        &neighbor.reservoir.reference,
        current->sd.P,
        current->sd.N,
        current->sd.time,
        current->sd.shader,
        current->path_flag,
        &neighbor.reservoir.light);

    radiance_eval(
        kg, state, &current->sd, &neighbor.reservoir.light, &neighbor.reservoir.radiance, nullptr, visibility);
    const float neighbor_at_canonical = compute_luminance(kg, neighbor.reservoir.radiance);

    if (copy_direction) { // !!!
      neighbor.reservoir.total_weight *= neighbor.reservoir.light.jacobian_solid_angle_to_area();
    }

    /* TODO(weizhen): either the MIS weight computation should be a member function of
     * SpatialReservoir, or the struct SpatialReservoir should be removed. */
    neighbor.reservoir.total_weight *= mis_weight_pairwise(
        neighbor.reservoir.luminance, neighbor_at_canonical, neighbors);

    spatial_reservoir_add_reservoir(current, neighbor, rand.z, true); // !!!: require_valid_reference
  }

  if (!reservoir_is_empty(&canonical)) {
    /* Add canonical sample to the reservoir. */
    radiance_eval(kg, state, &current->sd, &canonical.light, &canonical.radiance, nullptr, visibility);
    canonical.total_weight *= canonical_mis_weight;
    const float rand = path_branched_rng_3D(
        kg,
        rng_state,
        INTEGRATOR_STATE(state, path, spatial_iteration) ,
        kernel_data.integrator.restir_spatial_iterations * samples,
        PRNG_SPATIAL_RESAMPLING).z;

    // current->reservoir.add_reservoir(canonical, rand);
    spatial_reservoir_add_reservoir(current, canonical, rand, true); // !!!: require_valid_reference
  }

  current->reservoir.total_weight /= valid_neighbors;

  return reservoir_finalize(&current->reservoir, true); // !!!: require_valid_reference
}

/* Temporal counterpart of streaming_di_samples()'s per-neighbor step.
 * v1: identity reprojection only (same pixel, same render sample count as previous). 
 * Real motion-vector reprojection for animated/interactive cases is deferred. */
// !!!: SpatialResampling::get_neighbor() is needed for reporjection once it lands
// SpatialResampling-style is_valid_neighbor check will need a depth/normal-threshold analog for temporal once reprojection is inexact...
ccl_device_inline bool streaming_temporal_sample(KernelGlobals kg,
                                                 IntegratorState state,
                                                 ccl_private SpatialReservoir *current,
                                                 const ccl_private RNGState *rng_state,
                                                 const bool visibility,
                                                 ccl_global float *ccl_restrict render_buffer)
{
  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  Reservoir history;
  reservoir_init(kg, &history);

  ccl_global const float *buffer = pixel_render_buffer(kg, render_pixel_index, render_buffer) +
                                   kernel_data.film.pass_restir_temporal_history_reservoir;

  if (!restir_unpack_reservoir(kg, &history, buffer)) { // !!!: Always failing here 
    printf("streaming_temporal_sample() RETURNED EARLY! can't unpack history\n\n\n");
    return false; /* No valid history yet (first sample, or history invalidated). */
  }

  /* Cap history so a lighting change or moving object doesn't get stuck
   * behind decades of stale confidence. */
  history.M = min(history.M, current->reservoir.M * kernel_data.integrator.restir_temporal_history_limit); // restir_temporal_max_history

  light_sample_from_reference(kg,
                              &history.reference,
                              current->sd.P,
                              current->sd.N,
                              current->sd.time,
                              current->sd.shader,
                              current->path_flag,
                              &history.light);

  // !!!(Tri):
  radiance_eval(kg, state, &current->sd, &history.light, &history.radiance, nullptr, visibility);
  const float history_at_current = compute_luminance(kg, history.radiance);

  /* TODO(Tri): Unify spatial + temporal into one combined-M pairwise estimator
     Right now, it's separate from the spatial loop's constant-neighbors weight. */
  /* Reuse the existing pairwise weighting, but with the *true* persisted M
   * of the history reservoir standing in for the constant "neighbors" count
   * used by the spatial loop; this is the one place temporal and spatial
   * pairwise weighting genuinely differ; unifying them into a single
   * combined-M estimator is a follow-up once this is validated in isolation. */
  history.total_weight *= mis_weight_pairwise(history.luminance, history_at_current, (float)history.M);

  const float rand = path_state_rng_1D(kg, rng_state, PRNG_TEMPORAL_RESAMPLING); // New PRNG dimension
  spatial_reservoir_add_reservoir(current, history, rand, true);

  current->reservoir.M += history.M;

printf("\n--- streaming_temporal_sample(), returned true! total_weight=%f ---\n\n\n",
       (double)history.total_weight);
  return true;
}

/* Standalone temporal-resampling pass, dispatched once per sample before
 * spatial resampling begins. */
ccl_device bool integrator_restir_temporal(KernelGlobals kg,
                                           IntegratorState state,
                                           ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);

  SpatialReservoir current;
  spatial_reservoir_init(kg, &current, true);

  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  if (!integrator_restir_unpack_shader(kg, &current, render_pixel_index, render_buffer)) {
    integrator_path_terminate(kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_RESTIR_TEMPORAL);
    return false;
  }

  shader_data_setup_from_restir(kg, state, &current, render_buffer);

  RNGState rng_state;
  path_state_rng_load(state, &rng_state);

  const bool visibility = kernel_data.integrator.restir_unbiased;

  current.reservoir = restir_unpack_canonical_reservoir(kg, state, &current, render_buffer, false);

  streaming_temporal_sample(kg, state, &current, &rng_state, visibility, render_buffer);

  film_write_pass_reservoir(kg, state, &current.reservoir, render_buffer, /*write_prev=*/false);

  if (!use_spatial) {
    /* Only persist here when integrator_restir() won't run this sample —
    * Persist this sample's temporally-resampled reservoir as next sample's history. */
    film_write_pass_reservoir_temporal(kg, state, &current.reservoir, render_buffer);
  }

  integrator_path_terminate(kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_RESTIR_TEMPORAL);

printf("integrator_restir_temporal(): radiance=(%f, %f, %f)\n",
       (double)current.reservoir.radiance.sum.x, (double)current.reservoir.radiance.sum.y, (double)current.reservoir.radiance.sum.z);
  return true;
}

/* Evaluate ReSTIR DI final samples. */
ccl_device void integrator_evaluate_final_samples(KernelGlobals kg, // DEVICE_KERNEL_INTEGRATOR_FINAL_EVALUATION
                                                  IntegratorState state,
                                                  ccl_global float *ccl_restrict render_buffer)
{
  SpatialReservoir current;
  spatial_reservoir_init(kg, &current, true); // !!!: require_valid_reference

  Reservoir reservoir;
  reservoir_init(kg, &reservoir);

  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  const bool read_prev = INTEGRATOR_STATE(state, path, spatial_iteration) % 2;

  const bool reservoir_ok = integrator_restir_unpack_reservoir(
      kg, &reservoir, render_pixel_index, render_buffer, read_prev);
  const bool shader_ok = integrator_restir_unpack_shader(
      kg, &current, render_pixel_index, render_buffer);

  if (!reservoir_ok || !shader_ok) {
    integrator_path_next(state,
                         DEVICE_KERNEL_INTEGRATOR_FINAL_EVALUATION,
                         DEVICE_KERNEL_INTEGRATOR_RESTIR_PT_EVALUATION); // !!!
    return;
  }

  shader_data_setup_from_restir(kg, state, &current, render_buffer); // !!!: 

  // light_sample_from_uv(kg, &current.sd, current.path_flag, &reservoir.ls); // !!!
  light_sample_from_reference( // !!!: Re-Constructs DI analytic lights only
      kg,
      &reservoir.reference,
      current.sd.P,
      current.sd.N,
      current.sd.time,
      current.sd.shader,
      current.path_flag,
      &reservoir.light);

  radiance_eval(kg, state, &current.sd, &reservoir.light, &reservoir.radiance, nullptr); // TODO: Remove &reservoir.reference?

  reservoir_add_reservoir(&current.reservoir, reservoir, 0.0f, true); // !!!: require_valid_reference

  reservoir_finalize(&current.reservoir, true); // !!!: require_valid_reference

  bsdf_eval_mul(&current.reservoir.radiance, current.reservoir.total_weight); // !!!: DI Blackout?

  /* Load random number state. */
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);
  /*
                      ┌──────────────► INTERSECT_SHADOW
                      │                       │
  FINAL_EVALUATION ───┤                       ▼
                      │                 SHADE_SHADOW
                      │
                      └──────────────► RESTIR_PT_EVALUATION
  */
  integrate_direct_light_create_shadow_path<true>(
      kg, state, &rng_state, &current.sd, &current.reservoir.light, &current.reservoir.radiance, 0);

  integrator_path_next(state,
                       DEVICE_KERNEL_INTEGRATOR_FINAL_EVALUATION,
                       DEVICE_KERNEL_INTEGRATOR_RESTIR_PT_EVALUATION);
}

ccl_device_inline void streaming_pt_samples(KernelGlobals kg, // !!!: GI
                                            IntegratorState state,
                                            const ccl_private SpatialResampling *resampling,
                                            ccl_private GlobalReservoir *reservoir,
                                            const ccl_private RNGState *rng_state,
                                            const int samples,
                                            ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);

  const bool read_prev = INTEGRATOR_STATE(state, path, spatial_iteration) % 2;

  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  GlobalReservoir canonical;
  global_reservoir_init(kg, &canonical);

  // TODO: Do not ignore the return value.
  integrator_restir_unpack_reservoir(kg, &canonical, render_pixel_index, render_buffer, read_prev); // !!!
  integrator_restir_unpack_shader(kg, &canonical.sd, render_pixel_index, render_buffer);
  
// !!!: BasedOnDebugs: canonical is valid reservoir
  if (global_reservoir_finalize(&canonical)) {

    /* TODO(weizhen): should use rand_pick but it doesn't matter when the reservoir is empty. */
    global_reservoir_add_reservoir(reservoir, canonical, 0.0f);
  }

  int valid_neighbors = 1;
  const int neighbors = samples - 1;

  for (int i = 1; i < samples; i++) {
    const float4 rand = path_branched_rng_4D(
      kg,
      rng_state,
      INTEGRATOR_STATE(state, path, spatial_iteration) * samples + i,
      kernel_data.integrator.restir_spatial_iterations * samples,
      PRNG_SPATIAL_RESAMPLING);

    const float3 rand_neighbor = float4_to_float3(rand);
    /* Use `rand.w` because `rand.z` was used for DI. */
    const float rand_pick = rand.w;

    /* Unpack neighbor reservoir and shader data. */
    GlobalReservoir neighbor;
    global_reservoir_init(kg, &neighbor);

    const bool is_unpacked = restir_unpack_neighbor(
            kg, resampling, &canonical, &neighbor, i, rand_neighbor, render_buffer, read_prev);
    if (!is_unpacked)
    {
      printf("streaming_pt_samples() is_unpacked=%d\n", is_unpacked);
      continue;
    }

printf(
    "\n=== streaming_pt_samples: PT CANONICAL ===\n"
    "pixel=%u read_prev=%d\n"
    "canonical BEFORE finalize: W=%f flag=%u rcv=%d rad=(%f,%f,%f)\n\n",
    render_pixel_index,
    (int)read_prev,
    (double)canonical.total_weight, // !!!: Always W=0
    canonical.path_flag,
    canonical.rcv_index,
    (double)canonical.radiance.x, // !!!: Always rad=0
    (double)canonical.radiance.y,
    (double)canonical.radiance.z);

    valid_neighbors++;
    if (global_reservoir_finalize(&neighbor)) {
printf(
    "streaming_pt_samples: canonical AFTER finalize: W=%f flag=%u rcv=%d rad=(%f,%f,%f)\n",
    (double)canonical.total_weight,
    canonical.path_flag,
    canonical.rcv_index,
    (double)canonical.radiance.x,
    (double)canonical.radiance.y,
    (double)canonical.radiance.z);

      global_reservoir_add_reservoir(reservoir, neighbor, rand_pick);

printf(
    "streaming_pt_samples: current_pt AFTER CANONICAL: W=%f flag=%u rcv=%d rad=(%f,%f,%f)\n",
    (double)reservoir->total_weight,
    reservoir->path_flag,
    reservoir->rcv_index,
    (double)reservoir->radiance.x,
    (double)reservoir->radiance.y,
    (double)reservoir->radiance.z);

    }
  }

  reservoir->total_weight /= valid_neighbors;


printf(
    "streaming_pt_samples: PT CURRENT FINAL: pixel=%u valid_neighbors=%d W=%f "
    "rad=(%f,%f,%f)\n",
    render_pixel_index,
    valid_neighbors,
    (double)reservoir->total_weight,
    (double)reservoir->radiance.x,
    (double)reservoir->radiance.y,
    (double)reservoir->radiance.z);
}

/* Evaluate ReSTIR PT final samples. */
/* TODO(weizhen): merge with restir DI. */
ccl_device void integrator_evaluate_restir_pt(KernelGlobals kg, // DEVICE_KERNEL_INTEGRATOR_RESTIR_PT_EVALUATION
                                              IntegratorState state,
                                              ccl_global float *ccl_restrict render_buffer) // !!!: GI
{
  PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);

  const bool read_prev = INTEGRATOR_STATE(state, path, spatial_iteration) % 2;
  
  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  GlobalReservoir reservoir;
  global_reservoir_init(kg, &reservoir);

  if (integrator_restir_unpack_reservoir(
          kg, &reservoir, render_pixel_index, render_buffer, read_prev))
  {
    const int sample = INTEGRATOR_STATE(state, path, sample);
    ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

    global_reservoir_finalize(&reservoir);

    /* Clamp indirect light. */
    film_clamp_light(kg, &reservoir.radiance, 1);

    /* Direct light shadow. */
    // !!!: PathRayVisibility path_visibility
    const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);

    // TODO(Tri): Change this maybe?
    film_write_combined_pass(kg, path_visibility, reservoir.path_flag, sample, reservoir.radiance, buffer);
  }

  integrator_path_terminate(kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_RESTIR_PT_EVALUATION);
}

// TODO(Tri): Silence unused arguments' warns
ccl_device bool integrator_restir(KernelGlobals kg,
                                  IntegratorState state,
                                  ccl_global const KernelWorkTile *ccl_restrict tile,
                                  ccl_global float *ccl_restrict render_buffer,
                                  const int x,
                                  const int y,
                                  const int scheduled_sample)
{
  (void)x;
  (void)y;
  (void)scheduled_sample;

  PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);

  SpatialReservoir current;
  spatial_reservoir_init(kg, &current, true); // !!!: require_valid_reference

  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  if (!integrator_restir_unpack_shader(kg, &current, render_pixel_index, render_buffer)) { // !!!: Returns type... So, LIGHT_SUN==false?
    integrator_path_terminate(kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_RESTIR);
    printf("--- integrator_restir(), early Returned!!! can't unpack! ---\n\n\n");
    return false;
  }

  shader_data_setup_from_restir(kg, state, &current, render_buffer);

  /* Load random number state. */
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);

  /* Plus one to account for the current pixel. */
  const int samples = kernel_data.integrator.restir_spatial_neighbors + 1;

  const int radius = kernel_data.integrator.restir_spatial_radius;
  SpatialResampling spatial_resampling(tile, radius);

  const bool visibility = kernel_data.integrator.restir_unbiased; // !!!: power heuristics

  // TODO(Tri): M is only incremented with spatial_iteration!

  /* Uniformly sample neighboring reservoirs within a radius. There is probability to pick the same
   * reservoir twice, but the chance should be low if the radius is big enough and low descrepancy
   * samples are used. */
  streaming_di_samples(
      kg, state, &spatial_resampling, &current, &rng_state, samples, visibility, render_buffer);

  GlobalReservoir current_pt;
  global_reservoir_init(kg, &current_pt);

  streaming_pt_samples(
      kg, state, &spatial_resampling, &current_pt, &rng_state, samples, render_buffer);

  const bool write_prev = (INTEGRATOR_STATE(state, path, spatial_iteration) + 1) % 2; // !!!: Read uses const bool read_prev = spatial_iteration % 2;
// !!!: Does film_add_reservoir_pt() execute against the same buffer that the subsequent spatial iteration expects to read? Yes

  film_write_pass_reservoir(kg, state, &current.reservoir, render_buffer, write_prev);
  // !!!: film_finalize_reservoir_pt not needed
  film_write_pass_reservoir_pt(kg, state, &current_pt, render_buffer, write_prev); // light_pass

  // !!!: Very Important to keep here!
  // TODO(Tri): Add proper comments here
  // TODO(Tri): Write the finalized current.reservoir into pass_restir_temporal_history_reservoir
  film_write_pass_reservoir_temporal(kg, state, &current.reservoir, render_buffer); // New, separate from film_write_pass_reservoir's ping-pong write

  /* Spatial RESTIR is a terminal GPU pass.
   * The next spatial iteration creates fresh states. */
  integrator_path_terminate(kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_RESTIR);

  return true;
}

CCL_NAMESPACE_END