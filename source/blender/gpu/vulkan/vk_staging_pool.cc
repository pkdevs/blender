/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_staging_pool.hh"

#include <limits>

#include "MEM_guardedalloc.h"

namespace blender::gpu {

void VKStagingPool::init(size_t buffer_size)
{
  std::scoped_lock lock(mutex_);
  buffer_size_ = buffer_size;
  free_buffers_.append(new_pool_buffer());
  free_buffers_.append(new_pool_buffer());
}

void VKStagingPool::free()
{
  std::scoped_lock lock(mutex_);
  timeline_buffers_.remove_old(std::numeric_limits<TimelineValue>::max(),
                                [&](PoolBuffer *buf) { free_buffers_.append(buf); });
  if (active_buffer_) {
    free_buffers_.append(active_buffer_);
    active_buffer_ = nullptr;
  }
  for (PoolBuffer *buf : free_buffers_) {
    buf->buffer.free();
    MEM_delete(buf);
  }
  free_buffers_.clear();
}

VKStagingPool::PoolBuffer *VKStagingPool::new_pool_buffer()
{
  PoolBuffer *buf = MEM_new<PoolBuffer>(__func__);
  buf->buffer.create(buffer_size_,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_MAPPED_BIT |
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                      0.4f,
                      false,
                      "StagingPool");
  buf->mapped_base = buf->buffer.mapped_memory_get();
  buf->capacity = buffer_size_;
  buf->offset = 0;
  return buf;
}

VKStagingAllocation VKStagingPool::sub_allocate(size_t size, size_t alignment)
{
  std::scoped_lock lock(mutex_);

  if (active_buffer_) {
    size_t aligned_offset = (active_buffer_->offset + alignment - 1) & ~(alignment - 1);
    if (aligned_offset + size <= active_buffer_->capacity) {
      VKStagingAllocation alloc;
      alloc.vk_buffer = active_buffer_->buffer.vk_handle();
      alloc.offset = aligned_offset;
      alloc.mapped_ptr = static_cast<uint8_t *>(active_buffer_->mapped_base) + aligned_offset;
      active_buffer_->offset = aligned_offset + size;
      return alloc;
    }
    /* Active buffer is full for this generation. It stays alive (not yet submitted) and
     * will be tagged with a timeline value once submit() is called. */
    used_buffers_.append(active_buffer_);
    active_buffer_ = nullptr;
  }

  if (size > buffer_size_) {
    /* Allocation larger than a single pool buffer: caller falls back to a plain allocation. */
    return {VK_NULL_HANDLE, 0, nullptr};
  }

  active_buffer_ = free_buffers_.is_empty() ? new_pool_buffer() : free_buffers_.pop_last();

  VKStagingAllocation alloc;
  alloc.vk_buffer = active_buffer_->buffer.vk_handle();
  alloc.offset = 0;
  alloc.mapped_ptr = active_buffer_->mapped_base;
  active_buffer_->offset = size;
  return alloc;
}

void VKStagingPool::submit(TimelineValue timeline)
{
  std::scoped_lock lock(mutex_);
  for (PoolBuffer *buf : used_buffers_) {
    timeline_buffers_.append_timeline(timeline, buf);
  }
  used_buffers_.clear();
  if (active_buffer_) {
    timeline_buffers_.append_timeline(timeline, active_buffer_);
    active_buffer_ = nullptr;
  }
}

void VKStagingPool::reclaim(TimelineValue completed_timeline)
{
  std::scoped_lock lock(mutex_);
  timeline_buffers_.remove_old(completed_timeline, [&](PoolBuffer *buf) {
    buf->offset = 0;
    free_buffers_.append(buf);
  });
}

}  // namespace blender::gpu
