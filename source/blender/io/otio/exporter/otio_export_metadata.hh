/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "opentimelineio/generatorReference.h"
#include "opentimelineio/item.h"
#include "opentimelineio/transition.h"

namespace blender {
struct IDProperty;
struct Scene;
struct Strip;
}  // namespace blender

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

void add_strip_metadata(const Strip *strip, SerializableObjectWithMetadata *object);
void add_foreign_metadata(IDProperty *idp, SerializableObjectWithMetadata *object);
void add_strip_effect(const Scene *scene,
                      const Strip *strip,
                      const Strip *effect_strip,
                      Item *item);

SerializableObject::Retainer<GeneratorReference> create_generator_reference(const Strip *strip);
SerializableObject::Retainer<Transition> create_transition(const Strip *strip,
                                                           RationalTime in_offset,
                                                           RationalTime out_offset);

}  // namespace blender::io::otio
