/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include <opentimelineio/imageSequenceReference.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/transition.h>

namespace blender {

struct ReportList;
struct Scene;
struct Main;

namespace io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

struct ImageStripParams {
  char name[FILE_MAX];
  char path[FILE_MAX];
  /* Image Sequence. */
  char name_prefix[FILE_MAX];
  char name_suffix[FILE_MAX];
  int start_frame = 0;
  int padding = 0;
  int count = 1;
  ImageSequenceReference::MissingFramePolicy missing_policy;
};

struct TransitionParams {
  Transition *otio_transition = nullptr;
  Strip *input1 = nullptr;
  Strip *input2 = nullptr;
  int channel = 1;

  /**
   * \return `true` when all members are non-null, otherwise `false`.
   */
  bool set_input(Strip *strip)
  {
    if (!strip) {
      /* Reset incase `strip` is meant to be `input2` but it is `nullptr` so that the next call to
       * this function should set `input1` and not `input2`. */
      reset();
      return false;
    }

    if (!input1 || !otio_transition) {
      input1 = strip;
      return false;
    }

    input2 = strip;
    return true;
  }

  void set_transition(Transition *transition)
  {
    otio_transition = transition;
  }

  void set_channel(int ch)
  {
    channel = ch;
  }

  void reset()
  {
    otio_transition = nullptr;
    input1 = nullptr;
    input2 = nullptr;
    channel = 1;
  }
};

void build_blender_timeline(Main *bmain,
                            Scene *scene,
                            SerializableObject::Retainer<Timeline> &timeline,
                            ReportList *reports);

}  // namespace io::otio
}  // namespace blender
