/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <cstring>

#include "BKE_idprop.hh"
#include "BKE_main.hh"

#include "BLI_fileops.hh"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"
#include "DNA_vfont_types.h"

#include "SEQ_render.hh"
#include "SEQ_transform.hh"

#include "opentimelineio/anyDictionary.h"
#include "opentimelineio/anyVector.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/deserialization.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/freezeFrame.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/generatorReference.h"
#include "opentimelineio/imageSequenceReference.h"
#include "opentimelineio/linearTimeWarp.h"
#include "opentimelineio/missingReference.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

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

static void set_color_strip_params(const Strip *strip, AnyDictionary &params)
{
  const SolidColorVars *color = static_cast<SolidColorVars *>(strip->effectdata);

  params["name"] = "Color";
  params["col"] = AnyVector{static_cast<double>(color->col[0]),
                            static_cast<double>(color->col[1]),
                            static_cast<double>(color->col[2])};
  params["width"] = static_cast<int64_t>(color->width);
  params["height"] = static_cast<int64_t>(color->height);
}

static void set_text_strip_params(const Strip *strip, AnyDictionary &params)
{
  const TextVars *text = static_cast<TextVars *>(strip->effectdata);

  params["name"] = "Text";
  params["text"] = std::string(text->text_ptr);
  params["text_font.filepath"] = text->text_font ? std::string(text->text_font->filepath) : "";
  params["text_blf_id"] = static_cast<int64_t>(text->text_blf_id);
  params["text_size"] = static_cast<double>(text->text_size);
  params["space_line"] = static_cast<double>(text->space_line);
  params["abs_space_line"] = static_cast<double>(text->abs_space_line);
  params["loc"] = AnyVector{static_cast<double>(text->loc[0]), static_cast<double>(text->loc[0])};
  params["wrap_width"] = static_cast<double>(text->wrap_width);
  params["box_margin"] = static_cast<double>(text->box_margin);
  params["box_roundness"] = static_cast<double>(text->box_roundness);
  params["shadow_angle"] = static_cast<double>(text->shadow_angle);
  params["shadow_offset"] = static_cast<double>(text->shadow_offset);
  params["shadow_blur"] = static_cast<double>(text->shadow_blur);
  params["outline_witdh"] = static_cast<double>(text->outline_width);
  params["flag"] = static_cast<int64_t>(text->flag);
  params["align"] = static_cast<int64_t>(text->align);
  params["cursor_offset"] = static_cast<int64_t>(text->cursor_offset);
  params["selection_start_offset"] = static_cast<int64_t>(text->selection_start_offset);
  params["selection_end_offset"] = static_cast<int64_t>(text->selection_end_offset);
  params["anchor_x"] = static_cast<int64_t>(text->anchor_x);
  params["anchor_y"] = static_cast<int64_t>(text->anchor_y);
  params["textbox_state.visible_lines"] = static_cast<int64_t>(text->textbox_state.visible_lines);
  params["textbox_state.scroll"] = static_cast<int64_t>(text->textbox_state.scroll);

  params["color"] = AnyVector{static_cast<double>(text->color[0]),
                              static_cast<double>(text->color[1]),
                              static_cast<double>(text->color[2]),
                              static_cast<double>(text->color[3])};

  params["shadow_color"] = AnyVector{static_cast<double>(text->shadow_color[0]),
                                     static_cast<double>(text->shadow_color[1]),
                                     static_cast<double>(text->shadow_color[2]),
                                     static_cast<double>(text->shadow_color[3])};

  params["box_color"] = AnyVector{static_cast<double>(text->box_color[0]),
                                  static_cast<double>(text->box_color[1]),
                                  static_cast<double>(text->box_color[2]),
                                  static_cast<double>(text->box_color[3])};

  params["outline_color"] = AnyVector{static_cast<double>(text->outline_color[0]),
                                      static_cast<double>(text->outline_color[1]),
                                      static_cast<double>(text->outline_color[2]),
                                      static_cast<double>(text->outline_color[3])};
}

static SerializableObject::Retainer<GeneratorReference> create_generator_reference(
    const Strip *strip)
{
  auto generator_reference = SerializableObject::Retainer<GeneratorReference>(
      new GeneratorReference());
  AnyDictionary params;

  auto add_effect_params_common = [](const Strip *strip, AnyDictionary &params) {
    if (strip->input1) {
      params["input1"] = std::string(strip->input1->name);
    }
    if (strip->input2) {
      params["input2"] = std::string(strip->input2->name);
    }
  };

  switch (strip->type) {
    /* Zero input effect strips. */
    case STRIP_TYPE_COLOR:
      generator_reference->set_name("Color");
      generator_reference->set_generator_kind("Color");
      set_color_strip_params(strip, params);
      break;

    case STRIP_TYPE_TEXT:
      generator_reference->set_name("Text");
      generator_reference->set_generator_kind("Text");
      set_text_strip_params(strip, params);
      break;

    case STRIP_TYPE_ADJUSTMENT:
      generator_reference->set_name("Adjustment");
      generator_reference->set_generator_kind("Adjustment");
      params["name"] = "Adjustment";
      break;

    /* Two input effect strips. */
    case STRIP_TYPE_ADD:
      generator_reference->set_name("Add");
      generator_reference->set_generator_kind("Add");
      params["name"] = "Add";
      add_effect_params_common(strip, params);
      break;

    case STRIP_TYPE_SUB:
      generator_reference->set_name("Subtract");
      generator_reference->set_generator_kind("Subtract");
      params["name"] = "Subtract";
      add_effect_params_common(strip, params);
      break;

    case STRIP_TYPE_MUL:
      generator_reference->set_name("Multiply");
      generator_reference->set_generator_kind("Multiply");
      params["name"] = "Multiply";
      add_effect_params_common(strip, params);
      break;

    case STRIP_TYPE_ALPHAOVER:
      generator_reference->set_name("Alpha Over");
      generator_reference->set_generator_kind("Alpha Over");
      params["name"] = "Alpha Over";
      params["default_fade"] = static_cast<bool>(strip->flag & SEQ_USE_EFFECT_DEFAULT_FADE);
      params["effect_fader"] = static_cast<double>(strip->effect_fader);
      add_effect_params_common(strip, params);
      break;

    case STRIP_TYPE_ALPHAUNDER:
      generator_reference->set_name("Alpha Under");
      generator_reference->set_generator_kind("Alpha Under");
      params["name"] = "Alpha Under";
      params["default_fade"] = static_cast<bool>(strip->flag & SEQ_USE_EFFECT_DEFAULT_FADE);
      params["effect_fader"] = static_cast<double>(strip->effect_fader);
      add_effect_params_common(strip, params);
      break;

    case STRIP_TYPE_COLORMIX: {
      generator_reference->set_name("Color Mix");
      generator_reference->set_generator_kind("Color Mix");
      params["name"] = "Color Mix";
      const ColorMixVars *mix = static_cast<ColorMixVars *>(strip->effectdata);
      params["blend_effect"] = static_cast<int64_t>(mix->blend_effect);
      params["blend_factor"] = static_cast<double>(mix->factor);
      add_effect_params_common(strip, params);
      break;
    }

    default:
      break;
  }

  generator_reference->parameters()["blender"] = params;
  return generator_reference;
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

template<typename T>
static void attach_foreign_metadata(IDProperty *idp, SerializableObject::Retainer<T> &clip)
{
  if (!idp || idp->type != IDP_GROUP) {
    return;
  }

  IDProperty *otio_group = IDP_GetPropertyFromGroup(idp, "otio_metadata");
  if (!otio_group) {
    return;
  }

  IDP_foreach_property(otio_group, IDP_TYPE_FILTER_STRING, [&](IDProperty *prop) {
    if (strcmp(prop->name, "blender") || !prop->data.pointer) {
      return;
    }
    std::any dict = AnyDictionary();
    if (deserialize_json_from_string(
            static_cast<const char *>(prop->data.pointer), &dict, nullptr))
    {
      try {
        AnyDictionary metadata = std::any_cast<AnyDictionary>(dict);
        clip->metadata()[prop->name] = metadata;
      }
      catch (const std::bad_any_cast & /*e*/) {
        return;
      }
    }
  });
}

void attach_foreign_metadata_scene(const Scene *scene,
                                   SerializableObject::Retainer<Timeline> &timeline)
{
  attach_foreign_metadata(scene->id.system_properties, timeline);
}

template<typename T>
void attach_foreign_metadata_strip(const Strip *strip, SerializableObject::Retainer<T> &clip)
{
  attach_foreign_metadata(strip->system_properties, clip);
}

template void attach_foreign_metadata_strip(const Strip *strip,
                                            SerializableObject::Retainer<Stack> &clip);
template void attach_foreign_metadata_strip(const Strip *strip,
                                            SerializableObject::Retainer<Transition> &clip);

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

template<typename T>
static void handle_speed_effect_strip(const Scene *scene,
                                      const Strip *strip,
                                      const Strip *effect_strip,
                                      SerializableObject::Retainer<T> &clip)
{
  float speed_factor = 1.0f;
  const SpeedControlVars *speed = static_cast<const SpeedControlVars *>(effect_strip->effectdata);

  switch (speed->speed_control_type) {
    case SEQ_SPEED_STRETCH: {
      const float source_len = strip->length(scene) - strip->startofs;
      const float effect_len = effect_strip->right_handle(scene) - effect_strip->left_handle();
      speed_factor = (effect_len != 0.0f) ? source_len / effect_len : 0.0f;
      break;
    }

    case SEQ_SPEED_MULTIPLY:
      speed_factor = speed->speed_fader;
      break;

    case SEQ_SPEED_LENGTH:
    case SEQ_SPEED_FRAME_NUMBER:
      speed_factor = 0.0f;
      break;

    default:
      break;
  }

  if (speed_factor != 0.0f) {
    auto ltw = SerializableObject::Retainer<LinearTimeWarp>(
        new LinearTimeWarp(effect_strip->name + 2, "Speed"));
    ltw->set_time_scalar(speed_factor);
    attach_foreign_metadata_strip(effect_strip, ltw);
    clip->effects().push_back(static_cast<SerializableObject::Retainer<otio::Effect>>(ltw.value));
  }
  else {
    auto ff = SerializableObject::Retainer<FreezeFrame>(new FreezeFrame(effect_strip->name + 2));
    ff->set_effect_name("Speed");
    attach_foreign_metadata_strip(effect_strip, ff);
    clip->effects().push_back(static_cast<SerializableObject::Retainer<otio::Effect>>(ff.value));
  }
}

template<typename T>
static void handle_gaussian_blur_effect_strip(const Strip *effect_strip,
                                              SerializableObject::Retainer<T> &clip)
{
  const GaussianBlurVars *blur = static_cast<const GaussianBlurVars *>(effect_strip->effectdata);

  auto eff = SerializableObject::Retainer<otio::Effect>(
      new otio::Effect(effect_strip->name + 2, "Gaussian Blur"));

  AnyDictionary metadata;
  metadata["name"] = "Gaussian Blur";
  metadata["size_x"] = static_cast<double>(blur->size_x);
  metadata["size_y"] = static_cast<double>(blur->size_y);
  eff->metadata()["blender"] = metadata;
  attach_foreign_metadata_strip(effect_strip, eff);

  clip->effects().push_back(eff);
}

template<typename T>
static void handle_glow_effect_strip(const Strip *effect_strip,
                                     SerializableObject::Retainer<T> &clip)
{
  const GlowVars *glow = static_cast<const GlowVars *>(effect_strip->effectdata);

  auto eff = SerializableObject::Retainer<otio::Effect>(
      new otio::Effect(effect_strip->name + 2, "Glow"));

  AnyDictionary metadata;
  metadata["name"] = "Glow";
  metadata["fMini"] = static_cast<double>(glow->fMini);
  metadata["fClamp"] = static_cast<double>(glow->fClamp);
  metadata["fBoost"] = static_cast<double>(glow->fBoost);
  metadata["dDist"] = static_cast<double>(glow->dDist);
  metadata["dQuality"] = static_cast<int64_t>(glow->dQuality);
  metadata["bNoComp"] = static_cast<int64_t>(glow->bNoComp);
  eff->metadata()["blender"] = metadata;
  attach_foreign_metadata_strip(effect_strip, eff);

  clip->effects().push_back(eff);
}

template<typename T>
void add_effects_to_clip(
    const Scene *scene,
    Strip *strip,
    SerializableObject::Retainer<T> &clip,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects)
{
  if (!single_input_effects.contains(strip)) {
    return;
  }

  for (Strip *effect_strip : single_input_effects[strip]) {
    switch (effect_strip->type) {
      case STRIP_TYPE_SPEED:
        handle_speed_effect_strip(scene, strip, effect_strip, clip);
        break;

      case STRIP_TYPE_GAUSSIAN_BLUR:
        handle_gaussian_blur_effect_strip(effect_strip, clip);
        break;

      case STRIP_TYPE_GLOW:
        handle_glow_effect_strip(effect_strip, clip);
        break;

      default:
        break;
    }
  }
}

/* Force instantiate `add_effects_to_clip<Stack>` as compiler won't do it implicitly as it is being
 * called from a different translation unit (`otio_export.cc`). */
template void add_effects_to_clip(
    const Scene *scene,
    Strip *strip,
    SerializableObject::Retainer<Stack> &clip,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects);

void StripExporter::add_gap_if_necessary()
{
  int space_between = strip_->left_handle() - last_strip_end - 1;
  if (space_between > 0) {
    auto gap_duration = RationalTime(space_between, scene_->frames_per_second());
    auto gap = SerializableObject::Retainer<Gap>(new Gap(gap_duration));
    track_->append_child(gap);
  }
  last_strip_end = strip_->right_handle(scene_) - 1;
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

void StripExporter::export_with_missing_reference(
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects)
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

  attach_foreign_metadata_strip(strip_, clip);
  add_effects_to_clip(scene_, strip_, clip, single_input_effects);
  track_->append_child(clip);
}

/***** Handle Export for each strip type. *****/

void MovieStripExporter::export_strip(
    Main *bmain,
    const OTIOExportParams * /*export_params*/,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects)
{
  add_gap_if_necessary();

  float media_fps = scene_->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      bmain, strip_, scene_, media_fps, filepath_);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(strip_->name + 2, external_reference, strip_source_range));

  attach_foreign_metadata_strip(strip_, clip);
  add_effects_to_clip(scene_, strip_, clip, single_input_effects);
  track_->append_child(clip);
}

void SoundStripExporter::export_strip(
    Main *bmain,
    const OTIOExportParams * /*export_params*/,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects)
{
  add_gap_if_necessary();

  float media_fps = scene_->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      bmain, strip_, scene_, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(strip_->name + 2, external_reference, strip_source_range));

  attach_foreign_metadata_strip(strip_, clip);
  add_sound_strip_metadata(clip, strip_);
  add_effects_to_clip(scene_, strip_, clip, single_input_effects);
  track_->append_child(clip);
}

void ImageStripExporter::export_strip(
    Main *bmain,
    const OTIOExportParams *export_params,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects)
{
  float media_fps = scene_->frames_per_second();

  if (seq::transform_single_image_check(strip_)) {
    add_gap_if_necessary();

    TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);
    SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
        bmain, strip_, scene_, media_fps);

    auto clip = otio::SerializableObject::Retainer<otio::Clip>(
        new Clip(strip_->name + 2, external_reference, strip_source_range));

    attach_foreign_metadata_strip(strip_, clip);
    add_effects_to_clip(scene_, strip_, clip, single_input_effects);
    track_->append_child(clip);
  }
  else {
    /* Image Sequence. */
    if (!strip_->data || !strip_->data->stripdata) {
      export_with_missing_reference(single_input_effects);
      return;
    }

    if (export_params->img_sequence_export == ExportOption::RENDER_MOVIE) {
      auto exporter = RenderAsMovieExporter(strip_, scene_, track_, last_strip_end, filepath_);
      exporter.export_strip(bmain, export_params, single_input_effects);
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
          exporter.export_strip(bmain, export_params, single_input_effects);
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
        export_with_missing_reference(single_input_effects);
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

    attach_foreign_metadata_strip(strip_, clip);
    add_effects_to_clip(scene_, strip_, clip, single_input_effects);
    track_->append_child(clip);
  }
}

void RenderAsMovieExporter::export_strip(
    Main *bmain,
    const OTIOExportParams *export_params,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects)

{
  if (!filepath_) {
    export_with_missing_reference(single_input_effects);
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
    export_with_missing_reference(single_input_effects);
    return;
  }
  auto exporter = MovieStripExporter(strip_, scene_, track_, last_strip_end, render_filepath);
  exporter.export_strip(bmain, export_params, single_input_effects);
  last_strip_end = exporter.last_strip_end;

  UNUSED_VARS(include_audio_);
}

void GeneratorStripExporter::export_strip(
    Main * /*bmain*/,
    const OTIOExportParams * /*export_params*/,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects)
{
  add_gap_if_necessary();

  float media_fps = scene_->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(strip_, scene_, media_fps);
  SerializableObject::Retainer<GeneratorReference> generator_reference =
      create_generator_reference(strip_);
  generator_reference->set_available_range(strip_source_range);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(strip_->name + 2, generator_reference, strip_source_range));

  attach_foreign_metadata_strip(strip_, clip);
  add_effects_to_clip(scene_, strip_, clip, single_input_effects);
  track_->append_child(clip);
}

}  // namespace blender::io::otio
