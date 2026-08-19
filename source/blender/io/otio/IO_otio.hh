/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "BLI_path_utils.hh"

namespace blender {

struct bContext;
struct ReportList;
struct Scene;
struct Main;
struct wmOperator;

namespace io::otio {

enum class SceneStripRes {
  Percent25,
  Percent50,
  Percent75,
  Percent100,
};

enum class ImgSeqFallback {
  Rename,
  Symlink,
  RenderMovie,
};

short get_scene_strip_resolution_percent(SceneStripRes resolution);

}  // namespace io::otio

struct OTIOExportParams {
  /* Scene Strip Options. */
  bool bake_scene_strips = true;
  io::otio::SceneStripRes scene_strip_res = io::otio::SceneStripRes::Percent100;

  /* Image Sequence Options. */
  io::otio::ImgSeqFallback img_sequence_fallback = io::otio::ImgSeqFallback::Symlink;
};

namespace io::otio {

struct ExportJobData {
  Main *bmain;
  Scene *scene;

  char filepath[FILE_MAX];
  OTIOExportParams params;
};

}  // namespace io::otio

void OTIO_export(const bContext *C, const char *filepath, const OTIOExportParams *export_params);

bool OTIO_validate_timeline_blender(ReportList *reports, const Scene *scene);

}  // namespace blender
