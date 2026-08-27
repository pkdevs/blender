/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_METALFX

#  include "integrator/denoiser_mtlfx.h"
#  include "integrator/pass_accessor_gpu.h"

#  include "device/metal/device_impl.h"
#  include "device/metal/queue.h"

#  include <MetalFX/MetalFX.h>
#  include <MetalFX/MTLFXTemporalDenoisedScaler.h>

CCL_NAMESPACE_BEGIN

MetalFXDenoiser::MetalFXDenoiser(Device *denoiser_device, const DenoiseParams &params)
  : DenoiserGPU(denoiser_device, params)
{
  
}

MetalFXDenoiser::~MetalFXDenoiser()
{
  // TODO: Destroy everything.
}

bool MetalFXDenoiser::is_device_supported(const DeviceInfo &device)
{
  if (device.type != DEVICE_METAL) {
    return false;
  }
  
  auto usable_devices = MetalInfo::get_usable_devices();
  id<MTLDevice> mtlDevice = usable_devices[device.num];

  if (@available(macOS 26.0, *)) {
    if ([MTLFXTemporalDenoisedScalerDescriptor supportsDevice: mtlDevice]) {
      return device.denoisers & DENOISER_MTLFX;
    }
  }
  
  return false;
}

bool MetalFXDenoiser::denoise_create_if_needed(DenoiseContext &context)
{
  if (mtlfx_denoiser != nullptr) {
    return true;
  }

  if (@available(macos 26.0, *)) {
    
    auto usable_devices = MetalInfo::get_usable_devices();
    id<MTLDevice> device = usable_devices[denoiser_device_->info.num];
    
    MTLFXTemporalDenoisedScalerDescriptor *desc = [[MTLFXTemporalDenoisedScalerDescriptor alloc] init];
    BufferParams buffer_params = context.buffer_params;
    
    desc.autoExposureEnabled = false;
    desc.reactiveMaskTextureEnabled = false;
    desc.transparencyOverlayTextureEnabled = false;
    desc.denoiseStrengthMaskTextureEnabled = false;
    // TODO: This aux texture may be one we could (somewhat easily) implement to improve quality
    desc.specularHitDistanceTextureEnabled = false;
    desc.inputWidth = buffer_params.width;
    desc.inputHeight = buffer_params.height;
    desc.outputWidth = buffer_params.width;
    desc.outputHeight = buffer_params.height;

    desc.colorTextureFormat = MTLPixelFormatRGBA16Float;
    desc.depthTextureFormat = MTLPixelFormatDepth32Float;
    desc.motionTextureFormat = MTLPixelFormatRG16Float;
    
    desc.normalTextureFormat = MTLPixelFormatRGBA16Float;
    desc.roughnessTextureFormat = MTLPixelFormatR16Float;
    desc.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA8Unorm;
    desc.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;

    desc.outputTextureFormat = MTLPixelFormatRGBA16Float;
    mtlfx_denoiser = [desc newTemporalDenoisedScalerWithDevice: device];
    
    // Create the textures needed for denoising
    MTLTextureDescriptor *tex_desc = [MTLTextureDescriptor
                                      texture2DDescriptorWithPixelFormat: MTLPixelFormatRGBA16Float
                                      width: context.render_buffers->params.width
                                      height: context.render_buffers->params.height
                                      mipmapped: NO];
    
    tex_desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    tex_desc.storageMode = MTLStorageModePrivate;
    
    color_texture = [device newTextureWithDescriptor: tex_desc];
    
    tex_desc.pixelFormat = MTLPixelFormatDepth32Float;
    depth_texture = [device newTextureWithDescriptor: tex_desc];
    
    tex_desc.pixelFormat = MTLPixelFormatRG16Float;
    motion_texture = [device newTextureWithDescriptor: tex_desc];
    
    // NOTE TO SELF: normals are expected in world space, not view space.
    tex_desc.pixelFormat = MTLPixelFormatRGBA16Float;
    normal_texture = [device newTextureWithDescriptor: tex_desc];
    
    tex_desc.pixelFormat = MTLPixelFormatR16Float;
    roughness_texture = [device newTextureWithDescriptor: tex_desc];
    
    tex_desc.pixelFormat = MTLPixelFormatRGBA16Float;
    diffuse_albedo_texture = [device newTextureWithDescriptor: tex_desc];
    
    specular_albedo_texture = [device newTextureWithDescriptor: tex_desc];
  
    tex_desc.pixelFormat = MTLPixelFormatRGBA16Float;
    output_texture = [device newTextureWithDescriptor: tex_desc];
    
    // After textures are created, load the shader libraries for texture processing.
    
    return true;
  }
  
  return false;
}

bool MetalFXDenoiser::denoise_configure_if_needed(DenoiseContext &context)
{
  return true;
}

bool MetalFXDenoiser::denoise_filter_color_preprocess(const DenoiseContext &context, const DenoisePass &pass)
{
  return false;
}

bool MetalFXDenoiser::denoise_filter_color_postprocess(const DenoiseContext &context, const DenoisePass &pass)
{
  return false;
}

bool MetalFXDenoiser::denoise_filter_guiding_preprocess(const DenoiseContext &context)
{
  return false;
}

bool MetalFXDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  // TODO: Add proper textures to enable denoising, right now this crashes blender.
  // encode and denoise
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
  
  [(id<MTLFXTemporalDenoisedScaler>) mtlfx_denoiser encodeToCommandBuffer:command_buffer];
    
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  return true;
}

CCL_NAMESPACE_END

#endif
