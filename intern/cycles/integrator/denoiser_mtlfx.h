/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_METALFX

#  include "integrator/denoiser_gpu.h"

CCL_NAMESPACE_BEGIN

/* Implementation of denoising API which uses MTLFXTemporalDenoisedScaler. */
class MetalFXDenoiser : public DenoiserGPU {
public:
  MetalFXDenoiser(Device *denoiser_device, const DenoiseParams &params);
  ~MetalFXDenoiser();
  
  static bool is_device_supported(const DeviceInfo &device);
  
private:
  void *mtlfx_denoiser = nullptr;
  
  void *color_texture = nullptr;
  void *motion_texture = nullptr;
  void *diffuse_albedo_texture = nullptr;
  void *specular_albedo_texture = nullptr;
  void *specular_hit_distance_texture = nullptr;
  void *normal_texture = nullptr;
  void *roughness_texture = nullptr;
  void *depth_texture = nullptr;
  void *output_texture = nullptr;
  
  void *color_pre_fn = nullptr;
  void *guiding_pre_fn = nullptr;
  void *color_post_fn = nullptr;
  
  void *color_pre_pso = nullptr;
  void *guiding_pre_pso = nullptr;
  void *color_post_pso = nullptr;
  
  int last_width = 0;
  int last_height = 0;
  float last_upscale_factor = 0.0f;
  
  virtual bool denoise_create_if_needed(DenoiseContext &context) override;
  
  virtual bool denoise_configure_if_needed(DenoiseContext &context) override;
  
  bool denoise_filter_color_preprocess(const DenoiseContext &context, const DenoisePass &pass) override;
  
  bool denoise_filter_guiding_preprocess(DenoiseContext &context) override;

  bool denoise_filter_color_postprocess(const DenoiseContext &context, const DenoisePass &pass) override;
  
  virtual bool denoise_run(const DenoiseContext &context, const DenoisePass &pass) override;
  
  struct PreProcessParams {
    int full_x;
    int full_y;
    int width;
    int height;
    int offset;
    int stride;
    int pass_stride;
    int pass_denoised;
    int num_components;
  };

  struct GuidingParams {
    int offset;
    int stride;
    int pass_stride;
    int pass_sample_count;
    int pass_depth;
    int pass_albedo;
    int pass_specular_albedo;
    int pass_specular_hit_distance;
    int pass_normal;
    int pass_roughness;
    int pass_motion;
    int full_x;
    int full_y;
    int width;
    int height;
    int num_samples;
  };

  struct PostProcessParams {
    int full_x;
    int full_y;
    int width;
    int height;
    int offset;
    int stride;
    int pass_stride;
    int num_samples;
    int pass_noisy;
    int pass_denoised;
    int pass_sample_count;
    int num_components;
    float upscale_factor;
  };
  
  // This is here for testing. Eventually I'd like to load this from a real Metal
  // file or somehow integrate with the Cycles kernel abstraction
  
  inline static const char *texture_process_kernels = R"(
    #include <metal_stdlib>
    using namespace metal;

    struct PreProcessParams {
      int full_x;
      int full_y;
      int width;
      int height;
      int offset;
      int stride;
      int pass_stride;
      int pass_denoised;
      int num_components;
    };

    struct GuidingParams {
      int offset;
      int stride;
      int pass_stride;
      int pass_sample_count;
      int pass_depth;
      int pass_albedo;
      int pass_specular_albedo;
      int pass_specular_hit_distance;
      int pass_normal;
      int pass_roughness;
      int pass_motion;
      int full_x;
      int full_y;
      int width;
      int height;
      int num_samples;
    };

    struct PostProcessParams {
      int full_x;
      int full_y;
      int width;
      int height;
      int offset;
      int stride;
      int pass_stride;
      int num_samples;
      int pass_noisy;
      int pass_denoised;
      int pass_sample_count;
      int num_components;
      float upscale_factor;
    };

    // MARK: Color preprocess
    kernel void filter_color_preprocess(
                                        device const float *render_buffer [[buffer(0)]],
                                        texture2d<float, access::write> color_tex [[texture(0)]],
                                        constant PreProcessParams &params [[buffer(1)]],
                                        uint2 tid [[thread_position_in_grid]]
                                        )
    {
      if (tid.x >= uint(params.width) || tid.y >= uint(params.height)) return;
      
      uint64_t pixel_index = params.offset + (tid.x + params.full_x) + (tid.y + params.full_y) * params.stride;
      device const float *buffer = render_buffer + pixel_index * params.pass_stride;

      device const float *denoised_pixel = buffer + params.pass_denoised;
      
      float4 color;
      color.x = denoised_pixel[0];
      color.y = denoised_pixel[1];
      color.z = denoised_pixel[2];
      color.w = params.num_components > 3 ? denoised_pixel[3] : 1.0f;

      uint2 flip_tid = uint2(tid.x, params.height - 1 - tid.y);
      
      color_tex.write(color, flip_tid);
    }

    // MARK: Guiding preprocess
    kernel void filter_guiding_preprocess(
                                          device const float *render_buffer [[buffer(0)]],
                                          texture2d<float, access::write> depth_tex [[texture(0)]],
                                          texture2d<float, access::write> motion_tex [[texture(1)]],
                                          texture2d<float, access::write> normal_tex [[texture(2)]],
                                          texture2d<float, access::write> roughness_tex [[texture(3)]],
                                          texture2d<float, access::write> diffuse_albedo_tex [[texture(4)]],
                                          texture2d<float, access::write> specular_albedo_tex [[texture(5)]],
                                          texture2d<float, access::write> specular_hit_distance_tex [[texture(6)]],
                                          constant GuidingParams &params [[buffer(1)]],
                                          uint2 tid [[thread_position_in_grid]]
                                          )
    {
      if (tid.x >= uint(params.width) || tid.y >= uint(params.height)) return;
      
      uint64_t pixel_index = params.offset + (tid.x + params.full_x) + (tid.y + params.full_y) * params.stride;
      device const float *buffer = render_buffer + pixel_index * params.pass_stride;
      
      int PASS_UNUSED = -1;
      
      float pixelScale;
      
      if (params.pass_sample_count == PASS_UNUSED) {
        pixelScale = 1.0f / params.num_samples;
      } else {
        pixelScale = 1.0f / float(as_type<uint>(buffer[params.pass_sample_count]));
      }

      uint2 flip_tid = uint2(tid.x, params.height - 1 - tid.y);
      
      // MARK: Depth pass
      
      if (params.pass_depth != PASS_UNUSED) {
        const device float *depth = buffer + params.pass_depth;
        float color = depth[0] * pixelScale;
        
        depth_tex.write(float4(color, 0.0f, 0.0f, 1.0f), flip_tid);
      }
      
      // MARK: Motion pass
      
      if (params.pass_motion != PASS_UNUSED) {
        const device float *motion = buffer + params.pass_motion;
        float4 color;
        
        color.x = motion[0] * pixelScale;
        color.y = -motion[1] * pixelScale;
        
        motion_tex.write(float4(color.x, color.y, 0.0f, 1.0f), flip_tid);
      }
      
      // MARK: Normal Pass
      
      if (params.pass_normal != PASS_UNUSED) {
        const device float *normal = buffer + params.pass_normal;
        float4 normalColor;
        
        normalColor.x = normal[0] * pixelScale;
        normalColor.y = normal[1] * pixelScale;
        normalColor.z = normal[2] * pixelScale;
        normalColor.w = 1.0f;
        
        normal_tex.write(normalColor, flip_tid);
      }
      
      // MARK: Roughness pass
      
      if (params.pass_roughness != PASS_UNUSED) {
        const device float *roughness = buffer + params.pass_roughness;
        float4 rough;
        
        rough.x = roughness[0] * pixelScale;
        
        roughness_tex.write(float4(rough.x, 0.0f, 0.0f, 1.0f), flip_tid);
      }
      
      // MARK: Diffuse albedo pass
      
      if (params.pass_albedo != PASS_UNUSED) {
        const device float *diffuse = buffer + params.pass_albedo;
        
        float4 albedo;
        albedo.x = diffuse[0] * pixelScale;
        albedo.y = diffuse[1] * pixelScale;
        albedo.z = diffuse[2] * pixelScale;
        albedo.w = 1.0f;
        
        // Tonemap from DLSS
        albedo.x = clamp(albedo.x / (1.0f + albedo.x), 0.0f, 1.0f);
        albedo.y = clamp(albedo.y / (1.0f + albedo.y), 0.0f, 1.0f);
        albedo.z = clamp(albedo.z / (1.0f + albedo.z), 0.0f, 1.0f);
        
        diffuse_albedo_tex.write(albedo, flip_tid);
      }
      
      // MARK: Specular Albedo Pass
      
      if (params.pass_specular_albedo != PASS_UNUSED) {
        const device float *specular = buffer + params.pass_specular_albedo;
        
        float4 albedo;
        albedo.x = specular[0] * pixelScale;
        albedo.y = specular[1] * pixelScale;
        albedo.z = specular[2] * pixelScale;
        albedo.w = 1.0f;
        
        specular_albedo_tex.write(albedo, flip_tid);
      }

      // MARK: Specular Hit Distance Pass
      
      if (params.pass_specular_hit_distance != PASS_UNUSED) {
        const device float *specular_dist = buffer + params.pass_specular_hit_distance;

        float distance = specular_dist[0] * pixelScale;

        specular_hit_distance_tex.write(float4(distance, 0.0f, 0.0f, 1.0f), flip_tid);
      }
    }

    // MARK: Color postprocess
    kernel void filter_color_postprocess(
                                         device float *render_buffer [[buffer(0)]],
                                         texture2d<float, access::read> output_texture [[texture(0)]],
                                         constant PostProcessParams &params [[buffer(1)]],
                                         uint2 tid [[thread_position_in_grid]]
                                         )
    {
      if (tid.x >= uint(params.width) || tid.y >= uint(params.height)) return;
      
      uint64_t render_pixel_index = params.offset +
                                    (tid.x + params.full_x) +
                                    (tid.y + params.full_y) *
                                    params.stride;
      
      device float *buffer = render_buffer + render_pixel_index * params.pass_stride;
      
      float pixelScale;
      
      const int PASS_UNUSED = -1;
      
      if (params.pass_sample_count == PASS_UNUSED) {
        pixelScale = params.num_samples;
      } else if (params.upscale_factor == 1.0f) {
        pixelScale = float(as_type<uint>(buffer[params.pass_sample_count]));
      }
      
      /* const uint64_t denoised_pixel_index = params.offset + (tid.x + params.full_x) + (tid.y + params.full_y) * params.stride; */
      /* device float *denoised_pixel = render_buffer + denoised_pixel_index * params.pass_stride + params.pass_denoised; */
      device float *denoised_pixel = buffer + params.pass_denoised;
      
      uint2 flip_tid = uint2(tid.x, params.height - 1 - tid.y);
      
      float4 color_value = output_texture.read(flip_tid);
      
      denoised_pixel[0] = color_value.x;
      denoised_pixel[1] = color_value.y;
      denoised_pixel[2] = color_value.z;
      if (params.num_components > 3) {
        /* Convert alpha back to transparency. */
        denoised_pixel[3] = 1.0f - color_value.w;
      }

      if (params.pass_sample_count == PASS_UNUSED || params.upscale_factor == 1.0f) {
        for (int c = 0; c < 4 && c < params.num_components; c++) {
          denoised_pixel[c] *= pixelScale;
        }
      }
    }
  )"; 
};

CCL_NAMESPACE_END

#endif
