/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#ifdef WITH_FFMPEG

#  include "ffmpeg_vulkan.hh"

#  ifdef WITH_FFMPEG_VULKAN

#    include <mutex>

#    include "BLI_mutex.hh"

#    include "CLG_log.h"

#    include "GPU_vulkan_interop.hh"

extern "C" {
#    include <libavcodec/avcodec.h>
#    include <libavutil/error.h>
#    include <libavutil/hwcontext.h>
#    include <libavutil/hwcontext_vulkan.h>
#    include <libavutil/pixfmt.h>
}

/* FFmpeg's Vulkan compute decoders (ProRes, ProRes RAW, FFV1, APV, DPX) are only relied upon from
 * FFmpeg 9 (libavcodec 63) onwards; the support in FFmpeg 8 and earlier was too early to enable.
 * Below that, software-fallback stubs are compiled instead so decoding always uses the CPU path. */
#    if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(63, 0, 100)

/* The decoder's `get_format` callback. Declared with C language linkage to match the
 * `AVCodecContext::get_format` function pointer type. */
extern "C" {
static AVPixelFormat blender_ffmpeg_vulkan_get_format(AVCodecContext * /*codec_ctx*/,
                                                      const AVPixelFormat *formats)
{
  for (const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == AV_PIX_FMT_VULKAN) {
      return AV_PIX_FMT_VULKAN;
    }
  }
  /* Vulkan was not offered for this stream: fall back to the first (software) format. Never return
   * AV_PIX_FMT_NONE, which would make the decoder fail outright. */
  return formats[0];
}
}

namespace blender {

static CLG_LogRef LOG = {"video.read"};

/* Process-global cached Vulkan hardware device, built lazily on first use and shared by all movie
 * readers (the device is the same for the whole process). */
static Mutex vulkan_device_mutex;
static AVBufferRef *vulkan_device_ctx = nullptr;
static bool vulkan_device_initialized = false;

/* Build (once) the AVHWDeviceContext wrapping Blender's active Vulkan device. Returns a reference
 * owned by the cache (do not unref), or nullptr when hardware decoding is unavailable. The caller
 * must hold #vulkan_device_mutex. */
static AVBufferRef *vulkan_device_get_locked()
{
  if (vulkan_device_initialized) {
    return vulkan_device_ctx;
  }
  vulkan_device_initialized = true;

  GPUVulkanFFmpegDeviceInfo info = {};
  if (!GPU_vulkan_ffmpeg_device_info(&info)) {
    /* Active backend is not Vulkan, or the device cannot satisfy FFmpeg's requirements. */
    return nullptr;
  }

  AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
  if (ref == nullptr) {
    /* FFmpeg was built without Vulkan support. */
    CLOG_INFO(&LOG, "FFmpeg has no Vulkan hwdevice support, using software video decoding");
    return nullptr;
  }

  AVHWDeviceContext *hwdev = reinterpret_cast<AVHWDeviceContext *>(ref->data);
  AVVulkanDeviceContext *vkdev = static_cast<AVVulkanDeviceContext *>(hwdev->hwctx);
  vkdev->get_proc_addr = info.get_instance_proc_addr;
  vkdev->inst = info.instance;
  vkdev->phys_dev = info.physical_device;
  vkdev->act_dev = info.device;
  if (info.device_features != nullptr) {
    vkdev->device_features = *info.device_features;
  }
  vkdev->enabled_inst_extensions = nullptr;
  vkdev->nb_enabled_inst_extensions = 0;
  vkdev->enabled_dev_extensions = info.enabled_extensions;
  vkdev->nb_enabled_dev_extensions = int(info.enabled_extensions_count);

  /* A single queue family providing both compute and transfer; this satisfies the compute decoders
   * (ProRes/ProRes RAW/FFV1/APV/DPX), which need no dedicated video-decode queue. Blender created
   * the queue as internally synchronized and lists VK_KHR_internally_synchronized_queues among the
   * enabled extensions, so libavutil skips its own queue locking and shares the queue safely. */
  vkdev->qf[0].idx = int(info.queue_family);
  vkdev->qf[0].num = 1;
  vkdev->qf[0].flags = VkQueueFlagBits(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT);
  vkdev->qf[0].video_caps = VkVideoCodecOperationFlagBitsKHR(0);
  vkdev->nb_qf = 1;

  const int ret = av_hwdevice_ctx_init(ref);
  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(err, sizeof(err), ret);
    CLOG_WARN(&LOG,
              "Failed to initialize FFmpeg Vulkan hwdevice (%s), using software video decoding",
              err);
    av_buffer_unref(&ref);
    return nullptr;
  }

  vulkan_device_ctx = ref;
  CLOG_INFO(&LOG, "Initialized FFmpeg Vulkan hardware video decoding device");
  return vulkan_device_ctx;
}

static bool codec_has_vulkan_hwconfig(const AVCodec *codec)
{
  for (int i = 0;; i++) {
    const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
    if (config == nullptr) {
      return false;
    }
    if (config->device_type == AV_HWDEVICE_TYPE_VULKAN &&
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
    {
      return true;
    }
  }
}

bool movie_ffmpeg_vulkan_attach(AVCodecContext *codec_ctx,
                                const AVCodec *codec,
                                AVBufferRef **r_hw_device_ctx)
{
  *r_hw_device_ctx = nullptr;

  if (!codec_has_vulkan_hwconfig(codec)) {
    return false;
  }

  std::scoped_lock lock(vulkan_device_mutex);
  AVBufferRef *device = vulkan_device_get_locked();
  if (device == nullptr) {
    return false;
  }

  codec_ctx->hw_device_ctx = av_buffer_ref(device);
  if (codec_ctx->hw_device_ctx == nullptr) {
    return false;
  }
  codec_ctx->get_format = blender_ffmpeg_vulkan_get_format;
  *r_hw_device_ctx = av_buffer_ref(device);
  return true;
}

void movie_ffmpeg_vulkan_exit()
{
  std::scoped_lock lock(vulkan_device_mutex);
  if (vulkan_device_ctx != nullptr) {
    av_buffer_unref(&vulkan_device_ctx);
  }
  vulkan_device_initialized = false;
}

}  // namespace blender

#    else /* FFmpeg < 9: Vulkan compute decode is not enabled. */

namespace blender {

bool movie_ffmpeg_vulkan_attach(AVCodecContext * /*codec_ctx*/,
                                const AVCodec * /*codec*/,
                                AVBufferRef **r_hw_device_ctx)
{
  *r_hw_device_ctx = nullptr;
  return false;
}

void movie_ffmpeg_vulkan_exit() {}

}  // namespace blender

#    endif /* LIBAVCODEC_VERSION_INT >= 63 */

#  endif /* WITH_FFMPEG_VULKAN */

#endif /* WITH_FFMPEG */
