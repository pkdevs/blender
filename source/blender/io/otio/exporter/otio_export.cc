/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <map>

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "SEQ_sequencer.hh"

#include "WM_types.hh"

#include "opentime/rationalTime.h"
#include "opentime/timeRange.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"

#include "IO_otio.hh"
#include "otio_export.hh"
#include "otio_strip.hh"

namespace blender {
namespace io::otio {

using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

static void export_scene_markers(const Scene *scene, SerializableObject::Retainer<Stack> &stack)
{
  std::vector<SerializableObject::Retainer<Marker>> &otio_markers = stack->markers();

  for (const TimeMarker &blender_marker : scene->markers) {
    TimeRange marked_range = TimeRange(
        RationalTime(blender_marker.frame, scene->frames_per_second()),
        RationalTime(1, scene->frames_per_second()));

    auto marker = SerializableObject::Retainer<Marker>(
        new Marker(blender_marker.name, marked_range, Marker::Color::white));

    otio_markers.push_back(marker);
  }
}

static SerializableObject::Retainer<Stack> otio_export_recursive(
    Scene *scene,
    const blender::OTIOExportParams *export_params,
    ListBaseT<Strip> *strips,
    int _last_strip_end,
    int stack_end)
{
  auto stack = SerializableObject::Retainer<Stack>(new Stack());

  /* Separate video and audio channels.
   * Use negative channel number as key in std::map to store the sound strips.
   */
  auto compare_strip_start = [](const Strip *a, const Strip *b) { return a->start < b->start; };
  std::map<int, std::set<Strip *, decltype(compare_strip_start)>> channels;

  for (Strip &strip : *strips) {
    if (ELEM(strip.type, STRIP_TYPE_SOUND)) {
      channels[-strip.channel].insert(&strip);
    }
    else {
      channels[strip.channel].insert(&strip);
    }
  }

  /* Iterate through the channels and strips to create OTIO timeine. */
  for (auto [original_channel, strips] : channels) {

    const std::string track_type = original_channel < 0 ? Track::Kind::audio : Track::Kind::video;

    auto track_source_range = otio::TimeRange(
        RationalTime(0, scene->frames_per_second()),
        RationalTime(stack_end - _last_strip_end, scene->frames_per_second()));

    auto track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, track_type));

    int last_strip_end = _last_strip_end;

    /* Append all the strips of this channel in the track. */
    for (Strip *strip : strips) {
      StripExporter *strip_exporter = nullptr;

      switch (strip->type) {
        case STRIP_TYPE_MOVIE:
          strip_exporter = new MovieStripExporter(strip, scene, track, last_strip_end);
          break;

        case STRIP_TYPE_SOUND:
          strip_exporter = new SoundStripExporter(strip, scene, track, last_strip_end);
          break;

        case STRIP_TYPE_IMAGE:
          strip_exporter = new ImageStripExporter(strip, scene, track, last_strip_end);
          break;

        case STRIP_TYPE_META: {
          StripExporter::add_gap_if_necessary(
              track, last_strip_end + 1, strip->left_handle() - 1, scene->frames_per_second());

          SerializableObject::Retainer<Stack> meta_stack = otio_export_recursive(
              scene,
              export_params,
              &strip->seqbase,
              strip->left_handle() - 1,
              strip->right_handle(scene));

          last_strip_end = strip->right_handle(scene);
          track->append_child(meta_stack);
        } break;

        default:
          break;
      }

      if (strip_exporter) {
        strip_exporter->export_strip(export_params);
        last_strip_end = strip_exporter->last_strip_end;

        delete strip_exporter;
      }
    }
    StripExporter::add_gap_if_necessary(
        track, last_strip_end + 1, stack_end, scene->frames_per_second());

    stack->append_child(track);
  }
  TimeRange source_range = TimeRange(
      RationalTime(0, scene->frames_per_second()),
      RationalTime(stack_end - _last_strip_end, scene->frames_per_second()));
  stack->set_source_range(source_range);
  return stack;
}

void otio_export_job_start(void *custom_data, wmJobWorkerStatus *worker_status)
{
  ExportJobData *job_data = static_cast<ExportJobData *>(custom_data);
  OTIOExportParams *export_params = &job_data->params;

  Scene *scene = job_data->scene;
  Editing *editing = seq::editing_get(scene);
  ListBaseT<Strip> *seqbase = &editing->seqbase;

  if (!scene || !editing) {
    BKE_report(worker_status->reports, RPT_ERROR, "No Sequencer Scene found");
    return;
  }

  worker_status->progress = 0.0f;
  worker_status->do_update = true;

  auto timeline = SerializableObject::Retainer<Timeline>(
      new Timeline(scene->id.name, RationalTime(0, scene->frames_per_second())));

  SerializableObject::Retainer<Stack> main_stack = otio_export_recursive(
      scene, export_params, seqbase, 0, scene->r.efra);

  export_scene_markers(scene, main_stack);

  timeline->set_tracks(main_stack);
  timeline->to_json_file(job_data->filepath);

  worker_status->progress = 1.0f;
  worker_status->do_update = true;

  BKE_report(worker_status->reports, RPT_INFO, "File exported successfully");
}

}  // namespace io::otio
}  // namespace blender
