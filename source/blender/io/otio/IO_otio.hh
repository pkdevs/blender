/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "BLI_path_utils.hh"

#include "DNA_windowmanager_enums.h"

namespace blender {

struct bContext;
struct ReportList;

namespace io::otio {
enum scene_strip_resolution {
  SCENE_STRIP_25_PERCENT,
  SCENE_STRIP_50_PERCENT,
  SCENE_STRIP_75_PERCENT,
  SCENE_STRIP_100_PERCENT,
};

enum export_fallback {
  FALLBACK_IMG_SEQUENCE_RENAME,
  FALLBACK_IMG_SEQUENCE_SYMLINK,
};

}  // namespace io::otio

struct OTIOExportParams {
  /* Full path to the to-be-saved OTIO file. */
  char filepath[FILE_MAX] = "";

  /* Scene Strip Options. */
  bool bake_scene_strips = true;
  io::otio::scene_strip_resolution scene_strip_res = io::otio::SCENE_STRIP_100_PERCENT;

  /* Fallback Options. */
  io::otio::export_fallback img_sequence_fallback = io::otio::FALLBACK_IMG_SEQUENCE_RENAME;

  ReportList *reports = nullptr;
};

wmOperatorStatus OTIO_export(bContext *C, const OTIOExportParams *export_params);

}  // namespace blender
