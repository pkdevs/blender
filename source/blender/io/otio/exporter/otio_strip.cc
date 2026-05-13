/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <cstring>

#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/imageSequenceReference.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/track.h"

#include "IO_otio.hh"
#include "otio_strip.hh"

namespace blender::io::otio {

using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

void StripExporter::add_gap_if_necessary()
{
  int space_between = _strip->left_handle() - last_strip_end - 1;
  if (space_between > 0) {
    auto gap_duration = RationalTime(space_between, _scene->frames_per_second());
    auto gap = SerializableObject::Retainer<Gap>(new Gap(gap_duration));
    _track->append_child(gap);
  }
  last_strip_end = _strip->right_handle(_scene);
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

/************** Helper Functions. **************/

static void get_media_filename(const Strip *strip, char *filename_out)
{
  strcpy(filename_out, strip->data->stripdata->filename);
}

static void get_media_filepath(const Strip *strip, char *filepath_out)
{
  BLI_path_join(filepath_out, FILE_MAX, strip->data->dirpath, strip->data->stripdata->filename);
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
  if (strip->type == STRIP_TYPE_IMAGE && strip->flag | SEQ_SINGLE_FRAME_CONTENT) {
    media_length = get_strip_duration(strip, scene);
  }

  return TimeRange(otio::RationalTime(0, media_fps), otio::RationalTime(media_length, media_fps));
}

static TimeRange get_strip_source_range(const Strip *strip,
                                        const Scene *scene,
                                        const float media_fps)
{
  /* The strips could be moved left of the timeline start and could have -ve `strip->start`. Most
   * NLE's doesn't have the concept of moving a strip left of the timeline. */
  int left_offset = max_ii(0, -strip->start);

  return TimeRange(RationalTime(strip->startofs + left_offset, media_fps),
                   RationalTime(get_strip_duration(strip, scene) - left_offset, media_fps));
}

static SerializableObject::Retainer<ExternalReference> create_external_reference(
    const Strip *strip, const Scene *scene, float media_fps)
{
  char media_filename[FILE_MAX];
  char media_filepath[FILE_MAX];
  get_media_filename(strip, media_filename);
  get_media_filepath(strip, media_filepath);

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
    int start_frame_nr,
    int frame_step,
    int fps,
    int num_digits)
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

static bool img_seq_need_fallback(StripElem *se, size_t img_count)
{
  /* Get sequence number of the first image. */
  int frame_nr = 0;
  int num_digits = 0;
  if (!BLI_path_frame_get(se->filename, &frame_nr, &num_digits)) {
    return true;
  }

  if (img_count == 1) {
    return false;
  }

  /* Get sequence number of the second image. */
  int frame_nr_2 = 0;
  int num_digits_2 = 0;
  if (!BLI_path_frame_get(se->filename, &frame_nr_2, &num_digits_2)) {
    return true;
  }
  if (frame_nr_2 - frame_nr < 1) {
    return true;
  }

  return false;
}

static void img_sequence_rename(StripElem *se, const char *dirpath, int img_count, int &num_digits)
{
  /* Count number of digits required to represent the largest number in the sequence. */
  num_digits = 0;
  for (int imc = img_count; imc; imc /= 10, ++num_digits) {
  };

  char common_prefix[FILE_MAX];
  BLI_strncpy(common_prefix, se->filename, sizeof(common_prefix));
  BLI_path_extension_strip(common_prefix);

  for (int seq_num = 1; seq_num <= img_count; ++seq_num, ++se) {
    char old_path[FILE_MAX];
    BLI_path_join(old_path, sizeof(old_path), dirpath, se->filename);

    char new_path[FILE_MAX];
    char ext[FILE_MAX];
    char mask[FILE_MAX];

    {
      int curr = 0;
      for (; curr < min_ii(num_digits, FILE_MAX - 1); ++curr) {
        mask[curr] = '#';
      }
      mask[curr] = '\0';
    }

    BLI_strncpy(ext, BLI_path_extension(se->filename), sizeof(ext));
    BLI_strncpy(se->filename, common_prefix, FILE_MAX);

    BLI_strncat(se->filename, ".", FILE_MAX);
    BLI_strncat(se->filename, mask, FILE_MAX);
    BLI_strncat(se->filename, ext, FILE_MAX);

    BLI_path_frame(se->filename, FILE_MAX, seq_num, num_digits);
    BLI_path_join(new_path, sizeof(new_path), dirpath, se->filename);

    std::rename(old_path, new_path);
  }
}

/***** Handle Export for each strip type. *****/

void MovieStripExporter::export_strip(const OTIOExportParams * /*export_params*/)
{
  add_gap_if_necessary();

  float media_fps = _scene->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(_strip, _scene, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      _strip, _scene, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(_strip->name, external_reference, strip_source_range));

  _track->append_child(clip);
}

void SoundStripExporter::export_strip(const OTIOExportParams * /*export_params*/)
{
  add_gap_if_necessary();

  float media_fps = _scene->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(_strip, _scene, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      _strip, _scene, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(_strip->name, external_reference, strip_source_range));

  _track->append_child(clip);
}

void ImageStripExporter::export_strip(const OTIOExportParams *export_params)
{
  add_gap_if_necessary();
  bool is_single_image = _strip->flag & SEQ_SINGLE_FRAME_CONTENT;
  float media_fps = _scene->frames_per_second();

  if (is_single_image) {
    TimeRange strip_source_range = get_strip_source_range(_strip, _scene, media_fps);
    SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
        _strip, _scene, media_fps);

    auto clip = otio::SerializableObject::Retainer<otio::Clip>(
        new Clip(_strip->name, external_reference, strip_source_range));

    _track->append_child(clip);
  }
  else {
    /* Image Sequence. */
    if (!_strip->data || !_strip->data->stripdata) {
      return;
    }

    StripElem *se = _strip->data->stripdata;
    size_t img_count = MEM_allocN_len(se) / sizeof(*se);

    const char *target_url_base = _strip->data->dirpath;
    char name_prefix[FILE_MAX];

    int frame_step = 1;
    int start_frame_nr = 1;
    int num_digits = 0;

    if (img_seq_need_fallback(se, img_count)) {
      switch (export_params->img_sequence_fallback) {
        case FALLBACK_IMG_SEQUENCE_RENAME:
          img_sequence_rename(se, _strip->data->dirpath, img_count, num_digits);
          break;

        case FALLBACK_IMG_SEQUENCE_SYMLINK:
          break;

        default:
          break;
      }
    }
    else {
      if (!BLI_path_frame_get(se->filename, &start_frame_nr, &num_digits)) {
        return;
      }
    }

    const char *name_suffix = BLI_path_extension(se->filename);
    const char *curr = name_suffix;

    /* Copy the name prefix. */
    for (int i = 0; i < num_digits; ++i, curr--) {
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

    add_gap_if_necessary();

    SerializableObject::Retainer<ImageSequenceReference> img_seq_ref =
        create_image_sequence_reference(_strip,
                                        _scene,
                                        target_url_base,
                                        name_prefix,
                                        name_suffix,
                                        start_frame_nr,
                                        frame_step,
                                        _scene->frames_per_second(),
                                        num_digits);
    TimeRange source_range = get_strip_source_range(_strip, _scene, _scene->frames_per_second());

    auto clip = SerializableObject::Retainer<Clip>(
        new Clip(_strip->name, img_seq_ref, source_range));

    _track->append_child(clip);
  }
}

}  // namespace blender::io::otio
