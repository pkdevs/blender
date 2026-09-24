/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "IO_otio.hh"

namespace blender {

struct Main;
struct Scene;

namespace io::otio {

void exporter_main(Main *bmain, Scene *scene, const OTIOExportParams &params);

}  // namespace io::otio
}  // namespace blender
