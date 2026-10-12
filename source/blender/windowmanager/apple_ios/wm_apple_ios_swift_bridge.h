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
/** One of Blender's icons by key (such as `tool.object.builtin.move`), or the symbol. */
UIImage *_Nullable wm_apple_ios_icon(NSString *_Nonnull key, NSString *_Nullable symbol);
/** A brush's preview, round, at a diameter in points. */
UIImage *_Nullable wm_apple_ios_brush_image(NSString *_Nonnull preview, CGFloat diameter);
void WM_apple_ios_ui_tool_set_by_id(const char *_Nonnull tool_id);
void WM_apple_ios_ui_brush_activate(const char *_Nonnull brush_id);
char *_Nullable WM_apple_ios_ui_panels_take_json(void);
char *_Nullable WM_apple_ios_ui_timeline_take_json(void);
char *_Nullable WM_apple_ios_ui_assets_take_json(void);
/** How much of the window's height, from the bottom, the asset pane and timeline cover. */
void WM_apple_ios_ui_bottom_tray_set(float fraction);
UIDocument *_Nullable wm_apple_ios_open_document(void);
/** Choose a template and create a new document, presented from `presenter`. */
void wm_apple_ios_new_document(UIViewController *_Nonnull presenter);
void wm_apple_ios_save_template(NSString *_Nonnull title,
                                NSDictionary *_Nonnull presentation,
                                void (^_Nonnull completion)(NSString *_Nullable error));
void wm_apple_ios_window_set_finger_tool(void *_Nullable window, bool use_tool);
void wm_apple_ios_window_set_pencil_menu(void *_Nullable window,
                                         UIView<GHOSTIOSPencilMenu> *_Nullable menu);
/** Files dropped on the viewport, at `point` in its points. */
void wm_apple_ios_window_drop_files(void *_Nullable window,
                                    NSArray<NSString *> *_Nonnull paths,
                                    CGPoint point);
/** Blender's context menu in the Pencil's radial menu, for the window's viewport. */
NSObject<GHOSTIOSPencilMenuDelegate> *_Nonnull wm_apple_ios_pencil_menu_source(
    void *_Nullable window, UIView *_Nonnull viewport);
void wm_apple_ios_pencil_menu_source_detach(NSObject *_Nonnull source);
/** Shader compilation in Blender's current burst of work, and how long Blender has been held up.
 */
bool wm_apple_ios_shader_compilation(int *_Nonnull finished,
                                     int *_Nonnull total,
                                     double *_Nonnull busy_seconds);
/** Blender's menus in the iPadOS menu bar. */
void wm_apple_ios_build_menu_bar(id<UIMenuBuilder> _Nonnull builder, NSArray *_Nonnull items);
uint64_t wm_apple_ios_menu_bar_request(void);
NSArray *_Nullable wm_apple_ios_menu_result(uint64_t request);
/** Ask for one of Blender's menus by its JSON route, or run its item when `execute`; the answer
 * is taken with #WM_apple_ios_ui_menu_take_result. */
uint64_t WM_apple_ios_ui_menu_request(const char *_Nonnull route, bool execute);
char *_Nullable WM_apple_ios_ui_menu_take_result(uint64_t identifier);
/** The document window's controller is closing. */
void wm_apple_ios_document_controller_closed(id _Nonnull controller);

#ifdef __cplusplus
}
#endif
