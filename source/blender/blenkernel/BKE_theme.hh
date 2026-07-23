/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"

#include "DNA_userdef_types.h"

namespace blender::bke {

/** Resolve #UserDef::active_theme_type, accounting for "System" mode. */
UserPrefThemeType resolve_active_theme_type();

/** The #bTheme referenced by whichever of dark/light is currently active. */
const bTheme &active_theme_get();

/**
 * Reset an already-loaded theme's values to the compiled-in baseline, keeping its `filepath`
 * and position in #UserDef::themes. Only meaningful for a theme whose values may have
 * diverged from a fresh load of its own `filepath` (e.g. live edits): callers still need to
 * load `filepath`'s own preset values back on top afterward, only implemented in Python, see
 * `_rna_xml.py`.
 */
void reset_theme(bTheme &theme);

/**
 * Find the #bTheme in #UserDef::themes backed by `filepath` (creating it if none exists yet),
 * and point #UserDef::active_theme_dark or #UserDef::active_theme_light (per `slot`) at it.
 *
 * A newly created theme has its values reset to the compiled-in baseline (see #reset_theme),
 * ready for the caller to overlay the preset's own values on top (loading the XML itself is
 * currently only implemented in Python, see `_rna_xml.py`).
 */
bTheme &ensure_theme(StringRefNull filepath, UserPrefThemeType slot);

}  // namespace blender::bke
