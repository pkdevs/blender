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
  // void *denoiser_desc_ = nullptr;
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

    kernel void filter_color_preprocess(
                                        device const float *render_buffer [[buffer(0)]],
                                        texture2d<float, access::write> color_tex [[texture(0)]],
                                        constant KernelParams &params [[buffer(1)]],
                                        uint2 tid [[thread_position_in_grid]]
                                        )
    {
      if (tid.x >= params.width || tid.y >= params.height) return;
      
      uint64_t pixel_index = params.offset + (tid.x + params.full_x) + (tid.y + params.full_y) * params.stride;
      device const float *output = render_buffer + pixel_index * params.pass_stride + params.pass_denoised;
      
      float4 color = float4(output[0], output[1], output[2], 1.0f);
      
      color_tex.write(color, tid);
    }
  )";
};

CCL_NAMESPACE_END

#endif
