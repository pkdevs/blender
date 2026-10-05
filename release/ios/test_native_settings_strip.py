# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the production native top bar, grouped overflow and setting dispatch."""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest


class NativeSettingsStripTest(unittest.TestCase):
    def test_native_groups_preserve_settings_and_selection(self):
        if sys.platform != "darwin":
            self.skipTest("Requires Xcode and an iOS simulator")
        sdk = subprocess.run(["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"],
                             capture_output=True, text=True)
        if sdk.returncode:
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
        start = source.index("@interface GHOSTIOSNavigationBar")
        view_class = source[start:source.index("\n@end", source.index("@implementation", start)) + 5]
        start = source.index("@interface GHOSTIOSNavigationController")
        view_class += source[start:source.index("\n@end", source.index("@implementation", start)) + 5]
        start = source.index("enum GHOSTIOSUICommand")
        command_enum = source[start:source.index("};", start) + 2]
        methods = []
        for signature in ("- (UIAction *)settingsActionWithTitle:",
                          "- (UIBarButtonItem *)settingsMenuButtonWithAccessibilityLabel:",
                          "- (UIBarButtonItemGroup *)topSettingsControlGroup\n",
                          "- (void)setTopSettingsVisible:",
                          "- (void)setTopSettingsValues:",
                          "- (UIBarButtonItem *)meshSelectModeButtonWithTitle:",
                          "- (UIBarButtonItemGroup *)meshSelectModeControlGroup\n",
                          "- (void)setMeshSelectModeVisible:",
                          "- (void)setMeshSelectModeValue:",
                          "- (UIAction *)modeActionWithTitle:",
                          "- (UIAction *)viewportShadingActionWithTitle:",
                          "- (void)setViewportShadingValue:",
                          "- (UIBarButtonItem *)viewportShadingButton\n",
                          "- (UIBarButtonItem *)modePickerButton\n",
                          "- (UIBarButtonItem *)actionsButton\n",
                          "- (UIBarButtonItem *)undoButton\n",
                          "- (void)handleUndoButton:",
                          "- (void)setFingerInputMode:",
                          "- (UIBarButtonItem *)fingerInputButton\n",
                          "- (void)updateAxisConstraintButton:",
                          "- (UIButton *)actionButtonWithTitle:",
                          "- (void)installActionStripLayout\n",
                          "- (void)updatePhoneActionStripLayout\n",
                          "- (CGFloat)expandedToolbarWidth\n",
                          "- (void)installNativeTopBar\n"):
            start = source.rindex(signature)
            methods.append(source[start:source.index("\n}", start) + 2])
        harness = r'''
#import <UIKit/UIKit.h>
#include <cmath>
#include <initializer_list>
#define UNUSED_VARS(...) ((void)0)
static NSMutableArray *queued;
static UIImage *blenderIOSIcon(NSString *, NSString *) { return [UIImage systemImageNamed:@"cube"]; }
static UIImage *blenderIOSCommandIcon(NSInteger, NSString *) { return [UIImage systemImageNamed:@"cube"]; }
static UIImage *blenderIOSActionIcon(NSString *, NSString *, NSString *) { return nil; }
extern "C" void WM_apple_ios_ui_command_enqueue(int) {}
extern "C" void WM_apple_ios_ui_operator_enqueue(const char *) {}
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value) {
  [queued addObject:@[[NSString stringWithUTF8String:setting], [NSString stringWithUTF8String:value]]];
}
''' + command_enum + view_class + r'''
static BOOL fingerTool;
@interface GHOSTUIWindow : UIWindow
- (void)setFingerToolEnabled:(BOOL)enabled;
@end
@implementation GHOSTUIWindow
- (void)setFingerToolEnabled:(BOOL)enabled { fingerTool = enabled; }
@end
@interface StripTest : UIViewController {
@public
  UIView *_overlay_view;
  GHOSTIOSNavigationBar *_top_bar;
  UINavigationItem *_top_bar_item;
  NSLayoutConstraint *_top_bar_height_constraint;
  UIBarButtonItem *_mode_button, *_actions_button, *_viewport_shading_button, *_undo_button;
  UIBarButtonItem *_finger_input_button;
  BOOL _finger_tool_enabled;
  UIView *_action_rail, *_tool_rail, *_view_gizmo_hotspot;
  NSLayoutConstraint *_action_phone_bottom_constraint, *_action_phone_trailing_constraint;
  UIBarButtonItemGroup *_top_settings_group, *_mesh_select_group;
  NSDictionary *_top_settings_values;
  NSMutableArray *_mesh_select_buttons;
  NSMutableArray *_action_buttons;
}
@end
@implementation StripTest
- (void)handleActionButton:(UIButton *)button {}
- (void)updateActionsButtonMenu {
  _actions_button.menu = [UIMenu menuWithTitle:@"Actions" children:@[
    [UIAction actionWithTitle:@"Move" image:nil identifier:nil handler:^(UIAction *) {}]]];
  _actions_button.menuRepresentation = _actions_button.menu;
}
''' + "\n".join(methods) + r'''
@end
static void invoke(UIAction *action) {
  UIButton *trigger = [UIButton buttonWithType:UIButtonTypeSystem];
  [trigger addAction:action forControlEvents:UIControlEventTouchUpInside];
  [trigger sendActionsForControlEvents:UIControlEventTouchUpInside];
}
static void checkMenu(UIMenu *actual, UIMenu *expected) {
  NSCAssert([actual.title isEqual:expected.title] && actual.children.count == expected.children.count,
            @"Overflow menu differs");
  for (NSUInteger i = 0; i < expected.children.count; i++) {
    UIMenuElement *a = actual.children[i], *b = expected.children[i];
    NSCAssert([a.title isEqual:b.title], @"Overflow action title differs");
    if ([b isKindOfClass:UIMenu.class]) {
      checkMenu((UIMenu *)a, (UIMenu *)b);
    } else {
      NSCAssert(((UIAction *)a).state == ((UIAction *)b).state, @"Overflow checkmark differs");
    }
  }
}
static void collectControls(UIView *view, NSMutableArray *controls) {
  if (view.hidden || view.alpha < 0.01) return;
  if ([view isKindOfClass:UIControl.class]) {
    [controls addObject:view];
    return;
  }
  for (UIView *child in view.subviews) collectControls(child, controls);
}
static void checkControls(UINavigationBar *bar) {
  NSMutableArray *controls = [NSMutableArray array];
  collectControls(bar, controls);
  NSCAssert(controls.count >= 2, @"Missing visible native bar controls");
  for (NSUInteger i = 0; i < controls.count; i++) {
    UIControl *control = controls[i];
    CGRect rect = [control convertRect:control.bounds toView:bar];
    NSCAssert(CGRectContainsRect(CGRectInset(bar.bounds, -1, -1), rect), @"Bar control clipped");
    if (control.enabled) {
      UIView *hit = [bar hitTest:CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect)) withEvent:nil];
      NSCAssert(hit == control || [hit isDescendantOfView:control], @"Native control cannot receive taps");
    }
    for (NSUInteger j = 0; j < i; j++) {
      UIView *other = controls[j];
      CGRect otherRect = [other convertRect:other.bounds toView:bar];
      NSCAssert(!CGRectIntersectsRect(CGRectInset(rect, 0.5, 0.5), otherRect), @"Bar controls overlap");
    }
  }
}
static UIWindow *testWindow;
static void runChecks() {
  @autoreleasepool {
    queued = [NSMutableArray array];
    StripTest *builder = [[[StripTest alloc] init] autorelease];
    testWindow.rootViewController = builder;
    builder->_overlay_view = builder.view;
    builder->_mesh_select_buttons = [NSMutableArray array];
    [builder installNativeTopBar];
    [testWindow makeKeyAndVisible];
    [UIView setAnimationsEnabled:NO];
    UINavigationItem *item = builder->_top_bar_item;
    NSCAssert(item.style == UINavigationItemStyleEditor, @"Missing native editor overflow");
    // Render the real axis controls on a hosted UIKit surface in both appearances.
    builder->_action_buttons = [NSMutableArray array];
    builder.view.frame = CGRectMake(0, 0, 466, 800);
    UIView *axisPanel = [[[UIView alloc] initWithFrame:CGRectMake(20, 120, 224, 412)] autorelease];
    axisPanel.backgroundColor = UIColor.secondarySystemBackgroundColor;
    [builder.view addSubview:axisPanel];
    NSUInteger rowIndex = 0;
    for (NSUInteger mask : {0, 1, 2, 4, 3, 5, 6}) {
      NSMutableArray *buttons = [NSMutableArray array];
      for (NSUInteger axis = 0; axis < 3; axis++) {
        NSString *name = @[@"X", @"Y", @"Z"][axis];
        UIButton *button = [builder actionButtonWithTitle:name systemImage:@""
                                                operator:@"" property:@"" value:@"" key:name];
        [builder updateAxisConstraintButton:button selected:(mask & (1 << axis)) != 0];
        [buttons addObject:button];
      }
      UIStackView *row = [[[UIStackView alloc] initWithArrangedSubviews:buttons] autorelease];
      row.spacing = 8;
      row.translatesAutoresizingMaskIntoConstraints = NO;
      [axisPanel addSubview:row];
      [NSLayoutConstraint activateConstraints:@[
        [row.leadingAnchor constraintEqualToAnchor:axisPanel.leadingAnchor constant:14],
        [row.topAnchor constraintEqualToAnchor:axisPanel.topAnchor constant:16 + rowIndex * 56],
      ]];
      rowIndex++;
    }
    for (UIUserInterfaceStyle style : {UIUserInterfaceStyleLight, UIUserInterfaceStyleDark}) {
      builder.view.overrideUserInterfaceStyle = style;
      [builder.view layoutIfNeeded];
      for (UIButton *button in builder->_action_buttons) {
        NSCAssert(CGSizeEqualToSize(button.bounds.size, CGSizeMake(60, 44)), @"Axis button resized");
        NSCAssert(button.titleLabel.frame.size.width > 0, @"Axis letter missing");
        NSCAssert(CGRectContainsRect(button.bounds, button.titleLabel.frame), @"Axis letter clipped");
        if (button.selected) {
          NSCAssert(button.imageView.frame.size.width > 0, @"Checkmark missing");
          NSCAssert(CGRectContainsRect(button.bounds, button.imageView.frame), @"Checkmark clipped");
          NSCAssert(!CGRectIntersectsRect(button.titleLabel.frame, button.imageView.frame), @"Axis and checkmark overlap");
        }
      }
      UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc] initWithSize:axisPanel.bounds.size] autorelease];
      UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        [axisPanel drawViewHierarchyInRect:axisPanel.bounds afterScreenUpdates:YES];
      }];
      NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
      NSString *filename = style == UIUserInterfaceStyleLight ? @"axis-light.png" : @"axis-dark.png";
      [UIImagePNGRepresentation(image) writeToFile:[documents stringByAppendingPathComponent:filename] atomically:YES];
    }
    [axisPanel removeFromSuperview];
    [builder->_action_buttons removeAllObjects];
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
      NSCAssert(builder->_finger_input_button != nil && fingerTool, @"Phone tool input missing");
      invoke((UIAction *)builder->_finger_input_button.menu.children[1]);
      NSCAssert(!fingerTool, @"Orbit mode not routed to input window");
      invoke((UIAction *)builder->_finger_input_button.menu.children[0]);
      NSCAssert(fingerTool, @"Tool mode not restored");
    }
    NSCAssert(item.pinnedTrailingGroup.barButtonItems.firstObject == builder->_undo_button,
              @"Undo must remain available outside overflow");
    UIBarButtonItemGroup *strip = builder->_top_settings_group;
    NSCAssert(strip.hidden && builder->_mesh_select_group.hidden, @"Initial context groups visible");
    [builder setTopSettingsVisible:YES animated:NO];
    NSCAssert(!strip.hidden, @"Transform settings did not appear");
    NSArray *settings = @[@"orientation", @"pivot", @"snap", @"proportional"];
    NSArray *values = @[
      @[@"global", @"local", @"normal", @"gimbal", @"view", @"cursor", @"parent"],
      @[@"median", @"bounds", @"cursor", @"individual", @"active"],
      @[@"off", @"increment", @"grid", @"vertex", @"edge", @"face", @"volume"],
      @[@"off", @"on", @"connected", @"projected"],
    ];
    NSCAssert(strip.barButtonItems.count == 4, @"Missing settings buttons");
    checkMenu((UIMenu *)strip.menuRepresentation, strip.representativeItem.menu);
    for (NSUInteger i = 0; i < 4; i++) {
      UIBarButtonItem *button = strip.barButtonItems[i];
      NSCAssert(button.customView == nil && button.image != nil, @"Not a native icon item");
      UIMenu *overflow = (UIMenu *)((UIMenu *)strip.menuRepresentation).children[i];
      checkMenu(overflow, button.menu);
      checkMenu((UIMenu *)button.menuRepresentation, button.menu);
      NSCAssert(button.menu.children.count == [values[i] count], @"Missing settings actions");
      for (NSUInteger j = 0; j < button.menu.children.count; j++) {
        UIAction *action = (UIAction *)button.menu.children[j];
        invoke(action);
        NSArray *expected = @[settings[i], values[i][j]];
        NSCAssert([queued.lastObject isEqual:expected], @"Wrong setting dispatched");
        invoke((UIAction *)overflow.children[j]);
        NSCAssert([queued.lastObject isEqual:expected], @"Wrong overflow setting dispatched");
        [builder setTopSettingsVisible:YES animated:YES];
        NSCAssert(!strip.hidden, @"Settings disappeared after dispatch");
      }
    }
    NSMutableDictionary *state = [@{@"orientation": @"global", @"pivot": @"median",
                                    @"snap": @"off", @"proportional": @"off"} mutableCopy];
    [builder setTopSettingsValues:state];
    for (NSUInteger i = 0; i < 4; i++) {
      for (NSString *value in values[i]) {
        UIMenu *unrelatedMenu = ((UIBarButtonItem *)strip.barButtonItems[(i + 1) % 4]).menu;
        state[settings[i]] = value;
        [builder setTopSettingsValues:state];
        UIBarButtonItem *button = strip.barButtonItems[i];
        NSUInteger checked = 0;
        for (UIAction *action in button.menu.children) {
          BOOL expected = [action.identifier isEqual:[NSString stringWithFormat:@"%@.%@", settings[i], value]];
          NSCAssert((action.state == UIMenuElementStateOn) == expected, @"Wrong settings checkmark");
          if (expected) {
            checked++;
            NSCAssert([button.image isEqual:action.image], @"Toolbar retained the old settings icon");
            NSCAssert([button.accessibilityValue isEqual:action.title], @"Wrong accessible setting value");
          }
          invoke(action);
          NSArray *expectedCommand = @[settings[i], [action.identifier componentsSeparatedByString:@"."].lastObject];
          NSCAssert([queued.lastObject isEqual:expectedCommand], @"Copied settings action lost dispatch");
        }
        NSCAssert(checked == 1, @"Settings checkmark missing or duplicated");
        NSCAssert(((UIBarButtonItem *)strip.barButtonItems[(i + 1) % 4]).menu == unrelatedMenu,
                  @"Updating one setting rebuilt a different menu");
        checkMenu((UIMenu *)button.menuRepresentation, button.menu);
        checkMenu((UIMenu *)strip.menuRepresentation, strip.representativeItem.menu);
        checkMenu((UIMenu *)strip.representativeItem.menu.children[i], button.menu);
        NSCAssert([strip.representativeItem.image isEqual:((UIBarButtonItem *)strip.barButtonItems[0]).image],
                  @"Collapsed settings icon does not follow orientation");
        UIMenu *menu = button.menu, *compactMenu = strip.representativeItem.menu;
        for (NSUInteger tick = 0; tick < 10; tick++) {
          NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
          [builder setTopSettingsValues:[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]];
          NSCAssert(button.menu == menu && strip.representativeItem.menu == compactMenu,
                    @"Unchanged settings snapshot replaced a menu during dismissal");
        }
      }
    }
    state[@"orientation"] = @"custom";
    state[@"snap"] = @"mixed";
    [builder setTopSettingsValues:state];
    for (NSUInteger index : {0, 2}) {
      UIBarButtonItem *button = strip.barButtonItems[index];
      NSCAssert(button.image != nil, @"Custom/multiple state has no icon");
      for (UIAction *action in button.menu.children) {
        NSCAssert(action.state == UIMenuElementStateOff, @"Unknown or combined state falsely checked a single option");
      }
    }
    UIMenu *stableSettings = strip.representativeItem.menu;
    [builder setTopSettingsValues:nil];
    [builder setTopSettingsValues:(id)@42];
    [builder setTopSettingsValues:@{@"orientation": @42, @"pivot": @"invalid"}];
    NSCAssert(strip.representativeItem.menu == stableSettings, @"Malformed snapshot changed settings UI");
    [state release];
    [builder setMeshSelectModeVisible:YES animated:NO];
    UIBarButtonItemGroup *mesh = builder->_mesh_select_group;
    NSCAssert(!mesh.hidden, @"Mesh selection did not appear");
    for (NSString *value in @[@"VERT", @"EDGE", @"FACE", @"VERT"]) {
      [builder setMeshSelectModeValue:value];
      UIMenu *menu = mesh.representativeItem.menu;
      checkMenu((UIMenu *)mesh.menuRepresentation, menu);
      NSUInteger selected = 0;
      for (NSUInteger i = 0; i < mesh.barButtonItems.count; i++) {
        UIBarButtonItem *button = mesh.barButtonItems[i];
        UIAction *action = (UIAction *)menu.children[i];
        NSCAssert(((UIAction *)button.menuRepresentation).state == action.state,
                  @"Individual overflow lost selection state");
        NSCAssert((action.state == UIMenuElementStateOn) == button.selected, @"Wrong checkmark");
        if (button.selected) {
          selected++;
          NSCAssert([button.accessibilityIdentifier isEqual:value], @"Wrong selection");
          NSCAssert(mesh.representativeItem.image == button.image, @"Wrong compact icon");
        }
        invoke(action);
        NSArray *expected = @[@"mesh_select_mode", button.accessibilityIdentifier];
        NSCAssert([queued.lastObject isEqual:expected], @"Wrong compact selection dispatch");
        invoke(button.primaryAction);
        NSCAssert([queued.lastObject isEqual:expected], @"Wrong expanded selection dispatch");
      }
      NSCAssert(selected == 1, @"Selection not exclusive");
      [builder setMeshSelectModeValue:value];
      NSCAssert(mesh.representativeItem.menu == menu, @"Unchanged snapshot rebuilt selection menu");
    }
    for (UIUserInterfaceStyle style : {UIUserInterfaceStyleLight, UIUserInterfaceStyleDark}) {
      builder.view.overrideUserInterfaceStyle = style;
      [builder setTopSettingsVisible:NO animated:NO];
      NSCAssert(strip.hidden, @"Settings did not hide");
      [builder setTopSettingsVisible:YES animated:NO];
      for (CGFloat width : {1194., 834., 669., 600., 480., 466., 375., 320., 834.}) {
        builder.view.frame = CGRectMake(0, 0, width, 800);
        [builder.view layoutIfNeeded];
        [builder->_top_bar layoutIfNeeded];
        NSCAssert(std::abs(builder->_top_bar.bounds.size.width -
                          builder.view.safeAreaLayoutGuide.layoutFrame.size.width) < 0.01,
                  @"Bar did not follow window width");
        for (CGFloat topInset : {0.0, 24.0, 48.0}) {
          builder.additionalSafeAreaInsets = UIEdgeInsetsMake(topInset, 0, 0, 0);
          [builder.view setNeedsLayout];
          [builder.view layoutIfNeeded];
          [builder->_top_bar layoutIfNeeded];
          CGRect safe = builder.view.safeAreaLayoutGuide.layoutFrame;
          NSCAssert(std::abs(CGRectGetMinY(builder->_top_bar.frame) - CGRectGetMinY(safe) - 12.0) < 0.01,
                    @"Floating iPad toolbar lost its top clearance");
          NSMutableArray *controls = [NSMutableArray array];
          collectControls(builder->_top_bar, controls);
          for (UIView *control in controls) {
            CGRect rect = [control convertRect:control.bounds toView:builder.view];
            NSCAssert(CGRectGetMinY(rect) >= CGRectGetMinY(safe) + 12.0 - 0.5,
                      @"Toolbar control is flush against the window/system area");
          }
          checkControls(builder->_top_bar);
        }
        builder.additionalSafeAreaInsets = UIEdgeInsetsZero;
        [builder.view setNeedsLayout];
        [builder.view layoutIfNeeded];
        [builder->_top_bar layoutIfNeeded];
        if (width == 320) {
          NSCAssert(item.overflowPresentationSource != nil || strip.displayingRepresentativeItem ||
                    mesh.displayingRepresentativeItem, @"Narrow bar did not collapse or overflow");
        }
        if (width == 1194) {
          NSCAssert(!strip.displayingRepresentativeItem && !mesh.displayingRepresentativeItem,
                    @"Wide bar did not expand its groups");
          NSCAssert([builder->_top_bar hitTest:CGPointMake(280, 28) withEvent:nil] == nil,
                    @"Empty toolbar space blocks viewport gestures");
        }
        checkControls(builder->_top_bar);
        if (width == 320 || width == 466 || width == 669 || width == 834 || width == 1194) {
          UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc]
              initWithSize:CGSizeMake(width, 110)] autorelease];
          UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
            [builder.view drawViewHierarchyInRect:builder.view.bounds afterScreenUpdates:YES];
          }];
          NSString *filename = [NSString stringWithFormat:@"toolbar-%@-%.0f.png",
              style == UIUserInterfaceStyleLight ? @"light" : @"dark", width];
          NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
              NSUserDomainMask, YES).firstObject;
          [UIImagePNGRepresentation(image) writeToFile:[documents stringByAppendingPathComponent:filename]
                                            atomically:YES];
        }
      }
    }
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
      builder->_action_rail = [[[UIView alloc] init] autorelease];
      builder->_action_rail.translatesAutoresizingMaskIntoConstraints = NO;
      builder->_tool_rail = [[[UIView alloc] init] autorelease];
      builder->_view_gizmo_hotspot = [[[UIView alloc] init] autorelease];
      for (UIView *view in @[builder->_action_rail, builder->_tool_rail, builder->_view_gizmo_hotspot]) {
        [builder.view addSubview:view];
      }
      [builder installActionStripLayout];
      for (CGSize size : {CGSizeMake(466, 678), CGSizeMake(678, 466),
                           CGSizeMake(669, 951), CGSizeMake(951, 669), CGSizeMake(320, 568)}) {
        for (BOOL expanded : {NO, YES}) {
          builder.view.frame = CGRectMake(0, 0, size.width, size.height);
          [builder.view layoutIfNeeded];
          CGRect safe = builder.view.safeAreaLayoutGuide.layoutFrame;
          CGFloat toolWidth = expanded ? [builder expandedToolbarWidth] : 64;
          builder->_tool_rail.frame = CGRectMake(CGRectGetMinX(safe) + 14,
              CGRectGetMaxY(safe) - 232, toolWidth, 220);
          builder->_view_gizmo_hotspot.frame = CGRectMake(CGRectGetMaxX(safe) - 118,
              CGRectGetMaxY(safe) - 124, 112, 112);
          [builder updatePhoneActionStripLayout];
          [builder.view layoutIfNeeded];
          CGRect lane = builder->_action_rail.frame;
          NSCAssert(CGRectContainsRect(safe, lane) && lane.size.width >= 44,
                    @"Phone shortcut lane clipped or unusable");
          NSCAssert(!CGRectIntersectsRect(lane, builder->_tool_rail.frame), @"Shortcuts cover tools");
          NSCAssert(!CGRectIntersectsRect(lane, builder->_view_gizmo_hotspot.frame), @"Shortcuts cover gizmo");
        }
      }
    }
    // Exercise the real phone container, including Duo's system-managed vertical bar.
    StripTest *phone = [[[StripTest alloc] init] autorelease];
    phone.edgesForExtendedLayout = UIRectEdgeAll;
    phone.extendedLayoutIncludesOpaqueBars = YES;
    GHOSTIOSNavigationController *navigation = [[[GHOSTIOSNavigationController alloc]
        initWithNavigationBarClass:GHOSTIOSNavigationBar.class toolbarClass:nil] autorelease];
    [navigation setViewControllers:@[phone] animated:NO];
    testWindow.rootViewController = navigation;
    phone.view.backgroundColor = UIColor.systemTealColor;
    phone->_overlay_view = phone.view;
    phone->_mesh_select_buttons = [NSMutableArray array];
    [phone installNativeTopBar];
    [phone setMeshSelectModeVisible:YES animated:NO];
    [phone setTopSettingsVisible:YES animated:NO];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      @try {
        [testWindow layoutIfNeeded];
        NSCAssert(phone->_top_bar_height_constraint == nil, @"Phone bar still manually sized");
        NSCAssert(navigation.navigationBar == phone->_top_bar, @"Not the system navigation bar");
        NSCAssert(CGRectEqualToRect(navigation.view.bounds, testWindow.bounds), @"Letterboxed root container");
        CGRect content = [phone.view convertRect:phone.view.bounds toView:testWindow];
        NSCAssert(CGRectEqualToRect(content, testWindow.bounds), @"Viewport does not fill the Duo display");
        BOOL landscape = [NSProcessInfo.processInfo.environment[@"TOOLBAR_ORIENTATION"] isEqual:@"landscape"];
        NSCAssert((testWindow.bounds.size.width > testWindow.bounds.size.height) == landscape,
                  @"Simulator did not enter the requested orientation");
        fprintf(stderr, "Duo window=%s content=%s safe=%s bar=%s\n",
            NSStringFromCGRect(testWindow.bounds).UTF8String,
            NSStringFromCGRect(phone.view.frame).UTF8String,
            NSStringFromUIEdgeInsets(phone.view.safeAreaInsets).UTF8String,
            NSStringFromCGRect(phone->_top_bar.frame).UTF8String);
        UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc]
            initWithSize:testWindow.bounds.size] autorelease];
        UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
          [testWindow drawViewHierarchyInRect:testWindow.bounds afterScreenUpdates:YES];
        }];
        NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
            NSUserDomainMask, YES).firstObject;
        NSString *filename = landscape ? @"duo-native-landscape.png" : @"duo-native-portrait.png";
        [UIImagePNGRepresentation(image) writeToFile:[documents stringByAppendingPathComponent:filename]
                                          atomically:YES];
        fprintf(stderr, "PASS: native toolbar, 23 setting actions, selection/overflow state, light/dark layout\n");
        exit(0);
      } @catch (NSException *error) {
        fprintf(stderr, "FAIL: %s\n", error.description.UTF8String); exit(1);
      }
    });
  }
}
@interface TestAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation TestAppDelegate
@end
@interface TestSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end
@implementation TestSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
  testWindow = [[GHOSTUIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  testWindow.rootViewController = [[[UIViewController alloc] init] autorelease];
  [testWindow makeKeyAndVisible];
  BOOL landscape = [NSProcessInfo.processInfo.environment[@"TOOLBAR_ORIENTATION"] isEqual:@"landscape"];
  UIWindowSceneGeometryPreferencesIOS *geometry = [[[UIWindowSceneGeometryPreferencesIOS alloc]
      initWithInterfaceOrientations:landscape ? UIInterfaceOrientationMaskLandscapeRight :
                                               UIInterfaceOrientationMaskPortrait] autorelease];
  [(UIWindowScene *)scene requestGeometryUpdateWithPreferences:geometry errorHandler:^(NSError *error) {
    fprintf(stderr, "FAIL: orientation %s\n", error.description.UTF8String);
  }];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    @try { runChecks(); }
    @catch (NSException *error) { fprintf(stderr, "FAIL: %s\n", error.description.UTF8String); exit(1); }
  });
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(TestAppDelegate.class));
  }
}
'''
        icon_bundle = os.environ.get("IOS_TEST_ICON_BUNDLE")
        if icon_bundle:
            for signature, stub in (
                ("static UIImage *blenderIOSIcon(",
                 'static UIImage *blenderIOSIcon(NSString *, NSString *) { return [UIImage systemImageNamed:@"cube"]; }'),
                ("static UIImage *blenderIOSCommandIcon(",
                 'static UIImage *blenderIOSCommandIcon(NSInteger, NSString *) { return [UIImage systemImageNamed:@"cube"]; }'),
            ):
                start = source.index(signature)
                harness = harness.replace(stub, source[start:source.index("\n}", start) + 2])
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "settings_test.mm"
            app = Path(directory) / "ToolbarTest.app"
            app.mkdir()
            if icon_bundle:
                shutil.copy(Path(icon_bundle) / "Assets.car", app / "Assets.car")
                shutil.copy(root / "release/ios/blender_icons.json", app / "blender_icons.json")
            executable = app / "ToolbarTest"
            bundle_id = "org.blender.tests.native-toolbar"
            (app / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": bundle_id, "CFBundleExecutable": executable.name,
                "CFBundleName": "ToolbarTest", "CFBundlePackageType": "APPL",
                "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
                "MinimumOSVersion": "26.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
                "UIStatusBarHidden": True, "UIViewControllerBasedStatusBarAppearance": False,
                "UISupportedInterfaceOrientations": ["UIInterfaceOrientationPortrait",
                    "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
                "UIApplicationSceneManifest": {"UISceneConfigurations": {
                    "UIWindowSceneSessionRoleApplication": [{
                        "UISceneConfigurationName": "Tests", "UISceneDelegateClassName": "TestSceneDelegate",
                    }],
                }},
            }))
            source_file.write_text(harness)
            result = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fblocks", "-fno-objc-arc",
                "-isysroot", sdk.stdout.strip(), "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation", "-framework", "CoreGraphics",
                str(source_file), "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run(["codesign", "--force", "--sign", "-", str(app)],
                           check=True, capture_output=True)
            subprocess.run(["xcrun", "simctl", "install", device, str(app)], check=True,
                           capture_output=True)
            try:
                result = subprocess.run(["xcrun", "simctl", "launch", "--console-pty", device, bundle_id],
                                        capture_output=True, text=True, timeout=60,
                                        env={**os.environ, "SIMCTL_CHILD_TOOLBAR_ORIENTATION":
                                             os.environ.get("IOS_TEST_ORIENTATION", "portrait")})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS: native toolbar", result.stdout + result.stderr)
            finally:
                artifacts = os.environ.get("IOS_TEST_ARTIFACT_DIR")
                if artifacts:
                    container = subprocess.check_output(
                        ["xcrun", "simctl", "get_app_container", device, bundle_id, "data"], text=True).strip()
                    shutil.copytree(Path(container) / "Documents", artifacts, dirs_exist_ok=True)
                subprocess.run(["xcrun", "simctl", "uninstall", device, bundle_id],
                               check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
