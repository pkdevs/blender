/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>

#include "BKE_global.hh"

#include "BLI_index_range.hh"

#include "vk_resource_state_tracker.hh"

namespace blender::gpu::render_graph {

/* -------------------------------------------------------------------- */
/** \name Adding resources
 * \{ */
ResourceHandle VKResourceStateTracker::create_resource_slot()
{
  ResourceHandle handle;
  static Resource new_resource = {};
  if (unused_handles_.is_empty()) {
    handle = resources_.append_and_get_index(new_resource);
  }
  else {
    handle = unused_handles_.pop_last();
    resources_[handle] = new_resource;
  }

  return handle;
}

ResourceHandle VKResourceStateTracker::add_image(VkImage vk_image,
                                                 bool use_subresource_tracking,
                                                 VKResourceBarrierState barrier_state,
                                                 const char *name)
{
  UNUSED_VARS_NDEBUG(name);
  std::scoped_lock lock(mutex);
  BLI_assert_msg(!image_resources_.contains(vk_image),
                 "Image resource is added twice to the render graph.");
  ResourceHandle handle = create_resource_slot();
  Resource &resource = resources_[handle];
  image_resources_.add_new(vk_image, handle);

  resource = {
      .type = VKResourceType::IMAGE,
      .image = {.vk_image = vk_image, .use_subresource_tracking = use_subresource_tracking},
      .ref_count = 1,
      .stamp = 0,
      .barrier_state = barrier_state,
  };

#ifndef NDEBUG
  if (name) {
    resource.name = name;
  }
#endif
  return handle;
}

ResourceHandle VKResourceStateTracker::add_image(VkImage vk_image,
                                                 bool use_subresource_tracking,
                                                 const char *name)
{
  return add_image(vk_image, use_subresource_tracking, {}, name);
}

ResourceHandle VKResourceStateTracker::add_aliased_image(VkImage vk_image,
                                                         bool use_subresource_tracking,
                                                         const char *name)
{
  return add_image(vk_image,
                   use_subresource_tracking,
                   {VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED},
                   name);
}

ResourceHandle VKResourceStateTracker::add_swapchain_image(VkImage vk_image, const char *name)
{
  return add_image(vk_image,
                   false,
                   {
                       VK_ACCESS_NONE,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   },
                   name);
}

ResourceHandle VKResourceStateTracker::add_buffer(VkBuffer vk_buffer, const char *name)
{
  UNUSED_VARS_NDEBUG(name);
  std::scoped_lock lock(mutex);
  ResourceHandle handle = create_resource_slot();
  Resource &resource = resources_[handle];

  resource = {
      .type = VKResourceType::BUFFER,
      .buffer = {.vk_buffer = vk_buffer},
      .ref_count = 1,
      .stamp = 0,
      .barrier_state = {},
  };

#ifndef NDEBUG
  if (name) {
    resource.name = name;
  }
#endif

  return handle;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image layout
 * \{ */

void VKResourceStateTracker::update_image_layout(VkImage vk_image, VkImageLayout vk_image_layout)
{
  std::scoped_lock lock(mutex);
  ResourceHandle handle = image_resources_.lookup(vk_image);
  Resource &resource = get_image_resource(handle);
  resource.barrier_state.image_layout = vk_image_layout;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove resources
 * \{ */

void VKResourceStateTracker::discard_image(ResourceHandle image_handle)
{
  std::scoped_lock lock(mutex);
  Resource &resource = get_image_resource(image_handle);
  BLI_assert(resource.ref_count != 0);
  resource.ref_count -= 1;
}

void VKResourceStateTracker::discard_buffer(ResourceHandle buffer_handle)
{
  std::scoped_lock lock(mutex);
  Resource &resource = get_buffer_resource(buffer_handle);
  BLI_assert(resource.ref_count != 0);
  resource.ref_count -= 1;
}

VkBuffer VKResourceStateTracker::remove_buffer(ResourceHandle buffer_handle)
{
  std::scoped_lock lock(mutex);
  BLI_assert(resources_[buffer_handle].ref_count == 0);
  VkBuffer vk_buffer = get_buffer_resource(buffer_handle).buffer.vk_buffer;
  resources_[buffer_handle] = {
    .type = VKResourceType::NONE, .buffer = {.vk_buffer = VK_NULL_HANDLE}, .ref_count = 0,
  .stamp = 0,
};
unused_handles_.append(buffer_handle);

return vk_buffer;
}  // namespace blender::gpu::render_graph

void VKResourceStateTracker::remove_image(ResourceHandle image_handle)
{
  std::scoped_lock lock(mutex);
  BLI_assert(resources_[image_handle].ref_count == 0);
  VkImage vk_image = get_image_resource(image_handle).image.vk_image;
  // TODO: this is unique compared to buffers.
  image_resources_.pop(vk_image);
  resources_[image_handle] = {
      .type = VKResourceType::NONE,
      .image = {.vk_image = VK_NULL_HANDLE, .use_subresource_tracking = false},
      .ref_count = 0,
      .stamp = 0,
  };
  unused_handles_.append(image_handle);
}

/** \} */

void VKResourceStateTracker::debug_print() const
{
  std::ostream &os = std::cout;
  os << "VKResourceStateTracker\n";
  os << " resources=(" << resources_.size() << "/" << resources_.capacity() << ")\n";
  os << " images=(" << image_resources_.size() << "/" << image_resources_.capacity() << ")\n";
  os << " unused=(" << unused_handles_.size() << "/" << unused_handles_.capacity() << ")\n";
}

}  // namespace blender::gpu::render_graph
