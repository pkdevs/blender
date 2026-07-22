/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_userdef_types.h"

namespace blender::bke {

const bTheme &active_theme_get();

/** Reload a theme to match its source file. */
void reset_theme(bTheme &theme);

/** Save a theme's current state to its source file. */
void save_theme(bTheme &theme);

}  // namespace blender::bke
