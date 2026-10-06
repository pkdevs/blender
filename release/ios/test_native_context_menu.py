# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise the production menu builder with UIKit on a booted iOS simulator.

Run with DEVELOPER_DIR pointing to Xcode and optionally IOS_TEST_SIMULATOR_UDID.
No Blender simulator build or app installation is required.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class NativeContextMenuTest(unittest.TestCase):
    def test_inline_sections_operator_dispatch_and_shading_state(self):
        if sys.platform != "darwin":
            self.skipTest("Requires Xcode and an iOS simulator")
        sdk_result = subprocess.run(
            ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], capture_output=True, text=True)
        if sdk_result.returncode:
            self.skipTest("iOS Simulator SDK unavailable")
        devices = json.loads(subprocess.check_output(
            ["xcrun", "simctl", "list", "devices", "booted", "--json"], text=True))
        candidates = [device for runtime, entries in devices["devices"].items()
                      if ".iOS-" in runtime for device in entries if device["isAvailable"]]
        if not candidates:
            self.skipTest("No booted iOS simulator")
        device = os.environ.get("IOS_TEST_SIMULATOR_UDID", candidates[0]["udid"])
        root = Path(__file__).resolve().parents[2]
        source = (root / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        # Compile the real method, so changing its menu options or dispatch is
        # covered by the test rather than a duplicate implementation.
        update_start = source.index("- (void)updateActionsButtonMenu\n")
        method = source[update_start:source.index("\n}", update_start) + 2]
        radial_start = source.index("- (NSArray<NSDictionary *> *)radialItemsFromBlenderMenu:")
        radial_method = source[radial_start:source.index("\n}\n", radial_start) + 2]
        for signature in (
                "- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldReceiveTouch:",
                "- (void)handleActionsMenuPress:", "- (UIMenu *)nativeBlenderContextMenu\n"):
            start = source.rindex(signature)
            method += source[start:source.index("\n}", start) + 2]
        enum_start = source.index("enum GHOSTIOSUICommand")
        command_enum = source[enum_start:source.index("};", enum_start) + 2]
        shading_start = source.index("- (UIAction *)viewportShadingActionWithTitle:")
        shading_methods = source[shading_start:source.index("- (UIButton *)viewPieButtonWithTitle:", shading_start)]
        button_start = source.index("- (UIBarButtonItem *)viewportShadingButton\n")
        shading_methods += source[button_start:source.index("- (UIBarButtonItem *)modePickerButton\n", button_start)]
        icon_start = source.index("static UIImage *blenderIOSActionIcon(")
        action_icon = source[icon_start:source.index("\n}", icon_start) + 2]
        action_start = source.index("- (void)updateAxisConstraintButton:")
        action_button = source[action_start:source.index("- (NSString *)actionSignatureForActions:", action_start)]
        helper_start = source.index("static NSString *blenderIOSMenuRoute(")
        menu_helpers = source[helper_start:source.index("@interface GHOST_IOSViewController", helper_start)]
        for signature in ("- (NSString *)actionSignatureForActions:", "- (void)handleActionButton:",
                          "- (UIBarButtonItem *)actionsButton\n"):
            start = source.index(signature)
            action_button += source[start:source.index("\n}", start) + 2]
        harness = r'''
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#define UNUSED_VARS(...) ((void)0)
static NSMutableArray *queued;
static NSMutableDictionary *icons;
static NSMutableArray *iconKeys;
static NSDictionary *mapping;
static BOOL missingAssets;
static NSInteger queuedCommand;
static NSMutableArray *settings;
static NSMutableArray *menuRequests;
static NSString *menuReply;
static void (^menuProvider)(void (^)(NSArray<UIMenuElement *> *));
@interface UIDeferredMenuElement (MenuTest)
+ (instancetype)testElementWithProvider:(void (^)(void (^)(NSArray<UIMenuElement *> *)))provider;
@end
@implementation UIDeferredMenuElement (MenuTest)
+ (instancetype)testElementWithProvider:(void (^)(void (^)(NSArray<UIMenuElement *> *)))provider {
  menuProvider = [[provider copy] autorelease];
  return [self testElementWithProvider:provider];
}
@end
static uint64_t WM_apple_ios_ui_menu_request(const char *route, bool execute) {
  [menuRequests addObject:@{@"route": [NSString stringWithUTF8String:route], @"execute": @(execute)}];
  return menuRequests.count;
}
static char *WM_apple_ios_ui_menu_take_result(uint64_t) {
  if (!menuReply) return nullptr;
  char *result = strdup(menuReply.UTF8String);
  menuReply = nil;
  return result;
}
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value) {
  [settings addObject:@[[NSString stringWithUTF8String:setting], [NSString stringWithUTF8String:value]]];
}
extern "C" void WM_apple_ios_ui_operator_enum_enqueue(const char *, const char *, const char *) {}
static void checkMenu(UIMenu *actual, UIMenu *expected) {
  NSCAssert([actual.title isEqual:expected.title] && actual.children.count == expected.children.count,
            @"Overflow menu differs");
  for (NSUInteger i = 0; i < expected.children.count; i++) {
    UIMenuElement *a = actual.children[i], *b = expected.children[i];
    NSCAssert([a.title isEqual:b.title], @"Overflow action title differs");
    if ([b isKindOfClass:UIMenu.class]) {
      checkMenu((UIMenu *)a, (UIMenu *)b);
    } else if ([b isKindOfClass:UIAction.class]) {
      NSCAssert(((UIAction *)a).state == ((UIAction *)b).state, @"Overflow checkmark differs");
    } else {
      NSCAssert([a isKindOfClass:UIDeferredMenuElement.class], @"Overflow lost lazy submenu");
    }
  }
}
static UIImage *blenderIOSIcon(NSString *key, NSString *fallback)
{
  [iconKeys addObject:key];
  if (missingAssets || !mapping[key]) {
    return fallback ? [UIImage systemImageNamed:fallback] : nil;
  }
  if (!icons[key]) {
    icons[key] = [UIImage systemImageNamed:fallback ?: @"cube"];
  }
  return icons[key];
}
static UIImage *blenderIOSCommandIcon(NSInteger command, NSString *fallback)
{
  return blenderIOSIcon([NSString stringWithFormat:@"command.%ld", (long)command], fallback);
}
extern "C" void WM_apple_ios_ui_command_enqueue(int command) { queuedCommand = command; }
extern "C" void WM_apple_ios_ui_operator_enqueue(const char *name)
{
  [queued addObject:[NSString stringWithUTF8String:name]];
}
''' + command_enum + "\n" + action_icon + "\n" + menu_helpers + r'''
@interface MenuCountingButton : UIBarButtonItem
@property(nonatomic) NSUInteger menuAssignments;
@property(nonatomic) NSUInteger enabledAssignments;
@end
@implementation MenuCountingButton
- (void)setMenu:(UIMenu *)menu {
  self.menuAssignments++;
  [super setMenu:menu];
}
- (void)setEnabled:(BOOL)enabled {
  self.enabledAssignments++;
  [super setEnabled:enabled];
}
@end
@interface MenuTestTouch : NSObject
@property(nonatomic, retain) UIView *view;
@property(nonatomic) UITouchType type;
@end
@implementation MenuTestTouch
@end
@interface MenuTestPencilMenu : NSObject
@property(nonatomic) BOOL active;
@end
@implementation MenuTestPencilMenu
@end
@interface MenuTestPress : UILongPressGestureRecognizer
@property(nonatomic) UIGestureRecognizerState testState;
@end
@implementation MenuTestPress
- (UIGestureRecognizerState)state { return self.testState; }
- (CGPoint)locationInView:(UIView *)view { return CGPointMake(120, 240); }
@end
@interface MenuTestInteraction : NSObject
@property(nonatomic) NSUInteger presentations;
@end
@implementation MenuTestInteraction
- (void)presentEditMenuWithConfiguration:(UIEditMenuConfiguration *)configuration {
  self.presentations++;
}
@end
@interface MenuTest : NSObject {
  UIView *_view;
  MenuTestPencilMenu *_pencil_menu;
  MenuTestInteraction *_actions_edit_menu_interaction;
  UIBarButtonItem *_viewport_shading_button;
  UIBarButtonItem *_undo_button;
  UIBarButtonItem *_actions_button;
  NSArray *_actions_menu_items;
  NSArray *_native_menubar_items;
  NSMutableArray *_action_buttons;
}
@property(nonatomic, retain) NSArray *items;
@property(nonatomic, retain) NSArray *menuRoots;
@property(nonatomic, retain) NSDictionary *model;
@property(nonatomic, retain) UIBarButtonItem *shadingButton;
@property(nonatomic, retain) UIBarButtonItem *undoControl;
@property(nonatomic, retain) UIBarButtonItem *actionsControl;
@property(nonatomic, retain) UIView *viewport;
@property(nonatomic, retain) MenuTestPencilMenu *pencilMenu;
@property(nonatomic, retain) MenuTestInteraction *editInteraction;
@end
@implementation MenuTest
@synthesize shadingButton = _viewport_shading_button;
@synthesize undoControl = _undo_button;
@synthesize actionsControl = _actions_button;
@synthesize viewport = _view;
@synthesize pencilMenu = _pencil_menu;
@synthesize editInteraction = _actions_edit_menu_interaction;
@synthesize menuRoots = _native_menubar_items;
- (instancetype)init {
  self = [super init];
  if (self) _action_buttons = [[NSMutableArray alloc] init];
  return self;
}
- (void)sendBlenderKeyNamed:(NSString *)key { NSCAssert(NO, @"Axis button sent a keyboard event"); }
- (NSDictionary *)toolbarModel { return self.model; }
''' + method + shading_methods + action_button + radial_method + r'''
@end
@interface AxisCountingMenuTest : MenuTest
@property(nonatomic) NSUInteger axisUpdates;
@end
@implementation AxisCountingMenuTest
- (void)updateAxisConstraintButton:(UIButton *)button selected:(BOOL)selected {
  self.axisUpdates++;
  [super updateAxisConstraintButton:button selected:selected];
}
@end
int main()
{
  @autoreleasepool {
    queued = [NSMutableArray array];
    settings = [NSMutableArray array];
    menuRequests = [NSMutableArray array];
    icons = [NSMutableDictionary dictionary];
    iconKeys = [NSMutableArray array];
    NSData *manifest = [NSData dataWithContentsOfFile:@MANIFEST_PATH];
    mapping = [NSJSONSerialization JSONObjectWithData:manifest options:0 error:nil];
    AxisCountingMenuTest *builder = [[[AxisCountingMenuTest alloc] init] autorelease];
    UIBarButtonItem *more = [builder actionsButton];
    NSCAssert([more.image isEqual:[UIImage systemImageNamed:@"ellipsis.circle"]],
              @"More must use the native circled ellipsis, not Blender artwork");
    NSCAssert(more.customView == nil && [more.accessibilityLabel isEqual:@"Blender Menus"],
              @"More lost its native control or accessible label");
    NSDictionary *separator = @{ @"separator": @YES };
    NSArray *expectedTitles = @[@"Move", @"Rotate", @"Duplicate", @"Frame Selected"];
    NSArray *expectedOperators = @[@"TRANSFORM_OT_translate", @"TRANSFORM_OT_rotate",
                                   @"OBJECT_OT_duplicate_move", @"VIEW3D_OT_view_selected"];
    NSMutableArray *expectedRoutes = [NSMutableArray array];
    for (NSString *name in expectedOperators) {
      [expectedRoutes addObject:@{@"root": @"VIEW3D_MT_object_context_menu", @"path": @[name]}];
    }
    builder.items = @[
      separator,
      @{ @"title": @"Move", @"operator": expectedOperators[0], @"route": expectedRoutes[0] },
      @{ @"title": @"Rotate", @"operator": expectedOperators[1], @"route": expectedRoutes[1] },
      separator, separator,
      @"invalid item", @{ @"title": @"", @"operator": @"OBJECT_OT_delete" },
      @{ @"title": @"Invalid", @"operator": @42 },
      @{ @"title": @"Duplicate", @"operator": expectedOperators[2], @"route": expectedRoutes[2] },
      separator,
      @{ @"title": @"Frame Selected", @"operator": expectedOperators[3], @"route": expectedRoutes[3] },
      separator,
    ];
    Method providerMethod = class_getClassMethod(UIDeferredMenuElement.class,
        @selector(elementWithUncachedProvider:));
    Method captureMethod = class_getClassMethod(UIDeferredMenuElement.class,
        @selector(testElementWithProvider:));
    method_exchangeImplementations(providerMethod, captureMethod);
    UIMenu *context = [builder nativeBlenderContextMenu];
    method_exchangeImplementations(providerMethod, captureMethod);
    NSCAssert([context.title isEqual:@"Actions"] && context.children.count == 1 &&
              [context.children.firstObject isKindOfClass:UIDeferredMenuElement.class],
              @"Context menu must resolve Blender definitions asynchronously, not invent fallback commands");
    __block NSArray *resolved = nil;
    menuProvider(^(NSArray<UIMenuElement *> *items) { resolved = [items retain]; });
    NSDictionary *contextRoute = [NSJSONSerialization JSONObjectWithData:
        [menuRequests.lastObject[@"route"] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    NSDictionary *expectedContextRoute = @{@"root": @"context", @"path": @[]};
    NSCAssert([contextRoute isEqual:expectedContextRoute] &&
              ![menuRequests.lastObject[@"execute"] boolValue], @"Wrong Blender context-menu request");
    [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    NSCAssert(resolved == nil, @"Busy engine caused invented fallback commands");
    menuReply = @"[]";
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!resolved && deadline.timeIntervalSinceNow > 0) {
      [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.025]];
    }
    NSCAssert(resolved.count == 1 && [resolved.firstObject isKindOfClass:UIAction.class] &&
              (((UIAction *)resolved.firstObject).attributes & UIMenuElementAttributesDisabled),
              @"Empty Blender menu must show no executable fallback commands");
    [resolved release];
    [menuRequests removeAllObjects];
    UIMenu *menu = [UIMenu menuWithTitle:@"Actions" children:blenderIOSNativeMenuElements(builder.items)];
    NSCAssert(menu.children.count == 3, @"Empty or missing sections");
    NSMutableArray *titles = [NSMutableArray array];
    for (UIMenu *section in menu.children) {
      NSCAssert(section.options & UIMenuOptionsDisplayInline, @"Blank submenu row");
      NSCAssert(section.children.count > 0, @"Empty section");
      for (UIAction *action in section.children) {
        NSCAssert([action isKindOfClass:UIAction.class] && action.title.length > 0,
                  @"Unlabelled or nested action");
        NSCAssert(action.image == nil, @"Iconless Blender entry gained a guessed operator icon");
        [titles addObject:action.title];
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        [button addAction:action forControlEvents:UIControlEventTouchUpInside];
        [button sendActionsForControlEvents:UIControlEventTouchUpInside];
      }
    }
    NSCAssert([titles isEqual:expectedTitles], @"Missing/reordered commands");
    NSCAssert(queued.count == 0 && menuRequests.count == expectedRoutes.count,
              @"Menu bypassed property-bearing Blender routes");
    for (NSUInteger i = 0; i < expectedRoutes.count; i++) {
      NSDictionary *request = menuRequests[i];
      NSDictionary *route = [NSJSONSerialization JSONObjectWithData:
          [request[@"route"] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
      NSCAssert([request[@"execute"] boolValue] && [route isEqual:expectedRoutes[i]],
                @"Action lost its Blender route");
    }
    // The 250 ms toolbar poll must not replace an unchanged, potentially presented menu.
    MenuCountingButton *actions = [[[MenuCountingButton alloc] init] autorelease];
    builder.actionsControl = actions;
    NSMutableArray *roots = [NSMutableArray array];
    for (NSString *title in @[@"File", @"Edit", @"Render", @"View", @"Select", @"Add", @"Object"]) {
      [roots addObject:@{@"title": title, @"submenu": @YES,
          @"route": @{@"root": @"VIEW3D_MT_editor_menus", @"path": @[title]}}];
    }
    builder.menuRoots = roots;
    [builder updateActionsButtonMenu];
    UIMenu *stableMenu = [[actions.menu retain] autorelease];
    UIMenu *rootSection = (UIMenu *)stableMenu.children.firstObject;
    NSCAssert(rootSection.children.count == 7, @"Full Blender menus missing from visible button");
    UIMenu *addMenu = (UIMenu *)rootSection.children[5];
    NSCAssert([addMenu.title isEqual:@"Add"] &&
              [addMenu.children.firstObject isKindOfClass:UIDeferredMenuElement.class],
              @"Add must load Blender's live object-creation commands");
    checkMenu((UIMenu *)actions.menuRepresentation, stableMenu);
    NSUInteger assignments = actions.menuAssignments;
    NSArray *contextItems = builder.items;
    builder.items = @[@{@"title": @"X", @"key": @"X"}];
    [builder updateActionsButtonMenu];
    NSCAssert(actions.menu == stableMenu && actions.menuAssignments == assignments,
              @"Contextual shortcuts replaced the full Blender menu hierarchy");
    builder.items = contextItems;
    NSData *snapshot = [NSJSONSerialization dataWithJSONObject:builder.menuRoots options:0 error:nil];
    for (NSUInteger tick = 0; tick < 100; tick++) {
      // The bridge returns new containers every poll, even when their contents are unchanged.
      builder.menuRoots = [NSJSONSerialization JSONObjectWithData:snapshot options:0 error:nil];
      [builder updateActionsButtonMenu];
      NSCAssert(actions.menu == stableMenu && actions.menuAssignments == assignments,
                @"Toolbar polling replaced the unchanged Actions menu");
    }
    NSArray *originalItems = builder.menuRoots;
    NSDictionary *meshRoute = @{@"root": @"VIEW3D_MT_editor_menus", @"path": @[@"Mesh"]};
    builder.menuRoots = @[@{@"title": @"Mesh", @"operator": @"TEST_OT_menu", @"route": meshRoute}];
    [builder updateActionsButtonMenu];
    NSCAssert(actions.menu != stableMenu && actions.menuAssignments == assignments + 1,
              @"Changed context did not replace the menu exactly once");
    UIMenu *changedSection = (UIMenu *)actions.menu.children.firstObject;
    UIAction *changedAction = (UIAction *)changedSection.children.firstObject;
    NSCAssert([changedAction.title isEqual:@"Mesh"], @"Menu retained stale contents");
    UIButton *changedTrigger = [UIButton buttonWithType:UIButtonTypeSystem];
    [changedTrigger addAction:changedAction forControlEvents:UIControlEventTouchUpInside];
    [changedTrigger sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSDictionary *submitted = [NSJSONSerialization JSONObjectWithData:
        [menuRequests.lastObject[@"route"] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    NSCAssert([submitted isEqual:meshRoute] && [menuRequests.lastObject[@"execute"] boolValue],
              @"Changed menu dispatched a stale route");
    builder.menuRoots = @[separator];
    [builder updateActionsButtonMenu];
    NSCAssert(!actions.enabled && actions.menu.children.count == 0, @"Empty menu remained enabled");
    assignments = actions.menuAssignments;
    [builder updateActionsButtonMenu];
    NSCAssert(actions.menuAssignments == assignments, @"Empty menu rebuilt on every poll");
    builder.menuRoots = originalItems;
    [builder updateActionsButtonMenu];
    NSCAssert(actions.enabled && actions.menu.children.count == 1, @"Menu did not recover with context");
    builder.viewport = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 834, 1210)] autorelease];
    builder.pencilMenu = [[[MenuTestPencilMenu alloc] init] autorelease];
    builder.editInteraction = [[[MenuTestInteraction alloc] init] autorelease];
    MenuTestPress *viewportPress = [[[MenuTestPress alloc] init] autorelease];
    [builder.viewport addGestureRecognizer:viewportPress];
    MenuTestTouch *touch = [[[MenuTestTouch alloc] init] autorelease];
    touch.type = UITouchTypeDirect;
    touch.view = builder.viewport;
    NSCAssert([builder gestureRecognizer:viewportPress shouldReceiveTouch:(id)touch],
              @"Bare viewport long press stopped working");
    UIView *overlay = [[[UIView alloc] initWithFrame:builder.viewport.bounds] autorelease];
    [builder.viewport addSubview:overlay];
    UIButton *tool = [UIButton buttonWithType:UIButtonTypeSystem];
    [tool setTitle:@"Move" forState:UIControlStateNormal];
    [overlay addSubview:tool];
    UIScrollView *scroll = [[[UIScrollView alloc] init] autorelease];
    [overlay addSubview:scroll];
    for (UIView *hit in @[tool, tool.titleLabel, scroll, overlay]) {
      touch.view = hit;
      NSCAssert(![builder gestureRecognizer:viewportPress shouldReceiveTouch:(id)touch],
                @"Viewport long press stole a native-control touch: %@", hit);
    }
    MenuTestPress *toolPress = [[[MenuTestPress alloc] init] autorelease];
    [tool addGestureRecognizer:toolPress];
    touch.view = tool;
    NSCAssert([builder gestureRecognizer:toolPress shouldReceiveTouch:(id)touch],
              @"Native tool press-and-slide was disabled");
    touch.view = builder.viewport;
    touch.type = UITouchTypePencil;
    NSCAssert(![builder gestureRecognizer:viewportPress shouldReceiveTouch:(id)touch],
              @"Pencil accepted by finger context-menu gesture");
    touch.type = UITouchTypeDirect;
    builder.pencilMenu.active = YES;
    NSCAssert(![builder gestureRecognizer:viewportPress shouldReceiveTouch:(id)touch],
              @"Viewport press competed with active radial menu");
    viewportPress.testState = UIGestureRecognizerStateBegan;
    [builder handleActionsMenuPress:viewportPress];
    NSCAssert(builder.editInteraction.presentations == 0,
              @"In-flight viewport press opened a menu after radial menu opened");
    builder.pencilMenu.active = NO;
    [builder handleActionsMenuPress:viewportPress];
    NSCAssert(builder.editInteraction.presentations == 1, @"Valid viewport press did not open menu");
    viewportPress.testState = UIGestureRecognizerStateChanged;
    [builder handleActionsMenuPress:viewportPress];
    NSCAssert(builder.editInteraction.presentations == 1, @"Long press presented multiple times");
    actions.menu = nil;
    [builder updateActionsButtonMenu];
    NSCAssert(actions.menu.children.count == 1, @"Missing menu was not restored");
    builder.model = @{@"mode": @"objectmode", @"canUndo": @YES,
                      @"actions": @[@{@"title": @"Move", @"operator": @"TRANSFORM_OT_translate"},
                                    @{@"title": @"X", @"key": @"X"}]};
    NSDictionary *rootRoute = @{@"root": @"VIEW3D_MT_object_context_menu", @"path": @[]};
    NSDictionary *originRoute = @{@"root": rootRoute[@"root"], @"path": @[@"origin_geometry"]};
    NSDictionary *cursorRoute = @{@"root": rootRoute[@"root"], @"path": @[@"origin_cursor"]};
    NSArray *projected = @[
      separator, @"invalid",
      @{@"title": @"Object", @"action": @NO, @"route": rootRoute},
      @{@"title": @"Origin to Geometry", @"operator": @"OBJECT_OT_origin_set",
        @"action": @YES, @"route": originRoute, @"checked": @YES},
      @{@"title": @"Origin to 3D Cursor", @"operator": @"OBJECT_OT_origin_set",
        @"action": @YES, @"route": cursorRoute, @"disabled": @YES},
      @{@"title": @"Set Origin", @"submenu": @YES, @"route": rootRoute},
      @{@"title": @"Delete", @"action": @YES, @"operator": @"OBJECT_OT_delete", @"route": rootRoute},
      @{@"title": @"Custom Add-on Action", @"action": @YES, @"operator": @"TEST_OT_custom", @"route": rootRoute},
    ];
    NSArray *radial = [builder radialItemsFromBlenderMenu:projected];
    NSCAssert(radial.count == 5, @"Headers, shortcuts, or fabricated undo in radial menu");
    NSCAssert([radial[0][@"title"] isEqual:@"Origin to Geometry"] &&
              [radial[1][@"title"] isEqual:@"Origin to 3D Cursor"], @"Blender labels/order were replaced");
    NSCAssert(radial[0][@"route"] == originRoute && radial[1][@"route"] == cursorRoute,
              @"Distinct operator properties were lost");
    NSCAssert([radial[0][@"checked"] boolValue] && [radial[1][@"disabled"] boolValue], @"Lost state");
    NSCAssert([radial[2][@"submenu"] boolValue] && radial[2][@"image"] != nil, @"Lost submenu");
    NSCAssert([radial[3][@"destructive"] boolValue], @"Lost destructive hint");
    NSCAssert(radial[4][@"image"] == nil, @"Unknown command gained an arbitrary icon");
    NSCAssert([builder radialItemsFromBlenderMenu:@[]].count == 0, @"Invented fallback actions");
    NSCAssert(radial[0][@"image"] == nil && radial[1][@"image"] == nil && radial[3][@"image"] == nil,
              @"Iconless context entries gained guessed operator icons");
    builder.items = @[separator, separator];
    NSCAssert(blenderIOSNativeMenuElements(builder.items).count == 0,
              @"Separator-only input produced blank entries");
    builder.shadingButton = [builder viewportShadingButton];
    NSArray *modes = @[@"wireframe", @"solid", @"material", @"rendered", @"solid"];
    NSArray *commands = @[@20, @21, @22, @23, @21];
    for (NSUInteger i = 0; i < modes.count; i++) {
      [builder setViewportShadingValue:modes[i]];
      UIBarButtonItem *button = builder.shadingButton;
      NSInteger command = [commands[i] integerValue];
      NSCAssert(button.tag == command, @"Shading state did not follow Blender");
      NSString *key = [NSString stringWithFormat:@"command.%ld", (long)command];
      NSCAssert([button.image isEqual:icons[key]], @"Wrong selected shading icon");
      NSCAssert(button.menu == nil && button.action == @selector(showViewportShading:), @"Shading opens the native controls popover");
      NSUInteger selectedCount = 0;
      for (UIAction *action in ((UIMenu *)button.menuRepresentation).children) {
        if (action.state == UIMenuElementStateOn) {
          selectedCount++;
          NSCAssert(action.identifier.integerValue == command, @"Wrong shading checkmark");
          NSCAssert([button.accessibilityValue isEqual:action.title], @"Wrong accessible shading name");
        }
        UIButton *trigger = [UIButton buttonWithType:UIButtonTypeSystem];
        [trigger addAction:action forControlEvents:UIControlEventTouchUpInside];
        [trigger sendActionsForControlEvents:UIControlEventTouchUpInside];
        NSCAssert(queuedCommand == action.identifier.integerValue, @"Wrong shading command dispatched");
      }
      NSCAssert(selectedCount == 1, @"Missing/duplicate shading checkmarks");
      UIMenuElement *menu = button.menuRepresentation;
      [builder setViewportShadingValue:modes[i]];
      NSCAssert(button.menuRepresentation == menu, @"Unchanged state unnecessarily rebuilt menu");
      [builder setViewportShadingValue:@"invalid"];
      [builder setViewportShadingValue:nil];
      [builder setViewportShadingValue:(id)@42];
      NSCAssert(button.menuRepresentation == menu, @"Invalid snapshot changed shading state");
    }
    for (NSString *op in @[@"OBJECT_OT_select_all", @"MESH_OT_select_all"]) {
      for (NSString *value in @[@"SELECT", @"DESELECT", @"INVERT"]) {
        UIImage *image = blenderIOSActionIcon(op, @"action", value);
        NSString *key = [NSString stringWithFormat:@"operator.%@.action.%@", op, value];
        NSCAssert(image && [image isEqual:icons[key]], @"Selection action lost Blender artwork");
      }
    }
    NSArray *textOnly = @[
      @[@"Select Ring", @"MESH_OT_edgering_select", @"", @"circle.hexagongrid"],
      @[@"Unknown", @"TEST_OT_unknown", @"", @"circle"],
      @[@"Unknown", @"TEST_OT_unknown", @"", @"arrow.uturn.backward"],
    ];
    for (NSArray *item in textOnly) {
      UIButton *button = [builder actionButtonWithTitle:item[0] systemImage:item[3]
                                               operator:item[1] property:@"" value:@"" key:item[2]];
      NSCAssert(button.configuration.image == nil, @"Weak or generic shortcut icon survived");
      NSCAssert(button.configuration.imagePadding == 0, @"Text-only action kept an icon gap");
      NSCAssert([button.configuration.title isEqual:item[0]], @"Text-only action lost its title");
    }
    NSArray *axisNames = @[@"X", @"Y", @"Z"];
    NSArray *axisColors = @[UIColor.systemRedColor, UIColor.systemGreenColor, UIColor.systemBlueColor];
    NSMutableArray *axisButtons = [NSMutableArray array];
    for (NSUInteger i = 0; i < 3; i++) {
      NSString *axis = axisNames[i];
      UIButton *button = [builder actionButtonWithTitle:axis systemImage:@"xmark"
                                               operator:@"" property:@"" value:@"" key:axis];
      [axisButtons addObject:button];
      NSCAssert([button.configuration.attributedTitle.string isEqual:axis], @"Axis is not a letter");
      NSCAssert([button.configuration.baseForegroundColor isEqual:axisColors[i]], @"Wrong axis color");
      NSCAssert(!button.selected && button.configuration.image == nil, @"Initial axis incorrectly active");
      NSCAssert([button.accessibilityValue isEqual:@"Off"], @"Missing initial accessibility state");
      [builder performSelector:@selector(handleActionButton:) withObject:button];
      NSCAssert(([settings.lastObject isEqual:@[@"constraint_axis", axis]]), @"Axis did not queue a toggle");
    }
    NSString *stableSignature = nil;
    UIView *axisHost = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 240, 80)] autorelease];
    UIStackView *axisStack = [[[UIStackView alloc] initWithArrangedSubviews:axisButtons] autorelease];
    axisStack.spacing = 6;
    axisStack.translatesAutoresizingMaskIntoConstraints = NO;
    [axisHost addSubview:axisStack];
    [NSLayoutConstraint activateConstraints:@[
      [axisStack.leadingAnchor constraintEqualToAnchor:axisHost.leadingAnchor],
      [axisStack.topAnchor constraintEqualToAnchor:axisHost.topAnchor],
    ]];
    for (NSUInteger mask = 0; mask < 8; mask++) {
      NSMutableArray *actions = [NSMutableArray array];
      for (NSUInteger i = 0; i < 3; i++) {
        [actions addObject:@{@"title": axisNames[i], @"key": axisNames[i],
                              @"selected": @((mask & (1 << i)) != 0)}];
      }
      NSString *signature = [builder actionSignatureForActions:actions];
      if (stableSignature) NSCAssert([signature isEqual:stableSignature], @"Selection rebuilt the strip");
      stableSignature = signature;
      [builder updateAxisConstraintsWithActions:actions];
      [axisHost layoutIfNeeded];
      for (NSUInteger i = 0; i < 3; i++) {
        UIButton *button = axisButtons[i];
        BOOL selected = (mask & (1 << i)) != 0;
        NSCAssert(button.selected == selected, @"Axis state disagrees with Blender");
        NSCAssert((button.configuration.image != nil) == selected, @"Missing/incorrect checkmark");
        NSCAssert([button.accessibilityValue isEqual:selected ? @"On" : @"Off"], @"Wrong accessible state");
        NSCAssert(!!(button.accessibilityTraits & UIAccessibilityTraitSelected) == selected,
                  @"Wrong selected accessibility trait");
        CGSize size = button.bounds.size;
        NSCAssert(size.width == 60 && size.height == 44, @"Axis changed size when toggled: %@", NSStringFromCGSize(size));
        CGSize content = [button systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
        NSCAssert(content.width <= 60 && content.height <= 44, @"Axis contents exceed the button");
        NSUInteger updates = builder.axisUpdates;
        [builder updateAxisConstraintsWithActions:actions];
        NSCAssert(updates == builder.axisUpdates, @"Unchanged poll restyled a button");
        for (NSNumber *style in @[@(UIUserInterfaceStyleLight), @(UIUserInterfaceStyleDark)]) {
          UITraitCollection *traits = [UITraitCollection traitCollectionWithUserInterfaceStyle:(UIUserInterfaceStyle)style.integerValue];
          UIColor *expected = selected ? UIColor.blackColor : axisColors[i];
          NSCAssert([[button.configuration.baseForegroundColor resolvedColorWithTraitCollection:traits]
                      isEqual:[expected resolvedColorWithTraitCollection:traits]], @"Axis color lost theme support");
        }
      }
    }
    UIButton *frame = [builder actionButtonWithTitle:@"Frame" systemImage:@"not.a.symbol"
                                            operator:@"VIEW3D_OT_view_selected" property:@"" value:@"" key:@""];
    NSCAssert(frame.configuration.image != nil && frame.configuration.imagePadding == 5,
              @"Blender shortcut artwork was removed");
    missingAssets = YES;
    for (NSString *op in @[@"ED_OT_undo", @"ED_OT_redo", @"OBJECT_OT_duplicate_move",
                           @"OBJECT_OT_delete", @"MESH_OT_delete"]) {
      NSCAssert(blenderIOSActionIcon(op, @"", @"") != nil, @"Strong SF fallback missing");
    }
    NSCAssert(blenderIOSActionIcon(@"MESH_OT_edgering_select", @"", @"") == nil,
              @"Unmapped action gained a placeholder");
    missingAssets = NO;
    builder.undoControl = [builder undoButton];
    UIBarButtonItem *undo = builder.undoControl;
    NSCAssert(!undo.enabled && undo.image != nil, @"Undo needs disabled initial state and icon");
    NSCAssert([undo.accessibilityLabel isEqual:@"Undo"], @"Undo missing accessible label");
    NSCAssert([undo.image isEqual:icons[@"operator.ED_OT_undo"]], @"Wrong undo artwork");
    NSCAssert([undo.title isEqual:@"Undo"] && undo.customView == nil,
              @"Undo needs a native label and appearance");
    NSUInteger queuedBefore = queued.count;
    [builder handleUndoButton:undo];
    NSCAssert(queued.count == queuedBefore, @"Disabled undo queued a command");
    [builder setUndoAvailable:@YES];
    NSCAssert(undo.enabled, @"Undo did not enable with history");
    NSCAssert(undo.target == builder && undo.action == @selector(handleUndoButton:),
              @"Undo tap target missing");
    // This command-line UIKit harness has no UIApplication to route target/actions.
    [builder performSelector:undo.action withObject:undo];
    NSCAssert(queued.count == queuedBefore + 1 && [queued.lastObject isEqual:@"ED_OT_undo"],
              @"Undo did not dispatch exactly one Blender operator");
    [builder setUndoAvailable:@NO];
    NSCAssert(!undo.enabled, @"Undo stayed enabled without history");
    [builder setUndoAvailable:@"true"];
    NSCAssert(!undo.enabled, @"Malformed undo state enabled the control");
    [builder setUndoAvailable:nil];
    NSCAssert(!undo.enabled, @"Missing undo state enabled the control");
    MenuCountingButton *countedUndo = [[[MenuCountingButton alloc] init] autorelease];
    builder.undoControl = countedUndo;
    [builder setUndoAvailable:@YES];
    NSUInteger enabledAssignments = countedUndo.enabledAssignments;
    for (NSUInteger tick = 0; tick < 100; tick++) [builder setUndoAvailable:@YES];
    NSCAssert(countedUndo.enabledAssignments == enabledAssignments, @"Unchanged poll reconfigured native Undo");
    [builder setUndoAvailable:@NO];
    NSCAssert(countedUndo.enabledAssignments == enabledAssignments + 1 && !countedUndo.enabled,
              @"Undo change did not propagate exactly once");
    NSLog(@"PASS: inline sections, operator dispatch, shading state, curated icons, undo control");
  }
  return 0;
}
'''
        harness = harness.replace("MANIFEST_PATH", json.dumps(str(root / "release/ios/blender_icons.json")))
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "menu_test.mm"
            executable = Path(directory) / "menu_test"
            source_file.write_text(harness)
            architecture = os.uname().machine
            result = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-fblocks", "-fno-objc-arc",
                "-isysroot", sdk_result.stdout.strip(),
                "-target", f"{architecture}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation",
                str(source_file), "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(
                ["xcrun", "simctl", "spawn", device, str(executable)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: inline sections", result.stderr)


if __name__ == "__main__":
    unittest.main()
