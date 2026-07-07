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

void build_blender_timeline(Main *bmain,
                            Scene *scene,
                            SerializableObject::Retainer<Timeline> &timeline,
                            ReportList *reports);

}  // namespace io::otio
}  // namespace blender
