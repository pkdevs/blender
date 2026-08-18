/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_mutex.hh"
#include "BLI_vector.hh"

#include "vk_buffer.hh"
#include "vk_common.hh"
#include "vk_resource_pool.hh"

namespace blender::gpu {

struct VKStagingAllocation {
  VKResourceWithHandle<VkBuffer> vk_buffer;
  size_t offset;
  size_t size;
  void *mapped_ptr;

  bool is_valid() const
  {
    return mapped_ptr != nullptr;
  }
};

class VKStagingPool {
 public:
  void init(size_t buffer_size = 256 * 1024 * 1024);
  void free();

  VKStagingAllocation sub_allocate(size_t size, size_t alignment = 16);
  void submit(TimelineValue timeline);
  void reclaim(TimelineValue completed_timeline);

 private:
  struct PoolBuffer {
    VKBuffer buffer;
    size_t capacity = 0;
    size_t offset = 0;
    void *mapped_base = nullptr;
  };

  Vector<PoolBuffer *> free_buffers_;
  PoolBuffer *active_buffer_ = nullptr;
  /** Buffers filled during the current, not yet submitted generation. */
  Vector<PoolBuffer *> used_buffers_;
  TimelineResources<PoolBuffer *> timeline_buffers_;
  size_t buffer_size_ = 0;
  Mutex mutex_;

  PoolBuffer *new_pool_buffer();
};

}  // namespace blender::gpu
