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
struct Scene;
struct Main;
struct wmOperator;

namespace io::otio {

enum class SceneStripRes {
  PERCENT_25,
  PERCENT_50,
  PERCENT_75,
  PERCENT_100,
};

enum class ExportOption {
  DEFAULT,
  RENDER_MOVIE,
  MISSING_REFERENCE,
};

enum class ImgSeqFallback {
  RENAME,
  SYMLINK,
  RENDER_MOVIE,
};

short get_scene_strip_resolution_percent(SceneStripRes resolution);

}  // namespace io::otio

struct OTIOExportParams {
  /* Scene Strip Options. */
  bool bake_scene_strips = true;
  io::otio::SceneStripRes scene_strip_res = io::otio::SceneStripRes::PERCENT_100;

  /* Export Options. */
  io::otio::ExportOption img_sequence_export = io::otio::ExportOption::DEFAULT;
  io::otio::ImgSeqFallback img_sequence_fallback = io::otio::ImgSeqFallback::SYMLINK;

  io::otio::ExportOption meta_strip_export = io::otio::ExportOption::DEFAULT;
};

namespace io::otio {

struct ExportJobData {
  Main *bmain;
  Scene *scene;

  char filepath[FILE_MAX];
  OTIOExportParams params;
};

}  // namespace io::otio

wmOperatorStatus OTIO_export(const bContext *C,
                             wmOperator *op,
                             const char *filepath,
                             const OTIOExportParams *export_params);

}  // namespace blender
