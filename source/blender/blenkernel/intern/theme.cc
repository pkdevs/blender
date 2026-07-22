/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_theme.hh"

namespace blender::bke {

const bTheme &active_theme_get()
{
  switch (U.active_theme_type) {
    case UserPrefActiveThemeType::Dark:
      return *U.active_theme_dark;
    case UserPrefActiveThemeType::Light:
      return *U.active_theme_light;
    case UserPrefActiveThemeType::System:
      /* TODO */
      return false ? *U.active_theme_light : *U.active_theme_dark;
  }
  return *U.active_theme_dark;
}

/** Reload a theme to match its source file. */
void reset_theme(bTheme &theme)
{
  // TODO
}

/** Save a theme's current state to its source file. */
void save_theme(bTheme &theme)
{
  // TODO
}

}  // namespace blender::bke
