/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include "BKE_idprop.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"

#include "DNA_color_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"
#include "DNA_vec_types.h"
#include "DNA_vfont_types.h"

#include "opentimelineio/anyDictionary.h"
#include "opentimelineio/anyVector.h"
#include "opentimelineio/deserialization.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/freezeFrame.h"
#include "opentimelineio/linearTimeWarp.h"

#include "otio_export_metadata.hh"

namespace blender::io::otio {

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
  params["loc"] = AnyVector{static_cast<double>(text->loc[0]), static_cast<double>(text->loc[1])};
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

SerializableObject::Retainer<GeneratorReference> create_generator_reference(const Strip *strip)
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

void add_foreign_metadata(IDProperty *idp, SerializableObjectWithMetadata *object)
{
  if (!idp || idp->type != IDP_GROUP) {
    return;
  }

  IDProperty *otio_group = IDP_GetPropertyFromGroup(idp, "otio_metadata");
  if (!otio_group) {
    return;
  }

  IDP_foreach_property(otio_group, IDP_TYPE_FILTER_STRING, [&](IDProperty *prop) {
    if (STREQ(prop->name, "blender") || !prop->data.pointer) {
      return;
    }
    std::any dest;
    if (deserialize_json_from_string(
            static_cast<const char *>(prop->data.pointer), &dest, nullptr))
    {
      object->metadata()[prop->name] = dest;
    }
  });
}

SerializableObject::Retainer<Transition> create_transition(const Strip *strip,
                                                           RationalTime in_offset,
                                                           RationalTime out_offset)
{
  auto transition = SerializableObject::Retainer<Transition>(new Transition(
      strip->name + 2,
      strip->type == STRIP_TYPE_WIPE ? Transition::Type::Custom : Transition::Type::SMPTE_Dissolve,
      in_offset,
      out_offset));

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
  add_foreign_metadata(strip->system_properties, transition.value);
  return transition;
}

static void handle_speed_effect_strip(const Scene *scene,
                                      const Strip *strip,
                                      const Strip *effect_strip,
                                      Item *item)
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
    add_foreign_metadata(effect_strip->system_properties, ltw.value);
    item->effects().push_back(static_cast<SerializableObject::Retainer<otio::Effect>>(ltw.value));
  }
  else {
    auto ff = SerializableObject::Retainer<FreezeFrame>(new FreezeFrame(effect_strip->name + 2));
    ff->set_effect_name("Speed");
    add_foreign_metadata(effect_strip->system_properties, ff.value);
    item->effects().push_back(static_cast<SerializableObject::Retainer<otio::Effect>>(ff.value));
  }
}

static void handle_gaussian_blur_effect_strip(const Strip *effect_strip, Item *item)
{
  const GaussianBlurVars *blur = static_cast<const GaussianBlurVars *>(effect_strip->effectdata);

  auto eff = SerializableObject::Retainer<otio::Effect>(
      new otio::Effect(effect_strip->name + 2, "Gaussian Blur"));

  AnyDictionary metadata;
  metadata["name"] = "Gaussian Blur";
  metadata["size_x"] = static_cast<double>(blur->size_x);
  metadata["size_y"] = static_cast<double>(blur->size_y);
  eff->metadata()["blender"] = metadata;
  add_foreign_metadata(effect_strip->system_properties, eff.value);

  item->effects().push_back(eff);
}

static void handle_glow_effect_strip(const Strip *effect_strip, Item *item)
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
  add_foreign_metadata(effect_strip->system_properties, eff.value);

  item->effects().push_back(eff);
}

void add_strip_effect(const Scene *scene,
                      const Strip *strip,
                      const Strip *effect_strip,
                      Item *item)
{
  switch (effect_strip->type) {
    case STRIP_TYPE_SPEED:
      handle_speed_effect_strip(scene, strip, effect_strip, item);
      break;

    case STRIP_TYPE_GAUSSIAN_BLUR:
      handle_gaussian_blur_effect_strip(effect_strip, item);
      break;

    case STRIP_TYPE_GLOW:
      handle_glow_effect_strip(effect_strip, item);
      break;

    default:
      break;
  }
}

static void add_strip_metadata_sound(const Strip *strip, SerializableObjectWithMetadata *object)
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
  metadata["preserve_pitch"] = static_cast<bool>(strip->flag & SEQ_AUDIO_PITCH_CORRECTION);
  metadata["display_waveforms"] = static_cast<bool>(strip->flag & SEQ_AUDIO_DRAW_WAVEFORM);
  if (strip->sound) {
    metadata["mono"] = static_cast<bool>(strip->sound->flags & SOUND_FLAGS_MONO);
  }

  try {
    std::any_cast<AnyDictionary &>(object->metadata()["blender"])["sound"] = metadata;
  }
  catch (const std::bad_any_cast & /*e*/) {
    return;
  }
}

static void add_strip_metadata_crop(const Strip *strip, SerializableObjectWithMetadata *object)
{
  if (!strip->data || !strip->data->crop) {
    return;
  }
  const StripCrop *crop = strip->data->crop;
  AnyDictionary metadata;

  metadata["top"] = static_cast<int64_t>(crop->top);
  metadata["bottom"] = static_cast<int64_t>(crop->bottom);
  metadata["left"] = static_cast<int64_t>(crop->left);
  metadata["right"] = static_cast<int64_t>(crop->right);

  try {
    std::any_cast<AnyDictionary &>(object->metadata()["blender"])["crop"] = metadata;
  }
  catch (const std::bad_any_cast & /*e*/) {
    return;
  }
}

static void add_strip_metadata_transform(const Strip *strip,
                                         SerializableObjectWithMetadata *object)
{
  if (!strip->data || !strip->data->transform) {
    return;
  }
  const StripTransform *transform = strip->data->transform;
  AnyDictionary metadata;

  metadata["xofs"] = static_cast<double>(transform->xofs);
  metadata["yofs"] = static_cast<double>(transform->yofs);
  metadata["scale_x"] = static_cast<double>(transform->scale_x);
  metadata["scale_y"] = static_cast<double>(transform->scale_y);
  metadata["rotation"] = static_cast<double>(transform->rotation);
  metadata["origin"] = AnyVector{static_cast<double>(transform->origin[0]),
                                 static_cast<double>(transform->origin[1])};
  metadata["filter"] = static_cast<int64_t>(transform->filter);
  metadata["flipx"] = static_cast<bool>(strip->flag & SEQ_FLIPX);
  metadata["flipy"] = static_cast<bool>(strip->flag & SEQ_FLIPY);

  try {
    std::any_cast<AnyDictionary &>(object->metadata()["blender"])["transform"] = metadata;
  }
  catch (const std::bad_any_cast & /*e*/) {
    return;
  }
}

static void add_strip_metadata_compositing(const Strip *strip,
                                           SerializableObjectWithMetadata *object)
{
  AnyDictionary metadata;

  metadata["blend_mode"] = static_cast<int64_t>(strip->blend_mode);
  metadata["blend_opacity"] = static_cast<double>(strip->blend_opacity);

  try {
    std::any_cast<AnyDictionary &>(object->metadata()["blender"])["compositing"] = metadata;
  }
  catch (const std::bad_any_cast & /*e*/) {
    return;
  }
}

static void add_strip_metadata_color(const Strip *strip, SerializableObjectWithMetadata *object)
{
  AnyDictionary metadata;

  metadata["saturation"] = static_cast<double>(strip->sat);
  metadata["multiply"] = static_cast<double>(strip->mul);
  metadata["multiply_alpha"] = static_cast<bool>(strip->flag & SEQ_MULTIPLY_ALPHA);
  metadata["convert_to_float"] = static_cast<bool>(strip->flag & SEQ_MAKE_FLOAT);

  try {
    std::any_cast<AnyDictionary &>(object->metadata()["blender"])["color"] = metadata;
  }
  catch (const std::bad_any_cast & /*e*/) {
    return;
  }
}

static void add_modifier_metadata_to_container(AnyDictionary &metadata,
                                               AnyVector &container,
                                               StripModifierData &smd)
{
  AnyDictionary parent;
  parent["data"] = metadata;
  parent["name"] = std::string(smd.name);
  parent["type"] = static_cast<int64_t>(smd.type);
  parent["mute"] = static_cast<bool>(smd.flag & STRIP_MODIFIER_FLAG_MUTE);

  container.push_back(parent);
}

static void add_modifier_metadata_brightness_contrast(StripModifierData &smd, AnyVector &container)
{
  const BrightContrastModifierData *bcmd = reinterpret_cast<BrightContrastModifierData *>(&smd);
  AnyDictionary metadata;
  metadata["bright"] = static_cast<double>(bcmd->bright);
  metadata["contrast"] = static_cast<double>(bcmd->contrast);

  add_modifier_metadata_to_container(metadata, container, smd);
}

static void add_modifier_metadata_color_balance(StripModifierData &smd, AnyVector &container)
{
  const ColorBalanceModifierData *cbmd = reinterpret_cast<ColorBalanceModifierData *>(&smd);
  AnyDictionary metadata;
  metadata["color_multiply"] = static_cast<double>(cbmd->color_multiply);
  metadata["method"] = static_cast<int64_t>(cbmd->color_balance.method);
  metadata["flag"] = static_cast<int64_t>(cbmd->color_balance.flag);
  metadata["lift"] = AnyVector{static_cast<double>(cbmd->color_balance.lift[0]),
                               static_cast<double>(cbmd->color_balance.lift[1]),
                               static_cast<double>(cbmd->color_balance.lift[2])};

  metadata["gamma"] = AnyVector{static_cast<double>(cbmd->color_balance.gamma[0]),
                                static_cast<double>(cbmd->color_balance.gamma[1]),
                                static_cast<double>(cbmd->color_balance.gamma[2])};

  metadata["gain"] = AnyVector{static_cast<double>(cbmd->color_balance.gain[0]),
                               static_cast<double>(cbmd->color_balance.gain[1]),
                               static_cast<double>(cbmd->color_balance.gain[2])};

  metadata["slope"] = AnyVector{static_cast<double>(cbmd->color_balance.slope[0]),
                                static_cast<double>(cbmd->color_balance.slope[1]),
                                static_cast<double>(cbmd->color_balance.slope[2])};

  metadata["offset"] = AnyVector{static_cast<double>(cbmd->color_balance.offset[0]),
                                 static_cast<double>(cbmd->color_balance.offset[1]),
                                 static_cast<double>(cbmd->color_balance.offset[2])};

  metadata["power"] = AnyVector{static_cast<double>(cbmd->color_balance.power[0]),
                                static_cast<double>(cbmd->color_balance.power[1]),
                                static_cast<double>(cbmd->color_balance.power[2])};

  add_modifier_metadata_to_container(metadata, container, smd);
}

static void add_modifier_metadata_tonemap(StripModifierData &smd, AnyVector &container)
{
  const SequencerTonemapModifierData *tmd = reinterpret_cast<SequencerTonemapModifierData *>(&smd);
  AnyDictionary metadata;
  metadata["key"] = static_cast<double>(tmd->key);
  metadata["offset"] = static_cast<double>(tmd->offset);
  metadata["gamma"] = static_cast<double>(tmd->gamma);
  metadata["intensity"] = static_cast<double>(tmd->intensity);
  metadata["contrast"] = static_cast<double>(tmd->contrast);
  metadata["adaptation"] = static_cast<double>(tmd->adaptation);
  metadata["correction"] = static_cast<double>(tmd->correction);
  metadata["type"] = static_cast<int64_t>(tmd->type);

  add_modifier_metadata_to_container(metadata, container, smd);
}

static void add_modifier_metadata_white_balance(StripModifierData &smd, AnyVector &container)
{
  const WhiteBalanceModifierData *wbmd = reinterpret_cast<WhiteBalanceModifierData *>(&smd);
  AnyDictionary metadata;
  metadata["white_value"] = AnyVector{static_cast<double>(wbmd->white_value[0]),
                                      static_cast<double>(wbmd->white_value[1]),
                                      static_cast<double>(wbmd->white_value[2])};

  add_modifier_metadata_to_container(metadata, container, smd);
}

static void add_modifier_metadata_pitch(StripModifierData &smd, AnyVector &container)
{
  const PitchModifierData *pmd = reinterpret_cast<PitchModifierData *>(&smd);
  AnyDictionary metadata;
  metadata["mode"] = static_cast<int64_t>(pmd->mode);
  metadata["semitones"] = static_cast<int64_t>(pmd->semitones);
  metadata["cents"] = static_cast<int64_t>(pmd->cents);
  metadata["quality"] = static_cast<int64_t>(pmd->quality);
  metadata["ratio"] = static_cast<double>(pmd->ratio);
  metadata["preserve_formant"] = static_cast<bool>(pmd->preserve_formant);

  add_modifier_metadata_to_container(metadata, container, smd);
}

static void add_modifier_metadata_echo(StripModifierData &smd, AnyVector &container)
{
  const EchoModifierData *emd = reinterpret_cast<EchoModifierData *>(&smd);
  AnyDictionary metadata;
  metadata["delay"] = static_cast<double>(emd->delay);
  metadata["feedback"] = static_cast<double>(emd->feedback);
  metadata["mix"] = static_cast<double>(emd->mix);

  add_modifier_metadata_to_container(metadata, container, smd);
}

static AnyDictionary serialize_curve_mapping(const CurveMapping *cmp)
{
  AnyDictionary root;
  root["flag"] = static_cast<int64_t>(cmp->flag);
  root["preset"] = static_cast<int64_t>(cmp->preset);
  root["tone"] = static_cast<int64_t>(cmp->tone);
  root["cur"] = static_cast<int64_t>(cmp->cur);
  root["curr"] = AnyVector{static_cast<double>(cmp->curr.xmin),
                           static_cast<double>(cmp->curr.xmax),
                           static_cast<double>(cmp->curr.ymin),
                           static_cast<double>(cmp->curr.ymax)};

  root["clipr"] = AnyVector{static_cast<double>(cmp->clipr.xmin),
                            static_cast<double>(cmp->clipr.xmax),
                            static_cast<double>(cmp->clipr.ymin),
                            static_cast<double>(cmp->clipr.ymax)};

  root["black"] = AnyVector{static_cast<double>(cmp->black[0]),
                            static_cast<double>(cmp->black[1]),
                            static_cast<double>(cmp->black[2])};

  root["white"] = AnyVector{static_cast<double>(cmp->white[0]),
                            static_cast<double>(cmp->white[1]),
                            static_cast<double>(cmp->white[2])};

  AnyVector curve_maps;
  for (const int c : IndexRange(4)) {
    const CurveMap &cm = cmp->cm[c];
    AnyDictionary cm_dict;

    cm_dict["default_handle_type"] = static_cast<int64_t>(cm.default_handle_type);
    cm_dict["range"] = static_cast<double>(cm.range);

    AnyVector curve;
    curve.reserve(cm.totpoint);
    for (const int i : IndexRange(cm.totpoint)) {
      AnyDictionary point;
      point["x"] = static_cast<double>(cm.curve[i].x);
      point["y"] = static_cast<double>(cm.curve[i].y);
      point["flag"] = static_cast<int64_t>(cm.curve[i].flag);

      curve.push_back(point);
    }
    cm_dict["curve"] = curve;
    curve_maps.push_back((cm_dict));
  }

  root["curve_maps"] = curve_maps;
  return root;
}

static void add_modifier_metadata_curves(StripModifierData &smd, AnyVector &container)
{
  AnyDictionary metadata;
  switch (smd.type) {
    case eSeqModifierType_Curves: {
      const CurvesModifierData *cmd = reinterpret_cast<CurvesModifierData *>(&smd);
      metadata = serialize_curve_mapping(&cmd->curve_mapping);
      break;
    }

    case eSeqModifierType_HueCorrect: {
      const HueCorrectModifierData *hcmd = reinterpret_cast<HueCorrectModifierData *>(&smd);
      metadata = serialize_curve_mapping(&hcmd->curve_mapping);
      break;
    }

    case eSeqModifierType_SoundEqualizer: {
      const SoundEqualizerModifierData *semd = reinterpret_cast<SoundEqualizerModifierData *>(
          &smd);
      AnyVector graphics;
      for (const EQCurveMappingData &cmd : semd->graphics) {
        graphics.push_back(serialize_curve_mapping(&cmd.curve_mapping));
      }
      metadata["graphics"] = graphics;
      break;
    }

    default:
      break;
  }

  add_modifier_metadata_to_container(metadata, container, smd);
}

static void add_strip_metadata_modifiers(const Strip *strip,
                                         SerializableObjectWithMetadata *object)
{
  if (!object->metadata().has_key("blender")) {
    object->metadata()["blender"] = AnyDictionary();
  }

  AnyVector container;

  for (StripModifierData &smd : strip->modifiers) {
    switch (smd.type) {
      case eSeqModifierType_BrightContrast:
        add_modifier_metadata_brightness_contrast(smd, container);
        break;

      case eSeqModifierType_ColorBalance:
        add_modifier_metadata_color_balance(smd, container);
        break;

      case eSeqModifierType_Tonemap:
        add_modifier_metadata_tonemap(smd, container);
        break;

      case eSeqModifierType_WhiteBalance:
        add_modifier_metadata_white_balance(smd, container);
        break;

      case eSeqModifierType_Curves:
      case eSeqModifierType_HueCorrect:
      case eSeqModifierType_SoundEqualizer:
        add_modifier_metadata_curves(smd, container);
        break;

      case eSeqModifierType_Pitch:
        add_modifier_metadata_pitch(smd, container);
        break;

      case eSeqModifierType_Echo:
        add_modifier_metadata_echo(smd, container);
        break;

      default:
        break;
    }
  }

  try {
    AnyDictionary &bDict = std::any_cast<AnyDictionary &>(object->metadata()["blender"]);
    if (!bDict.has_key("modifiers")) {
      bDict["modifiers"] = container;
    }
  }
  catch (const std::bad_any_cast & /*e*/) {
    return;
  }
}

void add_strip_metadata(const Strip *strip, SerializableObjectWithMetadata *object)
{
  if (!object->metadata().has_key("blender")) {
    object->metadata()["blender"] = AnyDictionary();
  }
  if (strip->type == STRIP_TYPE_SOUND) {
    add_strip_metadata_sound(strip, object);
  }
  else {
    add_strip_metadata_transform(strip, object);
    add_strip_metadata_crop(strip, object);
    add_strip_metadata_compositing(strip, object);
    add_strip_metadata_color(strip, object);
  }
  add_strip_metadata_modifiers(strip, object);

  try {
    std::any_cast<AnyDictionary &>(
        object->metadata()["blender"])["original_channel"] = static_cast<int64_t>(strip->channel);
  }
  catch (const std::bad_any_cast & /*e*/) {
    return;
  }
}

}  // namespace blender::io::otio
