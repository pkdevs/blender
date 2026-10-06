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
        start = source.index("@interface GHOSTIOSPassThroughOverlayView")
        view_class = source[start:source.index("/* GHOSTUIWindow interface.", start)]
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
                          "- (NSString *)modeTitleForIdentifier:",
                          "- (void)setModePickerTitleForMode:",
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
                          "- (void)updateAxisConstraintsWithActions:",
                          "- (UIButton *)actionButtonWithTitle:",
                          "- (void)rebuildActionStripWithActions:",
                          "- (void)installActionStripToggle\n",
                          "- (void)updateRedoPillLayout:",
                          "- (void)toggleActionStrip:",
                          "- (void)setActionsCollapsed:",
                          "- (void)installActionStripLayout\n",
                          "- (void)updatePhoneActionStripLayout\n",
                          "- (CGFloat)expandedToolbarWidth\n",
                          "- (void)installNativeTopBar\n"):
            start = source.rindex(signature)
            methods.append(source[start:source.index("\n}", start) + 2])
        start = source.index("  _action_rail =")
        action_setup = source[start:source.index("  NSArray *view_pie_specs", start)]
        start = source.index("  NSLayoutConstraint *action_content_width =")
        action_width = source[start:source.index("  [NSLayoutConstraint activateConstraints:", start)]
        start = source.index("    [_action_scroll.leadingAnchor")
        action_constraints = source[start:source.index("\n  [self layoutViewPie];", start)]
        methods.append("- (void)installTestActionStrip\n{\n" + action_setup + action_width +
                       "  [NSLayoutConstraint activateConstraints:@[\n" + action_constraints + "\n}")
        start = source.index("    UIBarButtonItem *view =")
        view_button = source[start:source.index("    _top_bar_item.trailingItemGroups", start)]
        methods.append("- (UIBarButtonItem *)testPhoneViewButton:(NSArray *)actions\n{\n" +
                       view_button + "  return view;\n}")
        harness = r'''
#import <UIKit/UIKit.h>
#import "GHOST_IOSScenePanels.hh"
#import "GHOST_IOSRedoStrip.hh"
#define GHOST_IOSViewController StripTest
@interface GHOSTIOSRedoStrip (Testing)
- (void)dismiss:(id)sender;
- (void)expand:(id)sender;
@end
#import <QuartzCore/QuartzCore.h>
#include <cmath>
#include <initializer_list>
#define UNUSED_VARS(...) ((void)0)
static NSMutableArray *queued;
static NSInteger lastCommand = -1;
static UIImage *blenderIOSIcon(NSString *, NSString *) { return [UIImage systemImageNamed:@"cube"]; }
static UIImage *blenderIOSCommandIcon(NSInteger, NSString *) { return [UIImage systemImageNamed:@"cube"]; }
static UIImage *blenderIOSActionIcon(NSString *, NSString *, NSString *) { return nil; }
extern "C" void WM_apple_ios_ui_command_enqueue(int command) { lastCommand = command; }
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
  GHOSTIOSScenePanels *_scene_panels;
  GHOSTIOSRedoStrip *_redo_strip;
  GHOSTIOSNavigationBar *_top_bar;
  UINavigationItem *_top_bar_item;
  NSLayoutConstraint *_top_bar_height_constraint;
  UIBarButtonItem *_mode_button, *_actions_button, *_viewport_shading_button, *_undo_button;
  UIBarButtonItem *_finger_input_button;
  BOOL _finger_tool_enabled;
  UIView *_tool_rail, *_view_gizmo_hotspot;
  GHOSTIOSPassThroughVisualEffectView *_action_rail;
  UIScrollView *_action_scroll;
  UIStackView *_action_stack;
  UIView *_action_group;
  UIButton *_action_toggle;
  NSLayoutConstraint *_action_expanded_width_constraint, *_action_collapsed_width_constraint;
  NSLayoutConstraint *_action_toggle_width_constraint, *_action_toggle_trailing_constraint;
  NSLayoutConstraint *_action_scroll_trailing_constraint, *_redo_pill_width_constraint;
  NSLayoutConstraint *_redo_pill_min_width_constraint;
  BOOL _actions_collapsed;
  NSUInteger _action_transition_generation;
  NSLayoutConstraint *_action_phone_bottom_constraint, *_action_phone_trailing_constraint;
  UIBarButtonItemGroup *_top_settings_group, *_mesh_select_group;
  UISegmentedControl *_mesh_select_control;
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
    builder->_scene_panels = [[[GHOSTIOSScenePanels alloc] initWithOverlay:builder.view
        icon:^UIImage *(NSString *) { return nil; }] autorelease];
    builder->_mesh_select_buttons = [NSMutableArray array];
    [builder installNativeTopBar];
    [testWindow makeKeyAndVisible];
    [UIView setAnimationsEnabled:NO];
    UINavigationItem *item = builder->_top_bar_item;
    UIAction *viewAction = [builder viewportShadingActionWithTitle:@"Frame Selected"
        systemImage:@"viewfinder" command:GHOST_IOS_UI_COMMAND_FRAME_SELECTED];
    UIBarButtonItem *viewButton = [builder testPhoneViewButton:@[viewAction]];
    NSCAssert(viewButton.image.isSymbolImage &&
              [viewButton.image isEqual:[UIImage systemImageNamed:@"eye"]],
              @"Phone View button does not use the native eye symbol");
    NSCAssert([viewButton.accessibilityLabel isEqual:@"View"] &&
              [viewButton.menu.title isEqual:@"View"] &&
              [viewButton.menu.children.firstObject.title isEqual:viewAction.title],
              @"Changing the View icon changed its menu or accessible name");
    checkMenu((UIMenu *)viewButton.menuRepresentation, viewButton.menu);
    invoke((UIAction *)viewButton.menu.children.firstObject);
    NSCAssert(lastCommand == GHOST_IOS_UI_COMMAND_FRAME_SELECTED, @"View command dispatch changed");
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
    NSArray *modeIdentifiers = @[@"objectmode", @"mesh_edit", @"sculpt_mode",
                                @"vertexpaint", @"weightpaint", @"imagepaint"];
    const NSInteger modeCommands[] = {GHOST_IOS_UI_COMMAND_SET_MODE_OBJECT,
        GHOST_IOS_UI_COMMAND_SET_MODE_EDIT, GHOST_IOS_UI_COMMAND_SET_MODE_SCULPT,
        GHOST_IOS_UI_COMMAND_SET_MODE_VERTEX_PAINT, GHOST_IOS_UI_COMMAND_SET_MODE_WEIGHT_PAINT,
        GHOST_IOS_UI_COMMAND_SET_MODE_TEXTURE_PAINT};
    UIMenu *modeMenu = builder->_mode_button.menu;
    NSCAssert(builder->_mode_button.image == ((UIAction *)modeMenu.children.firstObject).image,
              @"Initial mode picker does not use the Object Mode menu icon");
    for (NSUInteger i = 0; i < modeIdentifiers.count; i++) {
      UIAction *action = (UIAction *)modeMenu.children[i];
      [builder setModePickerTitleForMode:modeIdentifiers[i]];
      NSCAssert(action.image != nil && builder->_mode_button.image == action.image &&
                [builder->_mode_button.title isEqual:action.title] &&
                [builder->_mode_button.accessibilityValue isEqual:action.title],
                @"Mode snapshot did not synchronize the menu icon and accessible name");
      [builder setModePickerTitleForMode:@"objectmode"];
      invoke(action);
      NSCAssert(builder->_mode_button.image == action.image && lastCommand == modeCommands[i],
                @"Mode menu tap did not update the icon immediately and dispatch its command");
    }
    for (NSString *alias in @[@"curve_edit", @"armature_edit", @"edit_mesh", @"sculpt", @"texturepaint"]) {
      [builder setModePickerTitleForMode:alias];
      for (UIAction *action in modeMenu.children) {
        if ([action.title isEqual:builder->_mode_button.title]) {
          NSCAssert(builder->_mode_button.image == action.image, @"Mode alias lost its Blender icon");
        }
      }
    }
    [builder setModePickerTitleForMode:@"posemode"];
    NSCAssert(builder->_mode_button.image != nil &&
              [builder->_mode_button.accessibilityValue isEqual:@"Pose Mode"], @"Pose icon missing");
    [builder setModePickerTitleForMode:@"objectmode"];
    NSCAssert(builder->_mode_button.menu == modeMenu, @"Mode updates rebuilt the open menu");
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
      NSCAssert(builder->_finger_input_button != nil && fingerTool, @"Phone tool input missing");
      invoke((UIAction *)builder->_finger_input_button.menu.children[1]);
      NSCAssert(!fingerTool, @"Orbit mode not routed to input window");
      invoke((UIAction *)builder->_finger_input_button.menu.children[0]);
      NSCAssert(fingerTool, @"Tool mode not restored");
    }
    NSArray *leading = @[builder->_mode_button, builder->_mesh_select_group.barButtonItems.firstObject,
                        builder->_actions_button, builder->_undo_button];
    for (NSUInteger i = 0; i < leading.count; i++) {
      NSCAssert(item.leadingItemGroups[i].barButtonItems.firstObject == leading[i],
                @"Mesh selection must immediately follow Mode, before Actions and Undo");
    }
    NSCAssert(item.pinnedTrailingGroup.barButtonItems.firstObject ==
              builder->_viewport_shading_button, @"Shading must stay on the trailing side");
    NSArray *panelButtons = @[builder->_scene_panels.hierarchyButton, builder->_scene_panels.inspectorButton];
    NSCAssert(item.trailingItemGroups.count == 1 &&
              [item.trailingItemGroups[0].barButtonItems isEqual:panelButtons] &&
              !builder->_viewport_shading_button.sharesBackground,
              @"Panel toggles must share a separate trailing group");
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
    UISegmentedControl *segments = builder->_mesh_select_control;
    NSCAssert(mesh.barButtonItems.count == 1 && mesh.barButtonItems.firstObject.customView == segments &&
              segments.numberOfSegments == 3 && !segments.momentary,
              @"Mesh selector is not one persistent native segmented control");
    for (NSString *value in @[@"VERT", @"EDGE", @"FACE", @"VERT"]) {
      [builder setMeshSelectModeValue:value];
      UIMenu *menu = mesh.representativeItem.menu;
      checkMenu((UIMenu *)mesh.menuRepresentation, menu);
      NSUInteger selected = 0;
      for (NSUInteger i = 0; i < builder->_mesh_select_buttons.count; i++) {
        UIBarButtonItem *button = builder->_mesh_select_buttons[i];
        UIAction *action = (UIAction *)menu.children[i];
        NSCAssert(((UIAction *)button.menuRepresentation).state == action.state,
                  @"Individual overflow lost selection state");
        NSCAssert((action.state == UIMenuElementStateOn) == button.selected, @"Wrong checkmark");
        if (button.selected) {
          selected++;
          NSCAssert([button.accessibilityIdentifier isEqual:value], @"Wrong selection");
          NSCAssert(mesh.representativeItem.image == button.image, @"Wrong compact icon");
        }
      }
      NSCAssert(selected == 1, @"Selection not exclusive");
      NSCAssert((segments.selectedSegmentIndex == [@[@"VERT", @"EDGE", @"FACE"] indexOfObject:value]),
                @"Native indicator did not follow Blender's state");
      [builder setMeshSelectModeValue:value];
      NSCAssert(mesh.representativeItem.menu == menu, @"Unchanged snapshot rebuilt selection menu");
    }
    for (NSUInteger i = 0; i < 3; i++) {
      UIAction *action = [segments actionForSegmentAtIndex:i];
      NSCAssert(action.image != nil && action.title.length > 0, @"Segment lost icon or accessible name");
      invoke(action);
      NSCAssert(segments.selectedSegmentIndex == i, @"Selection did not update immediately");
      NSCAssert(([queued.lastObject isEqual:@[@"mesh_select_mode", action.identifier]]),
                @"Native segment did not dispatch the correct mode");
      invoke((UIAction *)mesh.representativeItem.menu.children[(i + 1) % 3]);
      NSCAssert(segments.selectedSegmentIndex == (i + 1) % 3, @"Overflow selection not synchronized");
    }
    NSInteger lastSegment = segments.selectedSegmentIndex;
    [builder setMeshSelectModeValue:@""];
    NSCAssert(segments.selectedSegmentIndex == lastSegment, @"Invalid snapshot cleared selection");
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
          NSMutableArray *controls = [NSMutableArray array];
          collectControls(builder->_top_bar, controls);
          BOOL checkedEmptySpace = NO;
          for (CGFloat x = 24; x < width - 24; x += 12) {
            CGPoint point = CGPointMake(x, 28);
            BOOL nearControl = NO;
            for (UIView *control in controls) {
              CGRect rect = [control convertRect:control.bounds toView:builder->_top_bar];
              nearControl |= CGRectContainsPoint(CGRectInset(rect, -24, -24), point);
            }
            if (!nearControl) {
              NSCAssert([builder->_top_bar hitTest:point withEvent:nil] == nil,
                        @"Empty toolbar space blocks viewport gestures");
              checkedEmptySpace = YES;
            }
          }
          NSCAssert(checkedEmptySpace, @"Wide toolbar has no usable viewport gesture gap");
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
    {
      builder->_tool_rail = [[[UIView alloc] init] autorelease];
      builder->_view_gizmo_hotspot = [[[UIView alloc] init] autorelease];
      for (UIView *view in @[builder->_tool_rail, builder->_view_gizmo_hotspot]) {
        [builder.view addSubview:view];
      }
      builder->_redo_strip = [[[GHOSTIOSRedoStrip alloc] initWithOverlay:builder.view
          icon:^UIImage *(NSString *) { return nil; }] autorelease];
      [builder installTestActionStrip];
      NSMutableArray *actions = [NSMutableArray array];
      for (NSUInteger i = 0; i < 12; i++) {
        [actions addObject:@{@"title": [NSString stringWithFormat:@"Action %lu", i],
            @"symbol": @"", @"operator": @"MESH_OT_select_all"}];
      }
      [builder rebuildActionStripWithActions:actions];
      for (CGSize size : {CGSizeMake(466, 678), CGSizeMake(678, 466),
                           CGSizeMake(669, 951), CGSizeMake(951, 669), CGSizeMake(320, 568),
                           CGSizeMake(599, 800), CGSizeMake(600, 800), CGSizeMake(601, 800)}) {
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
          NSCAssert(std::abs(CGRectGetMaxY(safe) - CGRectGetMaxY(lane) - 6.0) < 0.01,
                    @"Phone shortcut lane jumped away from the bottom edge");
          NSCAssert(CGRectContainsRect(safe, lane) && lane.size.width >= 44,
                    @"Phone shortcut lane clipped or unusable");
          NSCAssert(!CGRectIntersectsRect(lane, builder->_tool_rail.frame), @"Shortcuts cover tools");
          NSCAssert(!CGRectIntersectsRect(lane, builder->_view_gizmo_hotspot.frame), @"Shortcuts cover gizmo");
          for (BOOL collapsed : {YES, NO, YES, NO}) {
            [builder setActionsCollapsed:collapsed animated:NO];
            [builder.view layoutIfNeeded];
            UIButton *toggle = builder->_action_toggle;
            CGRect toggleFrame = [toggle convertRect:toggle.bounds toView:builder.view];
            NSCAssert(CGRectContainsRect(lane, toggleFrame) && toggleFrame.size.width >= 44,
                      @"Quick actions toggle clipped or too small to tap");
            NSCAssert(builder->_action_scroll.userInteractionEnabled == !collapsed &&
                      builder->_action_scroll.accessibilityElementsHidden == collapsed,
                      @"Hidden quick actions can still receive input or accessibility focus");
            if (collapsed) {
              NSCAssert([toggle.configuration.title isEqual:@"Actions"] &&
                        toggle.configuration.image == nil && toggleFrame.size.width <= 97,
                        @"Collapsed actions are not a compact text-only pill");
              [builder rebuildActionStripWithActions:actions];
              NSCAssert(builder->_actions_collapsed && builder->_action_scroll.alpha == 0,
                        @"Blender snapshot reopened the collapsed actions");
              CGRect pill = [toggle convertRect:toggle.bounds toView:builder->_action_rail];
              for (CGFloat x = 4; x < lane.size.width - 4; x += 8) {
                CGPoint empty = CGPointMake(x, 28);
                if (!CGRectContainsPoint(CGRectInset(pill, -2, -2), empty)) {
                  NSCAssert([builder->_action_rail hitTest:empty withEvent:nil] == nil,
                            @"Collapsed row blocks viewport input outside its pill");
                }
              }
            } else {
              NSCAssert(toggle.configuration.image != nil && toggle.configuration.title == nil,
                        @"Expanded actions lost the close icon");
              NSCAssert(builder->_action_scroll.bounds.size.width >= 44,
                        @"Close control consumed the scrolling lane");
              [builder->_action_scroll setContentOffset:CGPointMake(80, 0) animated:NO];
              [builder->_action_scroll layoutIfNeeded];
              CAGradientLayer *fade = (CAGradientLayer *)builder->_action_scroll.layer.mask;
              NSCAssert([fade isKindOfClass:CAGradientLayer.class] &&
                        CGRectEqualToRect(fade.frame, builder->_action_scroll.bounds),
                        @"Action fade is not attached to the visible scrolling window");
              NSCAssert(CGColorGetAlpha((CGColorRef)fade.colors.firstObject) == 0 &&
                        CGColorGetAlpha((CGColorRef)fade.colors.lastObject) == 0,
                        @"Overflowing actions do not fade at both scrolling edges");
              NSCAssert(CGRectEqualToRect(toggleFrame,
                  [toggle convertRect:toggle.bounds toView:builder.view]), @"Close control scrolled away");
            }
          }
          if (size.width == 466 || size.width == 951) {
            for (BOOL collapsed : {YES, NO}) {
              [builder setActionsCollapsed:collapsed animated:NO];
              UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc]
                  initWithBounds:CGRectMake(0, size.height - 180, size.width, 180)] autorelease];
              UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
                [builder.view drawViewHierarchyInRect:builder.view.bounds afterScreenUpdates:YES];
              }];
              NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                  NSUserDomainMask, YES).firstObject;
              NSString *filename = [NSString stringWithFormat:@"quick-actions-%.0f-%d-%d.png",
                  size.width, expanded, collapsed];
              [UIImagePNGRepresentation(image) writeToFile:[documents stringByAppendingPathComponent:filename]
                                                atomically:YES];
            }
          }
        }
      }
      [builder setActionsCollapsed:YES animated:NO];
      [builder rebuildActionStripWithActions:@[]];
      NSCAssert(builder->_action_rail.hidden, @"Empty actions left an empty floating control");
      [builder rebuildActionStripWithActions:actions];
      NSCAssert(!builder->_action_rail.hidden && builder->_actions_collapsed,
                @"Action availability did not preserve the collapsed state");
      [builder->_action_toggle sendActionsForControlEvents:UIControlEventTouchUpInside];
      NSCAssert(!builder->_actions_collapsed, @"Tapping Actions did not expand the row");
      for (BOOL atEnd : {NO, YES}) {
        UIScrollView *scroll = builder->_action_scroll;
        [scroll setContentOffset:CGPointMake(atEnd ? scroll.contentSize.width - scroll.bounds.size.width : 0, 0)
                        animated:NO];
        [scroll layoutIfNeeded];
        CAGradientLayer *fade = (CAGradientLayer *)scroll.layer.mask;
        NSCAssert(CGColorGetAlpha((CGColorRef)fade.colors.firstObject) == (atEnd ? 0 : 1) &&
                  CGColorGetAlpha((CGColorRef)fade.colors.lastObject) == (atEnd ? 1 : 0),
                  @"A scroll boundary fades its fully visible action");
      }
      [builder rebuildActionStripWithActions:[actions subarrayWithRange:NSMakeRange(0, 4)]];
      for (CGFloat width : {834.0, 1194.0}) {
        builder.view.frame = CGRectMake(0, 0, width, 800);
        [builder.view layoutIfNeeded];
        CGRect safe = builder.view.safeAreaLayoutGuide.layoutFrame;
        builder->_tool_rail.frame = CGRectMake(CGRectGetMinX(safe) + 14,
            CGRectGetMaxY(safe) - 76, 64, 64);
        builder->_view_gizmo_hotspot.frame = CGRectMake(CGRectGetMaxX(safe) - 118,
            CGRectGetMaxY(safe) - 124, 112, 112);
        [builder updatePhoneActionStripLayout];
        for (BOOL collapsed : {YES, NO}) {
          [builder setActionsCollapsed:collapsed animated:NO];
          [builder.view layoutIfNeeded];
          CGRect group = [builder->_action_group convertRect:builder->_action_group.bounds
                                                     toView:builder.view];
          NSCAssert(std::abs(CGRectGetMidX(group) - CGRectGetMidX(builder.view.bounds)) < 0.5,
                    @"Quick actions are centered in the leftover lane rather than the viewport");
        }
      }
      NSDictionary *redo = @{@"id": @"redo-layout", @"title": @"Duplicate Linked", @"ack": @0,
          @"properties": @[@{@"key": @"distance", @"type": @"FLOAT", @"label": @"Distance",
                              @"value": @1, @"editable": @YES}]};
      [builder->_redo_strip updateSnapshot:redo];
      [builder->_redo_strip dismiss:nil];
      for (CGFloat width : {393.0, 466.0, 744.0, 1194.0}) {
        builder.view.frame = CGRectMake(0, 0, width, 800);
        [builder.view layoutIfNeeded];
        CGRect safe = builder.view.safeAreaLayoutGuide.layoutFrame;
        builder->_tool_rail.frame = CGRectMake(CGRectGetMinX(safe) + 14, CGRectGetMaxY(safe) - 76, 64, 64);
        builder->_view_gizmo_hotspot.frame = CGRectMake(CGRectGetMaxX(safe) - 118, CGRectGetMaxY(safe) - 124, 112, 112);
        [builder updatePhoneActionStripLayout];
        [builder.view layoutIfNeeded];
        [builder updateRedoPillLayout:NO];
        for (BOOL collapsed : {YES, NO}) {
          [builder setActionsCollapsed:collapsed animated:NO];
          [builder.view layoutIfNeeded];
          UIButton *pill = builder->_redo_strip.collapsedButton;
          CGRect pillFrame = [pill convertRect:pill.bounds toView:builder.view];
          CGRect toggle = [builder->_action_toggle convertRect:builder->_action_toggle.bounds toView:builder.view];
          CGRect scroll = [builder->_action_scroll convertRect:builder->_action_scroll.bounds toView:builder.view];
          NSCAssert(CGRectContainsRect(builder->_action_rail.frame, pillFrame) && pillFrame.size.width >= 44,
                    @"Redo pill is clipped or too small to tap");
          NSCAssert(CGRectGetMaxX(toggle) <= CGRectGetMinX(pillFrame) - 7 &&
                    CGRectGetMaxX(scroll) <= CGRectGetMinX(pillFrame) - 7,
                    @"Redo pill overlaps Actions toggle or scrolling controls");
          NSCAssert(std::abs(CGRectGetMidY(toggle) - CGRectGetMidY(pillFrame)) < 0.5,
                    @"Collapsed pills do not share a baseline");
          NSCAssert(pill.titleLabel.frame.size.height <= 28 &&
                    builder->_action_toggle.titleLabel.frame.size.height <= 28,
                    @"Compact pill labels wrapped outside their controls");
          UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc]
              initWithSize:builder.view.bounds.size] autorelease];
          UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
            [builder.view drawViewHierarchyInRect:builder.view.bounds afterScreenUpdates:YES];
          }];
          NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
              NSUserDomainMask, YES).firstObject;
          NSString *filename = [NSString stringWithFormat:@"redo-pill-%.0f-%@.png", width,
              collapsed ? @"collapsed" : @"expanded"];
          [UIImagePNGRepresentation(image) writeToFile:[documents stringByAppendingPathComponent:filename]
                                            atomically:YES];
        }
      }
      [builder->_redo_strip expand:nil];
      NSCAssert(builder->_redo_pill_width_constraint.constant == 0, @"Reopening did not release reserved Actions space");
      [builder->_redo_strip updateSnapshot:nil];
      [builder rebuildActionStripWithActions:[actions subarrayWithRange:NSMakeRange(0, 1)]];
      [builder.view layoutIfNeeded];
      [builder->_action_scroll layoutIfNeeded];
      CAGradientLayer *fade = (CAGradientLayer *)builder->_action_scroll.layer.mask;
      NSCAssert(CGColorGetAlpha((CGColorRef)fade.colors.firstObject) == 1 &&
                CGColorGetAlpha((CGColorRef)fade.colors.lastObject) == 1,
                @"Actions that fit are needlessly faded");
      [builder->_action_toggle sendActionsForControlEvents:UIControlEventTouchUpInside];
      NSCAssert(builder->_actions_collapsed, @"Tapping the close icon did not collapse the row");
      [UIView setAnimationsEnabled:YES];
      [builder setActionsCollapsed:NO animated:YES];
      [builder setActionsCollapsed:YES animated:YES];
      [builder rebuildActionStripWithActions:[actions subarrayWithRange:NSMakeRange(0, 4)]];
      [builder setActionsCollapsed:NO animated:YES];
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
    phone->_scene_panels = [[[GHOSTIOSScenePanels alloc] initWithOverlay:phone.view
        icon:^UIImage *(NSString *) { return nil; }] autorelease];
    phone->_mesh_select_buttons = [NSMutableArray array];
    [phone installNativeTopBar];
    [phone setMeshSelectModeVisible:YES animated:NO];
    [phone setTopSettingsVisible:YES animated:NO];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      @try {
        NSCAssert(!builder->_actions_collapsed && !builder->_action_scroll.hidden &&
                  builder->_action_scroll.alpha == 1.0 && builder->_action_scroll.userInteractionEnabled,
                  @"Interrupted collapse completion hid the reopened quick actions");
        [testWindow layoutIfNeeded];
        NSCAssert(phone->_top_bar_height_constraint == nil, @"Phone bar still manually sized");
        NSCAssert(navigation.navigationBar == phone->_top_bar, @"Not the system navigation bar");
        NSCAssert(CGRectEqualToRect(navigation.view.bounds, testWindow.bounds), @"Letterboxed root container");
        CGRect content = [phone.view convertRect:phone.view.bounds toView:testWindow];
        NSCAssert(CGRectEqualToRect(content, testWindow.bounds), @"Viewport does not fill the Duo display");
        BOOL landscape = [NSProcessInfo.processInfo.environment[@"TOOLBAR_ORIENTATION"] isEqual:@"landscape"];
        if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
          NSCAssert((testWindow.bounds.size.width > testWindow.bounds.size.height) == landscape,
                    @"Simulator did not enter the requested orientation");
        }
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
  if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
    [(UIWindowScene *)scene requestGeometryUpdateWithPreferences:geometry errorHandler:^(NSError *error) {
      fprintf(stderr, "FAIL: orientation %s\n", error.description.UTF8String);
    }];
  }
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
                "-framework", "QuartzCore",
                "-I", str(root / "intern/ghost/intern"),
                str(root / "intern/ghost/intern/GHOST_IOSScenePanels.mm"),
                str(root / "intern/ghost/intern/GHOST_IOSRedoStrip.mm"),
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
