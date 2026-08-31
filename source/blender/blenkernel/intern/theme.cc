/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_theme.hh"

#include "BLI_listbase.hh"
#include "BLI_string.hh"
#include "BLI_utildefines.hh"

#include "BLO_userdef_default.h"

#include "MEM_guardedalloc.h"

namespace blender::bke::theme {

UserPrefThemeType resolve_active_type()
{
  switch (U.active_theme_type) {
    case UserPrefThemeChoice::Dark:
    case UserPrefThemeChoice::Light:
      return UserPrefThemeType(U.active_theme_type);
    case UserPrefThemeChoice::System:
      return U.runtime.system_theme_type;
  }
  return UserPrefThemeType::Dark;
}

const bTheme &active_get()
{
  if (resolve_active_type() == UserPrefThemeType::Light && U.active_theme_light) {
    return *U.active_theme_light;
  }
  /* No light theme has been set up yet (or dark is active): fall back to dark. */
  return *U.active_theme_dark;
}

void reset_theme(bTheme &theme)
{
  /* Reset to the compiled-in baseline, ready for a caller to overlay a preset's own values
   * on top (loading the XML itself is only implemented in Python, see `_rna_xml.py`).
   * Copying from after `filepath` leaves `name`/`filepath` untouched; `active_theme_area`
   * is the last member, so it still needs preserving separately. */
  const int active_theme_area = theme.active_theme_area;
  MEMCPY_STRUCT_AFTER(&theme, &U_theme_default, filepath);
  theme.active_theme_area = active_theme_area;
}

bTheme &ensure_theme(StringRefNull filepath, UserPrefThemeType slot)
{
  bTheme *btheme = static_cast<bTheme *>(
      BLI_findstring(&U.themes, filepath.c_str(), offsetof(bTheme, filepath)));
  if (!btheme) {
    btheme = MEM_new<bTheme>(__func__);
    BLI_addtail(&U.themes, btheme);
    STRNCPY(btheme->filepath, filepath.c_str());
    /* A theme that already exists is otherwise left untouched, so switching which preset is
     * active doesn't discard live edits made to a theme still in #UserDef::themes. */
    reset_theme(*btheme);
  }

  bTheme *&slot_ptr = (slot == UserPrefThemeType::Dark) ? U.active_theme_dark :
                                                          U.active_theme_light;
  slot_ptr = btheme;
  return *btheme;
}

}  // namespace blender::bke::theme
