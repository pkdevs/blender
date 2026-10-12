/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

/** Shared lifetime, not shared Blender state. UIKit publishes geometry and visibility;
 * the engine submits copies; a separate queue presents only completed immutable frames. */
class GHOST_IOSSurface : public std::enable_shared_from_this<GHOST_IOSSurface> {
  struct Frame {
    id<MTLTexture> texture = nil;
    ~Frame();
  };
  CAMetalLayer *layer_;
  id<MTLDevice> device_;
  id<MTLCommandQueue> presentation_queue_ = nil;
  id<MTLRenderPipelineState> pipeline_ = nil;
  dispatch_queue_t presenter_;
  std::atomic<bool> scheduled_{false};
  std::atomic<bool> dirty_{false};
  std::atomic<bool> visible_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> has_presented_frame_{false};
  std::atomic<uint64_t> size_{(uint64_t(1) << 32) | 1};
  std::mutex frame_mutex_;
  std::shared_ptr<Frame> completed_;
  /* Pool access is engine-only. References held by GPU completion/presentation prevent reuse:
   * the frames in flight, the newest completed one and the one being presented. */
  std::shared_ptr<Frame> pool_[4];

  void present();
  void presentationFinished(bool retry = false);

 public:
  explicit GHOST_IOSSurface(CAMetalLayer *layer);
  ~GHOST_IOSSurface();
  id<MTLDevice> device() const
  {
    return device_;
  }
  CGSize size() const;
  void resize(CGSize size);
  void setVisible(bool visible);
  void close();
  void requestPresent();
  bool hasPresentedFrame() const
  {
    return has_presented_frame_.load();
  }
  /** False means backpressure: retry the latest rendered frame on the engine's next iteration. */
  bool publish(id<MTLCommandQueue> rendering_queue, id<MTLTexture> source);
};
