/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */
#include "BLI_assert.hh"
#include "vk_shader.hh"
#include "vk_shader_interface.hh"
#include "vk_staging_buffer.hh"
#include "vk_state_manager.hh"
#include "vk_vertex_buffer.hh"

#include "vk_storage_buffer.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace gpu {

VKStorageBuffer::VKStorageBuffer(size_t size, GPUUsageType usage, const char *name)
    : StorageBuf(size, name), usage_(usage)
{
  UNUSED_VARS(usage_);
}
VKStorageBuffer::~VKStorageBuffer()
{
  if (read_buffer_) {
    MEM_delete(read_buffer_);
    read_buffer_ = nullptr;
  }
}

void VKStorageBuffer::update(const void *data)
{
  VKContext &context = *VKContext::get();
  ensure_allocated();
  if (!buffer_.is_allocated()) {
    CLOG_WARN(&LOG,
              "Unable to upload data to storage buffer as the storage buffer could not be "
              "allocated on GPU.");
    return;
  }

  if (usage_ == GPU_USAGE_STREAM) {
    const VKDevice &device = VKBackend::get().device;
    VKStreamingBuffer &streaming_buffer = *context.get_or_create_streaming_buffer(
        buffer_, device.physical_device_properties_get().limits.minStorageBufferOffsetAlignment);
    offset_ = streaming_buffer.update(context, data, usage_size_in_bytes_);
    return;
  }

  VKStagingBuffer staging_buffer(
      buffer_, VKStagingBuffer::Direction::HostToDevice, 0, usage_size_in_bytes_);
  VKBuffer &buffer = staging_buffer.host_buffer_get();
  if (buffer.is_allocated()) {
    buffer.update_immediately(data);
    staging_buffer.copy_to_device(context);
  }
  else {
    CLOG_ERROR(
        &LOG,
        "Unable to upload data to storage buffer via a staging buffer as the staging buffer "
        "could not be allocated. Storage buffer will be filled with on zeros to reduce "
        "drawing artifacts due to read from uninitialized memory.");
    buffer_.clear(context, 0u);
  }
}

void VKStorageBuffer::ensure_allocated()
{
  if (!buffer_.is_allocated()) {
    allocate();
  }
}

void VKStorageBuffer::allocate()
{
  const VkBufferUsageFlags buffer_usage_flags = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_.create(size_in_bytes_,
                 buffer_usage_flags,
                 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                 VmaAllocationCreateFlags(0),
                 0.8f,
                 false,
                 name_);
}

void VKStorageBuffer::bind(int slot)
{
  VKContext &context = *VKContext::get();
  context.state_manager_get().storage_buffer_bind(
      BindSpaceStorageBuffers::Type::StorageBuffer, this, slot, offset_);
}

void VKStorageBuffer::unbind()
{
  VKContext *context = VKContext::get();
  if (context) {
    context->state_manager_get().storage_buffer_unbind(this);
  }
}

void VKStorageBuffer::clear(uint32_t clear_value)
{
  ensure_allocated();
  VKContext &context = *VKContext::get();
  buffer_.clear(context, clear_value);
}

void VKStorageBuffer::copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size)
{
  ensure_allocated();

  VKVertexBuffer &src_vertex_buffer = *unwrap(src);
  src_vertex_buffer.upload();

  render_graph::VKCopyBufferNode::CreateInfo copy_buffer = {};
  copy_buffer.src_buffer = src_vertex_buffer.resource();
  copy_buffer.dst_buffer = resource();
  copy_buffer.region.srcOffset = src_offset;
  copy_buffer.region.dstOffset = dst_offset;
  copy_buffer.region.size = copy_size;

  VKContext &context = *VKContext::get();
  context.render_graph().add_node(copy_buffer);
}

void VKStorageBuffer::ensure_read_buffer()
{
  if (read_buffer_) {
    return;
  }

  read_buffer_ = MEM_new<VKStagingBuffer>(
      __func__, buffer_, VKStagingBuffer::Direction::DeviceToHost);
}

void VKStorageBuffer::async_flush_to_host()
{
  if (async_in_progress_) {
    return;
  }
  ensure_allocated();
  async_in_progress_ = true;
  /* Mapped buffers don't need a read buffer. */
  if (buffer_.is_mapped()) {
    return;
  }
  VKContext &context = *VKContext::get();
  ensure_read_buffer();
  read_buffer_->copy_from_device(context);
}

void VKStorageBuffer::read(void *data)
{
  VKContext &context = *VKContext::get();
  if (async_in_progress_) {
    if (buffer_.is_mapped()) {
      buffer_.read_async(context, data);
    }
    else {
      BLI_assert(read_buffer_);
      read_buffer_->host_buffer_get().read_async(context, data);
    }
  }
  else {
    if (buffer_.is_mapped()) {
      buffer_.read(context, data);
    }
    else {
      ensure_read_buffer();
      read_buffer_->copy_from_device(context);
      read_buffer_->host_buffer_get().read(context, data);
    }
  }
}

}  // namespace gpu
}  // namespace blender
