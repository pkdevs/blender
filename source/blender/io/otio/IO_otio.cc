/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.hh"

#include "CLG_log.h"
#include "DNA_listBase.h"
#include "DNA_sequence_types.h"

#include "SEQ_effects.hh"
#include "SEQ_sequencer.hh"

#include "IO_otio.hh"
#include "otio_export.hh"

namespace blender {

static CLG_LogRef LOG = {"io.otio"};

void OTIO_export(bContext *C, const OTIOExportParams &params)
{
  io::otio::exporter_main(CTX_data_main(C), CTX_data_sequencer_scene(C), params);
}

static bool validate_transitions(ReportList *reports, ListBaseT<Strip> *seqbase)
{
  for (Strip &strip : *seqbase) {
    if (!seq::effect_is_transition(strip.type)) {
      continue;
    }
    if (!strip.input1 || !strip.input2) {
      CLOG_ERROR(&LOG,
                 "Transition strip '%s' has insufficient inputs, input1 = '%s', input2 = '%s'",
                 strip.name + 2,
                 strip.input1 ? strip.input1->name + 2 : "nullptr",
                 strip.input2 ? strip.input2->name + 2 : "nullptr");
      BKE_report(reports, RPT_ERROR, "Transition strip has insufficient inputs");
      return false;
    }

    /* All three strips (input1, transition strip and input2) should be on the same channel. */
    if (strip.channel != strip.input1->channel || strip.channel != strip.input2->channel) {
      CLOG_ERROR(&LOG,
                 "Input strips '%s' and '%s' are not on the same channel as transition strip '%s'",
                 strip.input1->name + 2,
                 strip.input2->name + 2,
                 strip.name + 2);
      BKE_report(reports, RPT_ERROR, "Transition and input strips must be on the same channel");
      return false;
    }
  }
  return true;
}

bool OTIO_validate_timeline_blender(ReportList *reports, const Scene *scene)
{
  Editing *ed = seq::editing_get(scene);
  if (!scene || !ed) {
    BKE_report(reports, RPT_ERROR, "No sequencer scene found");
    return false;
  }

  ListBaseT<Strip> *seqbase = &ed->seqbase;

  if (!validate_transitions(reports, seqbase)) {
    return false;
  }

  return true;
}
}  // namespace blender
