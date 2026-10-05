# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compile the production iPadOS menu projection against UIKit."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class NativeMenuBarTest(unittest.TestCase):
    def test_menu_projection_and_routes(self):
        if sys.platform != "darwin":
            self.skipTest("Requires UIKit simulator")
        devices = json.loads(subprocess.check_output(
            ["xcrun", "simctl", "list", "devices", "booted", "--json"], text=True))
        candidates = [d for runtime, entries in devices["devices"].items()
                      if ".iOS-" in runtime for d in entries if d["isAvailable"]]
        if not candidates:
            self.skipTest("No booted iOS simulator")
        device = os.environ.get("IOS_TEST_SIMULATOR_UDID", candidates[0]["udid"])
        sdk = subprocess.check_output(
            ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip()
        root = Path(__file__).resolve().parents[2]
        source = (root / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        start = source.index("static NSString *blenderIOSMenuRoute(")
        helpers = source[start:source.index("@interface GHOST_IOSViewController", start)]
        helpers = helpers.replace("[UIImage imageNamed:", "[MenuTestImages imageNamed:")
        start = source.rindex("- (void)buildMenuWithBuilder:")
        build = source[start:source.index("\n- (void)refreshToolbarFromBlender", start)]
        start = source.index("@interface GHOSTIOSNavigationController")
        navigation = source[start:source.index("\n@end", source.index("@implementation", start)) + 5]
        system = (root / "intern/ghost/intern/GHOST_SystemIOS.mm").read_text()
        start = system.index("- (void)buildMenuWithBuilder:")
        delegate_method = system[start:system.index("\n}\n", start) + 2]
        harness = r'''
#import <UIKit/UIKit.h>
static NSMutableArray *loadedAssets;
@interface MenuTestImages : NSObject
+ (UIImage *)imageNamed:(NSString *)name;
@end
@implementation MenuTestImages
+ (UIImage *)imageNamed:(NSString *)name {
  [loadedAssets addObject:name];
  return [@[@"blender.mesh_cube", @"blender.outliner_ob_mesh"] containsObject:name] ?
      [UIImage systemImageNamed:@"cube"] : nil;
}
@end
static NSMutableArray *requests;
static uint64_t WM_apple_ios_ui_menu_request(const char *route, bool execute) {
  [requests addObject:@{@"route": [NSString stringWithUTF8String:route], @"execute": @(execute)}];
  return requests.count;
}
static char *WM_apple_ios_ui_menu_take_result(uint64_t identifier) { return nullptr; }
static UIImage *blenderIOSIcon(NSString *key, NSString *fallback) { return nil; }
''' + helpers + r'''
@interface MenuBuilder : NSObject
@property(nonatomic, retain) NSMutableDictionary<NSString *, UIMenu *> *menus;
@property(nonatomic, retain) NSMutableArray *insertions;
@end
@implementation MenuBuilder
- (UIMenuSystem *)system { return UIMenuSystem.mainSystem; }
- (UIMenu *)menuForIdentifier:(NSString *)identifier { return self.menus[identifier]; }
- (void)replaceMenuForIdentifier:(NSString *)identifier withMenu:(UIMenu *)menu {
  self.menus[identifier] = menu;
}
- (void)insertChildMenu:(UIMenu *)menu atEndOfMenuForIdentifier:(NSString *)identifier {
  if (self.menus[identifier]) {
    UIMenu *old = self.menus[identifier];
    self.menus[identifier] = [old menuByReplacingChildren:[old.children arrayByAddingObject:menu]];
  }
  else self.menus[menu.identifier] = menu;
  [self.insertions addObject:menu];
}
- (void)insertSiblingMenu:(UIMenu *)menu beforeMenuForIdentifier:(NSString *)identifier {
  self.menus[menu.identifier] = menu;
  [self.insertions addObject:menu];
}
@end
@interface MenuController : UIViewController {
@public NSArray *_native_menubar_items;
}
@end
@implementation MenuController
''' + build + r'''
@end
''' + navigation + r'''
@interface MenuWindow : NSObject
@property(nonatomic, retain) UIViewController *rootViewController;
@end
@implementation MenuWindow
- (BOOL)isKeyWindow { return YES; }
@end
@interface MenuScene : NSObject
@property(nonatomic, retain) NSArray *windows;
@end
@implementation MenuScene
@end
static MenuScene *g_window_scene;
struct TestGhostWindow { MenuWindow *rootWindow; };
class GHOST_ISystem {
public:
  static GHOST_ISystem *getSystem();
};
class GHOST_SystemIOS : public GHOST_ISystem {
public:
  TestGhostWindow *current_active_window_ = nullptr;
};
static GHOST_SystemIOS ghost;
GHOST_ISystem *GHOST_ISystem::getSystem() { return &ghost; }
@interface MenuAppDelegate : UIResponder
@end
@implementation MenuAppDelegate
''' + delegate_method + r'''
@end
int main() {
  @autoreleasepool {
    requests = [NSMutableArray array];
    loadedAssets = [NSMutableArray array];
    NSDictionary *route = @{@"root": @"TOPBAR_MT_editor_menus", @"path": @[@"File", @"Save Copy:copy=True"]};
    NSDictionary *action = @{@"title": @"Save Copy", @"operator": @"WM_OT_save_as_mainfile",
                             @"icon": @"MESH_CUBE", @"route": route};
    NSDictionary *toggle = @{@"title": @"Toggle", @"route": route, @"checked": @1};
    NSDictionary *disabled = @{@"title": @"Unavailable", @"route": route, @"disabled": @1};
    NSDictionary *submenu = @{@"title": @"Nested", @"route": route, @"submenu": @1,
                              @"icon": @"OUTLINER_OB_MESH"};
    NSArray *elements = blenderIOSNativeMenuElements(@[@{@"separator": @1}, action,
      @{@"separator": @1}, @{@"separator": @1}, toggle, disabled, submenu, @{@"separator": @1}]);
    NSCAssert(elements.count == 2, @"Empty sections or missing separators");
    UIMenu *first = (UIMenu *)elements[0], *second = (UIMenu *)elements[1];
    NSCAssert(first.options & UIMenuOptionsDisplayInline, @"Blank submenu instead of separator");
    UIAction *save = (UIAction *)first.children[0];
    NSCAssert(save.attributes == 0, @"Valid operator disabled");
    NSCAssert(save.image && save.image.renderingMode == UIImageRenderingModeAlwaysTemplate,
              @"Menu artwork must use native template tinting");
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button addAction:save forControlEvents:UIControlEventTouchUpInside];
    [button sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSData *data = [requests.lastObject[@"route"] dataUsingEncoding:NSUTF8StringEncoding];
    NSCAssert([[NSJSONSerialization JSONObjectWithData:data options:0 error:nil] isEqual:route],
              @"Lost operator properties/route");
    NSCAssert([requests.lastObject[@"execute"] boolValue], @"Did not enqueue execution");
    NSCAssert(((UIAction *)second.children[0]).state == UIMenuElementStateOn, @"Lost checkbox");
    NSCAssert(((UIAction *)second.children[1]).attributes & UIMenuElementAttributesDisabled, @"Lost poll");
    UIMenu *nested = (UIMenu *)second.children[2];
    NSCAssert(nested.image && nested.image.renderingMode == UIImageRenderingModeAlwaysTemplate,
              @"Submenu lost its Blender icon");
    NSCAssert([loadedAssets containsObject:@"blender.mesh_cube"] &&
              [loadedAssets containsObject:@"blender.outliner_ob_mesh"], @"Wrong Blender artwork names");
    NSCAssert(((UIAction *)second.children[0]).image == nil, @"Iconless entry gained artwork");
    for (id icon in @[@"NONE", @"BLANK1", @"", @42, NSNull.null, @"UNKNOWN_ADDON_ICON"]) {
      NSCAssert(blenderIOSMenuIcon(@{@"icon": icon}) == nil, @"Invalid/empty icon not handled gracefully");
    }
    NSCAssert([nested.children[0] isKindOfClass:UIDeferredMenuElement.class], @"Submenu not lazy");
    MenuController *controller = [[[MenuController alloc] init] autorelease];
    MenuBuilder *builder = [[[MenuBuilder alloc] init] autorelease];
    builder.menus = [NSMutableDictionary dictionary];
    builder.insertions = [NSMutableArray array];
    for (NSString *key in @[UIMenuApplication, UIMenuFile, UIMenuEdit, UIMenuView, UIMenuWindow, UIMenuHelp]) {
      builder.menus[key] = [UIMenu menuWithTitle:key image:nil identifier:key options:0 children:@[save]];
    }
    NSMutableArray *roots = [NSMutableArray arrayWithObject:@{@"separator": @1}];
    for (NSString *name in @[@"TOPBAR_MT_blender", @"TOPBAR_MT_file", @"TOPBAR_MT_edit",
                            @"TOPBAR_MT_window", @"TOPBAR_MT_help", @"VIEW3D_MT_view", @"VIEW3D_MT_edit_mesh"]) {
      [roots addObject:@{@"menu": name, @"title": name, @"submenu": @1, @"route": route}];
    }
    controller->_native_menubar_items = roots;
    MenuWindow *window = [[[MenuWindow alloc] init] autorelease];
    window.rootViewController = [[[GHOSTIOSNavigationController alloc]
        initWithRootViewController:controller] autorelease];
    g_window_scene = [[[MenuScene alloc] init] autorelease];
    g_window_scene.windows = @[window];
    TestGhostWindow ghostWindow{window};
    ghost.current_active_window_ = &ghostWindow;
    MenuAppDelegate *delegate = [[[MenuAppDelegate alloc] init] autorelease];
    [delegate buildMenuWithBuilder:(id<UIMenuBuilder>)builder];
    NSCAssert([builder.menus[UIMenuFile].children[0] isKindOfClass:UIDeferredMenuElement.class], @"Fake File menu");
    NSCAssert([builder.menus[UIMenuEdit].children[0] isKindOfClass:UIDeferredMenuElement.class], @"Fake Edit menu");
    NSCAssert([builder.menus[UIMenuWindow].children[0] isEqual:save], @"Removed system Window commands");
    NSCAssert([builder.menus[UIMenuApplication].children[0] isEqual:save], @"Removed system app commands");
    NSCAssert(builder.menus[@"org.blender.menu.VIEW3D_MT_edit_mesh"], @"Missing mode-specific menu");
    for (UIMenu *menu in builder.insertions) {
      NSCAssert(![menu.title isEqual:@"Commands"], @"Separator created an empty Commands menu");
    }
    NSLog(@"PASS: native menu bar, sections, state, deferred submenus, exact command routes");
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "menubar.mm"
            executable = Path(directory) / "menubar"
            src.write_text(harness)
            result = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-fblocks", "-fno-objc-arc",
                "-isysroot", sdk, "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation", str(src), "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(["xcrun", "simctl", "spawn", device, str(executable)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: native menu bar", result.stderr)


if __name__ == "__main__":
    unittest.main()
