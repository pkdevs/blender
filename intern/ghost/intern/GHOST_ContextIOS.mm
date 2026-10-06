/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GHOST_ContextIOS.hh"
#include "GHOST_Debug.hh"

#include <mutex>

MTLCommandQueue *GHOST_ContextIOS::s_shared_metal_command_queue = nil;
int GHOST_ContextIOS::s_shared_count = 0;
static std::mutex shared_queue_mutex;

GHOST_ContextIOS::GHOST_ContextIOS(const GHOST_ContextParams &params,
                                   std::shared_ptr<GHOST_IOSSurface> surface)
    : GHOST_Context(params),
      surface_(std::move(surface)),
      device_(surface_ ? [surface_->device() retain] : MTLCreateSystemDefaultDevice()),
      contextPresentCallback(nullptr),
      mtl_swap_internal(60)
{
  GHOST_ASSERT(!NSThread.isMainThread, "Metal contexts belong to Blender threads, not UIKit");
  for (int i = 0; i < METAL_SWAPCHAIN_SIZE; i++) {
    m_defaultFramebufferMetalTexture[i] = {nil, unsigned(i)};
  }
  metalInit();
}

GHOST_ContextIOS::~GHOST_ContextIOS()
{
  metalFree();
  [device_ release];
}

GHOST_TSuccess GHOST_ContextIOS::swapBufferRelease()
{
  return metalSwapBuffers();
}

GHOST_TSuccess GHOST_ContextIOS::setSwapInterval(int interval)
{
  mtl_swap_internal = interval;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextIOS::getSwapInterval(int &interval)
{
  interval = mtl_swap_internal;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextIOS::activateDrawingContext()
{
  active_context_ = this;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextIOS::releaseDrawingContext()
{
  active_context_ = nullptr;
  return GHOST_kSuccess;
}

unsigned int GHOST_ContextIOS::getDefaultFramebuffer()
{
  return 0;
}

GHOST_TSuccess GHOST_ContextIOS::updateDrawingContext()
{
  metalUpdateFramebuffer();
  return GHOST_kSuccess;
}

id<MTLTexture> GHOST_ContextIOS::metalOverlayTexture()
{
  current_swapchain_index = (current_swapchain_index + 1) % METAL_SWAPCHAIN_SIZE;
  updateDrawingContext();
  return m_defaultFramebufferMetalTexture[current_swapchain_index].texture;
}

MTLCommandQueue *GHOST_ContextIOS::metalCommandQueue()
{
  return s_shared_metal_command_queue;
}

MTLDevice *GHOST_ContextIOS::metalDevice()
{
  return (MTLDevice *)device_;
}

GHOST_TSuccess GHOST_ContextIOS::initializeDrawingContext()
{
  if (!device_ || !s_shared_metal_command_queue) {
    return GHOST_kFailure;
  }
  metalInitFramebuffer();
  return activateDrawingContext();
}

GHOST_TSuccess GHOST_ContextIOS::releaseNativeHandles()
{
  surface_.reset();
  return GHOST_kSuccess;
}

void GHOST_ContextIOS::metalInit()
{
  std::lock_guard lock(shared_queue_mutex);
  if (!s_shared_metal_command_queue) {
    s_shared_metal_command_queue = (MTLCommandQueue *)[device_
        newCommandQueueWithMaxCommandBufferCount:max_command_buffer_count];
  }
  s_shared_count++;
}

void GHOST_ContextIOS::metalFree()
{
  for (auto &slot : m_defaultFramebufferMetalTexture) {
    [slot.texture release];
    slot.texture = nil;
  }
  std::lock_guard lock(shared_queue_mutex);
  if (--s_shared_count == 0) {
    [s_shared_metal_command_queue release];
    s_shared_metal_command_queue = nil;
  }
}

void GHOST_ContextIOS::metalInitFramebuffer()
{
  updateDrawingContext();
}

void GHOST_ContextIOS::metalUpdateFramebuffer()
{
  const CGSize size = surface_ ? surface_->size() : CGSizeMake(1, 1);
  auto &slot = m_defaultFramebufferMetalTexture[current_swapchain_index];
  if (slot.texture && slot.texture.width == size.width && slot.texture.height == size.height) {
    return;
  }
  MTLTextureDescriptor *desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                   width:size.width
                                  height:size.height
                               mipmapped:NO];
  desc.storageMode = MTLStorageModePrivate;
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
  if (!texture) {
    return;
  }
  [slot.texture release];
  slot.texture = texture;
  id<MTLCommandBuffer> buffer = [s_shared_metal_command_queue commandBuffer];
  MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.294, 0.294, 0.294, 1);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
  [encoder endEncoding];
  [buffer commit];
}

void GHOST_ContextIOS::metalRegisterPresentCallback(void (*callback)(
    MTLRenderPassDescriptor *, id<MTLRenderPipelineState>, id<MTLTexture>, id<CAMetalDrawable>))
{
  contextPresentCallback = callback;
}

GHOST_TSuccess GHOST_ContextIOS::metalSwapBuffers()
{
  if (!surface_ || !contextPresentCallback) {
    return GHOST_kSuccess;
  }
  /* Do not resize here: this texture belongs to the frame just rendered. The next
   * render acquires the new geometry. Submit the copy after the backend's flush. */
  id<MTLTexture> texture = m_defaultFramebufferMetalTexture[current_swapchain_index].texture;
  contextPresentCallback(nil, nil, texture, nil);
  return surface_->publish((id<MTLCommandQueue>)s_shared_metal_command_queue, texture) ?
             GHOST_kSuccess :
             GHOST_kFailure;
}
