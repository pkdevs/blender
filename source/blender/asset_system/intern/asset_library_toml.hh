/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "AS_file_reader_result.hh"

namespace blender {
struct bUserAssetLibrary;

namespace asset_system {

/**
 * Read the Asset Library identifier (UUID) from the blender_asset_lib.toml library config file.
 * If the file is missing, a new UUID is assigned and the file is written to store it.
 */
void user_asset_library_uuid_ensure(bUserAssetLibrary &custom_library, ReportList *reports);

}  // namespace asset_system
}  // namespace blender
