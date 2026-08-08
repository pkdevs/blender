/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_staging_pool.hh"

namespace blender::gpu {

void VKStagingPool::init(size_t buffer_size)
{
  std::scoped_lock lock(mutex_);
  buffer_size_ = buffer_size;
  for (int i = 0; i < pool_buffer_count_; i++) {
    allocate_pool_buffer(buffers_[i]);
  }
  active_index_ = 0;
}

void VKStagingPool::free()
{
  std::scoped_lock lock(mutex_);
  for (int i = 0; i < pool_buffer_count_; i++) {
    buffers_[i].buffer.free();
    buffers_[i].mapped_base = nullptr;
    buffers_[i].offset = 0;
    buffers_[i].in_flight = false;
  }
}

void VKStagingPool::allocate_pool_buffer(PoolBuffer &buf)
{
  buf.buffer.create(buffer_size_,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VMA_MEMORY_USAGE_AUTO,
                    VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                    0.4f,
                    false,
                    "StagingPool");
  buf.mapped_base = buf.buffer.mapped_memory_get();
  buf.capacity = buffer_size_;
  buf.offset = 0;
  buf.timeline_tag = 0;
  buf.in_flight = false;
}

VKStagingAllocation VKStagingPool::sub_allocate(size_t size, size_t alignment)
{
  std::scoped_lock lock(mutex_);

  PoolBuffer &active = buffers_[active_index_];
  size_t aligned_offset = (active.offset + alignment - 1) & ~(alignment - 1);

  if (active.in_flight || aligned_offset + size > active.capacity) {
    return {VK_NULL_HANDLE, 0, nullptr};
  }

  VKStagingAllocation alloc;
  alloc.vk_buffer = active.buffer.vk_handle();
  alloc.offset = aligned_offset;
  alloc.mapped_ptr = static_cast<uint8_t *>(active.mapped_base) + aligned_offset;
  active.offset = aligned_offset + size;
  return alloc;
}

void VKStagingPool::tag_and_rotate(TimelineValue timeline)
{
  std::scoped_lock lock(mutex_);
  PoolBuffer &active = buffers_[active_index_];
  active.timeline_tag = timeline;
  active.in_flight = true;
  active_index_ = (active_index_ + 1) % pool_buffer_count_;
}

void VKStagingPool::reclaim(TimelineValue completed_timeline)
{
  std::scoped_lock lock(mutex_);
  for (int i = 0; i < pool_buffer_count_; i++) {
    if (buffers_[i].in_flight) {
      try_reclaim(buffers_[i], completed_timeline);
    }
  }
}

bool VKStagingPool::try_reclaim(PoolBuffer &buf, TimelineValue completed_timeline)
{
  if (completed_timeline >= buf.timeline_tag) {
    buf.offset = 0;
    buf.in_flight = false;
    return true;
  }
  return false;
}

}  // namespace blender::gpu
