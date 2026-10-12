/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Blender's side of the Swift bridge, `wm_apple_ios_swift_bridge.h`, for the panels harness: the
 * document window's sources are built whole, so the functions they call answer here with nothing,
 * as Blender would without a document. Those the panels use are answered in `Harness.swift`.
 * Test-only: never part of the application.
 */

#import "wm_apple_ios_swift_bridge.h"

char *WM_apple_ios_ui_assets_take_json(void)
{
  return NULL;
}
char *WM_apple_ios_ui_panels_take_json(void)
{
  return NULL;
}
char *WM_apple_ios_ui_timeline_take_json(void)
{
  return NULL;
}
char *WM_apple_ios_ui_toolbar_copy_json(void)
{
  return NULL;
}
char *WM_apple_ios_ui_toolbar_take_json(void)
{
  return NULL;
}
void WM_apple_ios_ui_toolbar_free_json(char *json)
{
  free(json);
}
void WM_apple_ios_ui_bottom_tray_set(float fraction) {}
void WM_apple_ios_ui_brush_activate(const char *brush_id) {}
void WM_apple_ios_ui_tool_set_by_id(const char *tool_id) {}
void WM_apple_ios_ui_operator_enum_enqueue(const char *op_name,
                                           const char *prop_name,
                                           const char *prop_value)
{
}
uint64_t WM_apple_ios_ui_menu_request(const char *route, bool execute)
{
  return 0;
}
char *WM_apple_ios_ui_menu_take_result(uint64_t identifier)
{
  return strdup("[]");
}

UIImage *wm_apple_ios_icon(NSString *key, NSString *symbol)
{
  return symbol ? [UIImage systemImageNamed:symbol] : nil;
}
UIImage *wm_apple_ios_action_icon(NSString *op_name, NSString *property, NSString *value)
{
  return nil;
}
UIImage *wm_apple_ios_brush_image(NSString *preview, CGFloat diameter)
{
  return nil;
}
UIImage *wm_apple_ios_command_icon(int command, NSString *symbol)
{
  return [UIImage systemImageNamed:symbol];
}
UIMenu *wm_apple_ios_context_menu(void)
{
  return [UIMenu menuWithChildren:@[]];
}
void wm_apple_ios_build_menu_bar(id<UIMenuBuilder> builder, NSArray *items) {}
uint64_t wm_apple_ios_menu_bar_request(void)
{
  return 0;
}
NSArray *wm_apple_ios_menu_result(uint64_t request)
{
  return @[];
}

bool wm_apple_ios_documents_available(void)
{
  return false;
}
void wm_apple_ios_show_documents(id sender) {}
void wm_apple_ios_document_controller_closed(id controller) {}
UIDocument *wm_apple_ios_open_document(void)
{
  return nil;
}
void wm_apple_ios_new_document(UIViewController *presenter) {}
void wm_apple_ios_save_template(NSString *title,
                                NSDictionary *presentation,
                                void (^completion)(NSString *error))
{
  completion(nil);
}

bool wm_apple_ios_shader_compilation(int *finished, int *total, double *busy_seconds)
{
  *finished = *total = 0;
  *busy_seconds = 0;
  return false;
}

NSObject<GHOSTIOSPencilMenuDelegate> *wm_apple_ios_pencil_menu_source(void *window,
                                                                      UIView *viewport)
{
  return nil;
}
void wm_apple_ios_pencil_menu_source_detach(NSObject *source) {}

bool wm_apple_ios_window_presented_frame(void *window)
{
  return true;
}
CGPoint wm_apple_ios_window_pixels(void *window, CGPoint point)
{
  return point;
}
void wm_apple_ios_window_send_key(void *window, const char *key) {}
void wm_apple_ios_window_navigation_tap(void *window, UITapGestureRecognizer *tap) {}
void wm_apple_ios_window_cancel_pencil_stroke(void *window) {}
void wm_apple_ios_window_set_finger_tool(void *window, bool use_tool) {}
void wm_apple_ios_window_set_pencil_menu(void *window, UIView<GHOSTIOSPencilMenu> *menu) {}
void wm_apple_ios_window_drop_files(void *window, NSArray<NSString *> *paths, CGPoint point) {}
