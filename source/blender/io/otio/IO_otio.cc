/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include "BLI_path_utils.hh"

#include "IO_otio.hh"
#include "otio_export.hh"

namespace blender {
wmOperatorStatus OTIO_export(bContext *C, const OTIOExportParams *export_params)
{
  return io::otio::otio_export_exec(C, export_params);
}
}  // namespace blender
