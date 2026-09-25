/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <algorithm>
#include <map>

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"

#include "CLG_log.h"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "SEQ_channels.hh"
#include "SEQ_effects.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_utils.hh"

#include "opentimelineio/clip.h"
#include "opentimelineio/color.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/missingReference.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include "IO_otio.hh"
#include "otio_export.hh"
#include "otio_export_strip.hh"

namespace blender {

static CLG_LogRef LOG = {"io.otio"};

namespace io::otio {

using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

struct UnsupportedStripCounts {
  int scene_strips = 0;
  int image_sequences = 0;
};

static void export_scene_markers(const Scene *scene, SerializableObject::Retainer<Stack> &stack)
{
  std::vector<SerializableObject::Retainer<Marker>> &otio_markers = stack->markers();

  for (const TimeMarker &blender_marker : scene->markers) {
    TimeRange marked_range = TimeRange(
        RationalTime(blender_marker.frame, scene->frames_per_second()),
        RationalTime(1, scene->frames_per_second()));

    auto marker = SerializableObject::Retainer<Marker>(
        new Marker(blender_marker.name, marked_range, Color::white));

    otio_markers.push_back(marker);
  }
}

static void export_transition(const Strip *strip,
                              const Scene *scene,
                              SerializableObject::Retainer<Track> &track,
                              int &last_strip_end)
{
  const double media_fps = scene->frames_per_second();
  const int strip_len = strip->right_handle(scene) - strip->left_handle();

  auto transition = SerializableObject::Retainer<Transition>(new Transition(
      strip->name + 2,
      strip->type == STRIP_TYPE_WIPE ? Transition::Type::Custom : Transition::Type::SMPTE_Dissolve,
      RationalTime(strip_len / 2, media_fps),
      RationalTime(strip_len - strip_len / 2, media_fps)));

  AnyDictionary metadata;
  metadata["default_fade"] = static_cast<bool>(strip->flag & SEQ_USE_EFFECT_DEFAULT_FADE);
  metadata["effect_fader"] = static_cast<double>(strip->effect_fader);

  if (strip->type == STRIP_TYPE_WIPE) {
    const WipeVars *wipe = static_cast<WipeVars *>(strip->effectdata);

    metadata["name"] = "Wipe";
    metadata["edgeWidth"] = static_cast<double>(wipe->edgeWidth);
    metadata["angle"] = static_cast<double>(wipe->angle);
    metadata["forward"] = static_cast<int64_t>(wipe->forward);
    metadata["wipetype"] = static_cast<int64_t>(wipe->wipetype);
  }
  else {
    metadata["name"] = strip->type == STRIP_TYPE_GAMCROSS ? "Gamma Crossfade" : "Crossfade";
    metadata["gamma"] = strip->type == STRIP_TYPE_GAMCROSS;
  }

  transition->metadata()["blender"] = metadata;
  attach_foreign_metadata_strip(strip, transition);
  track->append_child(transition);
  last_strip_end = strip->right_handle(scene);
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
                                  const OTIOExportParams &params,
                                  SerializableObject::Retainer<Stack> *primary_stack,
                                  SerializableObject::Retainer<Stack> *secondary_stack,
                                  ListBaseT<Strip> *strips,
                                  int stack_start,
                                  int stack_end,
                                  const ListBaseT<SeqTimelineChannel> *channel_listbase,
                                  UnsupportedStripCounts &unsupported)
{
  bool inside_meta = secondary_stack != nullptr;

  /* Separate video and audio channels.
   * Use negative channel number as key in std::map to store the sound strips.
   */
  std::map<int, std::vector<Strip *>> channels;
  std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> single_input_effects;
  /* Store input1 -> transition strip mapping. */
  std::unordered_map<Strip *, Strip *> transition_effects_1;
  /* Store input2 -> transition strip mapping. */
  std::unordered_map<Strip *, Strip *> transition_effects_2;

  for (Strip &strip : *strips) {
    if (ELEM(strip.type, STRIP_TYPE_SOUND)) {
      channels[-strip.channel].push_back(&strip);
    }
    else if (ELEM(strip.type, STRIP_TYPE_GAUSSIAN_BLUR, STRIP_TYPE_GLOW, STRIP_TYPE_SPEED)) {
      if (strip.input1) {
        single_input_effects[strip.input1].insert(&strip);
      }
    }
    else if (seq::effect_is_transition(strip.type) && strip.type != STRIP_TYPE_COMPOSITOR) {
      if (!strip.input1 || !strip.input2 || strip.input1->channel != strip.input2->channel) {
        CLOG_WARN(&LOG, "Skipping transition strip '%s'", strip.name + 2);
        BKE_reportf(
            params.reports,
            RPT_WARNING,
            "Transition strip '%s' was not exported, its inputs must be on the same channel",
            strip.name + 2);
        continue;
      }
      transition_effects_1[strip.input1] = &strip;
      transition_effects_2[strip.input2] = &strip;
    }
    else {
      channels[strip.channel].push_back(&strip);
    }
  }

  /* Iterate through the channels and strips to create OTIO timeine. */
  for (auto [original_channel, strips] : channels) {
    /* Sort the strips in the channel. */
    std::stable_sort(strips.begin(), strips.end(), [scene](const Strip *a, const Strip *b) {
      if (a->left_handle() != b->left_handle()) {
        return a->left_handle() < b->left_handle();
      }
      if (a->right_handle(scene) != b->right_handle(scene)) {
        return a->right_handle(scene) < b->right_handle(scene);
      }
      return false;
    });

    bool is_enabled = true;
    if (channel_listbase) {
      const SeqTimelineChannel *ch = seq::channel_get_by_index(channel_listbase, original_channel);
      if (ch && ch->is_muted()) {
        is_enabled = false;
      }
    }

    const std::string track_type = original_channel < 0 ? Track::Kind::audio : Track::Kind::video;

    auto track_source_range = otio::TimeRange(
        RationalTime(0, scene->frames_per_second()),
        RationalTime(stack_end - stack_start, scene->frames_per_second()));

    auto track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, track_type));

    auto meta_video_track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, Track::Kind::video));

    auto meta_audio_track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, Track::Kind::audio));

    track->set_enabled(is_enabled);
    meta_video_track->set_enabled(is_enabled);
    meta_audio_track->set_enabled(is_enabled);

    int last_strip_end = 0;

    /* Append all the strips of this channel in the track. */
    for (Strip *strip : strips) {
      if (transition_effects_2.contains(strip)) {
        CLOG_INFO(&LOG, "Exporting transition '%s'", transition_effects_2[strip]->name + 2);
        export_transition(transition_effects_2[strip],
                          scene,
                          inside_meta ? meta_video_track : track,
                          last_strip_end);
      }
      CLOG_INFO(&LOG, "Exporting strip '%s'", strip->name + 2);

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
            strip, scene, inside_meta ? meta_video_track : track, last_strip_end);
      }
      /* Nested Strips (Meta, Sequencer Scene). */
      else if (strip->type == STRIP_TYPE_META ||
               ((strip->type == STRIP_TYPE_SCENE) && (strip->flag & SEQ_SCENE_STRIPS)))
      {
        int r_offset;
        ListBaseT<SeqTimelineChannel> *r_channels;
        ListBaseT<Strip> *seqbase = seq::get_seqbase_from_strip(strip, &r_channels, &r_offset);

        if (!seqbase) {
          CLOG_WARN(&LOG, "No seqbase in meta or scene strip '%s'", strip->name + 2);

          StripExporter missing_reference_exporter_video = StripExporter(
              strip, scene, inside_meta ? meta_video_track : track, last_strip_end);

          StripExporter missing_reference_exporter_audio = StripExporter(
              strip, scene, inside_meta ? meta_audio_track : track, last_strip_end);

          missing_reference_exporter_video.export_with_missing_reference(single_input_effects);
          missing_reference_exporter_audio.export_with_missing_reference(single_input_effects);
        }
        else {
          auto primary_meta_stack = SerializableObject::Retainer<Stack>(new Stack());
          auto secondary_meta_stack = SerializableObject::Retainer<Stack>(new Stack());

          otio_export_recursive(bmain,
                                scene,
                                params,
                                &primary_meta_stack,
                                &secondary_meta_stack,
                                seqbase,
                                strip->left_handle() - 1,
                                strip->right_handle(scene) - 1,
                                r_channels,
                                unsupported);

          if (!primary_meta_stack->children().empty()) {
            primary_meta_stack->set_enabled(!(strip->flag & SEQ_MUTE));
            add_strip_metadata(strip, primary_meta_stack);
            attach_foreign_metadata_strip(strip, primary_meta_stack);
            add_effects_to_clip(scene, strip, primary_meta_stack, single_input_effects);
            StripExporter::add_gap_if_necessary(meta_video_track,
                                                last_strip_end + 1,
                                                strip->left_handle() - 1,
                                                scene->frames_per_second());
            meta_video_track->append_child(primary_meta_stack);
          }

          if (!secondary_meta_stack->children().empty()) {
            secondary_meta_stack->set_enabled(!(strip->flag & SEQ_MUTE));
            add_strip_metadata(strip, secondary_meta_stack);
            attach_foreign_metadata_strip(strip, secondary_meta_stack);
            add_effects_to_clip(scene, strip, secondary_meta_stack, single_input_effects);
            StripExporter::add_gap_if_necessary(meta_audio_track,
                                                last_strip_end + 1,
                                                strip->left_handle() - 1,
                                                scene->frames_per_second());
            meta_audio_track->append_child(secondary_meta_stack);
          }

          last_strip_end = strip->right_handle(scene) - 1;
        }
      }
      /* 3D Scene Strip. */
      else if (strip->type == STRIP_TYPE_SCENE && !(strip->flag & SEQ_SCENE_STRIPS) &&
               strip->scene)
      {
        unsupported.scene_strips++;
        auto missing_reference_exporter = StripExporter(
            strip, scene, inside_meta ? meta_video_track : track, last_strip_end);

        missing_reference_exporter.export_with_missing_reference(single_input_effects);
      }
      /* Color, Adjustment, Text, etc. Strips. */
      else if (ELEM(strip->type,
                    STRIP_TYPE_COLOR,
                    STRIP_TYPE_ADJUSTMENT,
                    STRIP_TYPE_TEXT,
                    STRIP_TYPE_ADD,
                    STRIP_TYPE_SUB,
                    STRIP_TYPE_MUL,
                    STRIP_TYPE_ALPHAOVER,
                    STRIP_TYPE_ALPHAUNDER,
                    STRIP_TYPE_COLORMIX))
      {
        strip_exporter = new GeneratorStripExporter(
            strip, scene, inside_meta ? meta_video_track : track, last_strip_end);
      }
      else {
        CLOG_WARN(&LOG, "Skipping strip '%s' of unsupported type", strip->name + 2);
        BKE_reportf(params.reports,
                    RPT_WARNING,
                    "Strip '%s' was not exported, its type is not supported",
                    strip->name + 2);
      }

      if (strip_exporter) {
        SerializableObject::Retainer<Clip> clip = strip_exporter->export_strip(
            bmain, &params, single_input_effects);
        std::optional<TimeRange> source_range = clip->source_range();
        last_strip_end = strip_exporter->last_strip_end;

        if (strip->type == STRIP_TYPE_IMAGE &&
            dynamic_cast<MissingReference *>(clip->media_reference()))
        {
          unsupported.image_sequences++;
        }

        if (source_range && transition_effects_1.contains(strip)) {
          Strip *transition_strip = transition_effects_1[strip];
          int strip_len = transition_strip->right_handle(scene) - transition_strip->left_handle();
          RationalTime extension = RationalTime(strip_len / 2, scene->frames_per_second());
          source_range = TimeRange{source_range->start_time(),
                                   source_range->duration() + extension};
          clip->set_source_range(source_range);
        }
        if (source_range && transition_effects_2.contains(strip)) {
          Strip *transition_strip = transition_effects_2[strip];
          int strip_len = transition_strip->right_handle(scene) - transition_strip->left_handle();
          RationalTime extension = RationalTime(strip_len - strip_len / 2,
                                                scene->frames_per_second());
          source_range = TimeRange{source_range->start_time() - extension,
                                   source_range->duration() + extension};
          clip->set_source_range(source_range);
        }

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
      RationalTime(stack_end - stack_start, scene->frames_per_second()));
  (*primary_stack)->set_source_range(source_range);
  if (secondary_stack) {
    (*secondary_stack)->set_source_range(source_range);
  }
}

void exporter_main(Main *bmain, Scene *scene, const OTIOExportParams &params)
{
  Editing *editing = seq::editing_get(scene);

  ListBaseT<Strip> *seqbase = &editing->seqbase;

  auto timeline = SerializableObject::Retainer<Timeline>(
      new Timeline(scene->id.name + 2, RationalTime(0, scene->frames_per_second())));

  UnsupportedStripCounts unsupported;
  auto main_stack = SerializableObject::Retainer<Stack>(new Stack());
  otio_export_recursive(bmain,
                        scene,
                        params,
                        &main_stack,
                        nullptr,
                        seqbase,
                        0,
                        scene->r.efra,
                        &editing->channels,
                        unsupported);

  attach_foreign_metadata_scene(scene, timeline);
  export_scene_markers(scene, main_stack);

  timeline->set_tracks(main_stack);

  opentimelineio::OPENTIMELINEIO_VERSION_NS::ErrorStatus error_status;
  if (!timeline->to_json_file(params.filepath, &error_status)) {
    CLOG_ERROR(&LOG,
               "Failed to write OTIO file '%s': %s",
               params.filepath,
               error_status.full_description.c_str());
    BKE_reportf(params.reports, RPT_ERROR, "Failed to write OTIO file '%s'", params.filepath);
    return;
  }

  BKE_report(params.reports, RPT_INFO, "File exported successfully");

  if (unsupported.scene_strips > 0) {
    BKE_reportf(params.reports,
                RPT_WARNING,
                "%d scene strip(s) found, this is currently unsupported",
                unsupported.scene_strips);
  }
  if (unsupported.image_sequences > 0) {
    BKE_reportf(params.reports,
                RPT_WARNING,
                "%d image sequence(s) with non-sequential filenames found, this is currently "
                "unsupported",
                unsupported.image_sequences);
  }
}

}  // namespace io::otio
}  // namespace blender
