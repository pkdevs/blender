/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <limits>

#include "BKE_report.hh"

#include "BLI_fileops.hh"
#include "BLI_listbase.hh"
#include "BLI_math_base_c.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"

#include "CLG_log.h"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "IMB_imbuf_types.hh"

#include <opentime/rationalTime.h>
#include <opentime/timeRange.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/imageSequenceReference.h>
#include <opentimelineio/item.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

#include "SEQ_add.hh"
#include "SEQ_utils.hh"

#include "IO_otio.hh"
#include "otio_import.hh"
#include "otio_import_metadata.hh"

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

/**
 * \brief Copies `filename_prev` to `filename` if missing frame policy is 'hold' or returns true if
 * missing frame policy is 'error'.
 * \note Does not handle 'black' missing frame policy as it is handled by default if a image frame
 * is missing.
 * \return Show missing frame error or not.
 */
static bool missing_frame_policy_impl(char *filename,
                                      char *filename_prev,
                                      char *dirpath,
                                      ImageSequenceReference::MissingFramePolicy missing_policy)
{
  char filepath[FILE_MAX];
  BLI_strncpy(filepath, dirpath, FILE_MAX);
  BLI_path_append(filepath, FILE_MAX, filename);

  if (BLI_exists(filepath)) {
    BLI_strncpy(filename_prev, filename, FILE_MAX);
    return false;
  }

  switch (missing_policy) {
    case ImageSequenceReference::MissingFramePolicy::error:
      return true;

    case ImageSequenceReference::MissingFramePolicy::hold:
      BLI_strncpy(filename, filename_prev, FILE_MAX);
      break;

    default:
      break;
  }
  return false;
}

static void image_strip_init(
    Main *bmain, Scene *scene, Strip *strip, ImageStripParams *params, ReportList *reports)
{
  char dirpath[sizeof(strip->data->dirpath)];
  char filename[FILE_MAXFILE];
  char filename_prev[FILE_MAXFILE];
  BLI_path_split_dir_part(params->path, dirpath, sizeof(dirpath));
  BLI_path_split_file_part(params->path, filename, sizeof(filename));

  STRNCPY(filename_prev, filename);
  bool missing_frames_error = missing_frame_policy_impl(
      filename, filename_prev, dirpath, params->missing_policy);

  seq::add_image_set_directory(strip, dirpath);
  seq::add_image_load_file(scene, strip, 0, filename);

  /* Initialize `StripElem.filename` for all other frames if it's an Image Sequence. */
  for (int i = 1; i < params->count; ++i) {
    STRNCPY(filename, params->name_prefix);
    path_append_sequence_number(
        filename, filename, params->start_frame + i, params->padding, false);
    BLI_strncat(filename, params->name_suffix, sizeof(filename));

    missing_frames_error |= missing_frame_policy_impl(
        filename, filename_prev, dirpath, params->missing_policy);
    seq::add_image_load_file(scene, strip, i, filename);
  }

  seq::add_image_init_alpha_mode(bmain, scene, strip);

  if (missing_frames_error) {
    BKE_reportf(
        reports, RPT_ERROR, "Image Sequence Strip '%s' Contains Missing Frames", params->name);
  }
}

static void add_transition(Scene *scene, ListBaseT<Strip> *seqbase, TransitionParams &params)
{
  int in_offset = params.otio_transition->in_offset().to_frames();
  int out_offset = params.otio_transition->out_offset().to_frames();

  int right_handle = params.input1->right_handle(scene);
  params.input1->right_handle_set(scene, right_handle - in_offset);

  int left_handle = params.input2->left_handle();
  params.input2->left_handle_set(scene, left_handle + out_offset);

  TransitionMetadata metadata = fetch_transition_metadata(params.otio_transition);

  seq::LoadData load_data;
  memset(&load_data, 0, sizeof(seq::LoadData));
  load_data.start_frame = right_handle;
  load_data.channel = params.channel;
  load_data.allow_invalid_file = true;
  load_data.fit_method = SEQ_SCALE_TO_FIT;
  load_data.flags |= seq::SEQ_LOAD_SET_VIEW_TRANSFORM;
  load_data.flags &= ~seq::SEQ_LOAD_MOVIE_SYNC_FPS;
  load_data.image.count = 1;
  load_data.image.length = 1;
  STRNCPY(load_data.name, params.otio_transition->name().c_str());
  load_data.effect.type = metadata.type;
  load_data.effect.input1 = params.input1;
  load_data.effect.input2 = params.input2;
  load_data.effect.length = in_offset + out_offset;

  Strip *effect_strip = seq::add_effect_strip(scene, seqbase, &load_data);

  effect_strip->effect_fader = metadata.effect_fader;
  if (!metadata.default_fade) {
    effect_strip->flag &= ~SEQ_USE_EFFECT_DEFAULT_FADE;
  }

  if (effect_strip->type == STRIP_TYPE_WIPE) {
    WipeVars *wipe = static_cast<WipeVars *>(effect_strip->effectdata);
    wipe->edgeWidth = metadata.edgeWidth;
    wipe->angle = metadata.angle;
    wipe->forward = metadata.forward;
    wipe->wipetype = metadata.wipetype;
  }
}

static void add_transitions(Scene *scene,
                            ListBaseT<Strip> *seqbase,
                            std::vector<TransitionParams> &transition_params)
{
  for (TransitionParams &params : transition_params) {
    add_transition(scene, seqbase, params);
  }
}

static Strip *add_item_recursive(Main *bmain,
                                 Scene *scene,
                                 ListBaseT<Strip> *seqbase,
                                 Item *item,
                                 int channel,
                                 int left_handle,
                                 bool is_sound_clip,
                                 ReportList *reports)
{
  TimeRange range = item->trimmed_range();
  int left_offset = range.start_time().to_frames();
  int duration = range.duration().to_frames();

  seq::LoadData load_data;
  memset(&load_data, 0, sizeof(seq::LoadData));
  load_data.start_frame = left_handle - left_offset;
  load_data.channel = channel;
  load_data.allow_invalid_file = true;
  load_data.fit_method = SEQ_SCALE_TO_FIT;
  load_data.flags |= seq::SEQ_LOAD_SET_VIEW_TRANSFORM;
  load_data.flags &= ~seq::SEQ_LOAD_MOVIE_SYNC_FPS;
  load_data.image.count = 1;
  load_data.image.length = 1;
  STRNCPY(load_data.name, item->name().c_str());

  Strip *strip = nullptr;

  if (auto clip = dynamic_cast<Clip *>(item)) {

    StripType strip_type = is_sound_clip ? STRIP_TYPE_SOUND : STRIP_TYPE_MOVIE;
    ImageStripParams image_params;
    STRNCPY(image_params.name, load_data.name);

    if (auto ext_ref = dynamic_cast<ExternalReference *>(clip->media_reference())) {
      STRNCPY(load_data.path, ext_ref->target_url().c_str());

      if (BLI_path_extension_check_array(load_data.path, imb_ext_image)) {
        strip_type = STRIP_TYPE_IMAGE;
      }
    }
    else if (auto img_seq_ref = dynamic_cast<ImageSequenceReference *>(clip->media_reference())) {
      strip_type = STRIP_TYPE_IMAGE;
      load_data.image.count = img_seq_ref->number_of_images_in_sequence();
      image_params.count = load_data.image.count;

      STRNCPY(image_params.name_prefix, img_seq_ref->name_prefix().c_str());
      STRNCPY(image_params.name_suffix, img_seq_ref->name_suffix().c_str());
      image_params.start_frame = img_seq_ref->start_frame();
      image_params.padding = img_seq_ref->frame_zero_padding();
      image_params.missing_policy = img_seq_ref->missing_frame_policy();

      STRNCPY(load_data.path, img_seq_ref->target_url_base().c_str());
      BLI_path_append(load_data.path, sizeof(load_data.path), image_params.name_prefix);
      path_append_sequence_number(
          load_data.path, load_data.path, image_params.start_frame, image_params.padding, false);
      BLI_strncat(load_data.path, image_params.name_suffix, sizeof(load_data.path));
    }
    STRNCPY(image_params.path, load_data.path);

    switch (strip_type) {
      case STRIP_TYPE_MOVIE:
        strip = seq::add_movie_strip(bmain, scene, seqbase, &load_data);
        break;

      case STRIP_TYPE_SOUND:
        strip = seq::add_sound_strip(bmain, scene, seqbase, &load_data);
        break;

      case STRIP_TYPE_IMAGE:
        strip = seq::add_image_strip(bmain, scene, seqbase, &load_data);
        image_strip_init(bmain, scene, strip, &image_params, reports);
        break;

      default:
        break;
    }
  }
  else if (dynamic_cast<Gap *>(item)) {
    return nullptr;
  }
  else if (auto stack = dynamic_cast<Stack *>(item)) {

    strip = seq::add_meta_strip(scene, seqbase, &load_data);
    int meta_end_frame = std::numeric_limits<int>::min();
    int channel_meta = 1;

    for (const auto &t : stack->children()) {
      if (auto track = dynamic_cast<Track *>(t.value)) {

        int left_handle_meta = 1;
        bool is_sound_clip = track->kind() == Track::Kind::audio ? true : false;

        std::vector<TransitionParams> transition_params;
        TransitionParams transition_params_curr;
        transition_params_curr.set_channel(channel_meta);

        for (auto &child : track->children()) {
          if (auto item = dynamic_cast<Item *>(child.value)) {

            Strip *strip_child = add_item_recursive(bmain,
                                                    scene,
                                                    &strip->seqbase,
                                                    item,
                                                    channel_meta,
                                                    left_handle_meta,
                                                    is_sound_clip,
                                                    reports);

            if (strip_child) {
              meta_end_frame = max_ii(strip_child->right_handle(scene), meta_end_frame);
            }

            if (transition_params_curr.set_input(strip_child)) {
              transition_params.push_back(transition_params_curr);
              transition_params_curr.reset();
            }

            left_handle_meta += item->trimmed_range().duration().to_frames();
          }
          else if (auto transition = dynamic_cast<Transition *>(child.value)) {
            transition_params_curr.set_transition(transition);
          }
        }
        add_transitions(scene, &strip->seqbase, transition_params);
        ++channel_meta;
      }
    }
    strip->len = meta_end_frame - load_data.start_frame;
  }

  if (strip) {
    strip->handles_set(scene, left_handle, left_handle + duration);
    if (!item->enabled()) {
      strip->flag |= SEQ_MUTE;
    }
  }
  else {
    CLOG_ERROR(&LOG, "File '%s' could not be loaded", load_data.path);
  }

  return strip;
}

static void add_scene_markers(Scene *scene, SerializableObject::Retainer<Timeline> &timeline)
{
  auto set_marker_name = [](TimeMarker *marker, std::string &&name, int frame) {
    if (name.length()) {
      STRNCPY(marker->name, name.c_str());
    }
    else {
      SNPRINTF_UTF8(marker->name, "F_%02d", frame);
    }
  };

  for (SerializableObject::Retainer<Marker> &otio_marker : timeline->tracks()->markers()) {
    int frame1 = otio_marker->marked_range().start_time().to_frames();
    int frame2 = frame1 + otio_marker->marked_range().duration().to_frames() - 1;

    TimeMarker *marker = MEM_new<TimeMarker>("TimeMarker");
    marker->frame = frame1;
    set_marker_name(marker, otio_marker->name(), frame1);
    BLI_addtail(&scene->markers, marker);

    if (frame2 > frame1) {
      TimeMarker *marker2 = MEM_new<TimeMarker>("TimeMarker");
      marker->frame = frame2;
      set_marker_name(marker2, otio_marker->name(), frame2);
      BLI_addtail(&scene->markers, marker2);
    }
  }
}

void build_blender_timeline(Main *bmain,
                            Scene *scene,
                            SerializableObject::Retainer<Timeline> &timeline,
                            ReportList *reports)
{
  /* Set Scene fps. */
  bool sync_fps = false;
  otio::ErrorStatus err;
  RationalTime dur = timeline->tracks()->duration(&err);
  if (is_error(err)) {
    CLOG_WARN(&LOG,
              "Failed to determine timeline FPS\nDetails: %s\nDescription: %s",
              err.details.c_str(),
              err.full_description.c_str());
    BKE_reportf(reports, RPT_WARNING, "OTIO Import: Failed to determine timeline FPS");
    sync_fps = true;
  }
  else {
    const double rate = dur.rate();
    scene->r.frs_sec = static_cast<short>(rate);
    scene->r.frs_sec_base = static_cast<float>(scene->r.frs_sec / rate);
  }
  int channel = 1;
  for (const auto &t : timeline->tracks()->children()) {
    if (auto track = dynamic_cast<Track *>(t.value)) {

      int left_handle = 1;
      bool is_sound_clip = track->kind() == Track::Kind::audio ? true : false;

      std::vector<TransitionParams> transition_params;
      TransitionParams transition_params_curr;
      transition_params_curr.set_channel(channel);

      for (auto &child : track->children()) {
        if (auto item = dynamic_cast<Item *>(child.value)) {

          Strip *strip_added = add_item_recursive(bmain,
                                                  scene,
                                                  &scene->ed->seqbase,
                                                  item,
                                                  channel,
                                                  left_handle,
                                                  is_sound_clip,
                                                  reports);

          left_handle += item->trimmed_range().duration().to_frames();

          if (transition_params_curr.set_input(strip_added)) {
            transition_params.push_back(transition_params_curr);
            transition_params_curr.reset();
          }
        }
        else if (auto transition = dynamic_cast<Transition *>(child.value)) {
          transition_params_curr.set_transition(transition);
        }
      }
      add_transitions(scene, &scene->ed->seqbase, transition_params);
      ++channel;
    }
  }

  add_scene_markers(scene, timeline);
}

}  // namespace blender::io::otio
