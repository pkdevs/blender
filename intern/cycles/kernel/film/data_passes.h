/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/surface_shader.h"

#include "kernel/camera/camera.h"

#include "kernel/geom/primitive.h"

#include "kernel/film/cryptomatte_passes.h"
#include "kernel/film/write.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline size_t film_write_cryptomatte_pass(ccl_global float *ccl_restrict buffer,
                                                     const size_t depth,
                                                     const float id,
                                                     const float matte_weight)
{
  film_write_cryptomatte_slots(buffer, depth * 2, id, matte_weight);
  return depth * 4;
}

ccl_device_inline bool restir_has_motion_pass_data(KernelGlobals kg)
{
  return kernel_data.integrator.use_object_motion_pass != 0;
}

/* primitive_motion_vector() dereferences object/geometry motion-transform
 * arrays that Cycles only uploads when KERNEL_FEATURE_OBJECT_MOTION is
 * requested. Calling it when the feature isn't present reads an unbound
 * array descriptor (width=0, data=nullptr) and aborts. Until temporal-frame
 * reuse requests this feature itself (see below), fail safe: no motion
 * data means no reprojection this pixel, not a crash. */
ccl_device_inline bool restir_has_motion_data(KernelGlobals kg) // !!!: Not sufficient
{
  return (kernel_data.kernel_features & KERNEL_FEATURE_OBJECT_MOTION) != 0;
}
// TODO(Tri): Add a film_read_motion_vector() or sth... helper for temporal reprojection... Done

/* Write ReSTIR temporal-reprojection ground truth for this pixel: the raster
 * motion vector toward the previous frame, plus enough surface data (N, depth)
 * to validate that reprojection next frame. */
ccl_device_inline void film_write_restir_reprojection_surface_pass( // !!!: Runs during ordinary shading
    KernelGlobals kg,
    const ccl_private ShaderData *sd,
    ccl_global float *ccl_restrict buffer)
{
  // const float4 motion = primitive_motion_vector(kg, sd); // !!!: Why does it crash in full pipeline path?
  float2 motion = zero_float2();
 if (restir_has_motion_pass_data(kg)) {
    const float4 mv = primitive_motion_vector(kg, sd);
    motion = make_float2(mv.x, mv.y);
  }
  /* else: motion blur owns the device motion table this render (or nothing does).
   * Falls back to identity reprojection. */


  // float2 motion = zero_float2();
  // const float4 mv = primitive_motion_vector(kg, sd);
  // motion = make_float2(mv.x, mv.y);
  
  const float depth = camera_z_depth(kg, sd->P);

  ccl_global float *ptr = buffer + kernel_data.film.pass_restir_reprojection_surface;

  film_overwrite_pass_float(ptr++, sd->N.x);
  film_overwrite_pass_float(ptr++, sd->N.y);
  film_overwrite_pass_float(ptr++, sd->N.z);
  film_overwrite_pass_float(ptr++, depth);
  film_overwrite_pass_float(ptr++, motion.x);
  film_overwrite_pass_float(ptr++, motion.y);
  film_overwrite_pass_float(ptr,   1.0f); /* is_valid */
}

ccl_device_inline void film_write_data_passes(KernelGlobals kg,
                                              IntegratorState state,
                                              const ccl_private ShaderData *sd,
                                              ccl_global float *ccl_restrict render_buffer)
{
#ifdef __PASSES__
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (!(path_flag & PATH_RAY_TRANSPARENT_BACKGROUND)) {
    return;
  }

  /* Don't write data passes for paths that were split off for shadow catchers
   * to avoid double-counting. */
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  const int flag = kernel_data.film.pass_flag;

  if (!(flag & PASS_ANY)) {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  if (!(path_flag & PATH_RAY_SINGLE_PASS_DONE)) {
    if (INTEGRATOR_STATE(state, path, sample) == 0) {
      if (flag & PASSMASK(DEPTH)) {
        const float depth = camera_z_depth(kg, sd->P);
        film_overwrite_pass_float(buffer + kernel_data.film.pass_depth, depth);
      }
      if (flag & PASSMASK(OBJECT_ID)) {
        const float id = object_pass_id(kg, sd->object);
        film_overwrite_pass_float(buffer + kernel_data.film.pass_object_id, id);
      }
      if (flag & PASSMASK(MATERIAL_ID)) {
        const float id = shader_pass_id(kg, sd);
        film_overwrite_pass_float(buffer + kernel_data.film.pass_material_id, id);
      }
      if (flag & PASSMASK(POSITION)) {
        const float3 position = sd->P;
        film_overwrite_pass_float3(buffer + kernel_data.film.pass_position, position);
      }
      if (kernel_data.integrator.use_temporal_frames) {
        film_write_restir_reprojection_surface_pass(kg, sd, buffer);
      }
    }

    // !!!: Store first-hit receiver data for later ReSTIR surface reconstruction. 
    if (flag & PASSMASK(SURFACE_DATA)) {
      ccl_global float *ptr = buffer + kernel_data.film.pass_surface_data;
      film_overwrite_pass_float(ptr++, (float)path_flag);

      film_overwrite_pass_float(ptr++, sd->u);
      film_overwrite_pass_float(ptr++, sd->v);
      film_overwrite_pass_float(ptr++, (float)sd->type);
      film_overwrite_pass_float(ptr++, (float)sd->object);
      film_overwrite_pass_float(ptr++, (float)sd->prim);
      film_overwrite_pass_float(ptr, (float)sd->lcg_state);
    }

    if (!(sd->flag & (SD_TRANSPARENT | SD_RAY_PORTAL)) ||
        kernel_data.film.pass_alpha_threshold == 0.0f ||
        average(surface_shader_alpha(sd)) >= kernel_data.film.pass_alpha_threshold)
    {
      if (flag & PASSMASK(NORMAL)) {
        const float3 normal = surface_shader_average_normal(sd);
        film_write_pass_float3(buffer + kernel_data.film.pass_normal, normal);
      }
      if (flag & PASSMASK(ROUGHNESS)) {
        const float roughness = surface_shader_average_roughness(sd);
        film_write_pass_float(buffer + kernel_data.film.pass_roughness, roughness);
      }
      if (flag & PASSMASK(UV)) {
        const float3 uv = primitive_uv(kg, sd);
        film_write_pass_float3(buffer + kernel_data.film.pass_uv, uv);
      }
      if (flag & PASSMASK(MOTION)) {
// !!!: For temporal, we need the motion vector corresponding to the current frame's pixel and then use the prev-frame render-buffer/history slot.
        const float4 speed = primitive_motion_vector(kg, sd); // !!!: written as part of current render sample
        film_write_pass_float4(buffer + kernel_data.film.pass_motion, speed);
        film_write_pass_float(buffer + kernel_data.film.pass_motion_weight, 1.0f);
      }

      INTEGRATOR_STATE_WRITE(state, path, flag) |= PATH_RAY_SINGLE_PASS_DONE;
    }
  }

  if (kernel_data.film.cryptomatte_passes) {
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    const float matte_weight = average(throughput) *
                               (1.0f - average(surface_shader_transparency(sd)));
    if (matte_weight > 0.0f) {
      ccl_global float *cryptomatte_buffer = buffer + kernel_data.film.pass_cryptomatte;
      if (kernel_data.film.cryptomatte_passes & CRYPT_OBJECT) {
        const float id = object_cryptomatte_id(kg, sd->object);
        cryptomatte_buffer += film_write_cryptomatte_pass(
            cryptomatte_buffer, kernel_data.film.cryptomatte_depth, id, matte_weight);
      }
      if (kernel_data.film.cryptomatte_passes & CRYPT_MATERIAL) {
        const float id = kernel_data_fetch(shaders, (sd->shader & SHADER_MASK)).cryptomatte_id;
        cryptomatte_buffer += film_write_cryptomatte_pass(
            cryptomatte_buffer, kernel_data.film.cryptomatte_depth, id, matte_weight);
      }
      if (kernel_data.film.cryptomatte_passes & CRYPT_ASSET) {
        const float id = object_cryptomatte_asset_id(kg, sd->object);
        cryptomatte_buffer += film_write_cryptomatte_pass(
            cryptomatte_buffer, kernel_data.film.cryptomatte_depth, id, matte_weight);
      }
    }
  }

  if (flag & PASSMASK(DIFFUSE_COLOR)) {
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_diffuse_color,
                             surface_shader_diffuse(kg, sd) * throughput);
  }
  if (flag & PASSMASK(GLOSSY_COLOR)) {
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_glossy_color,
                             surface_shader_glossy(kg, sd) * throughput);
  }
  if (flag & PASSMASK(TRANSMISSION_COLOR)) {
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_transmission_color,
                             surface_shader_transmission(kg, sd) * throughput);
  }
  if (flag & PASSMASK(MIST)) {
    /* Bring depth into 0..1 range. */
    const float mist_start = kernel_data.film.mist_start;
    const float mist_inv_depth = kernel_data.film.mist_inv_depth;

    const float depth = camera_distance(kg, sd->P);
    float mist = saturatef((depth - mist_start) * mist_inv_depth);

    /* Falloff */
    const float mist_falloff = kernel_data.film.mist_falloff;

    if (mist_falloff == 1.0f) {
      ;
    }
    else if (mist_falloff == 2.0f) {
      mist = mist * mist;
    }
    else if (mist_falloff == 0.5f) {
      mist = sqrtf(mist);
    }
    else {
      mist = powf(mist, mist_falloff);
    }

    /* Modulate by transparency */
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    const Spectrum alpha = surface_shader_alpha(sd);
    const float mist_output = (1.0f - mist) * average(throughput * alpha);

    /* Note that the final value in the render buffer we want is 1 - mist_output,
     * to avoid having to tracking this in the Integrator state we do the negation
     * after rendering. */
    film_write_pass_float(buffer + kernel_data.film.pass_mist, mist_output);
  }
#endif
}

ccl_device_inline void film_write_data_passes_background(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
#ifdef __PASSES__
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (!(path_flag & PATH_RAY_TRANSPARENT_BACKGROUND)) {
    return;
  }

  /* Don't write data passes for paths that were split off for shadow catchers
   * to avoid double-counting. */
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  const int flag = kernel_data.film.pass_flag;

  if (!(flag & PASS_ANY)) {
    return;
  }

  if (!(path_flag & PATH_RAY_SINGLE_PASS_DONE)) {
    ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

    if (INTEGRATOR_STATE(state, path, sample) == 0) {
      if (flag & PASSMASK(DEPTH)) {
        film_overwrite_pass_float(buffer + kernel_data.film.pass_depth, 0.0f);
      }
      if (flag & PASSMASK(OBJECT_ID)) {
        film_overwrite_pass_float(buffer + kernel_data.film.pass_object_id, 0.0f);
      }
      if (flag & PASSMASK(MATERIAL_ID)) {
        film_overwrite_pass_float(buffer + kernel_data.film.pass_material_id, 0.0f);
      }
      if (flag & PASSMASK(POSITION)) {
        film_overwrite_pass_float3(buffer + kernel_data.film.pass_position, zero_float3());
      }
    }

    if (flag & PASSMASK(MOTION)) {
      const float4 speed = camera_motion_vector_direction(kg, INTEGRATOR_STATE(state, ray, D));
      film_write_pass_float4(buffer + kernel_data.film.pass_motion, speed);
      film_write_pass_float(buffer + kernel_data.film.pass_motion_weight, 1.0f);
    }
  }
#endif
}

ccl_device_forceinline void film_write_reconnection_vertex(KernelGlobals kg,
                                                           IntegratorState state,
                                                           const float2 roughness,
                                                           const float ray_length,
                                                           ccl_global float *ccl_restrict
                                                               render_buffer)
{
  if (!path_use_restir(kg, state)) {
    return;
  }

  ccl_global float *reservoir_buffer = film_pass_pixel_render_buffer(kg, state, render_buffer) +
                                          kernel_data.film.pass_restir_pt_reservoir;

  const bool found_reconnection_vertex = (int)kernel_read_pass_float(reservoir_buffer + 2) > 1;
  const bool bounce_is_rough = roughness.x * roughness.y > RESTIR_ROUGHNESS_THRESH_SQ;

  if (!found_reconnection_vertex && ray_length >= RESTIR_RAY_LENGTH_THRESH && bounce_is_rough &&
      INTEGRATOR_STATE(state, path, last_bounce_is_rough))
  {
    /* Found reconnection vertex, write to reservoir. */
    film_overwrite_pass_float(reservoir_buffer + 2, (float)INTEGRATOR_STATE(state, path, bounce));
  }
  else {
    INTEGRATOR_STATE_WRITE(state, path, last_bounce_is_rough) = bounce_is_rough;
  }
}

CCL_NAMESPACE_END
