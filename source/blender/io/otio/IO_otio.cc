/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include "BKE_context.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"

#include "CLG_log.h"
#include "DNA_listBase.h"
#include "DNA_sequence_types.h"

#include "SEQ_effects.hh"
#include "SEQ_sequencer.hh"

#include "WM_api.hh"

#include "IO_otio.hh"
#include "otio_export.hh"

namespace blender {
namespace io::otio {
short get_scene_strip_resolution_percent(SceneStripRes resolution)
{
  switch (resolution) {
    case SceneStripRes::Percent25:
      return 25;

    case SceneStripRes::Percent50:
      return 50;

    case SceneStripRes::Percent75:
      return 75;

    case SceneStripRes::Percent100:
      return 100;

    default:
      return 100;
  }
}
}  // namespace io::otio

using namespace io::otio;

void OTIO_export(const bContext *C, const char *filepath, const OTIOExportParams *export_params)
{
  ExportJobData *job_data = MEM_new<ExportJobData>("OTIO export job data");
  job_data->bmain = CTX_data_main(C);
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
}

static bool validate_transitions(ReportList *reports, ListBaseT<Strip> *seqbase)
{
  for (Strip &strip : *seqbase) {
    if (!seq::effect_is_transition(strip.type)) {
      continue;
    }
    if (!strip.input1 || !strip.input2) {
      CLOG_ERROR(&LOG,
                 "The Effect Strip '%s' have insufficient inputs. input1 = '%s' ; input2 = '%s'",
                 strip.name + 2,
                 strip.input1 ? strip.input1->name + 2 : "nullptr",
                 strip.input2 ? strip.input2->name + 2 : "nullptr");
      BKE_report(reports, RPT_ERROR, "Insufficient Inputs Transition Strip");
      return false;
    }

    /* All three strips (input1, transition strip and input2) should be on the same channel. */
    if (strip.channel != strip.input1->channel || strip.channel != strip.input2->channel) {
      CLOG_ERROR(&LOG,
                 "The input1 '%s', input2 '%s' and the Transition Strip '%s' are not placed in "
                 "the same Channel",
                 strip.input1->name + 2,
                 strip.input2->name + 2,
                 strip.name + 2);
      BKE_report(reports,
                 RPT_ERROR,
                 "The Transition and the Input Strips Should be placed on the Same Channel");
      return false;
    }
  }
  return true;
}

bool OTIO_validate_timeline_blender(ReportList *reports, const Scene *scene)
{
  Editing *ed = seq::editing_get(scene);
  if (!scene || !ed) {
    BKE_report(reports, RPT_ERROR, "No Sequencer Scene found");
    return false;
  }

  ListBaseT<Strip> *seqbase = &ed->seqbase;

  if (!validate_transitions(reports, seqbase)) {
    return false;
  }

  return true;
}
}  // namespace blender
