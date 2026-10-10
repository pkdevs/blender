/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct GHOST_IOSDialog;

namespace blender {
struct bContext;
struct wmWindow;
struct ReportList;
struct wmOperatorType;

/* Engine-thread API. UIKit exchanges only owned values and request identifiers. */
bool WM_apple_ios_pencil_hover_active();
/** Something in Blender changed (notifiers were sent): describe it to the native interface. */
void WM_apple_ios_ui_tag_changed();
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
/* Documents. UIKit owns them, and Blender reads and writes their files when asked. */

/** Open the document's file. */
bool WM_apple_ios_document_open(bContext *C, const char *filepath, bool template_document);
/**
 * Write the open file to `filepath`, for iOS to move into place as the saved document. Without
 * `preview` the file stores no preview image, which needs the GPU.
 */
bool WM_apple_ios_document_write(bContext *C, const char *filepath, bool preview);
/** Save a portable template copy and PNG preview without marking the document saved. */
std::string WM_apple_ios_document_template_write(bContext *C, const char *filepath, const char *preview,
                                               std::string &attribution);
/** Switch between the native iPad interface and Blender's own, as on a desktop computer. */
void WM_OT_apple_ios_interface_toggle(wmOperatorType *ot);

}  // namespace blender
