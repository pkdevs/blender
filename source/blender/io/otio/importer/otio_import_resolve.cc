/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_color_c.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

#include "opentimelineio/anyVector.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/effect.h"

#include "otio_import_resolve.hh"
#include "otio_metadata.hh"

namespace blender::io::otio {

static const std::any *find(const AnyDictionary &dictionary, const std::string &key)
{
  auto it = dictionary.find(key);
  return it != dictionary.end() ? &it->second : nullptr;
}

template<typename T> static const T *find(const AnyDictionary &dictionary, const std::string &key)
{
  const std::any *value = find(dictionary, key);
  return value ? std::any_cast<T>(value) : nullptr;
}

static const AnyDictionary *resolve_metadata(SerializableObjectWithMetadata &object)
{
  return find<AnyDictionary>(object.metadata(), "Resolve_OTIO");
}

static const std::any *parameter(const AnyDictionary &effect, const std::string &id)
{
  const AnyVector *parameters = find<AnyVector>(effect, "Parameters");
  if (!parameters) {
    return nullptr;
  }
  for (const std::any &item : *parameters) {
    const AnyDictionary *parameter = std::any_cast<AnyDictionary>(&item);
    const std::string *name = parameter ? find<std::string>(*parameter, "Parameter ID") : nullptr;
    if (name && *name == id) {
      return find(*parameter, "Parameter Value");
    }
  }
  return nullptr;
}

static std::optional<double> number(const std::any *value)
{
  return value ? to_number(*value) : std::nullopt;
}

static void copy_number(AnyDictionary &dst,
                        const char *property,
                        const AnyDictionary &effect,
                        const std::string &id,
                        const double factor)
{
  if (const std::optional<double> value = number(parameter(effect, id))) {
    dst[property] = *value * factor;
  }
}

static const char *blend_type_from_composite_mode(const int64_t mode)
{
  static const char *blend_types[] = {
      "ALPHA_OVER", "ADD",        "SUBTRACT",    "DIFFERENCE",   "MULTIPLY",    "SCREEN",
      "OVERLAY",    "HARD_LIGHT", "SOFT_LIGHT",  "DARKEN",       "LIGHTEN",     "DODGE",
      "BURN",       "EXCLUSION",  "HUE",         "SATURATION",   "COLOR",       "VALUE",
      nullptr,      "ADD",        "LINEAR_BURN", "LINEAR_LIGHT", "VIVID_LIGHT", "PIN_LIGHT",
  };
  return mode >= 0 && mode < int64_t(std::size(blend_types)) ? blend_types[mode] : nullptr;
}

static AnyVector hex_to_color(const std::string &hex)
{
  float3 color(0.0f);
  hex_to_rgb(hex.c_str(), &color.x, &color.y, &color.z);
  return AnyVector{double(color.x), double(color.y), double(color.z)};
}

static void add_effect_properties(const AnyDictionary &effect,
                                  const Scene &scene,
                                  const Strip &strip,
                                  AnyDictionary &properties)
{
  const std::string *name = find<std::string>(effect, "Effect Name");
  const bool *enabled = find<bool>(effect, "Enabled");
  if (!name || (enabled && !*enabled)) {
    return;
  }

  const bool has_media = strip.data && strip.data->stripdata &&
                         strip.data->stripdata->orig_width > 0;
  const float2 media_size = has_media ? float2(strip.data->stripdata->orig_width,
                                               strip.data->stripdata->orig_height) :
                                        float2(scene.r.xsch, scene.r.ysch);
  const float2 fit_scale = strip.data && strip.data->transform ?
                               float2(strip.data->transform->scale_x,
                                      strip.data->transform->scale_y) :
                               float2(1.0f);

  if (*name == "Transform") {
    AnyDictionary transform;
    copy_number(transform, "scale_x", effect, "transformationZoomX", fit_scale.x);
    copy_number(transform, "scale_y", effect, "transformationZoomY", fit_scale.y);
    copy_number(transform, "offset_x", effect, "transformationPan", scene.r.xsch);
    copy_number(transform, "offset_y", effect, "transformationTilt", scene.r.ysch);
    copy_number(transform, "rotation", effect, "transformationRotationAngle", M_PI / 180.0);
    properties["transform"] = transform;
  }
  else if (*name == "Cropping") {
    AnyDictionary crop;
    copy_number(crop, "min_x", effect, "cropLeft", media_size.x);
    copy_number(crop, "max_x", effect, "cropRight", media_size.x);
    copy_number(crop, "min_y", effect, "cropBottom", media_size.y);
    copy_number(crop, "max_y", effect, "cropTop", media_size.y);
    properties["crop"] = crop;
  }
  else if (*name == "Composite") {
    copy_number(properties, "blend_alpha", effect, "opacity", 0.01);
    const std::any *mode = parameter(effect, "composite mode");
    const int64_t *mode_value = mode ? std::any_cast<int64_t>(mode) : nullptr;
    if (const char *blend_type = mode_value ? blend_type_from_composite_mode(*mode_value) :
                                              nullptr)
    {
      properties["blend_type"] = std::string(blend_type);
    }
  }
  else if (*name == "Fairlight Clip Volume and Fades") {
    if (const std::optional<double> decibels = number(parameter(effect, "volume"))) {
      properties["volume"] = std::pow(10.0, *decibels / 20.0);
    }
  }
  else if (*name == "Fairlight Clip Pan") {
    copy_number(properties, "pan", effect, "pan", 0.01);
  }
  else if (*name == "Solid Color") {
    const std::any *color = parameter(effect, "color");
    if (const std::string *hex = color ? std::any_cast<std::string>(color) : nullptr) {
      properties["color"] = hex_to_color(*hex);
    }
  }
  else if (*name == "Rich Text") {
    if (const std::any *text = parameter(effect, "rich text")) {
      properties["text"] = *text;
    }
    if (const std::any *position = parameter(effect, "position")) {
      properties["location"] = *position;
    }
  }
}

std::optional<StripType> resolve_generator_type(const GeneratorReference &generator)
{
  const std::string &kind = generator.generator_kind();
  if (kind == "Solid Color") {
    return STRIP_TYPE_COLOR;
  }
  if (kind == "Text" || StringRef(kind).startswith("Rich")) {
    return STRIP_TYPE_TEXT;
  }
  return std::nullopt;
}

AnyDictionary resolve_strip_properties(const Scene &scene,
                                       const Strip &strip,
                                       SerializableObjectWithMetadata &object)
{
  Vector<const AnyDictionary *> effects;
  if (auto *item = dynamic_cast<Item *>(&object)) {
    for (const SerializableObject::Retainer<otio::Effect> &effect : item->effects()) {
      if (const AnyDictionary *resolve = resolve_metadata(*effect)) {
        effects.append(resolve);
      }
    }
  }
  if (auto *clip = dynamic_cast<Clip *>(&object)) {
    if (auto *generator = dynamic_cast<GeneratorReference *>(clip->media_reference())) {
      if (const AnyVector *list = find<AnyVector>(generator->parameters(), "Resolve_OTIO")) {
        for (const std::any &item : *list) {
          if (const AnyDictionary *effect = std::any_cast<AnyDictionary>(&item)) {
            effects.append(effect);
          }
        }
      }
    }
  }

  AnyDictionary properties;
  for (const AnyDictionary *effect : effects) {
    add_effect_properties(*effect, scene, strip, properties);
  }
  return properties;
}

bool resolve_track_locked(Track &track)
{
  const AnyDictionary *resolve = resolve_metadata(track);
  const bool *locked = resolve ? find<bool>(*resolve, "Locked") : nullptr;
  return locked && *locked;
}

}  // namespace blender::io::otio
