/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_mutex.hh"

#include "vk_buffer.hh"
#include "vk_common.hh"

namespace blender::gpu {

struct VKStagingAllocation {
  VkBuffer vk_buffer;
  size_t offset;
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
  void tag_and_rotate(TimelineValue timeline);
  void reclaim(TimelineValue completed_timeline);

 private:
  struct PoolBuffer {
    VKBuffer buffer;
    size_t capacity = 0;
    size_t offset = 0;
    TimelineValue timeline_tag = 0;
    void *mapped_base = nullptr;
    bool in_flight = false;
  };

  static constexpr int pool_buffer_count_ = 2;
  PoolBuffer buffers_[pool_buffer_count_];
  int active_index_ = 0;
  size_t buffer_size_ = 0;
  Mutex mutex_;

  bool try_reclaim(PoolBuffer &buf, TimelineValue completed_timeline);
  void allocate_pool_buffer(PoolBuffer &buf);
};

}  // namespace blender::gpu
