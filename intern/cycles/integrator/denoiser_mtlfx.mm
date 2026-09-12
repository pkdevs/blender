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
  // TODO: finish fixing temporal history memory leak issue
  mtlfx_denoiser = nullptr;
  color_texture = nil;
  depth_texture = nil;
  motion_texture = nil;
  normal_texture = nil;
  roughness_texture = nil;
  diffuse_albedo_texture = nil;
  specular_albedo_texture = nil;
  output_texture = nil;
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
  const bool recreate_denoiser = last_width != context.denoised_buffer_params.width ||
                                 last_height != context.denoised_buffer_params.height ||
                                 last_upscale_factor != context.denoise_params.upscale_factor;
  
  if (mtlfx_denoiser != nullptr && !recreate_denoiser) {
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
    desc.depthTextureFormat = MTLPixelFormatR32Float;
    desc.motionTextureFormat = MTLPixelFormatRG16Float;
    
    desc.normalTextureFormat = MTLPixelFormatRGBA8Snorm;
    desc.roughnessTextureFormat = MTLPixelFormatR16Float;
    desc.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
    desc.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;

    desc.outputTextureFormat = MTLPixelFormatRGBA16Float;
    
    mtlfx_denoiser = [desc newTemporalDenoisedScalerWithDevice: device];
    
    // Create the textures needed for denoising
    MTLTextureDescriptor *tex_desc = [MTLTextureDescriptor
                                      texture2DDescriptorWithPixelFormat: MTLPixelFormatRGBA16Float
                                      width: buffer_params.width
                                      height: buffer_params.height
                                      mipmapped: NO];
    
    tex_desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    tex_desc.storageMode = MTLStorageModePrivate;
    
    color_texture = [device newTextureWithDescriptor: tex_desc];
    
    tex_desc.pixelFormat = MTLPixelFormatR32Float;
    depth_texture = [device newTextureWithDescriptor: tex_desc];
    
    tex_desc.pixelFormat = MTLPixelFormatRG16Float;
    motion_texture = [device newTextureWithDescriptor: tex_desc];
    
    // NOTE TO SELF: normals are expected in world space, not view space.
    tex_desc.pixelFormat = MTLPixelFormatRGBA8Snorm;
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
    
    guiding_pre_fn = (id<MTLFunction>) [library newFunctionWithName: @"filter_guiding_preprocess"];
    guiding_pre_pso = [device newComputePipelineStateWithFunction: (id<MTLFunction>) guiding_pre_fn error: &error];
    
    color_post_fn = (id<MTLFunction>) [library newFunctionWithName: @"filter_color_postprocess"];
    color_post_pso = [device newComputePipelineStateWithFunction: (id<MTLFunction>) color_post_fn error: &error];
    
    last_width = context.denoised_buffer_params.width;
    last_height = context.denoised_buffer_params.height;
    last_upscale_factor = context.denoise_params.upscale_factor;
    
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
  // TODO: Synchronize is a small workaround to make the timing of MetalFX denoising work with different command buffers.
  // In the future, I'd like to hook into the denoiser queue/buffer so that we don't have to do this.
  denoiser_queue_->synchronize();
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> cmd = [queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
  
  [encoder setComputePipelineState: (id<MTLComputePipelineState>) color_pre_pso];
  
  BufferParams buffer_params = context.buffer_params;
  
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

bool MetalFXDenoiser::denoise_filter_guiding_preprocess(DenoiseContext &context)
{
  denoiser_queue_->synchronize();
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> cmd = [queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
  
  [encoder setComputePipelineState: (id<MTLComputePipelineState>) guiding_pre_pso];
  
  BufferParams buffer_params = context.buffer_params;
  
  KernelParams params = {
    buffer_params.full_x,
    buffer_params.full_y,
    buffer_params.width,
    buffer_params.height,
    buffer_params.offset,
    buffer_params.stride,
    buffer_params.pass_stride,
    0,
  };
  
  PassParams passParams = {
    context.pass_sample_count,
    context.buffer_params.get_pass_offset(PASS_DENOISING_DEPTH),
    context.pass_motion,
    context.buffer_params.get_pass_offset(PASS_NORMAL),
    context.buffer_params.get_pass_offset(PASS_DENOISING_ROUGHNESS),
    context.buffer_params.get_pass_offset(PASS_DIFFUSE_COLOR),
    context.buffer_params.get_pass_offset(PASS_DENOISING_SPECULAR_ALBEDO),
    context.num_samples,
  };
  
  printf("diffuse: %d\n", passParams.render_pass_diffuse_albedo);
  
  id<MTLBuffer> render_buffer = (id<MTLBuffer>) denoiser_device_->get_native_buffer(context.render_buffers->buffer.device_pointer);
  
  [encoder setBuffer: render_buffer offset: 0 atIndex: 0];
  [encoder setTexture: (id<MTLTexture>) depth_texture atIndex: 0];
  [encoder setTexture: (id<MTLTexture>) motion_texture atIndex: 1];
  [encoder setTexture: (id<MTLTexture>) normal_texture atIndex: 2];
  [encoder setTexture: (id<MTLTexture>) roughness_texture atIndex: 3];
  [encoder setTexture: (id<MTLTexture>) diffuse_albedo_texture atIndex: 4];
  [encoder setTexture: (id<MTLTexture>) specular_albedo_texture atIndex: 5];
  [encoder setBytes: &params length: sizeof(params) atIndex: 1];
  [encoder setBytes: &passParams length: sizeof(passParams) atIndex:2];
  
  MTLSize gridSize = MTLSizeMake(params.width, params.height, 1);
  id<MTLComputePipelineState> pso = (id<MTLComputePipelineState>) guiding_pre_pso;
  MTLSize threadSize = MTLSizeMake(pso.threadExecutionWidth, pso.maxTotalThreadsPerThreadgroup / pso.threadExecutionWidth, 1);
  
  [encoder dispatchThreads: gridSize threadsPerThreadgroup: threadSize];
  [encoder endEncoding];
  
  // TODO: these may not be needed in the future
  [cmd commit];
  [cmd waitUntilCompleted];
  
  // TODO: return true only if the kernel succeeded.
  return true;
}

bool MetalFXDenoiser::denoise_filter_color_postprocess(const DenoiseContext &context, const DenoisePass &pass)
{
  denoiser_queue_->synchronize();
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> cmd = [queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
  
  [encoder setComputePipelineState: (id<MTLComputePipelineState>) color_post_pso];
  
  BufferParams buffer_params = context.buffer_params;
  
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
  
  PassParams passParams = {
    context.pass_sample_count,
    0,
    0,
    0,
    0,
    0,
    0,
    context.num_samples,
  };
  
  BufferParams denoised_buffer = context.denoised_buffer_params;
  
  PostprocessParams denoise {
    denoised_buffer.full_x,
    denoised_buffer.full_y,
    denoised_buffer.offset,
    denoised_buffer.stride,
    pass.noisy_offset,
    pass.use_compositing,
    pass.num_components,
    params_.upscale_factor,
  };
  
  id<MTLBuffer> render_buffer = (id<MTLBuffer>) denoiser_device_->get_native_buffer(context.render_buffers->buffer.device_pointer);
  
  [encoder setBuffer: render_buffer offset: 0 atIndex: 0];
  [encoder setTexture: (id<MTLTexture>) output_texture atIndex: 0];
  [encoder setBytes: &params length: sizeof(params) atIndex: 1];
  [encoder setBytes: &passParams length: sizeof(passParams) atIndex: 2];
  [encoder setBytes: &denoise length: sizeof(denoise) atIndex: 3];
  
  MTLSize gridSize = MTLSizeMake(params.width, params.height, 1);
  id<MTLComputePipelineState> pso = (id<MTLComputePipelineState>) color_pre_pso;
  MTLSize threadSize = MTLSizeMake(pso.threadExecutionWidth, pso.maxTotalThreadsPerThreadgroup / pso.threadExecutionWidth, 1);
  
  [encoder dispatchThreads: gridSize threadsPerThreadgroup: threadSize];
  [encoder endEncoding];
  
  // TODO: these may not be needed in the future
  [cmd commit];
  [cmd waitUntilCompleted];
  
  return true;
}

bool MetalFXDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  id<MTLFXTemporalDenoisedScaler> denoiser = (id<MTLFXTemporalDenoisedScaler>) mtlfx_denoiser;
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
  
  // TODO: account for pixel scale in the upscaler

  denoiser.colorTexture          = (id<MTLTexture>) color_texture;
  denoiser.depthTexture          = (id<MTLTexture>) depth_texture;
  denoiser.motionTexture         = (id<MTLTexture>) motion_texture;
  denoiser.normalTexture         = (id<MTLTexture>) normal_texture;
  denoiser.roughnessTexture      = (id<MTLTexture>) roughness_texture;
  denoiser.diffuseAlbedoTexture  = (id<MTLTexture>) diffuse_albedo_texture;
  denoiser.specularAlbedoTexture = (id<MTLTexture>) specular_albedo_texture;
  denoiser.outputTexture         = (id<MTLTexture>) output_texture;
  denoiser.jitterOffsetX         = context.pixel_jitter.x;
  denoiser.jitterOffsetY         = context.pixel_jitter.y;
  denoiser.motionVectorScaleX    = context.buffer_params.width;
  denoiser.motionVectorScaleY    = context.buffer_params.height;
  
  [denoiser encodeToCommandBuffer: command_buffer];
  
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  return true;
}

CCL_NAMESPACE_END

#endif
