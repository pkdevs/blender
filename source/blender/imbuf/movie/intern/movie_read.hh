/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2024-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#pragma once

#include <cstdint>

#include "IMB_imbuf_enums.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

#ifdef WITH_FFMPEG

extern "C" {
#  include <libavutil/rational.h>
}

#endif

namespace blender {

struct IDProperty;

struct MovieReader {
  enum class State { Uninitialized, Failed, Valid };
  ImBufFlags ib_flags = ImBufFlags::Zero;
  State state = State::Uninitialized;
  int cur_position = 0; /* index  0 = 1e,  1 = 2e, enz. */
  int duration_in_frames = 0;
  int frs_sec = 0;
  double frs_sec_base = 0.0;
  double start_offset = 0.0;
  int x = 0;
  int y = 0;
  int video_rotation = 0;

  /* for number */
  char filepath[/*FILE_MAX*/ 1024] = {};

  int streamindex = 0;

  bool keep_original_colorspace = false;

#ifdef WITH_FFMPEG
  AVFormatContext *pFormatCtx = nullptr;
  AVCodecContext *pCodecCtx = nullptr;
  const AVCodec *pCodec = nullptr;
  AVFrame *pFrameRGB = nullptr;
  AVFrame *pFrameDeinterlaced = nullptr;
  SwsContext *img_convert_ctx = nullptr;
  int videoStream = 0;

  AVFrame *pFrame = nullptr;
  bool pFrame_complete = false;
  AVFrame *pFrame_backup = nullptr;
  bool pFrame_backup_complete = false;

  int64_t cur_pts = 0;
  int64_t cur_key_frame_pts = 0;
  AVPacket *cur_packet = nullptr;

  AVRational frame_rate = {1, 1};

  bool seek_before_decode = false;
  bool is_float = false;

#  ifdef WITH_FFMPEG_VULKAN
  /* Hardware (Vulkan compute) decoding. When #is_hwaccel is set, decoded frames come out as
   * AV_PIX_FMT_VULKAN and are downloaded into #hw_sw_frame (a software frame in
   * #hw_download_format) before the usual swscale conversion. */
  bool is_hwaccel = false;
  /* Set true if a strip that attached a Vulkan device (#is_hwaccel) nonetheless produced a software
   * frame - i.e. the codec advertised a Vulkan config but fell back to software (e.g. H.264/HEVC,
   * which need a video-decode queue we do not provide). Hardware decoding is then considered active
   * when `is_hwaccel && !hw_decode_fallback`: optimistic until a fallback frame is actually seen, so
   * the first original frame is decoded (rather than a proxy) to discover the codec's real path. */
  bool hw_decode_fallback = false;
  /* Owned reference to the shared Vulkan AVHWDeviceContext. */
  AVBufferRef *hw_device_ctx = nullptr;
  /* Reusable destination for av_hwframe_transfer_data. */
  AVFrame *hw_sw_frame = nullptr;
  /* The software pixel format (an AVPixelFormat) that hardware frames are downloaded into. */
  int hw_download_format = -1 /*AV_PIX_FMT_NONE*/;
#  endif

  /* When set, never seek within the video, and only ever decode one frame.
   * This is a workaround for some Ogg files that have full audio but only
   * one frame of "album art" as a video stream in non-Theora format.
   * ffmpeg crashes/aborts when trying to seek within them
   * (https://trac.ffmpeg.org/ticket/10755). */
  bool never_seek_decode_one_frame = false;
#endif

  char proxy_dir[768] = {};

  int proxies_tried = 0;

  MovieReader *proxy_anim[IMB_PROXY_MAX_SLOT] = {};

  char colorspace[/*MAX_COLORSPACE_NAME*/ 64] = {};
  /** The maximum name from multi-view. */
  char suffix[/*MAX_NAME*/ 64] = {};

  IDProperty *metadata = nullptr;
};

}  // namespace blender
