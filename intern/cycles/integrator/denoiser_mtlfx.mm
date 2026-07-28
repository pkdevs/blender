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
  
  // TODO: insert denoising device instead of System Default Device
  
  if (@available(macOS 26.0, *)) {
    if ([MTLFXTemporalDenoisedScalerDescriptor supportsDevice: MTLCreateSystemDefaultDevice()]) {
      return device.denoisers & DENOISER_MTLFX;
    }
  }
  
  return false;
}

bool MetalFXDenoiser::denoise_create_if_needed(DenoiseContext &context)
{
  if (mtlfx_denoiser_ != nullptr) {
    return false;
  }

  if (@available(macos 26.0, *)) {
    MTLFXTemporalDenoisedScalerDescriptor *desc = [[MTLFXTemporalDenoisedScalerDescriptor alloc] init];
    BufferParams buffer_params = context.buffer_params;
    
    desc.autoExposureEnabled = false;
    desc.reactiveMaskTextureEnabled = false;
    desc.transparencyOverlayTextureEnabled = false;
    desc.denoiseStrengthMaskTextureEnabled = false;
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
    desc.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
    desc.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;

    desc.outputTextureFormat = MTLPixelFormatRGBA16Float;
    mtlfx_denoiser_ = (id<MTLFXTemporalDenoisedScaler>)[desc newTemporalDenoisedScalerWithDevice:MTLCreateSystemDefaultDevice()];
    
    return true;
  }
  
  return false;
}

bool MetalFXDenoiser::denoise_configure_if_needed(DenoiseContext &context)
{
  
//  MTLTextureDescriptor *desc = [MTLTextureDescriptor
//                                texture2DDescriptorWithPixelFormat: MTLPixelFormatRGBA16Float
//                                width: context.render_buffers->params.width
//                                height: context.render_buffers->params.height
//                                mipmapped: NO
//  ];
//  
//  id<MTLBuffer> mtl_buffer = (id<MTLBuffer>)denoiser_device_->get_native_buffer(context.render_buffers->buffer.device_pointer);
//  [mtl_buffer newTextureWithDescriptor:desc offset: 0 bytesPerRow: 4];
  
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
  // prepare work size and arguments.
  // return denoiser_queue_->enqueue(DEVICE_KERNEL_FILTER_GUIDING_PREPROCESS_TO_TEXTURE, work_size, args)
}

bool MetalFXDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  // TODO: Add proper textures to enable denoising, right now this crashes blender.
  
  // encode and denoise
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
  
  [(id<MTLFXTemporalDenoisedScaler>) mtlfx_denoiser_ encodeToCommandBuffer:command_buffer];
    
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  return true;
}

CCL_NAMESPACE_END

#endif
