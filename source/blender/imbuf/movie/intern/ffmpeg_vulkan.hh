/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * Helpers to hand Blender's Vulkan device to FFmpeg's libavutil so that the native Vulkan-compute
 * decoders (ProRes, ProRes RAW, FFV1, APV, DPX) run hardware accelerated.
 */

#pragma once

#ifdef WITH_FFMPEG

struct AVBufferRef;
struct AVCodec;
struct AVCodecContext;

namespace blender {

#ifdef WITH_FFMPEG_VULKAN

/**
 * If hardware Vulkan decoding is available for `codec`, attach a Vulkan `AVHWDeviceContext` wrapping
 * Blender's active Vulkan device to `codec_ctx` (sets `hw_device_ctx` and `get_format`) and return
 * true; `*r_hw_device_ctx` receives a new reference that the caller owns (free with
 * `av_buffer_unref`). Otherwise leave `codec_ctx` untouched and return false, meaning the caller
 * must decode in software.
 *
 * Must be called before `avcodec_open2()`.
 */
bool movie_ffmpeg_vulkan_attach(AVCodecContext *codec_ctx,
                                const AVCodec *codec,
                                AVBufferRef **r_hw_device_ctx);

/** Release the process-global cached Vulkan hardware device. Call once at exit. */
void movie_ffmpeg_vulkan_exit();

#endif /* WITH_FFMPEG_VULKAN */

}  // namespace blender

#endif /* WITH_FFMPEG */
