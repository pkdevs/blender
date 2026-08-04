/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "kernel/integrator/reservoir.h"
#include "kernel/integrator/state.h"
#include "util/profiling.h"

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
  sd->type = (uint)buffer[i++];
  sd->object = (int)buffer[i++];
  sd->prim = (int)buffer[i++];
  /* TODO(weizhen): only Huang Hair BSDF needs LCG now. Can we find a way to remove the usage? */
  /* TODO(weizhen): check if we can compute `lcg_state` after we support curve. */
  sd->lcg_state = (uint)buffer[i];

  /* TODO: do I need to write to this? */
  // sd->closure_transparent_extinction;

  /* Whether there is valid interesction at the current shading point. */
  /* TODO(weizhen): revisit this condition when we support background and distant lights. */
  return sd->type;
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

ccl_device_inline bool integrator_restir_unpack_reservoir(KernelGlobals kg,
                                                          ccl_private Reservoir *reservoir,
                                                          uint32_t render_pixel_index,
                                                          ccl_global float *ccl_restrict
                                                              render_buffer,
                                                          const bool read_prev)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);
  ccl_global const float *buffer = pixel_render_buffer(kg, render_pixel_index, render_buffer) +
                                   (read_prev ? kernel_data.film.pass_restir_previous_reservoir :
                                                kernel_data.film.pass_restir_reservoir);

  return restir_unpack_reservoir(kg, reservoir, buffer);
}

ccl_device_inline bool integrator_restir_unpack_reservoir(KernelGlobals kg,
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

  return restir_unpack_reservoir_pt(kg, reservoir, buffer);
}

ccl_device_inline void ray_setup(KernelGlobals kg,
                                 ConstIntegratorState state,
                                 ccl_private Ray *ccl_restrict ray,
                                 const uint3 render_pixel_index)
{
  if (render_pixel_index.z == -1) {
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
                                                          ConstIntegratorState state,
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
  surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>(
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

ccl_device_inline bool restir_unpack_neighbor(KernelGlobals kg,
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





ccl_device_inline void streaming_pt_samples(KernelGlobals kg,
                                            IntegratorState state,
                                            const ccl_private SpatialResampling *resampling,
                                            ccl_private GlobalReservoir *reservoir,
                                            const ccl_private RNGState *rng_state,
                                            const int samples,
                                            ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);
  // const bool read_prev = state->spatial_iteration % 2;
  const bool read_prev = INTEGRATOR_STATE(state, path, spatial_iteration) % 2;

  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  GlobalReservoir canonical;
  global_reservoir_init(kg, &canonical);

  integrator_restir_unpack_reservoir(kg, &canonical, render_pixel_index, render_buffer, read_prev);
  integrator_restir_unpack_shader(kg, &canonical.sd, render_pixel_index, render_buffer);
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
    //  state->spatial_iteration * samples + i,
      INTEGRATOR_STATE(state, path, spatial_iteration * samples + i),
      kernel_data.integrator.restir_spatial_iterations * samples,
      PRNG_SPATIAL_RESAMPLING);

    const float3 rand_neighbor = float4_to_float3(rand);
    /* Use `rand.w` because `rand.z` was used for DI. */
    const float rand_pick = rand.w;

    /* Unpack neighbor reservoir and shader data. */
    GlobalReservoir neighbor;
    global_reservoir_init(kg, &neighbor);

    if (!restir_unpack_neighbor(
            kg, resampling, &canonical, &neighbor, i, rand_neighbor, render_buffer, read_prev))
    {
      continue;
    }

    valid_neighbors++;
    if (global_reservoir_finalize(&neighbor)) {
      global_reservoir_add_reservoir(reservoir, neighbor, rand_pick);
    }
  }

  reservoir->total_weight /= valid_neighbors;
}

/* Evaluate ReSTIR PT final samples. */
/* TODO(weizhen): merge with restir DI. */
ccl_device void integrator_evaluate_restir_pt(KernelGlobals kg, // DEVICE_KERNEL_INTEGRATOR_RESTIR_PT_EVALUATION
                                              IntegratorState state,
                                              ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);
  // const bool read_prev = state->spatial_iteration % 2;
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

    film_write_combined_pass(kg, path_visibility, reservoir.path_flag, sample, reservoir.radiance, buffer);
  }

  integrator_path_terminate(kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_RESTIR_PT_EVALUATION);
}

// ccl_device bool integrator_restir(KernelGlobals kg,
//                                   IntegratorState state,
//                                   ccl_global const KernelWorkTile *ccl_restrict tile,
//                                   ccl_global float *ccl_restrict render_buffer,
//                                   const int x,
//                                   const int y,
//                                   const int scheduled_sample)
// {
//   PROFILING_INIT(kg, PROFILING_RESTIR_SPATIAL_RESAMPLING);

//   SpatialReservoir current;
//   spatial_reservoir_init(kg, &current, true); // !!!: require_valid_reference

//   const uint32_t render_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
//   if (!integrator_restir_unpack_shader(kg, &current, render_pixel_index, render_buffer)) {
//     return false;
//   }

//   shader_data_setup_from_restir(kg, state, &current, render_buffer);

//   /* Load random number state. */
//   RNGState rng_state;
//   path_state_rng_load(state, &rng_state);

//   /* Plus one to account for the current pixel. */
//   const int samples = kernel_data.integrator.restir_spatial_neighbors + 1;

//   const int radius = kernel_data.integrator.restir_spatial_radius;
//   SpatialResampling spatial_resampling(tile, radius);

//   const bool visibility = kernel_data.integrator.restir_unbiased; // !!!: power heuristics

//   /* Uniformly sample neighboring reservoirs within a radius. There is probability to pick the same
//    * reservoir twice, but the chance should be low if the radius is big enough and low descrepancy
//    * samples are used. */
//   streaming_di_samples(
//       kg, state, &spatial_resampling, &current, &rng_state, samples, visibility, render_buffer);

//   GlobalReservoir current_pt;
//   global_reservoir_init(kg, &current_pt);

//   streaming_pt_samples(
//       kg, state, &spatial_resampling, &current_pt, &rng_state, samples, render_buffer);

//   // const bool write_prev = (state->spatial_iteration + 1) % 2;
//   const bool write_prev = (INTEGRATOR_STATE(state, path, spatial_iteration) + 1) % 2;

//   film_write_pass_reservoir(kg, state, &current.reservoir, render_buffer, write_prev);
//   film_write_pass_reservoir_pt(kg, state, &current_pt, render_buffer, write_prev); // light_pass
//   return true;
// }

CCL_NAMESPACE_END