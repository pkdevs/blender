/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include <string>

namespace blender::asset_system {

/**
 * Returns every existing directory listed in the `BLENDER_PRODUCTION_DATAFILES` environment
 * variable, with the `assets` subfolder appended to each. These form the "Production" asset
 * library, a first-class library separate from Essentials.
 */
Vector<std::string> production_datafiles_directory_paths();

/**
 * The first directory returned by #production_datafiles_directory_paths(), or an empty string
 * if there are none. This is used as the "main" root path for the Production asset library.
 */
StringRefNull production_directory_path();

}  // namespace blender::asset_system
