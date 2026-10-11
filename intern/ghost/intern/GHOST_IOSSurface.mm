/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_IOSSurface.hh"

#import <UIKit/UIKit.h>

#import "GHOST_IOSPlatform.hh"

#include <algorithm>
#include <atomic>

/* -------------------------------------------------------------------- */
/** \name Frame Rate
 *
 * The system shows an app's frames at most 60 times a second unless it asks for more. While
 * Blender presents frames, a display link asks for the display's full rate (120 Hz with
 * ProMotion); without frames it stops, so the display can slow down again.
 * \{ */

static std::atomic<double> last_present_time{0.0};
static std::atomic<bool> frame_rate_requested{false};

@interface GHOSTIOSFrameRateRequest : NSObject
@end

@implementation GHOSTIOSFrameRateRequest {
  CADisplayLink *_link;
}

+ (GHOSTIOSFrameRateRequest *)shared
{
  static GHOSTIOSFrameRateRequest *request = [[GHOSTIOSFrameRateRequest alloc] init];
  return request;
}

- (void)start
{
  if (!_link) {
    _link = [[CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)] retain];
    const float maximum = float(GHOSTIOSMaximumFramesPerSecond());
    _link.preferredFrameRateRange = CAFrameRateRangeMake(std::min(80.0f, maximum), maximum, maximum);
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  }
  _link.paused = NO;
}

- (void)tick:(CADisplayLink *)link
{
  if (CACurrentMediaTime() - last_present_time.load() > 0.25) {
    link.paused = YES;
    frame_rate_requested.store(false);
  }
}

@end

static void frame_presented()
{
  last_present_time.store(CACurrentMediaTime());
  if (!frame_rate_requested.exchange(true)) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [[GHOSTIOSFrameRateRequest shared] start];
    });
  }
}

/** \} */

GHOST_IOSSurface::Frame::~Frame()
{
  [texture release];
}

GHOST_IOSSurface::GHOST_IOSSurface(CAMetalLayer *layer)
    : layer_([layer retain]), device_([layer.device retain])
{
  NSCAssert(NSThread.isMainThread, @"Create the native surface on UIKit");
  presenter_ = dispatch_queue_create("org.blender.viewport.presentation", DISPATCH_QUEUE_SERIAL);
}

GHOST_IOSSurface::~GHOST_IOSSurface()
{
  [pipeline_ release];
  [presentation_queue_ release];
  [device_ release];
  [layer_ release];
  dispatch_release(presenter_);
}

CGSize GHOST_IOSSurface::size() const
{
  const uint64_t packed = size_.load();
  return CGSizeMake(packed >> 32, uint32_t(packed));
}

void GHOST_IOSSurface::resize(CGSize size)
{
  const uint64_t packed = (uint64_t(std::max(1.0, size.width)) << 32) |
                          uint32_t(std::max(1.0, size.height));
  if (size_.exchange(packed) != packed) {
    requestPresent();
  }
}

void GHOST_IOSSurface::setVisible(bool visible)
{
  visible_.store(visible);
  if (visible) {
    requestPresent();
  }
}

void GHOST_IOSSurface::close()
{
  closed_.store(true);
  visible_.store(false);
}

void GHOST_IOSSurface::requestPresent()
{
  dirty_.store(true);
  if (closed_.load() || !visible_.load() || scheduled_.exchange(true)) {
    return;
  }
  auto self = shared_from_this();
  dispatch_async(presenter_, ^{
    @autoreleasepool {
      self->present();
    }
  });
}

void GHOST_IOSSurface::presentationFinished(bool retry)
{
  if (retry && visible_.load() && !closed_.load()) {
    dirty_.store(true);
    auto self = shared_from_this();
    /* A temporarily unavailable drawable must not lose the final idle frame. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC), presenter_, ^{
      self->presentationFinished();
    });
    return;
  }
  scheduled_.store(false);
  if (dirty_.load()) {
    requestPresent();
  }
}

bool GHOST_IOSSurface::publish(id<MTLCommandQueue> rendering_queue, id<MTLTexture> source)
{
  NSCAssert(!NSThread.isMainThread, @"Frame publication must not run on UIKit");
  if (!source || closed_.load()) {
    return true;
  }
  if (!visible_.load()) {
    return false;
  }
  std::shared_ptr<Frame> frame;
  for (auto &slot : pool_) {
    if (!slot || slot.use_count() == 1) {
      if (!slot || slot->texture.width != source.width || slot->texture.height != source.height) {
        slot = std::make_shared<Frame>();
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:source.pixelFormat
                                         width:source.width
                                        height:source.height
                                     mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead;
        slot->texture = [device_ newTextureWithDescriptor:desc];
        slot->texture.label = @"Completed Blender viewport";
      }
      frame = slot;
      break;
    }
  }
  /* Backpressure drops presentation copies, never input and never the last good frame. */
  if (!frame || !frame->texture) {
    return false;
  }
  id<MTLCommandBuffer> buffer = [rendering_queue commandBuffer];
  id<MTLBlitCommandEncoder> encoder = [buffer blitCommandEncoder];
  if (!encoder) {
    return false;
  }
  [encoder copyFromTexture:source toTexture:frame->texture];
  [encoder endEncoding];
  auto self = shared_from_this();
  [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    if (completed.status == MTLCommandBufferStatusCompleted && !self->closed_.load()) {
      {
        std::lock_guard lock(self->frame_mutex_);
        self->completed_ = frame;
      }
      self->requestPresent();
    }
  }];
  [buffer commit];
  return true;
}

void GHOST_IOSSurface::present()
{
  NSCAssert(!NSThread.isMainThread, @"Drawable acquisition must not run on UIKit");
  dirty_.store(false);
  std::shared_ptr<Frame> frame;
  {
    std::lock_guard lock(frame_mutex_);
    frame = completed_;
  }
  if (closed_.load() || !visible_.load() || !frame) {
    presentationFinished();
    return;
  }
  if (!pipeline_) {
    presentation_queue_ = [device_ newCommandQueue];
    presentation_queue_.label = @"Blender viewport presentation";
    NSString *source = @R"msl(
      #include <metal_stdlib>
      using namespace metal;
      struct Vertex { float4 position [[position]]; float2 uv; };
      vertex Vertex vertex_shader(uint i [[vertex_id]]) {
        float2 p = float2(float(i & 1), float(i >> 1)) * 4.0 - 1.0;
        return {float4(p, 0, 1), p * 0.5 + 0.5};
      }
      fragment float4 fragment_shader(Vertex v [[stage_in]], texture2d<float> t [[texture(0)]]) {
        constexpr sampler s(filter::linear);
        return float4(t.sample(s, v.uv).rgb, 1);
      }
    )msl";
    NSError *error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:source options:nil error:&error];
    MTLRenderPipelineDescriptor *desc = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    id<MTLFunction> vertex = [library newFunctionWithName:@"vertex_shader"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"fragment_shader"];
    desc.vertexFunction = vertex;
    desc.fragmentFunction = fragment;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    pipeline_ = [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
    [vertex release];
    [fragment release];
    [library release];
    if (!pipeline_) {
      [presentation_queue_ release];
      presentation_queue_ = nil;
      NSLog(@"Blender presentation pipeline: %@", error);
      presentationFinished();
      return;
    }
  }
  /* nextDrawable may block. It is deliberately never called by UIKit or the engine. */
  id<CAMetalDrawable> drawable = [layer_ nextDrawable];
  if (!drawable || closed_.load() || !visible_.load()) {
    presentationFinished(!drawable);
    return;
  }
  MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = drawable.texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLCommandBuffer> buffer = [presentation_queue_ commandBuffer];
  id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    presentationFinished(true);
    return;
  }
  [encoder setRenderPipelineState:pipeline_];
  [encoder setFragmentTexture:frame->texture atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  [buffer presentDrawable:drawable];
  frame_presented();
  auto self = shared_from_this();
  [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    /* Keep the pool slot immutable until presentation has finished reading it. */
    (void)frame;
    if (completed.status == MTLCommandBufferStatusCompleted && !self->closed_.load()) {
      self->has_presented_frame_.store(true);
    }
    self->presentationFinished(completed.status != MTLCommandBufferStatusCompleted);
  }];
  [buffer commit];
}
