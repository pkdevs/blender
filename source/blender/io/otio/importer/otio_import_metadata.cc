/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <any>
#include <concepts>
#include <string>

#include "BKE_colortools.hh"
#include "BKE_idprop.hh"

#include "BLI_math_base.hh"
#include "BLI_string.hh"

#include "CLG_log.h"

#include "DNA_curve_enums.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"

#include "SEQ_modifier.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_sound.hh"

#include <opentimelineio/anyDictionary.h>
#include <opentimelineio/anyVector.h>
#include <opentimelineio/effect.h>
#include <opentimelineio/serialization.h>

#include "IO_otio.hh"
#include "otio_import_metadata.hh"

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

static CLG_LogRef LOG = {"io.otio"};

/**
 * \return `true` if any_cast is successful
 */
template<typename T>
static bool any_cast_set(std::any any_value, T &destination, size_t size_max = 64)
{
  try {
    if constexpr (std::is_same_v<T, AnyDictionary> || std::is_same_v<T, AnyVector>) {
      destination = std::any_cast<T &>(any_value);
    }
    else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
      destination = static_cast<T>(std::any_cast<double>(any_value));
    }
    else if constexpr (std::is_same_v<T, bool>) {
      destination = std::any_cast<T>(any_value);
    }
    else if constexpr (std::is_same_v<T, std::string>) {
      destination = std::any_cast<std::string &>(any_value);
    }
    else if constexpr (std::is_same_v<T, char *> ||
                       (std::is_array_v<T> && std::is_same_v<std::remove_extent_t<T>, char>))
    {
      std::string &value = std::any_cast<std::string &>(any_value);
      BLI_strncpy(destination, value.c_str(), size_max);
    }
    else {
      /* int, short, enum. */
      destination = static_cast<T>(std::any_cast<int64_t>(any_value));
    }

    return true;
  }
  catch (const std::bad_any_cast &e) {
  }

  return false;
}

/**
 * \return `true` if any_cast is successful
 */
template<typename T>
static bool any_cast_set(AnyDictionary &any_dict,
                         const char *key,
                         T &destination,
                         size_t size_max = 64)
{
  if (!any_dict.has_key(key)) {
    return false;
  }
  return any_cast_set(any_dict[key], destination, size_max);
}

template<typename T>
static void any_cast_set_array(AnyDictionary &any_dict,
                               const char *key,
                               T *destination,
                               size_t size_max)
{
  if (!any_dict.has_key(key)) {
    return;
  }

  AnyVector vec;
  if (!any_cast_set(any_dict, key, vec)) {
    return;
  }

  for (size_t i = 0; i < std::min(vec.size(), size_max); ++i) {
    any_cast_set(vec[i], destination[i]);
  }
}

template<typename T>
concept BitwiseOperable = std::integral<T> ||
                          (std::is_enum_v<T> && std::integral<std::underlying_type_t<T>>);

/**
 * \return `true` if any_cast is successful
 */
template<BitwiseOperable T>
bool any_cast_set_flag(AnyDictionary &any_dict, const char *key, T &destination, T flag)
{
  bool value = false;

  if (!any_cast_set(any_dict, key, value)) {
    return false;
  }

  if (value) {
    destination |= flag;
  }
  else {
    destination &= ~flag;
  }
  return true;
}

TransitionMetadata fetch_transition_metadata(Transition *transition)
{
  TransitionMetadata transition_metadata;
  AnyDictionary metadata;

  if (!any_cast_set(transition->metadata(), "blender", metadata)) {
    return transition_metadata;
  }

  std::string name;
  any_cast_set(metadata, "name", name);
  if (name == "Wipe") {
    transition_metadata.type = STRIP_TYPE_WIPE;
  }
  else if (name == "Gamma Crossfade") {
    transition_metadata.type = STRIP_TYPE_GAMCROSS;
  }

  any_cast_set(metadata, "default_fade", transition_metadata.default_fade);
  any_cast_set(metadata, "effect_fader", transition_metadata.effect_fader);
  any_cast_set(metadata, "edgeWidth", transition_metadata.edgeWidth);
  any_cast_set(metadata, "angle", transition_metadata.angle);
  any_cast_set(metadata, "forward", transition_metadata.forward);
  any_cast_set(metadata, "wipetype", transition_metadata.wipetype);

  return transition_metadata;
}

static void set_strip_metadata_sound(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary sound;
  if (!any_cast_set(metadata, "sound", sound)) {
    return;
  }

  any_cast_set(sound, "volume", strip->volume);
  any_cast_set(sound, "speed_factor", strip->speed_factor);
  any_cast_set(sound, "pan", strip->pan);
  any_cast_set(sound, "sound_offset", strip->sound_offset);
  any_cast_set_flag(sound, "preserve_pitch", strip->flag, SEQ_AUDIO_PITCH_CORRECTION);
  any_cast_set_flag(sound, "display_waveforms", strip->flag, SEQ_AUDIO_DRAW_WAVEFORM);
  if (strip->sound) {
    any_cast_set_flag(sound, "mono", strip->sound->flags, SOUND_FLAGS_MONO);
  }
}

static void set_strip_metadata_transform(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary transform_metadata;
  if (!any_cast_set(metadata, "transform", transform_metadata) || !strip->data ||
      !strip->data->transform)
  {
    return;
  }

  StripTransform *transform = strip->data->transform;

  any_cast_set(transform_metadata, "xofs", transform->xofs);
  any_cast_set(transform_metadata, "yofs", transform->yofs);
  any_cast_set(transform_metadata, "scale_x", transform->scale_x);
  any_cast_set(transform_metadata, "scale_y", transform->scale_y);
  any_cast_set(transform_metadata, "rotation", transform->rotation);
  any_cast_set(transform_metadata, "filter", transform->filter);
  any_cast_set_flag(transform_metadata, "flipx", strip->flag, SEQ_FLIPX);
  any_cast_set_flag(transform_metadata, "flipy", strip->flag, SEQ_FLIPY);

  AnyVector origin;
  any_cast_set(transform_metadata, "origin", origin);
  for (size_t i = 0; i < std::min((size_t)2, origin.size()); ++i) {
    any_cast_set(origin[i], transform->origin[i]);
  }
}

static void set_strip_metadata_crop(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary crop_metadata;
  if (!any_cast_set(metadata, "crop", crop_metadata) || !strip->data || !strip->data->crop) {
    return;
  }

  StripCrop *crop = strip->data->crop;

  any_cast_set(crop_metadata, "top", crop->top);
  any_cast_set(crop_metadata, "bottom", crop->bottom);
  any_cast_set(crop_metadata, "left", crop->left);
  any_cast_set(crop_metadata, "right", crop->right);
}

static void set_strip_metadata_compositing(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary compositing_metadata;
  if (!any_cast_set(metadata, "compositing", compositing_metadata)) {
    return;
  }

  any_cast_set(compositing_metadata, "blend_mode", strip->blend_mode);
  any_cast_set(compositing_metadata, "blend_opacity", strip->blend_opacity);
}

static void set_strip_metadata_color(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary color_metadata;
  if (!any_cast_set(metadata, "color", color_metadata)) {
    return;
  }

  any_cast_set(color_metadata, "saturation", strip->sat);
  any_cast_set(color_metadata, "multiply", strip->mul);
  any_cast_set_flag(color_metadata, "multiply_alpha", strip->flag, SEQ_MULTIPLY_ALPHA);
  any_cast_set_flag(color_metadata, "convert_to_float", strip->flag, SEQ_MAKE_FLOAT);
}

static void set_modifier_metadata_brightness_contrast(AnyDictionary &data, StripModifierData *smd)
{
  BrightContrastModifierData *bcmd = reinterpret_cast<BrightContrastModifierData *>(smd);
  any_cast_set(data, "bright", bcmd->bright);
  any_cast_set(data, "contrast", bcmd->contrast);
}

static void set_modifier_metadata_color_balance(AnyDictionary &data, StripModifierData *smd)
{
  ColorBalanceModifierData *cbmd = reinterpret_cast<ColorBalanceModifierData *>(smd);
  StripColorBalance &cb = cbmd->color_balance;

  any_cast_set(data, "color_multiply", cbmd->color_multiply);
  any_cast_set(data, "method", cb.method);
  any_cast_set(data, "flag", cb.flag);
  any_cast_set_array(data, "lift", cb.lift, std::size(cb.lift));
  any_cast_set_array(data, "gamma", cb.gamma, std::size(cb.gamma));
  any_cast_set_array(data, "gain", cb.gain, std::size(cb.gain));
  any_cast_set_array(data, "slope", cb.slope, std::size(cb.slope));
  any_cast_set_array(data, "offset", cb.offset, std::size(cb.offset));
  any_cast_set_array(data, "power", cb.power, std::size(cb.power));
}

static void set_modifier_metadata_tonemap(AnyDictionary &data, StripModifierData *smd)
{
  SequencerTonemapModifierData *tmd = reinterpret_cast<SequencerTonemapModifierData *>(smd);

  any_cast_set(data, "key", tmd->key);
  any_cast_set(data, "offset", tmd->offset);
  any_cast_set(data, "gamma", tmd->gamma);
  any_cast_set(data, "intensity", tmd->intensity);
  any_cast_set(data, "contrast", tmd->contrast);
  any_cast_set(data, "adaptation", tmd->adaptation);
  any_cast_set(data, "correction", tmd->correction);
  any_cast_set(data, "type", tmd->type);
}

static void set_modifier_metadata_pitch(AnyDictionary &data, StripModifierData *smd)
{
  PitchModifierData *pmd = reinterpret_cast<PitchModifierData *>(smd);

  any_cast_set(data, "mode", pmd->mode);
  any_cast_set(data, "semitones", pmd->semitones);
  any_cast_set(data, "cents", pmd->cents);
  any_cast_set(data, "quality", pmd->quality);
  any_cast_set(data, "ratio", pmd->ratio);
  any_cast_set(data, "preserve_formant", pmd->preserve_formant);
}

static void set_modifier_metadata_echo(AnyDictionary &data, StripModifierData *smd)
{
  EchoModifierData *emd = reinterpret_cast<EchoModifierData *>(smd);

  any_cast_set(data, "delay", emd->delay);
  any_cast_set(data, "feedback", emd->feedback);
  any_cast_set(data, "mix", emd->mix);
}

static void set_modifier_metadata_white_balance(AnyDictionary &data, StripModifierData *smd)
{
  WhiteBalanceModifierData *wbmd = reinterpret_cast<WhiteBalanceModifierData *>(smd);
  any_cast_set_array(data, "white_value", wbmd->white_value, std::size(wbmd->white_value));
}

static void set_curve_mapping(AnyDictionary &metadata, CurveMapping *cmp)
{
  any_cast_set(metadata, "flag", cmp->flag);
  cmp->flag &= ~CUMA_PREMULLED;
  any_cast_set(metadata, "preset", cmp->preset);
  any_cast_set(metadata, "tone", cmp->tone);
  any_cast_set(metadata, "cur", cmp->cur);

  AnyVector curr;
  any_cast_set(metadata, "curr", curr);
  if (curr.size() >= 4) {
    any_cast_set(curr[0], cmp->curr.xmin);
    any_cast_set(curr[1], cmp->curr.xmax);
    any_cast_set(curr[2], cmp->curr.ymin);
    any_cast_set(curr[3], cmp->curr.ymax);
  }

  AnyVector clipr;
  any_cast_set(metadata, "clipr", clipr);
  if (clipr.size() >= 4) {
    any_cast_set(clipr[0], cmp->clipr.xmin);
    any_cast_set(clipr[1], cmp->clipr.xmax);
    any_cast_set(clipr[2], cmp->clipr.ymin);
    any_cast_set(clipr[3], cmp->clipr.ymax);
  }

  any_cast_set_array(metadata, "black", cmp->black, std::size(cmp->black));
  any_cast_set_array(metadata, "white", cmp->white, std::size(cmp->white));
  any_cast_set_array(metadata, "sample", cmp->sample, std::size(cmp->sample));

  AnyVector curve_maps;
  any_cast_set(metadata, "curve_maps", curve_maps);
  for (size_t c = 0; c < std::min(curve_maps.size(), (size_t)4); ++c) {
    CurveMap &cm = cmp->cm[c];

    AnyDictionary cm_dict;
    any_cast_set(curve_maps[c], cm_dict);

    any_cast_set(cm_dict, "default_handle_type", cm.default_handle_type);
    any_cast_set_array(cm_dict, "ext_in", cm.ext_in, std::size(cm.ext_in));
    any_cast_set_array(cm_dict, "ext_out", cm.ext_out, std::size(cm.ext_out));

    AnyVector curve;
    any_cast_set(cm_dict, "curve", curve);

    if (curve.size() < 2) {
      continue;
    }

    /* Free default curve array and allocate new one of appropriate size. */
    MEM_SAFE_DELETE(cm.curve);
    cm.totpoint = curve.size();
    cm.curve = MEM_new_array<CurveMapPoint>(curve.size(), "curve points");

    for (size_t i = 0; i < curve.size(); ++i) {
      AnyDictionary point;
      any_cast_set(curve[i], point);

      any_cast_set(point, "x", cm.curve[i].x);
      any_cast_set(point, "y", cm.curve[i].y);
      cm.curve[i].flag = cm.default_handle_type;
      any_cast_set(point, "flag", cm.curve[i].flag);
      any_cast_set(point, "shorty", cm.curve[i].shorty);
    }
  }
  BKE_curvemapping_changed_all(cmp);
}

static void set_modifier_metadata_curves(AnyDictionary &data, StripModifierData *smd)
{
  switch (smd->type) {
    case eSeqModifierType_Curves: {
      CurvesModifierData *cmd = reinterpret_cast<CurvesModifierData *>(smd);
      set_curve_mapping(data, &cmd->curve_mapping);
      break;
    }

    case eSeqModifierType_HueCorrect: {
      HueCorrectModifierData *hcmd = reinterpret_cast<HueCorrectModifierData *>(smd);
      set_curve_mapping(data, &hcmd->curve_mapping);
      break;
    }

    case eSeqModifierType_SoundEqualizer: {
      SoundEqualizerModifierData *semd = reinterpret_cast<SoundEqualizerModifierData *>(smd);
      AnyVector graphics;
      any_cast_set(data, "graphics", graphics);
      if (graphics.empty()) {
        break;
      }
      /* Remove the default graph. */
      seq::sound_equalizermodifier_free(smd);

      for (size_t i = 0; i < graphics.size(); ++i) {
        AnyDictionary curve_metadata;
        any_cast_set(graphics[i], curve_metadata);
        AnyVector clipr;
        any_cast_set(curve_metadata, "clipr", clipr);

        float min_freq = SOUND_EQUALIZER_DEFAULT_MIN_FREQ;
        float max_freq = SOUND_EQUALIZER_DEFAULT_MAX_FREQ;
        if (clipr.size() >= 2) {
          any_cast_set(clipr[0], min_freq);
          any_cast_set(clipr[1], max_freq);
        }

        EQCurveMappingData *eqcmd = seq::sound_equalizermodifier_add_graph(
            semd, min_freq, max_freq);
        set_curve_mapping(curve_metadata, &eqcmd->curve_mapping);
      }

      break;
    }

    default:
      break;
  }
}

static void set_strip_metadata_modifiers(AnyDictionary &metadata, Strip *strip)
{
  AnyVector modifiers;
  if (!any_cast_set(metadata, "modifiers", modifiers)) {
    return;
  }

  for (size_t i = 0; i < modifiers.size(); ++i) {
    AnyDictionary parent;
    any_cast_set(modifiers[i], parent);

    char name[64];
    eStripModifierType type = eSeqModifierType_None;
    AnyDictionary data;

    any_cast_set(parent, "name", name, sizeof(name));
    any_cast_set(parent, "type", type);

    if (any_cast_set(parent, "data", data)) {

      StripModifierData *smd = seq::modifier_new(strip, name, type);
      seq::modifier_persistent_uid_init(*strip, *smd);
      any_cast_set_flag(parent, "mute", smd->flag, STRIP_MODIFIER_FLAG_MUTE);

      switch (type) {
        case eSeqModifierType_BrightContrast:
          set_modifier_metadata_brightness_contrast(data, smd);
          break;

        case eSeqModifierType_ColorBalance:
          set_modifier_metadata_color_balance(data, smd);
          break;

        case eSeqModifierType_Tonemap:
          set_modifier_metadata_tonemap(data, smd);
          break;

        case eSeqModifierType_WhiteBalance:
          set_modifier_metadata_white_balance(data, smd);
          break;

        case eSeqModifierType_Curves:
        case eSeqModifierType_HueCorrect:
        case eSeqModifierType_SoundEqualizer:
          set_modifier_metadata_curves(data, smd);
          break;

        case eSeqModifierType_Pitch:
          set_modifier_metadata_pitch(data, smd);
          break;

        case eSeqModifierType_Echo:
          set_modifier_metadata_echo(data, smd);
          break;

        default:
          break;
      }
    }
  }
}

void set_strip_metadata(Item *item, Strip *strip)
{
  AnyDictionary metadata;
  if (!any_cast_set(item->metadata(), "blender", metadata)) {
    return;
  }

  if (strip->type == STRIP_TYPE_SOUND) {
    set_strip_metadata_sound(metadata, strip);
    set_strip_metadata_modifiers(metadata, strip);
    return;
  }

  set_strip_metadata_transform(metadata, strip);
  set_strip_metadata_crop(metadata, strip);
  set_strip_metadata_compositing(metadata, strip);
  set_strip_metadata_color(metadata, strip);
  set_strip_metadata_modifiers(metadata, strip);
}

void set_glow_metadata(otio::Effect *otio_effect, Strip *effect_strip)
{
  AnyDictionary metadata;
  if (!any_cast_set(otio_effect->metadata(), "blender", metadata)) {
    return;
  }

  GlowVars *glow = static_cast<GlowVars *>(effect_strip->effectdata);

  any_cast_set(metadata, "fMini", glow->fMini);
  any_cast_set(metadata, "fClamp", glow->fClamp);
  any_cast_set(metadata, "fBoost", glow->fBoost);
  any_cast_set(metadata, "dDist", glow->dDist);
  any_cast_set(metadata, "dQuality", glow->dQuality);
  any_cast_set(metadata, "bNoComp", glow->bNoComp);
}

void set_gaussian_blur_metadata(otio::Effect *otio_effect, Strip *effect_strip)
{
  AnyDictionary metadata;
  if (!any_cast_set(otio_effect->metadata(), "blender", metadata)) {
    return;
  }

  GaussianBlurVars *blur = static_cast<GaussianBlurVars *>(effect_strip->effectdata);

  any_cast_set(metadata, "size_x", blur->size_x);
  any_cast_set(metadata, "size_y", blur->size_y);
}

static void set_color_strip_params(AnyDictionary &params, Strip *effect_strip)
{
  AnyDictionary metadata;
  if (!any_cast_set(params, "blender", metadata)) {
    return;
  }

  SolidColorVars *color = static_cast<SolidColorVars *>(effect_strip->effectdata);

  any_cast_set_array(metadata, "col", color->col, std::size(color->col));
  any_cast_set(metadata, "width", color->width);
  any_cast_set(metadata, "height", color->height);
}

static void set_text_strip_params(AnyDictionary &params, Strip *effect_strip)
{
  AnyDictionary metadata;
  if (!any_cast_set(params, "blender", metadata)) {
    return;
  }

  TextVars *text = static_cast<TextVars *>(effect_strip->effectdata);

  MEM_SAFE_DELETE(text->text_ptr);
  std::string text_str;
  any_cast_set(metadata, "text", text_str);
  text->text_ptr = BLI_strdup(text_str.c_str());
  text->text_len_bytes = strlen(text->text_ptr);

  any_cast_set(metadata, "text_size", text->text_size);
  any_cast_set(metadata, "space_line", text->space_line);
  any_cast_set(metadata, "abs_space_line", text->abs_space_line);
  any_cast_set_array(metadata, "loc", text->loc, std::size(text->loc));
  any_cast_set(metadata, "wrap_width", text->wrap_width);
  any_cast_set(metadata, "box_margin", text->box_margin);
  any_cast_set(metadata, "box_roundness", text->box_roundness);
  any_cast_set(metadata, "shadow_angle", text->shadow_angle);
  any_cast_set(metadata, "shadow_offset", text->shadow_offset);
  any_cast_set(metadata, "shadow_blur", text->shadow_blur);
  any_cast_set(metadata, "outline_width", text->outline_width);
  any_cast_set(metadata, "flag", text->flag);
  any_cast_set(metadata, "align", text->align);
  any_cast_set(metadata, "cursor_offset", text->cursor_offset);
  any_cast_set(metadata, "selection_start_offset", text->selection_start_offset);
  any_cast_set(metadata, "selection_end_offset", text->selection_end_offset);
  any_cast_set(metadata, "anchor_x", text->anchor_x);
  any_cast_set(metadata, "anchor_y", text->anchor_y);
  any_cast_set(metadata, "textbox_state.visible_lines", text->textbox_state.visible_lines);
  any_cast_set(metadata, "textbox_state.scroll", text->textbox_state.scroll);
  any_cast_set_array(metadata, "color", text->color, std::size(text->color));
  any_cast_set_array(metadata, "shadow_color", text->shadow_color, std::size(text->shadow_color));
  any_cast_set_array(metadata, "box_color", text->box_color, std::size(text->box_color));
  any_cast_set_array(
      metadata, "outline_color", text->outline_color, std::size(text->outline_color));
}

void set_effect_strip_params(AnyDictionary &params,
                             Strip *effect_strip,
                             EffectStripWithInputsParams &input_effect_params)
{
  AnyDictionary metadata;
  if (!any_cast_set(params, "blender", metadata)) {
    return;
  }

  any_cast_set(
      metadata, "input1", input_effect_params.input1, std::size(input_effect_params.input1));
  any_cast_set(
      metadata, "input2", input_effect_params.input2, std::size(input_effect_params.input2));
  any_cast_set(metadata, "effect_fader", effect_strip->effect_fader);
  any_cast_set_flag(metadata, "default_fade", effect_strip->flag, SEQ_USE_EFFECT_DEFAULT_FADE);

  if (effect_strip->type == STRIP_TYPE_COLORMIX) {
    ColorMixVars *mix = static_cast<ColorMixVars *>(effect_strip->effectdata);
    any_cast_set(metadata, "blend_effect", mix->blend_effect);
    any_cast_set(metadata, "blend_factor", mix->factor);
  }
  else if (effect_strip->type == STRIP_TYPE_COLOR) {
    set_color_strip_params(params, effect_strip);
  }
  else if (effect_strip->type == STRIP_TYPE_TEXT) {
    set_text_strip_params(params, effect_strip);
  }
}

int get_strip_original_channel(Item *item)
{
  AnyDictionary metadata;
  if (!any_cast_set(item->metadata(), "blender", metadata)) {
    return 0;
  }

  int channel = 0;
  any_cast_set(metadata, "original_channel", channel);

  return math::clamp(channel, 0, seq::MAX_CHANNELS);
}

void store_foreign_metadata(AnyDictionary &root, IDProperty *&idp)
{
  if (idp && idp->type != IDP_GROUP) {
    return;
  }

  if (!idp) {
    idp = MEM_new<IDProperty>(__func__);
    idp->type = IDP_GROUP;
  }

  IDProperty *otio_group = IDP_GetPropertyFromGroup(idp, "otio_metadata");
  if (!otio_group) {
    IDPropertyTemplate val{};
    otio_group = IDP_New(IDP_GROUP, &val, "otio_metadata");
    IDP_AddToGroup(idp, otio_group);
  }

  for (const auto &[key, metadata] : root) {
    if (key == "blender") {
      continue;
    }

    ErrorStatus es;
    std::string serialized_str = serialize_json_to_string(metadata, nullptr, &es, 0);

    if (is_error(es)) {
      CLOG_ERROR(
          &LOG,
          "Failed to serialize foreign metadata with key: '%s'\nDetails: %s\nDescription: %s",
          key.c_str(),
          es.details.c_str(),
          es.full_description.c_str());
      continue;
    }

    IDP_AddToGroup(otio_group, IDP_NewString(serialized_str, key));
  }
}
}  // namespace blender::io::otio
