/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

namespace blender {
struct bUserAssetLibrary;

namespace asset_system {

/**
 * Reads the Asset Library identifier (UUID) from the `blender_asset_lib.toml` library config file,
 * this would assign a new UUID if the identifier is not assigned yet.
 */
void user_asset_library_uuid_ensure(bUserAssetLibrary &custom_library);

}  // namespace asset_system
}  // namespace blender
