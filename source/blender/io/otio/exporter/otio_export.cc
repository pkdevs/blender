/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <map>

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "SEQ_sequencer.hh"
#include "SEQ_utils.hh"

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

/**
 * We need two stacks when we have meta strips with both video and audio strips. It is not as
 * straight forward as separating clips to video or audio channels as metastrips can also contain
 * other metastrips. Even if we separate the clips inside a meta strip to video and audio tracks,
 * the metastrip will still be multi-media and could not be classified as a video/visual clip or
 * audio clip. Hence, no mono-media track could be used to hold the metastrip.
 *
 * So, for every metastrip in the `main_stack` (root stack), we create two stacks: `primary_stack`
 * and `secondary_stack`. Both stacks are identical in terms of nesting of metastrips but
 * `primary_stack` holds only video/visual clips and metastrips which contain only video/visual
 * clips whereas the `secondary_stack` holds audio clips and metastrips which contain only audio
 * clips.
 *
 * In simple words, in `primary_stack` the audio clips are omitted and in `secondary_stack` the
 * video/visual clips are omitted.
 *
 * NOTE: `secondary_stack` is nullptr when the function is not inside a metastrip.
 */
static void otio_export_recursive(Main *bmain,
                                  Scene *scene,
                                  const blender::OTIOExportParams *export_params,
                                  const char *filepath,
                                  SerializableObject::Retainer<Stack> *primary_stack,
                                  SerializableObject::Retainer<Stack> *secondary_stack,
                                  ListBaseT<Strip> *strips,
                                  int _last_strip_end,
                                  int stack_end)
{
  bool inside_meta = secondary_stack != nullptr;

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

    auto meta_video_track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, Track::Kind::video));

    auto meta_audio_track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, Track::Kind::audio));

    int last_strip_end = _last_strip_end;

    /* Append all the strips of this channel in the track. */
    for (Strip *strip : strips) {
      StripExporter *strip_exporter = nullptr;

      if (strip->type == STRIP_TYPE_MOVIE) {
        strip_exporter = new MovieStripExporter(
            strip, scene, inside_meta ? meta_video_track : track, last_strip_end);
      }
      else if (strip->type == STRIP_TYPE_SOUND) {
        strip_exporter = new SoundStripExporter(
            strip, scene, inside_meta ? meta_audio_track : track, last_strip_end);
      }
      else if (strip->type == STRIP_TYPE_IMAGE) {
        strip_exporter = new ImageStripExporter(
            strip, scene, inside_meta ? meta_video_track : track, last_strip_end, filepath);
      }
      /* Nested Strips (Meta, Sequencer Scene). */
      else if (strip->type == STRIP_TYPE_META ||
               ((strip->type == STRIP_TYPE_SCENE) && (strip->flag & SEQ_SCENE_STRIPS)))
      {
        if (export_params->meta_strip_export == ExportOption::RENDER_MOVIE) {
          strip_exporter = new RenderAsMovieExporter(
              strip, scene, inside_meta ? meta_video_track : track, last_strip_end, filepath);
        }
        else {
          if (inside_meta) {
            StripExporter::add_gap_if_necessary(meta_video_track,
                                                last_strip_end + 1,
                                                strip->left_handle() - 1,
                                                scene->frames_per_second());

            StripExporter::add_gap_if_necessary(meta_audio_track,
                                                last_strip_end + 1,
                                                strip->left_handle() - 1,
                                                scene->frames_per_second());
          }
          else {
            StripExporter::add_gap_if_necessary(
                track, last_strip_end + 1, strip->left_handle() - 1, scene->frames_per_second());
          }

          int r_offset;
          ListBaseT<SeqTimelineChannel> *r_channels;
          ListBaseT<Strip> *seqbase = seq::get_seqbase_from_strip(strip, &r_channels, &r_offset);

          if (!seqbase) {
            StripExporter missing_reference_exporter_video = StripExporter(
                strip, scene, inside_meta ? meta_video_track : track, last_strip_end);

            StripExporter missing_reference_exporter_audio = StripExporter(
                strip, scene, inside_meta ? meta_audio_track : track, last_strip_end);

            missing_reference_exporter_video.export_with_missing_reference();
            missing_reference_exporter_audio.export_with_missing_reference();
          }
          else {
            auto primary_meta_stack = SerializableObject::Retainer<Stack>(new Stack());
            auto secondary_meta_stack = SerializableObject::Retainer<Stack>(new Stack());

            otio_export_recursive(bmain,
                                  scene,
                                  export_params,
                                  filepath,
                                  &primary_meta_stack,
                                  &secondary_meta_stack,
                                  seqbase,
                                  strip->left_handle() - 1,
                                  strip->right_handle(scene));

            last_strip_end = strip->right_handle(scene);

            if (!primary_meta_stack->children().empty()) {
              meta_video_track->append_child(primary_meta_stack);
            }
            if (!secondary_meta_stack->children().empty()) {
              meta_audio_track->append_child(secondary_meta_stack);
            }
          }
        }
      }
      /* 3D Scene Strip. */
      else if (strip->type == STRIP_TYPE_SCENE && !(strip->flag & SEQ_SCENE_STRIPS) &&
               strip->scene)
      {
        if (export_params->bake_scene_strips) {
          strip_exporter = new RenderAsMovieExporter(
              strip, scene, inside_meta ? meta_video_track : track, last_strip_end, filepath);
        }
        else {
          auto missing_reference_exporter = StripExporter(
              strip, scene, inside_meta ? meta_video_track : track, last_strip_end);

          missing_reference_exporter.export_with_missing_reference();
        }
      }

      if (strip_exporter) {
        strip_exporter->export_strip(bmain, export_params);
        last_strip_end = strip_exporter->last_strip_end;

        delete strip_exporter;
      }
    }

    if (!track->children().empty()) {
      StripExporter::add_gap_if_necessary(
          track, last_strip_end + 1, stack_end, scene->frames_per_second());

      (*primary_stack)->append_child(track);
    }
    if (!meta_video_track->children().empty()) {
      StripExporter::add_gap_if_necessary(
          meta_video_track, last_strip_end + 1, stack_end, scene->frames_per_second());

      (*primary_stack)->append_child(meta_video_track);
    }
    if (!meta_audio_track->children().empty()) {
      StripExporter::add_gap_if_necessary(
          meta_audio_track, last_strip_end + 1, stack_end, scene->frames_per_second());

      if (inside_meta) {
        (*secondary_stack)->append_child(meta_audio_track);
      }
      else {
        (*primary_stack)->append_child(meta_audio_track);
      }
    }
  }

  TimeRange source_range = TimeRange(
      RationalTime(0, scene->frames_per_second()),
      RationalTime(stack_end - _last_strip_end, scene->frames_per_second()));
  (*primary_stack)->set_source_range(source_range);
  if (secondary_stack) {
    (*secondary_stack)->set_source_range(source_range);
  }
}

void otio_export_job_start(void *custom_data, wmJobWorkerStatus *worker_status)
{
  ExportJobData *job_data = static_cast<ExportJobData *>(custom_data);
  OTIOExportParams *export_params = &job_data->params;

  Main *bmain = job_data->bmain;
  Scene *scene = job_data->scene;
  Editing *editing = seq::editing_get(scene);

  if (!scene || !editing) {
    BKE_report(worker_status->reports, RPT_ERROR, "No Sequencer Scene found");
    return;
  }

  ListBaseT<Strip> *seqbase = &editing->seqbase;

  worker_status->progress = 0.0f;
  worker_status->do_update = true;

  auto timeline = SerializableObject::Retainer<Timeline>(
      new Timeline(scene->id.name + 2, RationalTime(0, scene->frames_per_second())));

  auto main_stack = SerializableObject::Retainer<Stack>(new Stack());
  otio_export_recursive(bmain,
                        scene,
                        export_params,
                        job_data->filepath,
                        &main_stack,
                        nullptr,
                        seqbase,
                        0,
                        scene->r.efra);

  export_scene_markers(scene, main_stack);

  timeline->set_tracks(main_stack);
  timeline->to_json_file(job_data->filepath);

  worker_status->progress = 1.0f;
  worker_status->do_update = true;

  BKE_report(worker_status->reports, RPT_INFO, "File exported successfully");
}

}  // namespace io::otio
}  // namespace blender
