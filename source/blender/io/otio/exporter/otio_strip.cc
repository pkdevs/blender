/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <cstring>

#include "BKE_main.hh"

#include "BLI_fileops.hh"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"

#include "SEQ_render.hh"
#include "SEQ_transform.hh"

#include "opentimelineio/anyDictionary.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/imageSequenceReference.h"
#include "opentimelineio/missingReference.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/track.h"

#include "IO_otio.hh"
#include "otio_strip.hh"

namespace blender::io::otio {

using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

/************** Helper Functions. **************/

static void get_media_filename(const Strip *strip, char *filename_out)
{
  BLI_strncpy(filename_out, strip->data->stripdata->filename, FILE_MAX);
}

static void get_media_filepath(const Main *bmain, const Strip *strip, char *filepath_out)
{
  BLI_path_join(filepath_out, FILE_MAX, strip->data->dirpath, strip->data->stripdata->filename);
  BLI_path_abs(filepath_out, BKE_main_blendfile_path(bmain));
}

static int get_strip_duration(const Strip *strip, const Scene *scene)
{
  return strip->right_handle(scene) - strip->left_handle();
}

static TimeRange get_media_available_range(const Strip *strip,
                                           const Scene *scene,
                                           const float media_fps)
{
  int media_length = strip->len;
  if (strip->type == STRIP_TYPE_IMAGE && seq::transform_single_image_check(strip)) {
    media_length = get_strip_duration(strip, scene);
  }

  return TimeRange(otio::RationalTime(0, media_fps), otio::RationalTime(media_length, media_fps));
}

static TimeRange get_strip_source_range(const Strip *strip,
                                        const Scene *scene,
                                        const float media_fps)
{
  /* The strips could be moved left of the timeline start and could have negative left handle.
   * Most NLEs don't have the concept of moving a strip left of the timeline. */
  int strip_timeline_start = max_ii(0, strip->left_handle());
  int left_offset = strip_timeline_start - strip->start;

  return TimeRange(RationalTime(left_offset, media_fps),
                   RationalTime(strip->right_handle(scene) - strip_timeline_start, media_fps));
}

static SerializableObject::Retainer<ExternalReference> create_external_reference(
    const Main *bmain,
    const Strip *strip,
    const Scene *scene,
    float media_fps,
    const char *filepath = nullptr)
{
  char media_filename[FILE_MAX];
  char media_filepath[FILE_MAX];
  if (!filepath) {
    get_media_filename(strip, media_filename);
    get_media_filepath(bmain, strip, media_filepath);
  }
  else {
    BLI_strncpy(media_filepath, filepath, sizeof(media_filepath));
    BLI_path_split_file_part(filepath, media_filename, sizeof(media_filename));
  }

  TimeRange media_available_range = get_media_available_range(strip, scene, media_fps);

  auto external_reference = SerializableObject::Retainer<ExternalReference>(
      new ExternalReference(media_filepath, media_available_range));

  external_reference->set_name(media_filename);

  return external_reference;
}

static SerializableObject::Retainer<ImageSequenceReference> create_image_sequence_reference(
    const Strip *strip,
    const Scene *scene,
    const char *target_url_base,
    const char *name_prefix,
    const char *name_suffix,
    const int start_frame_nr,
    const int frame_step,
    const float fps,
    const int num_digits)
{
  TimeRange media_available_range = get_media_available_range(strip, scene, fps);
  auto img_seq_ref = SerializableObject::Retainer<ImageSequenceReference>(
      new ImageSequenceReference(target_url_base,
                                 name_prefix,
                                 name_suffix,
                                 start_frame_nr,
                                 frame_step,
                                 fps,
                                 num_digits,
                                 ImageSequenceReference::MissingFramePolicy::error,
                                 media_available_range));

  return img_seq_ref;
}

static bool img_seq_need_fallback(StripElem *se)
{
  /* Get sequence number of the first image. */
  int frame_nr = 0;
  int num_digits = 0;
  if (!BLI_path_frame_get(se->filename, &frame_nr, &num_digits)) {
    return true;
  }

  /* Get sequence number of the second image. */
  int frame_nr_2 = 0;
  int num_digits_2 = 0;
  if (!BLI_path_frame_get((++se)->filename, &frame_nr_2, &num_digits_2)) {
    return true;
  }
  if (frame_nr_2 - frame_nr < 1) {
    return true;
  }

  return false;
}

static void path_append_sequence_number(
    const char *old_path, char *path_out, int frame_nr, int padding, bool prefix_period = true)
{
  /* Copy old path. */
  BLI_strncpy(path_out, old_path, FILE_MAX);

  /* Remove and store the extension. */
  char ext[FILE_MAX];
  ext[0] = '\0';
  if (BLI_path_extension(path_out)) {
    BLI_strncpy(ext, BLI_path_extension(path_out), sizeof(ext));
    BLI_path_extension_strip(path_out);
  }

  /* create ### mask. */
  char mask[FILE_MAX];

  {
    int curr = 0;
    for (; curr < min_ii(padding, FILE_MAX - 1); ++curr) {
      mask[curr] = '#';
    }
    mask[curr] = '\0';
  }

  if (prefix_period) {
    BLI_strncat(path_out, ".", FILE_MAX);
  }
  BLI_strncat(path_out, mask, FILE_MAX);
  BLI_strncat(path_out, ext, FILE_MAX);

  /* Replace mask with frame_nr and with `0` pading if required. */
  BLI_path_frame(path_out, FILE_MAX, frame_nr, padding);
}

static int calculate_padding(int img_count)
{
  /* Count number of digits required to represent the largest number in the sequence. */
  int padding = 0;

  for (int imc = img_count; imc; imc /= 10) {
    ++padding;
  };

  /* Keep the padding atleast 2. */
  return max_ii(padding, 2);
}

static void img_sequence_rename(StripElem *se, const char *dirpath, int img_count, int padding)
{
  char common_filename[FILE_MAX];
  BLI_strncpy(common_filename, se->filename, sizeof(common_filename));

  for (int seq_num = 1; seq_num <= img_count; ++seq_num, ++se) {
    char old_path[FILE_MAX];
    BLI_path_join(old_path, sizeof(old_path), dirpath, se->filename);

    path_append_sequence_number(common_filename, se->filename, seq_num, padding);

    char new_path[FILE_MAX];
    BLI_path_join(new_path, sizeof(new_path), dirpath, se->filename);

    std::rename(old_path, new_path);
  }
}

#ifndef WIN32
/* `BLI_create_symlink` is not implemented for Windows. Windows require admin previlages to create
 * symlinks. */

static void img_sequence_create_symlinks(const StripElem *se,
                                         char *target_url_base,
                                         const int img_count,
                                         int padding)
{
  char BL_links_path[FILE_MAX];
  BLI_path_join(BL_links_path, sizeof(BL_links_path), target_url_base, "BL_links");

  /* Create `BL_links` directory if it does not exist. */
  if (!BLI_dir_create_recursive(BL_links_path)) {
    return;
  }

  BLI_strncpy(target_url_base, BL_links_path, FILE_MAX);

  char symlink_base_path[FILE_MAX];
  BLI_path_join(symlink_base_path, sizeof(symlink_base_path), BL_links_path, se->filename);

  for (int seq_num = 1; seq_num <= img_count; ++seq_num, ++se) {
    char symlink_path[FILE_MAX];
    path_append_sequence_number(symlink_base_path, symlink_path, seq_num, padding);

    char symlink_target[FILE_MAX];
    BLI_path_join(symlink_target, sizeof(symlink_target), "..", se->filename);

    BLI_create_symlink(symlink_path, symlink_target);
  }
}

#endif

static void add_sound_strip_metadata(SerializableObject::Retainer<Clip> &clip, const Strip *strip)
{
  if (!strip->sound) {
    return;
  }

  AnyDictionary metadata;
  /* Cast all floats as doubles as OTIO does not support floats and writes them as null in the
   * file. */
  metadata["volume"] = static_cast<double>(strip->volume);
  metadata["speed_factor"] = static_cast<double>(strip->speed_factor);
  metadata["pan"] = static_cast<double>(strip->pan);
  metadata["sound_offset"] = static_cast<double>(strip->sound_offset);

  clip->metadata()["blender"] = metadata;
}

void StripExporter::add_gap_if_necessary()
{
  int space_between = strip_->left_handle() - last_strip_end - 1;
  if (space_between > 0) {
    auto gap_duration = RationalTime(space_between, scene_->frames_per_second());
    auto gap = SerializableObject::Retainer<Gap>(new Gap(gap_duration));
    track_->append_child(gap);
  }
  last_strip_end = strip_->right_handle(scene_);
}

void StripExporter::add_gap_if_necessary(SerializableObject::Retainer<Track> &track,
                                         int start_frame,
                                         int end_frame,
                                         double scene_fps)
{
  int space_between = end_frame - start_frame + 1;
  if (space_between > 0) {
    auto gap_duration = RationalTime(space_between, scene_fps);
    auto gap = SerializableObject::Retainer<Gap>(new Gap(gap_duration));
    track->append_child(gap);
  }
}

void StripExporter::export_with_missing_reference()
{
  add_gap_if_necessary();

  float media_fps = scene_->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);

  const char *filename = nullptr;
  if (strip_->data && strip_->data->stripdata) {
    filename = strip_->data->stripdata->filename;
  }

  auto missing_reference = SerializableObject::Retainer<MissingReference>(
      new MissingReference(filename ? filename : ""));

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(strip_->name + 2, missing_reference, strip_source_range));

  track_->append_child(clip);
}

/***** Handle Export for each strip type. *****/

void MovieStripExporter::export_strip(Main *bmain, const OTIOExportParams * /*export_params*/)
{
  add_gap_if_necessary();

  float media_fps = scene_->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      bmain, strip_, scene_, media_fps, filepath_);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(strip_->name + 2, external_reference, strip_source_range));

  track_->append_child(clip);
}

void SoundStripExporter::export_strip(Main *bmain, const OTIOExportParams * /*export_params*/)
{
  add_gap_if_necessary();

  float media_fps = scene_->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      bmain, strip_, scene_, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(strip_->name + 2, external_reference, strip_source_range));

  add_sound_strip_metadata(clip, strip_);

  track_->append_child(clip);
}

void ImageStripExporter::export_strip(Main *bmain, const OTIOExportParams *export_params)
{
  float media_fps = scene_->frames_per_second();

  if (seq::transform_single_image_check(strip_)) {
    add_gap_if_necessary();

    TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);
    SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
        bmain, strip_, scene_, media_fps);

    auto clip = otio::SerializableObject::Retainer<otio::Clip>(
        new Clip(strip_->name + 2, external_reference, strip_source_range));

    track_->append_child(clip);
  }
  else {
    /* Image Sequence. */
    if (!strip_->data || !strip_->data->stripdata) {
      export_with_missing_reference();
      return;
    }

    if (export_params->img_sequence_export == ExportOption::RENDER_MOVIE) {
      auto exporter = RenderAsMovieExporter(strip_, scene_, track_, last_strip_end, filepath_);
      exporter.export_strip(bmain, export_params);
      last_strip_end = exporter.last_strip_end;
      return;
    }

    StripElem *se = strip_->data->stripdata;
    size_t img_count = MEM_allocN_len(se) / sizeof(*se);

    char target_url_base[FILE_MAX];
    BLI_strncpy(target_url_base, strip_->data->dirpath, sizeof(target_url_base));
    BLI_path_abs(target_url_base, BKE_main_blendfile_path(bmain));

    char name_prefix[FILE_MAX];
    char name_suffix[FILE_MAX];
    BLI_strncpy(name_suffix, BLI_path_extension(se->filename), sizeof(name_suffix));

    int frame_step = 1;
    int start_frame_nr = 1;
    int padding = calculate_padding(img_count);

    if (img_seq_need_fallback(se)) {
      /* Update `name_prefix` to : filename without extension + '.' */
      BLI_strncpy(name_prefix, se->filename, sizeof(name_prefix));
      BLI_path_extension_strip(name_prefix);
      BLI_strncat(name_prefix, ".", sizeof(name_prefix));

      switch (export_params->img_sequence_fallback) {
        case ImgSeqFallback::RENDER_MOVIE: {
          auto exporter = RenderAsMovieExporter(strip_, scene_, track_, last_strip_end, filepath_);
          exporter.export_strip(bmain, export_params);
          last_strip_end = exporter.last_strip_end;
          return;
        }
        case ImgSeqFallback::RENAME:
          img_sequence_rename(se, target_url_base, img_count, padding);
          break;

#ifndef WIN32
        case ImgSeqFallback::SYMLINK:
          img_sequence_create_symlinks(se, target_url_base, img_count, padding);
          break;
#endif

        default:
          break;
      }
    }
    else {
      if (!BLI_path_frame_get(se->filename, &start_frame_nr, &padding)) {
        export_with_missing_reference();
        return;
      }

      const char *curr = BLI_path_extension(se->filename);

      /* Copy the name prefix. */
      for (int i = 0; i < padding; ++i) {
        --curr;
      };
      char *np = name_prefix;
      char *copy_p = se->filename;
      while (copy_p != curr) {
        *np = *copy_p;
        np++;
        copy_p++;
      }
      *np = '\0';

      if (img_count > 1) {
        int second_frame_nr, second_num_digits;
        BLI_path_frame_get((++se)->filename, &second_frame_nr, &second_num_digits);
        frame_step = second_frame_nr - start_frame_nr;
      }
    }

    add_gap_if_necessary();

    SerializableObject::Retainer<ImageSequenceReference> img_seq_ref =
        create_image_sequence_reference(strip_,
                                        scene_,
                                        target_url_base,
                                        name_prefix,
                                        name_suffix,
                                        start_frame_nr,
                                        frame_step,
                                        media_fps,
                                        padding);
    TimeRange source_range = get_strip_source_range(strip_, scene_, scene_->frames_per_second());

    auto clip = SerializableObject::Retainer<Clip>(
        new Clip(strip_->name + 2, img_seq_ref, source_range));

    track_->append_child(clip);
  }
}

void RenderAsMovieExporter::export_strip(Main *bmain, const OTIOExportParams *export_params)
{
  if (!filepath_) {
    export_with_missing_reference();
    return;
  }

  /* Create a unique filename for the strip containing the strip name, channel number and frame
   * range. */
  char render_filename[FILE_MAX];
  char temp[FILE_MAX];
  temp[0] = '\0';
  int padding = calculate_padding(strip_->right_handle(scene_) - 1);
  BLI_strncpy(render_filename, strip_->name + 2, sizeof(render_filename));
  BLI_strncat(render_filename, ".C", sizeof(render_filename));
  BLI_strncat(render_filename, std::to_string(strip_->channel).c_str(), sizeof(render_filename));
  BLI_strncat(render_filename, ".", sizeof(render_filename));

  BLI_strncat(temp, "[", sizeof(temp));
  path_append_sequence_number(temp, temp, strip_->left_handle(), padding, false);
  BLI_strncat(temp, "-", sizeof(temp));
  path_append_sequence_number(temp, temp, strip_->right_handle(scene_) - 1, padding, false);
  BLI_strncat(temp, "]", sizeof(temp));

  BLI_strncat(render_filename, temp, sizeof(render_filename));
  BLI_strncat(render_filename, ".mp4", sizeof(render_filename));

  char render_filepath[FILE_MAX];
  BLI_path_split_dir_part(filepath_, render_filepath, sizeof(render_filepath));
  BLI_path_append_dir(render_filepath, sizeof(render_filepath), "BL_render");
  if (!BLI_dir_create_recursive(render_filepath)) {
    return;
  }
  BLI_path_append(render_filepath, sizeof(render_filepath), render_filename);

  short render_res = 100;
  if (strip_->type == STRIP_TYPE_SCENE && !(strip_->flag & SEQ_SCENE_STRIPS) && strip_->scene) {
    render_res = get_scene_strip_resolution_percent(export_params->scene_strip_res);
  }

  const bool is_rendered = seq::render_strip_full(
      bmain, scene_, strip_, render_res, render_filepath, false);

  if (!is_rendered) {
    export_with_missing_reference();
    return;
  }
  auto exporter = MovieStripExporter(strip_, scene_, track_, last_strip_end, render_filepath);
  exporter.export_strip(bmain, export_params);
  last_strip_end = exporter.last_strip_end;

  UNUSED_VARS(include_audio_);
}

}  // namespace blender::io::otio
