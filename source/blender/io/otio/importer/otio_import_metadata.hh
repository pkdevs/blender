/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "DNA_sequence_types.h"

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

TransitionMetadata fetch_transition_metadata(Transition *transition);

}  // namespace blender::io::otio
