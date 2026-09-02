/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
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
  
  bool denoise_filter_guiding_preprocess(const DenoiseContext &context) override;

  bool denoise_filter_color_postprocess(const DenoiseContext &context, const DenoisePass &pass) override;
  
  virtual bool denoise_run(const DenoiseContext &context, const DenoisePass &pass) override;
  
  struct KernelParams {
    int full_x;
    int full_y;
    int width;
    int height;
    int offset;
    int stride;
    int pass_stride;
    int pass_denoised;
  };
  
  struct PassParams {
    int render_pass_sample_count;
    int render_pass_depth;
    int render_pass_motion;
    int render_pass_normal;
    int render_pass_roughness;
    int render_pass_diffuse_albedo;
    int render_pass_specular_albedo;
    int num_samples;
  };
  
  struct PostprocessParams {
    int render_full_x;
    int render_full_y;
    int render_offset;
    int render_stride;
    int pass_noisy;
    int use_compositing;
    int num_components;
    float upscale_factor;
  };
  
  // This is here for testing. Eventually I'd like to load this from a real Metal
  // file or somehow integrate with the Cycles kernel abstraction
  
  inline static const char *texture_process_kernels = R"(
    #include <metal_stdlib>
    using namespace metal;

    struct KernelParams {
      int full_x;
      int full_y;
      int width;
      int height;
      int offset;
      int stride;
      int pass_stride;
      int pass_denoised;
    };

    struct PassParams {
      int render_pass_sample_count;
      int render_pass_depth;
      int render_pass_motion;
      int render_pass_normal;
      int render_pass_roughness;
      int render_pass_diffuse_albedo;
      int render_pass_specular_albedo;
      int num_samples;
    };

    struct PostprocessParams {
      int render_full_x;
      int render_full_y;
      int render_offset;
      int render_stride;
      int pass_noisy;
      int use_compositing;
      int num_components;
      float upscale_factor;
    };

    // MARK: Color preprocess
    kernel void filter_color_preprocess(
                                        device const float *render_buffer [[buffer(0)]],
                                        texture2d<float, access::write> color_tex [[texture(0)]],
                                        constant KernelParams &params [[buffer(1)]],
                                        uint2 tid [[thread_position_in_grid]]
                                        )
    {
      if (tid.x >= uint(params.width) || tid.y >= uint(params.height)) return;
      
      uint64_t pixel_index = params.offset + (tid.x + params.full_x) + (tid.y + params.full_y) * params.stride;
      device const float *output = render_buffer + pixel_index * params.pass_stride + params.pass_denoised;
      
      float4 color = float4(output[0], output[1], output[2], 1.0f);

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
                                          constant KernelParams &params [[buffer(1)]],
                                          constant PassParams &pass [[buffer(2)]],
                                          uint2 tid [[thread_position_in_grid]]
                                          )
    {
      if (tid.x >= uint(params.width) || tid.y >= uint(params.height)) return;
      
      uint64_t pixel_index = params.offset + (tid.x + params.full_x) + (tid.y + params.full_y) * params.stride;
      device const float *buffer = render_buffer + pixel_index * params.pass_stride;
      
      int PASS_UNUSED = -1;
      
      float pixelScale;
      
      if (pass.render_pass_sample_count == PASS_UNUSED) {
        pixelScale = 1.0f / pass.num_samples;
      } else {
        pixelScale = 1.0f / buffer[pass.render_pass_sample_count];
      }

      uint2 flip_tid = uint2(tid.x, params.height - 1 - tid.y);
      
      // MARK: Depth pass
      
      if (pass.render_pass_depth != PASS_UNUSED) {
        const device float *depth = buffer + pass.render_pass_depth;
        float color = depth[0] * pixelScale;
        
        depth_tex.write(float4(color, 0.0f, 0.0f, 1.0f), flip_tid);
      }
      
      // MARK: Motion pass
      
      if (pass.render_pass_motion != PASS_UNUSED) {
        const device float *motion = buffer + pass.render_pass_motion;
        float4 color;
        
        color.x = motion[0] * pixelScale;
        color.y = motion[1] * pixelScale;
        
        motion_tex.write(float4(color.x, color.y, 0.0f, 1.0f), flip_tid);
      }
      
      // MARK: Normal Pass
      
      if (pass.render_pass_normal != PASS_UNUSED) {
        const device float *normal = buffer + pass.render_pass_normal;
        float4 normalColor;
        
        normalColor.x = normal[0] * pixelScale;
        normalColor.y = normal[1] * pixelScale;
        normalColor.z = normal[2] * pixelScale;
        normalColor.w = 1.0f;
        
        normal_tex.write(normalColor, flip_tid);
      }
      
      // MARK: Roughness pass
      
      if (pass.render_pass_roughness != PASS_UNUSED) {
        const device float *roughness = buffer + pass.render_pass_roughness;
        float4 rough;
        
        rough.x = roughness[0] * pixelScale;
        
        roughness_tex.write(float4(rough.x, 0.0f, 0.0f, 1.0f), flip_tid);
      }
      
      // MARK: Diffuse albedo pass
      
      if (pass.render_pass_diffuse_albedo != PASS_UNUSED) {
        const device float *diffuse = buffer + pass.render_pass_diffuse_albedo;
        
        float4 albedo;
        albedo.x = diffuse[0] * pixelScale;
        albedo.y = diffuse[1] * pixelScale;
        albedo.z = diffuse[2] * pixelScale;
        albedo.w = 1.0f;
        
        // TODO: To tonemap or not to tonemap?
        // This was from the DLSS implementation
        albedo.x = clamp(albedo.x / (1.0f + albedo.x), 0.0f, 1.0f);
        albedo.y = clamp(albedo.y / (1.0f + albedo.y), 0.0f, 1.0f);
        albedo.z = clamp(albedo.z / (1.0f + albedo.z), 0.0f, 1.0f);
        
        diffuse_albedo_tex.write(albedo, flip_tid);
      }
      
      // MARK: Specular Albedo Pass
      
      if (pass.render_pass_specular_albedo != PASS_UNUSED) {
        const device float *specular = buffer + pass.render_pass_specular_albedo;
        
        float4 albedo;
        albedo.x = specular[0] * pixelScale;
        albedo.y = specular[1] * pixelScale;
        albedo.z = specular[2] * pixelScale;
        albedo.w = 1.0f;
        
        specular_albedo_tex.write(albedo, flip_tid);
      }
    }

    // MARK: Color postprocess
    kernel void filter_color_postprocess(
                                         device float *render_buffer [[buffer(0)]],
                                         texture2d<float, access::read> output_texture [[texture(0)]],
                                         constant KernelParams &params [[buffer(1)]],
                                         constant PassParams &pass [[buffer(2)]],
                                         constant PostprocessParams &denoise [[buffer(3)]],
                                         uint2 tid [[thread_position_in_grid]]
                                         )
    {
      if (tid.x >= uint(params.width) || tid.y >= uint(params.height)) return;
      
      uint64_t render_pixel_index = denoise.render_offset +
                                    (int(tid.x / denoise.upscale_factor) + denoise.render_full_x) +
                                    (int(tid.y / denoise.upscale_factor) + denoise.render_full_y) *
                                    denoise.render_stride;
      
      const device float *buffer = render_buffer + render_pixel_index * params.pass_stride;
      
      float pixelScale;
      
      const int PASS_UNUSED = -1;
      
      if (pass.render_pass_sample_count == PASS_UNUSED) {
        pixelScale = pass.num_samples;
      } else {
        pixelScale = buffer[pass.render_pass_sample_count];
      }
      
      const uint64_t denoised_pixel_index = params.offset + (tid.x + params.full_x) + (tid.y + params.full_y) * params.stride;
      device float *denoised_pixel = render_buffer + denoised_pixel_index * params.pass_stride + params.pass_denoised;
      
      uint2 flip_tid = uint2(tid.x, params.height - 1 - tid.y);
      
      float4 color_value = output_texture.read(flip_tid);
      
      denoised_pixel[0] = color_value.x;
      denoised_pixel[1] = color_value.y;
      denoised_pixel[2] = color_value.z;
      
      if (pass.render_pass_sample_count == PASS_UNUSED || denoise.upscale_factor == 1.0f) {
       denoised_pixel[0] *= pixelScale;
       denoised_pixel[1] *= pixelScale;
       denoised_pixel[2] *= pixelScale;
      }
      
      if (denoise.num_components == 3) {
         /* Pass without alpha channel. */
       }
       else if (!denoise.use_compositing) {
         
         const device float *noisy_pixel = buffer + denoise.pass_noisy;
         denoised_pixel[3] = noisy_pixel[3];
     
         if (pass.render_pass_sample_count != PASS_UNUSED && denoise.upscale_factor != 1.0f) {
           denoised_pixel[3] /= pixelScale;
         }
       }
       else {
         /* Assigning to zero since this is a default alpha value for 3-component passes, and it
          * is an opaque pixel for 4 component passes. */
         denoised_pixel[3] = 0;
       }
    }
  )";
};

CCL_NAMESPACE_END

#endif
