/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"

#include "DNA_userdef_types.h"

namespace blender::bke {

/** Resolve #UserDef::active_theme_type to a concrete Dark or Light slot (never System). */
UserPrefActiveThemeType resolve_active_theme_type();

/** The #bTheme referenced by whichever of #UserDef::active_theme_dark/light is currently active. */
const bTheme &active_theme_get();

/**
 * Find the #bTheme in #UserDef::themes backed by `filepath` (creating it if none exists yet),
 * and point #UserDef::active_theme_dark or #UserDef::active_theme_light (per `slot`) at it.
 *
 * A newly created theme (or one reset via `force_reset`) has its values reset to the
 * compiled-in baseline, ready for the caller to overlay the preset's own values on top
 * (loading the XML itself is currently only implemented in Python, see `_rna_xml.py`). A
 * theme that already exists and isn't force-reset is left untouched, e.g. keeping any live
 * edits made while it was previously active.
 */
bTheme &ensure_theme(StringRefNull filepath,
                      UserPrefActiveThemeType slot,
                      bool force_reset = false);

}  // namespace blender::bke
