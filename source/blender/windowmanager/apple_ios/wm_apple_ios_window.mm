/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Blender's window: its controller, the SwiftUI document window in `swift/DocumentWindow.swift`,
 * and GHOST's window functions with C names, which Swift can call.
 */

#import "wm_apple_ios_interface.hh"

/** Blender's main window, shown over the launcher while a document is open. */
static UIViewController<WMIOSDocumentController> *document_controller = nil;

namespace blender {

void WM_apple_ios_document_window_changed()
{
  [document_controller updateDocumentControls];
}

void *WM_apple_ios_window_controller(void *metal_view, GHOST_WindowIOS *window, void **r_root)
{
  UIViewController<WMIOSDocumentController> *controller = [[blenderIOSSwiftClass(
      @"WMIOSSwiftDocumentController") alloc] initWithMetalView:(MTKView *)metal_view
                                                         window:window];
  document_controller = controller;
  *r_root = controller;
  return controller;
}

}  // namespace blender

extern "C" void wm_apple_ios_document_controller_closed(id controller)
{
  if (document_controller == controller) {
    document_controller = nil;
  }
}

static GHOST_WindowIOS *ghost_window(void *window)
{
  return static_cast<GHOST_WindowIOS *>(window);
}

extern "C" bool wm_apple_ios_window_presented_frame(void *window)
{
  return window && GHOST_ios_window_presented_frame(ghost_window(window));
}

extern "C" CGPoint wm_apple_ios_window_pixels(void *window, CGPoint point)
{
  return window ? GHOST_ios_window_pixels(ghost_window(window), point) : point;
}

extern "C" void wm_apple_ios_window_send_key(void *window, const char *key)
{
  if (!window || !key) {
    return;
  }
  const GHOST_TKey ghost_key = STREQ(key, "X") ? GHOST_kKeyX :
                               STREQ(key, "Y") ? GHOST_kKeyY :
                               STREQ(key, "Z") ? GHOST_kKeyZ :
                                                 GHOST_kKeyUnknown;
  if (ghost_key != GHOST_kKeyUnknown) {
    GHOST_ios_window_send_key(ghost_window(window), ghost_key);
  }
}

extern "C" bool wm_apple_ios_documents_available()
{
  return wm_apple_ios_launcher_can_show_documents();
}

extern "C" void wm_apple_ios_show_documents(id sender)
{
  wm_apple_ios_launcher_show_documents(sender);
}

extern "C" UIImage *wm_apple_ios_action_icon(NSString *op_name,
                                             NSString *property,
                                             NSString *value)
{
  return blenderIOSActionIcon(op_name, property, value);
}

extern "C" UIMenu *wm_apple_ios_context_menu()
{
  /* The same live context menu as Blender's and the Pencil's radial menu, resolved when it opens
   * so UIKit never waits for a busy engine. */
  return blenderIOSNativeSubmenu(
      @{
        @"title" : @"Actions",
        @"route" : @{@"root" : @"context", @"path" : @[]},
      },
      nil);
}

extern "C" void wm_apple_ios_window_navigation_tap(void *window, UITapGestureRecognizer *tap)
{
  if (window) {
    [GHOST_ios_window_native(ghost_window(window)) handleNavigationTap:tap];
  }
}

extern "C" void wm_apple_ios_window_cancel_pencil_stroke(void *window)
{
  if (window) {
    [GHOST_ios_window_native(ghost_window(window)) cancelPencilStroke];
  }
}

extern "C" UIImage *wm_apple_ios_command_icon(int command, NSString *symbol)
{
  return blenderIOSCommandIcon(command, symbol);
}

extern "C" UIImage *wm_apple_ios_icon(NSString *key, NSString *symbol)
{
  return blenderIOSIcon(key, symbol);
}

extern "C" UIImage *wm_apple_ios_brush_image(NSString *preview, CGFloat diameter)
{
  return blenderIOSBrushImage(preview, diameter);
}

extern "C" UIMenu *wm_apple_ios_property_menu(NSDictionary *property)
{
  /* A property's menu, such as a modifier's or a material slot's: Blender's own, resolved when it
   * opens. */
  NSMutableDictionary *route = [NSMutableDictionary dictionaryWithDictionary:@{
    @"root" : property[@"menu"] ?: @"",
    @"path" : @[]
  }];
  if (property[@"tab"]) {
    route[@"propertiesTab"] = property[@"tab"];
  }
  return blenderIOSNativeSubmenu(@{
    @"title" : property[@"label"] ?: @"",
    @"icon" : property[@"icon"] ?: @"",
    @"route" : route
  },
                                 property[@"menu"]);
}

extern "C" UIMenu *wm_apple_ios_blender_menu(NSDictionary *item, NSString *identifier)
{
  return blenderIOSNativeSubmenu(item, identifier);
}

extern "C" UIDocument *wm_apple_ios_open_document()
{
  return wm_apple_ios_launcher_document();
}

extern "C" void wm_apple_ios_new_document(UIViewController *presenter)
{
  wm_apple_ios_launcher_new_document(presenter);
}

extern "C" void wm_apple_ios_save_template(NSString *title,
                                           NSDictionary *presentation,
                                           void (^completion)(NSString *error))
{
  wm_apple_ios_template_save(title, presentation, completion);
}

extern "C" void wm_apple_ios_window_set_finger_tool(void *window, bool use_tool)
{
  if (window) {
    [GHOST_ios_window_native(ghost_window(window)) setFingerToolEnabled:use_tool];
  }
}

extern "C" void wm_apple_ios_window_set_pencil_menu(void *window, UIView<GHOSTIOSPencilMenu> *menu)
{
  if (window) {
    [GHOST_ios_window_native(ghost_window(window)) setPencilContextMenu:menu];
  }
}

extern "C" void wm_apple_ios_window_drop_files(void *window,
                                               NSArray<NSString *> *paths,
                                               CGPoint point)
{
  if (window) {
    GHOST_ios_window_drop_files(ghost_window(window), paths, point);
  }
}

/* -------------------------------------------------------------------- */
/** \name Blender's Menus in UIKit
 * \{ */

/** Blender's live context menu in the Pencil's radial menu, at the point it opens. */
@interface WMIOSPencilMenuSource : NSObject <GHOSTIOSPencilMenuDelegate>
@property(nonatomic) void *window;
@property(nonatomic, assign) UIView *viewport;
@end

@implementation WMIOSPencilMenuSource

- (NSArray<NSDictionary *> *)radialItems:(NSArray *)sources
{
  NSMutableArray *items = [NSMutableArray array];
  for (NSDictionary *source in sources) {
    if (![source isKindOfClass:NSDictionary.class]) {
      continue;
    }
    if ([source[@"heading"] isKindOfClass:NSString.class]) {
      /* The menu's own name, shown in the radial menu's center. */
      [items addObject:@{@"heading" : source[@"heading"]}];
      continue;
    }
    NSString *op = source[@"operator"];
    NSString *title = source[@"title"];
    if (![title isKindOfClass:NSString.class] || !title.length ||
        ![source[@"route"] isKindOfClass:NSDictionary.class] ||
        (![source[@"action"] boolValue] && ![source[@"submenu"] boolValue]))
    {
      continue;
    }
    /* Keep source order, full titles, and distinct property-bearing routes, even when several
     * entries invoke the same operator (e.g. Set Origin or Delete). */
    NSMutableDictionary *item = [[source mutableCopy] autorelease];
    item[@"destructive"] = @([op isEqual:@"OBJECT_OT_delete"] || [op isEqual:@"MESH_OT_delete"]);
    UIImage *image = blenderIOSMenuIcon(source);
    if (!image && [source[@"submenu"] boolValue]) {
      image = [UIImage systemImageNamed:@"chevron.right"];
    }
    if (image) {
      item[@"image"] = image;
    }
    [items addObject:item];
  }
  return items;
}

- (CGPoint)pixels:(CGPoint)point from:(UIView *)menu
{
  return wm_apple_ios_window_pixels(_window, [menu convertPoint:point toView:_viewport]);
}

- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
    requestItemsForRoute:(NSDictionary *)route
                 atPoint:(CGPoint)point
              completion:(void (^)(NSArray<NSDictionary *> *))completion
{
  if (!_window) {
    completion(@[]);
    return;
  }
  const CGPoint pixels = [self pixels:point from:menu];
  const uint64_t request = WM_apple_ios_ui_menu_at_position_request(
      blenderIOSMenuRoute(route).UTF8String, false, pixels.x, pixels.y);
  const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 60.0;
  NSTimer *poll = [NSTimer timerWithTimeInterval:0.025
                                         repeats:YES
                                           block:^(NSTimer *timer) {
                                             NSArray *result = blenderIOSMenuResult(request);
                                             if (result || CFAbsoluteTimeGetCurrent() >= deadline)
                                             {
                                               [timer invalidate];
                                               completion([self radialItems:result]);
                                             }
                                           }];
  [NSRunLoop.mainRunLoop addTimer:poll forMode:NSRunLoopCommonModes];
}

- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
     didChooseItem:(NSDictionary *)item
           atPoint:(CGPoint)point
{
  if (!_window || [item[@"disabled"] boolValue] ||
      ![item[@"route"] isKindOfClass:NSDictionary.class])
  {
    return;
  }
  const CGPoint pixels = [self pixels:point from:menu];
  WM_apple_ios_ui_menu_at_position_request(
      blenderIOSMenuRoute(item[@"route"]).UTF8String, true, pixels.x, pixels.y);
}

@end

extern "C" NSObject<GHOSTIOSPencilMenuDelegate> *wm_apple_ios_pencil_menu_source(void *window,
                                                                                 UIView *viewport)
{
  WMIOSPencilMenuSource *source = [[[WMIOSPencilMenuSource alloc] init] autorelease];
  source.window = window;
  source.viewport = viewport;
  return source;
}

extern "C" void wm_apple_ios_pencil_menu_source_detach(NSObject *source)
{
  ((WMIOSPencilMenuSource *)source).window = nullptr;
}

/** Blender's menus in the iPadOS menu bar, from the menu bar projection `items`. */
extern "C" void wm_apple_ios_build_menu_bar(id<UIMenuBuilder> builder, NSArray *items)
{
  if (builder.system != UIMenuSystem.mainSystem || !items.count) {
    return;
  }
  NSDictionary *systemIdentifiers = @{
    @"TOPBAR_MT_blender" : UIMenuApplication,
    @"TOPBAR_MT_file" : UIMenuFile,
    @"TOPBAR_MT_edit" : UIMenuEdit,
    @"TOPBAR_MT_window" : UIMenuWindow,
    @"TOPBAR_MT_help" : UIMenuHelp,
    @"VIEW3D_MT_view" : UIMenuView,
  };
  NSMutableArray *commands = [NSMutableArray array];
  for (NSDictionary *item in items) {
    if ([item[@"separator"] boolValue] || ![item[@"title"] length]) {
      continue;
    }
    if (![item[@"submenu"] boolValue]) {
      [commands addObject:item];
      continue;
    }
    NSString *menuName = item[@"menu"];
    NSString *systemIdentifier = systemIdentifiers[menuName];
    NSString *identifier = systemIdentifier ?:
                                              [@"org.blender.menu."
                                   stringByAppendingString:menuName ?: item[@"title"]];
    UIMenu *menu = blenderIOSNativeSubmenu(item, identifier);
    if (systemIdentifier && [builder menuForIdentifier:systemIdentifier]) {
      if ([systemIdentifier isEqualToString:UIMenuApplication] ||
          [systemIdentifier isEqualToString:UIMenuWindow])
      {
        /* Keep iPadOS window-management and application commands available. */
        UIMenu *group = [UIMenu menuWithTitle:@""
                                        image:nil
                                   identifier:nil
                                      options:UIMenuOptionsDisplayInline
                                     children:menu.children];
        [builder insertChildMenu:group atEndOfMenuForIdentifier:systemIdentifier];
      }
      else {
        [builder replaceMenuForIdentifier:systemIdentifier withMenu:menu];
      }
    }
    else if ([builder menuForIdentifier:UIMenuWindow]) {
      [builder insertSiblingMenu:menu beforeMenuForIdentifier:UIMenuWindow];
    }
    else {
      [builder insertChildMenu:menu atEndOfMenuForIdentifier:UIMenuRoot];
    }
  }
  if (commands.count) {
    [builder insertChildMenu:[UIMenu menuWithTitle:@"Commands"
                                          children:blenderIOSNativeMenuElements(commands)]
        atEndOfMenuForIdentifier:UIMenuRoot];
  }
}

/** Ask Blender for its menu bar's menus; the answer comes later, see the next function. */
extern "C" uint64_t wm_apple_ios_menu_bar_request()
{
  return WM_apple_ios_ui_menu_request("{\"root\":\"menubar\",\"path\":[]}", false);
}

extern "C" NSArray *wm_apple_ios_menu_result(uint64_t request)
{
  return blenderIOSMenuResult(request);
}

/** \} */

extern "C" bool wm_apple_ios_shader_compilation(int *finished, int *total, double *busy_seconds)
{
  *busy_seconds = GHOST_ios_engine_busy_seconds();
  return GHOST_ios_shader_compilation(finished, total);
}
