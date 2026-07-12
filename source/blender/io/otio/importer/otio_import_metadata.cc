/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <string>

#include "BLI_string.hh"

#include <opentimelineio/anyDictionary.h>

#include "otio_import_metadata.hh"

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

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
  try {
    if constexpr (std::is_same_v<T, AnyDictionary>) {
      destination = std::any_cast<AnyDictionary &>(any_dict[key]);
    }
    else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
      destination = static_cast<T>(std::any_cast<double>(any_dict[key]));
    }
    else if constexpr (std::is_same_v<T, bool>) {
      destination = std::any_cast<T>(any_dict[key]);
    }
    else if constexpr (std::is_same_v<T, std::string>) {
      destination = std::any_cast<std::string &>(any_dict[key]);
    }
    else if constexpr (std::is_same_v<T, char *>) {
      std::string &value = std::any_cast<std::string &>(any_dict[key]);
      BLI_strncpy(destination, value.c_str(), size_max);
    }
    else {
      /* int, short, enum. */
      destination = static_cast<T>(std::any_cast<int64_t>(any_dict[key]));
    }

    return true;
  }
  catch (const std::bad_any_cast &e) {
  }

  return false;
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
}  // namespace blender::io::otio
