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
    desc.inputWidth = context.render_buffers->params.width;
    desc.inputHeight = context.render_buffers->params.height;
    desc.outputWidth = context.render_buffers->params.width;
    desc.outputHeight = context.render_buffers->params.height;

    desc.colorTextureFormat = MTLPixelFormatRGBA16Float;
    desc.depthTextureFormat = MTLPixelFormatDepth32Float;
    desc.motionTextureFormat = MTLPixelFormatRG16Float;
    
    desc.normalTextureFormat = MTLPixelFormatRGBA16Float;
    desc.roughnessTextureFormat = MTLPixelFormatR16Float;
    desc.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
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
    
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource: @(texture_process_kernels)
                                                  options: nil
                                                    error: &error];
    
    if (error) NSLog(@"%@", error);
    if (!library) NSLog(@"%@", error);
    
    color_pre_fn = (id<MTLFunction>) [library newFunctionWithName: @"filter_color_preprocess"];
    color_pre_pso = [device newComputePipelineStateWithFunction: (id<MTLFunction>) color_pre_fn error: &error];
    
    NSCAssert(!error, @"Pipeline state creation failed: %@", error);
    
//    guiding_pre_fn = (id<MTLFunction>) [library newFunctionWithName: @"filter_guiding_preprocess"];
//    guiding_pre_pso = [device newComputePipelineStateWithFunction: (id<MTLFunction>) guiding_pre_fn error: &error];
//    
//    color_post_fn = (id<MTLFunction>) [library newFunctionWithName: @"filter_color_postprocess"];
//    color_post_pso = [device newComputePipelineStateWithFunction: (id<MTLFunction>) color_post_fn error: &error];
    
    return !error;
  }
  
  return false;
}

bool MetalFXDenoiser::denoise_configure_if_needed(DenoiseContext &context)
{
  return true;
}

bool MetalFXDenoiser::denoise_filter_color_preprocess(const DenoiseContext &context, const DenoisePass &pass)
{
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> cmd = [queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
  
  [encoder setComputePipelineState: (id<MTLComputePipelineState>) color_pre_pso];
  
  BufferParams buffer_params = context.render_buffers->params;
  
  KernelParams params = {
    buffer_params.full_x,
    buffer_params.full_y,
    buffer_params.width,
    buffer_params.height,
    buffer_params.offset,
    buffer_params.stride,
    buffer_params.pass_stride,
    pass.denoised_offset,
  };
  
  id<MTLBuffer> render_buffer = (id<MTLBuffer>) denoiser_device_->get_native_buffer(context.render_buffers->buffer.device_pointer);
  
  [encoder setBuffer: render_buffer offset: 0 atIndex: 0];
  [encoder setTexture: (id<MTLTexture>) color_texture atIndex: 0];
  [encoder setBytes: &params length: sizeof(params) atIndex: 1];
  
  MTLSize gridSize = MTLSizeMake(params.width, params.height, 1);
  id<MTLComputePipelineState> pso = (id<MTLComputePipelineState>) color_pre_pso;
  MTLSize threadSize = MTLSizeMake(pso.threadExecutionWidth, pso.maxTotalThreadsPerThreadgroup / pso.threadExecutionWidth, 1);
  
  [encoder dispatchThreads: gridSize threadsPerThreadgroup: threadSize];
  [encoder endEncoding];
  
  // TODO: these may not be needed in the future
  [cmd commit];
  [cmd waitUntilCompleted];
  
  // TODO: return true only if the kernel succeeded.
  return true;
}

bool MetalFXDenoiser::denoise_filter_guiding_preprocess(const DenoiseContext &context)
{
  return true;
}

bool MetalFXDenoiser::denoise_filter_color_postprocess(const DenoiseContext &context, const DenoisePass &pass)
{
  return false;
}

bool MetalFXDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  id<MTLFXTemporalDenoisedScaler> denoiser = (id<MTLFXTemporalDenoisedScaler>) mtlfx_denoiser;
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> command_buffer = [queue commandBuffer];

  denoiser.colorTexture          = (id<MTLTexture>) color_texture;
  denoiser.depthTexture          = (id<MTLTexture>) depth_texture;
  denoiser.motionTexture         = (id<MTLTexture>) motion_texture;
  denoiser.normalTexture         = (id<MTLTexture>) normal_texture;
  denoiser.roughnessTexture      = (id<MTLTexture>) roughness_texture;
  denoiser.diffuseAlbedoTexture  = (id<MTLTexture>) diffuse_albedo_texture;
  denoiser.specularAlbedoTexture = (id<MTLTexture>) specular_albedo_texture;
  denoiser.outputTexture         = (id<MTLTexture>) output_texture;
  
  [denoiser encodeToCommandBuffer: command_buffer];
    
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  return true;
}

CCL_NAMESPACE_END

#endif
