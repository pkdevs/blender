/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Implementation of the FFmpeg Vulkan interop API (see #GPU_vulkan_interop.hh). Exposes the active
 * Vulkan device handles so the imbuf movie code can build an `AVVulkanDeviceContext`.
 */

#ifdef WITH_FFMPEG_VULKAN

#  include "GPU_vulkan_interop.hh"

#  include "GPU_context.hh"

#  include "vk_backend.hh"
#  include "vk_device.hh"

bool GPU_vulkan_ffmpeg_ready()
{
  if (blender::GPU_backend_get_type() != blender::GPU_BACKEND_VULKAN) {
    return false;
  }
  return blender::gpu::VKBackend::get().device.vulkan_ffmpeg_ready();
}

bool GPU_vulkan_ffmpeg_device_info(GPUVulkanFFmpegDeviceInfo *r_info)
{
  if (r_info == nullptr || !GPU_vulkan_ffmpeg_ready()) {
    return false;
  }

  const blender::gpu::VKDevice &device = blender::gpu::VKBackend::get().device;
  r_info->get_instance_proc_addr = device.ffmpeg_get_instance_proc_addr();
  r_info->instance = device.instance_get();
  r_info->physical_device = device.physical_device_get();
  r_info->device = device.vk_handle();
  r_info->queue_family = device.queue_family_get();
  r_info->enabled_extensions = device.ffmpeg_enabled_extensions();
  r_info->enabled_extensions_count = device.ffmpeg_enabled_extensions_count();
  r_info->device_features = device.ffmpeg_device_features();
  return true;
}

#endif /* WITH_FFMPEG_VULKAN */
