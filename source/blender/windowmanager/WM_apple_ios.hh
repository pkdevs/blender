/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <functional>

struct GHOST_IOSDialog;

namespace blender {
struct bContext;
struct wmWindow;
struct ReportList;
struct wmOperatorType;

/* Engine-thread API. UIKit exchanges only owned values and request identifiers. */
bool WM_apple_ios_pencil_hover_active();
bool WM_apple_ios_dialog_begin(bContext *C,
                               const GHOST_IOSDialog &dialog,
                               std::function<void(bContext *, int, uint64_t)> complete,
                               std::function<void(bContext *)> cancel = {},
                               const char *key = "");
void WM_apple_ios_dialog_flush(bContext *C);
bool WM_apple_ios_dialog_active(const wmWindow *win);
void WM_apple_ios_reports(wmWindow *win, ReportList *reports, int minimum_level);
/**
 * Register the bundled "Starter" asset library, or refresh its path, since the app's install
 * location changes with every install. Call after loading the preferences.
 */
void WM_apple_ios_starter_library_ensure();
/** Keep auto-save on: iOS ends apps without warning. Call after loading the preferences. */
void WM_apple_ios_autosave_ensure();
/** Write the auto-save now, in any mode, when the app goes to the background. */
void WM_apple_ios_autosave_now(bContext *C);
/**
 * Once per launch, continue the last session: work left unsaved when Blender was closed, or the
 * last document. A document opened with the app takes precedence.
 */
void WM_apple_ios_session_restore(bContext *C);
/** Switch between the native iPad interface and Blender's own, as on a desktop computer. */
void WM_OT_apple_ios_interface_toggle(wmOperatorType *ot);

}  // namespace blender
