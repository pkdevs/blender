/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 *
 * Best effort conversion of DaVinci Resolve metadata.
 */

#pragma once

#include <optional>

#include "DNA_sequence_types.h"

#include "opentimelineio/generatorReference.h"
#include "opentimelineio/track.h"

namespace blender {
struct Scene;
}  // namespace blender

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

std::optional<StripType> resolve_generator_type(const GeneratorReference &generator);
AnyDictionary resolve_strip_properties(const Scene &scene,
                                       const Strip &strip,
                                       SerializableObjectWithMetadata &object);
bool resolve_track_locked(Track &track);

}  // namespace blender::io::otio
