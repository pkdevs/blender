/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Interop helpers that expose Blender's active Vulkan device so it can be handed to FFmpeg's
 * libavutil Vulkan hardware context (#AV_HWDEVICE_TYPE_VULKAN) for hardware accelerated video
 * decoding. Only available in builds configured with `WITH_FFMPEG_VULKAN`.
 */

#pragma once

#ifdef WITH_FFMPEG_VULKAN

#  include <vulkan/vulkan_core.h>

/**
 * Handles and metadata describing Blender's active Vulkan device, sufficient to populate an
 * `AVVulkanDeviceContext` (see `libavutil/hwcontext_vulkan.h`).
 */
struct GPUVulkanFFmpegDeviceInfo {
  /** `vkGetInstanceProcAddr` loader for libavutil to load Vulkan functions with. */
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  /** Generic graphics+compute queue family, used for FFmpeg's compute (and transfer) queues. */
  uint32_t queue_family;
  /** Device extensions enabled at device creation (persistent storage owned by GHOST). */
  const char *const *enabled_extensions;
  uint32_t enabled_extensions_count;
  /** Feature chain describing the features enabled at device creation (owned by GHOST). */
  const VkPhysicalDeviceFeatures2 *device_features;
};

/**
 * Returns true when the active GPU backend is Vulkan and its device was created with everything
 * FFmpeg's Vulkan compute decoders require. When false, video decoding must use the software path.
 */
bool GPU_vulkan_ffmpeg_ready();

/**
 * Fills `r_info` with the active Vulkan device handles. Returns true on success, or false when the
 * active backend is not Vulkan or the device is not FFmpeg-ready (in which case `r_info` is left
 * untouched).
 */
bool GPU_vulkan_ffmpeg_device_info(GPUVulkanFFmpegDeviceInfo *r_info);

#endif /* WITH_FFMPEG_VULKAN */
