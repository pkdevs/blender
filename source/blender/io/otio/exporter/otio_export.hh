/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "DNA_windowmanager_enums.h"

namespace blender {
struct bContext;
struct OTIOExportParams;

namespace io::otio {

wmOperatorStatus otio_export_exec(bContext *C, const OTIOExportParams *export_params);

}  // namespace io::otio
}  // namespace blender
