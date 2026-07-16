/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "DNA_sequence_types.h"

#include <opentimelineio/item.h>
#include <opentimelineio/transition.h>

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

struct TransitionMetadata {
  StripType type = STRIP_TYPE_CROSS;

  bool default_fade = true;
  float effect_fader = 0;

  /* Wipe. */
  float edgeWidth = 0;
  float angle = 0;
  short forward = 0;
  eEffectWipeType wipetype = SEQ_WIPE_SINGLE;
};

struct EffectStripWithInputsParams {
  Strip *effect_strip = nullptr;
  char input1[STRIP_NAME_MAXSTR];
  char input2[STRIP_NAME_MAXSTR];
  ListBaseT<Strip> *seqbase = nullptr;
};

TransitionMetadata fetch_transition_metadata(Transition *transition);
void set_strip_metadata(Item *item, Strip *strip);
void set_glow_metadata(otio::Effect *otio_effect, Strip *effect_strip);
void set_gaussian_blur_metadata(otio::Effect *otio_effect, Strip *effect_strip);
void set_effect_strip_params(AnyDictionary &params,
                             Strip *effect_strip,
                             EffectStripWithInputsParams &input_effect_params);

}  // namespace blender::io::otio
