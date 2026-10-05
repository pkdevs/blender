# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise production toolbar geometry and gesture ownership with UIKit."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class NativeToolbarScrollTest(unittest.TestCase):
    def test_scroll_and_handle_have_separate_gestures(self):
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
        classes = source[start:source.index("/* GHOSTUIWindow interface.", start)]
        start = source.index("  _tool_rail_glass_effect = [[UIGlassEffect")
        setup = source[start:source.index("\n  _action_rail =", start)]
        start = source.index("    [_tool_rail.leadingAnchor constraintEqualToAnchor:")
        constraints = source[start:source.index("\n    [_action_scroll.leadingAnchor", start)]
        signatures = (
            "- (UIButton *)toolbarButtonWithTitle:", "- (BOOL)isToolbarListVisible\n",
            "- (void)setActiveToolbarToolIdentifier:", "- (CGFloat)expandedToolbarHeight\n",
            "- (void)updateToolbarHeightForCurrentState\n", "- (CGFloat)expandedToolbarWidth\n",
            "- (CGFloat)toolRailBottomDockCenterY\n", "- (CGFloat)toolRailCenterDockCenterY\n",
            "- (void)setToolRailDockedLeftCenter:", "- (void)handleToolbarDockPan:",
            "- (BOOL)gestureRecognizerShouldBegin:", "- (UIButton *)toolbarDragHandle\n",
            "- (void)collapseToolbarFromHandle:", "- (void)handleToolbarHandlePan:",
            "- (void)updateToolbarVisibilityAnimated:", "- (void)setToolbarExpanded:",
            "- (void)handleToolbarToolButton:", "- (UIButton *)toolbarButtonAtGestureLocation:",
            "- (void)setToolbarHoverButton:", "- (void)finishToolbarPressSelectWithButton:",
            "- (void)handleToolbarPressSelectGesture:",
        )
        methods = []
        for signature in signatures:
            start = source.rindex(signature)
            methods.append(source[start:source.index("\n}", start) + 2])
        harness = r'''
#import <UIKit/UIKit.h>
#include <cmath>
#include <initializer_list>
#define UNUSED_VARS(...) ((void)0)
static int toolCalls;
static UIImage *blenderIOSIcon(NSString *, NSString *) { return [UIImage systemImageNamed:@"cube"]; }
static void WM_apple_ios_ui_tool_set_by_id(const char *) { toolCalls++; }
''' + classes + r'''
@interface CountingOverlay : GHOSTIOSPassThroughOverlayView
@property(nonatomic) NSUInteger layoutRequests;
@end
@implementation CountingOverlay
- (void)layoutIfNeeded {
  self.layoutRequests++;
  [super layoutIfNeeded];
}
@end
@interface CountingGlass : GHOSTIOSPassThroughVisualEffectView
@property(nonatomic) NSUInteger effectAssignments;
@end
@implementation CountingGlass
- (void)setEffect:(UIVisualEffect *)effect {
  self.effectAssignments++;
  [super setEffect:effect];
}
@end
@interface TestPan : UIPanGestureRecognizer
@property(nonatomic) CGPoint motion;
@property(nonatomic) CGPoint speed;
@property(nonatomic) UIGestureRecognizerState testState;
@end
@implementation TestPan
- (CGPoint)translationInView:(UIView *)view { return self.motion; }
- (CGPoint)velocityInView:(UIView *)view { return self.speed; }
- (UIGestureRecognizerState)state { return self.testState; }
@end
@interface TestPress : UILongPressGestureRecognizer
@property(nonatomic) UIGestureRecognizerState testState;
@end
@implementation TestPress
- (UIGestureRecognizerState)state { return self.testState; }
@end
@interface ToolbarTest : UIViewController <UIGestureRecognizerDelegate> {
@public
  GHOSTIOSPassThroughOverlayView *_overlay_view;
  UINavigationBar *_top_bar;
  GHOSTIOSPassThroughVisualEffectView *_tool_rail;
  UIGlassEffect *_tool_rail_glass_effect;
  UIScrollView *_tool_scroll;
  UIStackView *_tool_stack;
  UIButton *_tool_drag_handle, *_toolbar_hover_button;
  NSLayoutConstraint *_tool_drag_handle_height_constraint;
  NSLayoutConstraint *_tool_rail_width_constraint, *_tool_rail_height_constraint;
  NSLayoutConstraint *_tool_rail_bottom_constraint, *_tool_rail_center_y_constraint;
  NSMutableArray *_toolbar_buttons;
  NSString *_active_tool_identifier;
  BOOL _toolbar_expanded, _tool_rail_docked_left_center, _toolbar_press_select_active;
}
@end
@implementation ToolbarTest
- (instancetype)init {
  self = [super init];
  self.view = [[[CountingOverlay alloc] initWithFrame:CGRectMake(0, 0, 834, 1210)] autorelease];
  _overlay_view = (GHOSTIOSPassThroughOverlayView *)self.view;
  _top_bar = [[[UINavigationBar alloc] initWithFrame:CGRectMake(0, 0, 834, 68)] autorelease];
  _top_bar.autoresizingMask = UIViewAutoresizingFlexibleWidth;
  [_overlay_view addSubview:_top_bar];
  _toolbar_buttons = [[NSMutableArray alloc] init];
''' + setup.replace("[GHOSTIOSPassThroughVisualEffectView alloc]", "[CountingGlass alloc]") + r'''
  UILayoutGuide *safe_area = _overlay_view.safeAreaLayoutGuide;
  _tool_rail_width_constraint = [_tool_rail.widthAnchor constraintEqualToConstant:64];
  _tool_rail_height_constraint = [_tool_rail.heightAnchor constraintEqualToConstant:64];
  _tool_rail_bottom_constraint = [_tool_rail.bottomAnchor constraintEqualToAnchor:safe_area.bottomAnchor constant:-12];
  _tool_rail_center_y_constraint = [_tool_rail.centerYAnchor constraintEqualToAnchor:safe_area.centerYAnchor];
  [NSLayoutConstraint activateConstraints:@[
''' + constraints + r'''
  ]];
  for (int i = 0; i < 35; i++) {
    UIButton *button = [self toolbarButtonWithTitle:[NSString stringWithFormat:@"Tool %d", i]
        systemImage:@"cube" toolIdentifier:[NSString stringWithFormat:@"tool.%d", i] mode:@"mesh_edit"];
    [_tool_stack addArrangedSubview:button];
  }
  [self setActiveToolbarToolIdentifier:@"tool.0"];
  return self;
}
''' + "\n".join(methods) + r'''
@end
int main() {
  @autoreleasepool {
    [UIView setAnimationsEnabled:NO];
    ToolbarTest *controller = [[[ToolbarTest alloc] init] autorelease];
    CountingOverlay *overlay = (CountingOverlay *)controller.view;
    CountingGlass *glass = (CountingGlass *)controller->_tool_rail;
    NSCAssert(glass.effect != nil, @"Tool glass missing at installation");
    NSUInteger layouts = overlay.layoutRequests, effects = glass.effectAssignments;
    NSString *active = controller->_active_tool_identifier;
    for (NSUInteger tick = 0; tick < 100; tick++) {
      NSData *data = [NSJSONSerialization dataWithJSONObject:@{@"active": active} options:0 error:nil];
      NSDictionary *model = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
      [controller setActiveToolbarToolIdentifier:model[@"active"]];
      NSCAssert(overlay.layoutRequests == layouts, @"Unchanged toolbar poll interrupted native menu layout");
      [controller updateToolbarHeightForCurrentState];
      NSCAssert(glass.effectAssignments == effects, @"Layout recreated the glass effect");
    }
    [controller setActiveToolbarToolIdentifier:@"tool.1"];
    NSCAssert(overlay.layoutRequests > layouts, @"Changed tool did not refresh its layout");
    NSCAssert(!((UIButton *)controller->_toolbar_buttons[1]).hidden &&
              ((UIButton *)controller->_toolbar_buttons[0]).hidden, @"Changed tool not displayed");
    [controller setActiveToolbarToolIdentifier:@"tool.0"];
    TestPan *railPan = [[[TestPan alloc] init] autorelease];
    [controller->_tool_rail addGestureRecognizer:railPan];
    railPan.speed = CGPointMake(0, -700);
    NSCAssert([controller gestureRecognizerShouldBegin:railPan], @"Compact drag must still expand");
    NSCAssert(!controller->_tool_scroll.scrollEnabled, @"Compact tool button must not scroll");
    railPan.testState = UIGestureRecognizerStateEnded;
    [controller handleToolbarDockPan:railPan];
    NSCAssert([controller isToolbarListVisible], @"Upward compact drag did not expand");
    TestPan *handlePan = [[[TestPan alloc] init] autorelease];
    [controller->_tool_drag_handle addGestureRecognizer:handlePan];
    TestPress *press = [[[TestPress alloc] init] autorelease];
    [controller->_toolbar_buttons[0] addGestureRecognizer:press];
    for (BOOL labels : {NO, YES}) {
      [controller setToolbarExpanded:labels animated:NO];
      for (CGSize size : {CGSizeMake(834, 1210), CGSizeMake(1210, 834), CGSizeMake(520, 650),
                          CGSizeMake(466, 678), CGSizeMake(678, 466),
                          CGSizeMake(669, 951), CGSizeMake(951, 669)}) {
        controller.view.frame = CGRectMake(0, 0, size.width, size.height);
        [controller updateToolbarHeightForCurrentState];
        [controller.view setNeedsLayout];
        [controller.view layoutIfNeeded];
        [controller->_tool_rail setNeedsLayout];
        [controller->_tool_rail layoutIfNeeded];
        [controller->_tool_rail.contentView setNeedsLayout];
        [controller->_tool_rail.contentView layoutIfNeeded];
        NSCAssert(!controller->_tool_drag_handle.hidden, @"Missing list handle");
        NSCAssert(std::abs(controller->_tool_drag_handle.bounds.size.height - 44) < 0.01,
                  @"Handle hit area too small: %@, host %@, constraint %@", NSStringFromCGRect(controller->_tool_drag_handle.frame),
                  NSStringFromCGSize(size), controller->_tool_drag_handle_height_constraint);
        NSCAssert(std::abs(controller->_tool_scroll.frame.origin.y - 44) < 0.01, @"Handle overlaps scroll content");
        NSCAssert(controller->_tool_scroll.contentSize.height > controller->_tool_scroll.bounds.size.height,
                  @"Fixture must exercise overflowing tools");
        NSCAssert([controller->_tool_scroll touchesShouldCancelInContentView:controller->_toolbar_buttons[0]],
                  @"Tool buttons prevent scrolling");
        NSCAssert(![controller gestureRecognizerShouldBegin:railPan], @"List drag can move the whole toolbar");
        NSCAssert(![controller gestureRecognizerShouldBegin:press], @"Slow scroll can become tool picking");
        CGRect handleFrame = controller->_tool_drag_handle.frame;
        for (CGFloat offset : {100.0, 500.0, 0.0}) {
          [controller->_tool_scroll setContentOffset:CGPointMake(0, offset) animated:NO];
          [controller.view layoutIfNeeded];
          NSCAssert(CGRectEqualToRect(handleFrame, controller->_tool_drag_handle.frame), @"Handle scrolled away");
          NSCAssert([controller isToolbarListVisible] && toolCalls == 0, @"Scrolling changed tools or collapsed");
        }
      }
    }
    press.testState = UIGestureRecognizerStateCancelled;
    [controller handleToolbarPressSelectGesture:press];
    NSCAssert([controller isToolbarListVisible], @"Inactive picker cancellation collapsed list");
    handlePan.speed = CGPointMake(600, 100);
    NSCAssert(![controller gestureRecognizerShouldBegin:handlePan], @"Horizontal handle motion should not collapse");
    handlePan.speed = CGPointMake(0, 500);
    NSCAssert([controller gestureRecognizerShouldBegin:handlePan], @"Handle should accept downward drag");
    handlePan.motion = CGPointMake(0, 80);
    handlePan.testState = UIGestureRecognizerStateChanged;
    [controller handleToolbarHandlePan:handlePan];
    handlePan.testState = UIGestureRecognizerStateCancelled;
    [controller handleToolbarHandlePan:handlePan];
    NSCAssert([controller isToolbarListVisible] && CGAffineTransformIsIdentity(controller->_tool_rail.transform),
              @"Cancelled drag must restore the open toolbar");
    handlePan.motion = CGPointMake(0, 6);
    handlePan.testState = UIGestureRecognizerStateEnded;
    [controller handleToolbarHandlePan:handlePan];
    NSCAssert([controller isToolbarListVisible], @"Tiny handle movement collapsed list");
    handlePan.motion = CGPointMake(0, 80);
    handlePan.testState = UIGestureRecognizerStateEnded;
    [controller handleToolbarHandlePan:handlePan];
    [controller.view setNeedsLayout];
    [controller.view layoutIfNeeded];
    NSCAssert(![controller isToolbarListVisible] && controller->_tool_drag_handle.hidden,
              @"Handle did not collapse both list modes");
    NSCAssert(controller->_tool_rail.bounds.size.height == 64 && !controller->_tool_scroll.scrollEnabled,
              @"Collapsed geometry or scrolling incorrect");
    NSCAssert([controller gestureRecognizerShouldBegin:press], @"Compact press-and-slide picker lost");
    NSLog(@"PASS: scroll isolation, pinned handle, button cancellation, docked/labeled lists, resizing");
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "toolbar_test.mm"
            executable = Path(directory) / "toolbar_test"
            source_file.write_text(harness)
            result = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fblocks", "-fno-objc-arc",
                "-isysroot", sdk.stdout.strip(), "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation", "-framework", "QuartzCore",
                "-framework", "CoreGraphics",
                str(source_file), "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(["xcrun", "simctl", "spawn", device, str(executable)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: scroll isolation", result.stderr)


if __name__ == "__main__":
    unittest.main()
