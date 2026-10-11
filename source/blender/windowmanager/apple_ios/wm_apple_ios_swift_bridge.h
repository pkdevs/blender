/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * What the native interface's Swift code may call outside Swift: the engine inbox that UIKit
 * feeds. These functions only queue work or copy out the latest snapshot; none waits for
 * Blender, see `release/ios/README.md`.
 */

#pragma once

#import <UIKit/UIKit.h>

#import "GHOST_IOSPencilMenu.hh"
#import "GHOST_IOSRuntime.hh"

#ifdef __cplusplus
extern "C" {
#endif

void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value);
void WM_apple_ios_ui_operator_enqueue(const char *op_name);
void WM_apple_ios_ui_command_enqueue(int command);
void WM_apple_ios_ui_asset_drag(int phase, const char *asset_id, int x, int y);
/** The open document's file, or nil. */
NSURL *_Nullable wm_apple_ios_document_url(void);

/** Blender's toolbar snapshot as JSON: the latest one once (null when unchanged), or a copy. */
char *_Nullable WM_apple_ios_ui_toolbar_take_json(void);
char *_Nullable WM_apple_ios_ui_toolbar_copy_json(void);
void WM_apple_ios_ui_toolbar_free_json(char *_Nullable json);

/* Blender's window (`GHOST_WindowIOS`), see `wm_apple_ios_window.mm`. */
bool wm_apple_ios_window_presented_frame(void *_Nullable window);
CGPoint wm_apple_ios_window_pixels(void *_Nullable window, CGPoint point);
void wm_apple_ios_window_send_key(void *_Nullable window, const char *_Nonnull key);
/** Whether a document is open that the launcher can go back from, and going back. */
bool wm_apple_ios_documents_available(void);
void wm_apple_ios_show_documents(id _Nullable sender);
/** A quick action's icon: Blender's for the operator, or a symbol, or none. */
UIImage *_Nullable wm_apple_ios_action_icon(NSString *_Nonnull op_name,
                                            NSString *_Nonnull property,
                                            NSString *_Nonnull value);
/** Blender's context menu for the viewport, resolved when it opens. */
UIMenu *_Nonnull wm_apple_ios_context_menu(void);
void WM_apple_ios_ui_operator_enum_enqueue(const char *_Nonnull op_name,
                                           const char *_Nonnull prop_name,
                                           const char *_Nonnull prop_value);
/** A tap on the navigation gizmo, which picks the axis under it. */
void wm_apple_ios_window_navigation_tap(void *_Nullable window,
                                        UITapGestureRecognizer *_Nonnull tap);
/** End the Pencil's stroke, cancelling its tool, when a control takes the touch. */
void wm_apple_ios_window_cancel_pencil_stroke(void *_Nullable window);
/** A view command's icon: Blender's, or the symbol. */
UIImage *_Nullable wm_apple_ios_command_icon(int command, NSString *_Nonnull symbol);
/** The document window's controller is closing. */
void wm_apple_ios_document_controller_closed(id _Nonnull controller);

#ifdef __cplusplus
}
#endif
