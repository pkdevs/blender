/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_theme.hh"

#include "BLI_listbase.hh"
#include "BLI_string_utf8.hh"
#include "BLI_utildefines.hh"

#include "BLO_userdef_default.h"

#include "MEM_guardedalloc.h"

namespace blender::bke {

UserPrefActiveThemeType resolve_active_theme_type()
{
  switch (U.active_theme_type) {
    case UserPrefActiveThemeType::Dark:
    case UserPrefActiveThemeType::Light:
      return U.active_theme_type;
    case UserPrefActiveThemeType::System:
      /* TODO: Query the operating system for its light/dark preference. */
      return UserPrefActiveThemeType::Dark;
  }
  return UserPrefActiveThemeType::Dark;
}

const bTheme &active_theme_get()
{
  if (resolve_active_theme_type() == UserPrefActiveThemeType::Light && U.active_theme_light) {
    return *U.active_theme_light;
  }
  /* No light theme has been set up yet (or dark is active): fall back to dark. */
  return *U.active_theme_dark;
}

bTheme &ensure_theme(StringRefNull filepath, UserPrefActiveThemeType slot, bool force_reset)
{
  BLI_assert(slot == UserPrefActiveThemeType::Dark || slot == UserPrefActiveThemeType::Light);

  bTheme *btheme = static_cast<bTheme *>(
      BLI_findstring(&U.themes, filepath.c_str(), offsetof(bTheme, filepath)));
  const bool is_new = !btheme;
  if (is_new) {
    btheme = MEM_new<bTheme>(__func__);
    BLI_addtail(&U.themes, btheme);
  }

  if (is_new || force_reset) {
    /* Reset to the compiled-in baseline before the caller overlays the preset's own values
     * on top (loading the XML itself is only implemented in Python, see `_rna_xml.py`). A
     * theme that's already loaded is otherwise left untouched, so switching which preset is
     * active doesn't discard live edits made to a theme still in #UserDef::themes. */
    const int active_theme_area = btheme->active_theme_area;
    MEMCPY_STRUCT_AFTER(btheme, &U_theme_default, name);
    btheme->active_theme_area = active_theme_area;
    STRNCPY_UTF8(btheme->filepath, filepath.c_str());
  }

  bTheme *&slot_ptr = (slot == UserPrefActiveThemeType::Dark) ? U.active_theme_dark :
                                                                 U.active_theme_light;
  slot_ptr = btheme;
  return *btheme;
}

}  // namespace blender::bke
