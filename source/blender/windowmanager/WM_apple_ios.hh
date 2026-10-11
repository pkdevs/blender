/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct wmIOSDialog;
class GHOST_WindowIOS;

namespace blender {
struct bContext;
struct wmWindow;
struct ReportList;
struct wmOperatorType;

/* Engine-thread API. UIKit exchanges only owned values and request identifiers. */
/**
 * Register the native interface: with GHOST, for the app's windows and its loop, and with the
 * window manager, in place of parts of Blender's interface, see #wmNativeInterface. Called before
 * the app starts, as the launcher shows at once.
 */
void WM_apple_ios_native_interface_register();

/* The native interface's views, as GHOST asks for them, see #GHOST_IOSNativeInterface.
 * `UIViewController *` and `MTKView *` are passed as `void *`. */

/**
 * The document launcher, for the app's first window. It shows at once on launch, styled with
 * Blender's brand, logo and a large Suzanne to flick around, while Blender starts behind it. It
 * owns the open document; Blender's window shows only while one is open.
 */
void *WM_apple_ios_launcher_controller();
void *WM_apple_ios_window_controller(void *metal_view, GHOST_WindowIOS *window, void **r_root);
void WM_apple_ios_document_window_changed();
/** Follow Blender's unsaved changes, which make the document save itself. */
void WM_apple_ios_document_set_modified(bool modified);
/** Open the document at an `NSURL *` the system handed to the app. */
void WM_apple_ios_open_url(void *url);

bool WM_apple_ios_pencil_hover_active();
/** Something in Blender changed (notifiers were sent): describe it to the native interface. */
void WM_apple_ios_ui_tag_changed();
bool WM_apple_ios_dialog_begin(bContext *C,
                               const wmIOSDialog &dialog,
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
std::string WM_apple_ios_document_template_write(bContext *C,
                                                 const char *filepath,
                                                 const char *preview,
                                                 std::string &attribution);
/** Switch between the native iPad interface and Blender's own, as on a desktop computer. */
void WM_OT_apple_ios_interface_toggle(wmOperatorType *ot);

}  // namespace blender
