/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_context.hh"

#include "WM_api.hh";

#include "IO_otio.hh"
#include "otio_export.hh"

namespace blender {

using namespace io::otio;

wmOperatorStatus OTIO_export(const bContext *C,
                             const char *filepath,
                             const OTIOExportParams *export_params)
{
  ExportJobData *job_data = MEM_new<ExportJobData>("OTIO export job data");
  job_data->scene = CTX_data_sequencer_scene(C);
  job_data->params = *export_params;
  STRNCPY(job_data->filepath, filepath);

  wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                              CTX_wm_window(C),
                              job_data->scene,
                              "Exporting OTIO...",
                              WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                              WM_JOB_TYPE_OTIO_EXPORT);

  WM_jobs_customdata_set(
      wm_job, job_data, [](void *data) { MEM_delete(static_cast<ExportJobData *>(data)); });

  WM_jobs_timer(wm_job, 0.1, NC_SCENE | ND_FRAME, NC_SCENE | ND_FRAME);
  WM_jobs_callbacks(wm_job, otio_export_job_start, nullptr, nullptr, nullptr);
  WM_jobs_start(CTX_wm_manager(C), wm_job);

  return OPERATOR_FINISHED;
}
}  // namespace blender
