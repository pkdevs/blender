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
  void *mtlfx_denoiser_ = nullptr;
  
  void *color_texture_ = nullptr;
  void *motion_texture_ = nullptr;
  void *diffuse_albedo_texture = nullptr;
  void *specular_albedo_texture_ = nullptr;
  void *normal_texture_ = nullptr;
  void *roughness_texture = nullptr;
  void *depth_texture_ = nullptr;
  void *output_texture_ = nullptr;
  
  virtual bool denoise_create_if_needed(DenoiseContext &context) override;
  
  virtual bool denoise_configure_if_needed(DenoiseContext &context) override;
  
  bool denoise_filter_color_preprocess(const DenoiseContext &context, const DenoisePass &pass) override;

  bool denoise_filter_color_postprocess(const DenoiseContext &context, const DenoisePass &pass) override;

  bool denoise_filter_guiding_preprocess(const DenoiseContext &context) override;
  
  virtual bool denoise_run(const DenoiseContext &context, const DenoisePass &pass) override;
};

CCL_NAMESPACE_END

#endif
