/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.hh"

#include "BLT_translation.hh"

#include "CLG_log.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_listBase.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"
#include "DNA_workspace_types.h"

#include "SEQ_effects.hh"
#include "SEQ_sequencer.hh"

#include "WM_api.hh"

#include "opentimelineio/errorStatus.h"
#include "opentimelineio/timeline.h"

#include "IO_otio.hh"
#include "otio_export.hh"
#include "otio_import.hh"

namespace blender {

void OTIO_export(bContext *C, const OTIOExportParams &params)
{
  io::otio::exporter_main(C, params);
}

bool OTIO_import(bContext *C, const char *filepath, bool restore_channel, ReportList *reports)
{
  using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;
  using ErrorStatus = opentimelineio::OPENTIMELINEIO_VERSION_NS::ErrorStatus;

  ErrorStatus err;
  SerializableObject::Retainer<Timeline> timeline = dynamic_cast<Timeline *>(
      Timeline::from_json_file(filepath, &err));

  if (is_error(err)) {
    CLOG_ERROR(&LOG,
               "Failed to decode OTIO file:'%s'\nDetails: %s\nDescription: %s",
               filepath,
               err.details.c_str(),
               err.full_description.c_str());
    BKE_reportf(reports, RPT_ERROR, "OTIO Import: Cannot decode file '%s'", filepath);
    return false;
  }

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_sequencer_scene(C);

  std::string scene_name = timeline->name().length() ? timeline->name() :
                                                       std::string(DATA_("Scene"));

  if (!scene || (scene->ed && !BLI_listbase_is_empty(&scene->ed->seqbase))) {
    /* Need to create new `Scene`. */
    scene = BKE_scene_add(bmain, scene_name.c_str());
    CLOG_INFO(&LOG, "New Scene '%s' Added", scene->id.name + 2);
  }
  {
    /* Change name of existing scene. */
    BKE_id_rename(*bmain, scene->id, scene_name, IDNewNameMode::RenameExistingNever);
  }
  seq::editing_ensure(scene);
  build_blender_timeline(bmain, scene, timeline, restore_channel, reports);

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&scene->id, ID_RECALC_AUDIO_FPS | ID_RECALC_SEQUENCER_STRIPS);

  bScreen *screen = CTX_wm_screen(C);

  /* Check if sequencer is open. */
  if (screen && BKE_screen_find_big_area(screen, SPACE_SEQ, 0)) {
    WorkSpace *workspace = CTX_wm_workspace(C);
    wmWindow *win = CTX_wm_window(C);

    if (workspace) {
      workspace->sequencer_scene = scene;
    }
    if (win) {
      WM_window_set_active_scene(bmain, C, win, scene);
    }

    BKE_reportf(reports, RPT_WARNING, TIP_("Active scene changed to '%s'"), scene->id.name + 2);
    WM_event_add_notifier(C, NC_WINDOW, nullptr);
  }
  return true;
}
}  // namespace blender
