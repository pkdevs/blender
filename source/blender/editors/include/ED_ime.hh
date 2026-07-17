/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Draw the IME composition preview: the pre-edit text at the cursor, with its underlines.
 * Shared by the editors which support IME, #ARegionType::cursor_ime.
 */

#pragma once

#ifdef WITH_INPUT_IME

namespace blender {

struct ARegion;
struct ARegionIMECursor;
struct ScrArea;
struct wmWindow;

namespace ed::ime {

/**
 * Draw the composition preview for a region, when it's composing.
 * The editor keeps drawing its own cursor, this only draws the pre-edit text on the RHS.
 * The box is axis-aligned.
 *
 * \param cursor: As reported by `cursor_ime` on #ARegionIMECursorState::PositionSet.
 */
void region_overlay_draw(wmWindow *win,
                         const ARegion *region,
                         float pixelsize,
                         const ARegionIMECursor &cursor);

}  // namespace ed::ime

}  // namespace blender

#endif /* WITH_INPUT_IME */
