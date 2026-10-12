/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Private to the native iPhone, iPad and Apple Vision Pro interface: Blender's commands and the
 * helpers its files share, implemented in `wm_apple_ios_interface.mm`.
 */

#pragma once

#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include "BLI_utildefines.h"

#include "GHOST_IOSNativeInterface.hh"
#include "GHOST_IOSRuntime.hh"

#import "wm_apple_ios_blend_document.hh"
#import "wm_apple_ios_file_drop.hh"
#import "wm_apple_ios_icons.hh"
#import "wm_apple_ios_launcher.hh"

extern "C" void WM_apple_ios_ui_command_enqueue(int command);
extern "C" void WM_apple_ios_ui_operator_enqueue(const char *op_name);
extern "C" void WM_apple_ios_ui_operator_enum_enqueue(const char *op_name,
                                                      const char *prop_name,
                                                      const char *prop_value);
extern "C" uint64_t WM_apple_ios_ui_menu_request(const char *route, bool execute);
extern "C" uint64_t WM_apple_ios_ui_menu_at_position_request(const char *route,
                                                             bool execute,
                                                             int x,
                                                             int y);
extern "C" char *WM_apple_ios_ui_menu_take_result(uint64_t identifier);
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value);
extern "C" void WM_apple_ios_ui_tool_set_by_id(const char *tool_id);
extern "C" void WM_apple_ios_ui_brush_activate(const char *brush_id);
extern "C" void WM_apple_ios_ui_view_roll_enqueue(float angle);
extern "C" void WM_apple_ios_ui_navigation_hover_set(bool active);
extern "C" void WM_apple_ios_ui_pencil_hover_set(bool active, int x, int y);
extern "C" char *WM_apple_ios_ui_toolbar_copy_json();
extern "C" char *WM_apple_ios_ui_toolbar_take_json();
extern "C" char *WM_apple_ios_ui_panels_take_json();
extern "C" char *WM_apple_ios_ui_timeline_take_json();
extern "C" void WM_apple_ios_ui_bottom_tray_set(float fraction);
extern "C" char *WM_apple_ios_ui_assets_take_json();
extern "C" void WM_apple_ios_ui_toolbar_free_json(char *json);
enum WMIOSUICommand {
  WM_IOS_UI_COMMAND_SET_MODE_OBJECT = 0,
  WM_IOS_UI_COMMAND_SET_MODE_EDIT = 1,
  WM_IOS_UI_COMMAND_SET_MODE_SCULPT = 2,
  WM_IOS_UI_COMMAND_SET_MODE_VERTEX_PAINT = 3,
  WM_IOS_UI_COMMAND_SET_MODE_WEIGHT_PAINT = 4,
  WM_IOS_UI_COMMAND_SET_MODE_TEXTURE_PAINT = 5,
  WM_IOS_UI_COMMAND_SELECT_TOOL_SELECT_BOX = 6,
  WM_IOS_UI_COMMAND_SELECT_TOOL_CURSOR = 7,
  WM_IOS_UI_COMMAND_SELECT_TOOL_MOVE = 8,
  WM_IOS_UI_COMMAND_SELECT_TOOL_ROTATE = 9,
  WM_IOS_UI_COMMAND_SELECT_TOOL_SCALE = 10,
  WM_IOS_UI_COMMAND_SELECT_TOOL_TRANSFORM = 11,
  WM_IOS_UI_COMMAND_SELECT_TOOL_ANNOTATE = 12,
  WM_IOS_UI_COMMAND_SELECT_TOOL_MEASURE = 13,
  WM_IOS_UI_COMMAND_FRAME_SELECTED = 14,
  WM_IOS_UI_COMMAND_RESET_VIEW = 15,
  WM_IOS_UI_COMMAND_ORBIT_LEFT = 16,
  WM_IOS_UI_COMMAND_ORBIT_RIGHT = 17,
  WM_IOS_UI_COMMAND_ORBIT_UP = 18,
  WM_IOS_UI_COMMAND_ORBIT_DOWN = 19,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_WIREFRAME = 20,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_SOLID = 21,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_MATERIAL = 22,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_RENDERED = 23,
  WM_IOS_UI_COMMAND_VIEW_ALL = 24,
  WM_IOS_UI_COMMAND_VIEW_LOCAL = 25,
  WM_IOS_UI_COMMAND_VIEW_CAMERA = 26,
  WM_IOS_UI_COMMAND_VIEW_PERSP_ORTHO = 27,
  WM_IOS_UI_COMMAND_VIEW_FRONT = 28,
  WM_IOS_UI_COMMAND_VIEW_BACK = 29,
  WM_IOS_UI_COMMAND_VIEW_RIGHT = 30,
  WM_IOS_UI_COMMAND_VIEW_LEFT = 31,
  WM_IOS_UI_COMMAND_VIEW_TOP = 32,
  WM_IOS_UI_COMMAND_VIEW_BOTTOM = 33,
};

/* Helpers shared by the interface's files. */
UIImage *blenderIOSIcon(NSString *key, NSString *systemImage);
UIImage *blenderIOSBrushImage(NSString *previewPath, CGFloat diameter);
UIImage *blenderIOSCommandIcon(NSInteger command, NSString *systemImage);
UIImage *blenderIOSActionIcon(NSString *opName, NSString *propertyName, NSString *propertyValue);
Class blenderIOSSwiftClass(NSString *name);
NSString *blenderIOSMenuRoute(NSDictionary *route);
UIImage *blenderIOSMenuIcon(NSDictionary *item);
NSArray *blenderIOSMenuResult(uint64_t identifier);
UIMenu *blenderIOSNativeSubmenu(NSDictionary *item, NSString *identifier);
NSArray<UIMenuElement *> *blenderIOSNativeMenuElements(NSArray *items);

/** The SwiftUI document window, `WMIOSSwiftDocumentController` in `DocumentWindow.swift`. */
@protocol WMIOSDocumentController <GHOST_IOSWindowController>
- (instancetype)initWithMetalView:(MTKView *)metalView window:(void *)window;
- (void)updateDocumentControls;
@end
