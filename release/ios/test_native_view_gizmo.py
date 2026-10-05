# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise production view-gizmo feedback, hold selection and layout with UIKit."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class NativeViewGizmoTest(unittest.TestCase):
    def test_hold_feedback_selection_and_cancellation(self):
        if sys.platform != "darwin":
            self.skipTest("Requires Xcode and a booted iOS simulator")
        sdk = subprocess.check_output(
            ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip()
        devices = json.loads(subprocess.check_output(
            ["xcrun", "simctl", "list", "devices", "booted", "--json"], text=True))
        candidates = [d for runtime, entries in devices["devices"].items()
                      if ".iOS-" in runtime for d in entries if d["isAvailable"]]
        if not candidates:
            self.skipTest("No booted iOS simulator")
        device = os.environ.get("IOS_TEST_SIMULATOR_UDID", candidates[0]["udid"])
        root = Path(__file__).resolve().parents[2]
        source = (root / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        self.assertIn("view_gizmo_hold.minimumPressDuration = 0.32;", source)
        self.assertIn("view_gizmo_hold.allowedTouchTypes = @[ @(UITouchTypeDirect), @(UITouchTypePencil) ];", source)
        self.assertIn("[view_gizmo_drag requireGestureRecognizerToFail:view_gizmo_hold]", source)
        methods = []
        for signature in (
            "- (void)updateViewGizmoFeedback\n", "- (void)handleViewGizmoHover:",
            "- (void)handleViewGizmoPress:", "- (void)handleViewGizmoRelease:",
            "- (void)cancelViewGizmoInteraction\n", "- (void)layoutViewPie\n",
            "- (void)setViewPieHoverButton:", "- (UIButton *)viewPieButtonAtGestureLocation:",
            "- (void)setViewPieVisible:", "- (void)handleViewGizmoHold:",
            "- (void)handleViewGizmoDrag:",
        ):
            start = source.rindex(signature)
            methods.append(source[start:source.index("\n}", start) + 2])
        harness = r'''
#import <UIKit/UIKit.h>
#include <cassert>
static int commands, lastCommand, cancellations;
static void WM_apple_ios_ui_command_enqueue(int command) { commands++; lastCommand = command; }
@interface GHOSTUIWindow : UIWindow
- (void)cancelPencilStroke;
@end
@implementation GHOSTUIWindow
- (void)cancelPencilStroke { cancellations++; }
@end
@interface TestHold : UILongPressGestureRecognizer
@property(nonatomic) UIGestureRecognizerState testState;
@property(nonatomic) CGPoint point;
@end
@implementation TestHold
- (UIGestureRecognizerState)state { return self.testState; }
- (CGPoint)locationInView:(UIView *)view { return self.point; }
@end
@interface TestHover : UIHoverGestureRecognizer
@property(nonatomic) UIGestureRecognizerState testState;
@end
@implementation TestHover
- (UIGestureRecognizerState)state { return self.testState; }
@end
@interface GizmoTest : NSObject {
@public
  UIView *_view, *_overlay_view, *_view_gizmo_hotspot, *_view_gizmo_feedback;
  NSMutableArray *_view_pie_buttons;
  NSArray *_view_gizmo_geometry;
  UISelectionFeedbackGenerator *_view_gizmo_selection_feedback;
  UIImpactFeedbackGenerator *_view_gizmo_open_feedback;
  UIButton *_view_pie_hover_button;
  BOOL _view_pie_visible, _view_gizmo_hovered, _view_gizmo_pressed;
}
@end
@implementation GizmoTest
- (void)hideToolbarLabelAnimated:(BOOL)animated {}
''' + "\n".join(methods) + r'''
@end
static void settle() {
  [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
}
int main() {
  @autoreleasepool {
    [UIView setAnimationsEnabled:NO];
    GHOSTUIWindow *window = [[GHOSTUIWindow alloc] initWithFrame:CGRectMake(0, 0, 1024, 768)];
    GizmoTest *c = [GizmoTest new];
    c->_view = [[UIView alloc] initWithFrame:window.bounds];
    [window addSubview:c->_view];
    c->_overlay_view = c->_view;
    c->_view_gizmo_hotspot = [UIButton buttonWithType:UIButtonTypeCustom];
    c->_view_gizmo_hotspot.frame = CGRectMake(906, 644, 112, 112);
    [c->_view addSubview:c->_view_gizmo_hotspot];
    c->_view_gizmo_feedback = [[UIView alloc] initWithFrame:CGRectMake(14, 14, 84, 84)];
    [c->_view_gizmo_hotspot addSubview:c->_view_gizmo_feedback];
    c->_view_pie_buttons = [NSMutableArray new];
    for (int i = 0; i < 8; i++) {
      UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
      [b setTitle:@"Frame Selected" forState:UIControlStateNormal];
      b.tag = i + 1;
      [c->_view_pie_buttons addObject:b];
      [c->_view addSubview:b];
    }
    [c cancelViewGizmoInteraction];
    assert(c->_view_gizmo_feedback.alpha == 0);
    TestHover *hover = [TestHover new];
    hover.testState = UIGestureRecognizerStateBegan;
    [c handleViewGizmoHover:hover];
    assert(c->_view_gizmo_feedback.alpha == 1 && !c->_view_pie_visible);
    [c handleViewGizmoPress:nil];
    assert(c->_view_gizmo_feedback.transform.a < 1);
    TestHold *hold = [TestHold new];
    hold.point = c->_view_gizmo_hotspot.center;
    hold.testState = UIGestureRecognizerStateBegan;
    [c handleViewGizmoHold:hold];
    assert(c->_view_pie_visible && cancellations == 1 && commands == 0);
    assert(c->_view_gizmo_feedback.transform.a > 1);
    hover.testState = UIGestureRecognizerStateEnded;
    [c handleViewGizmoHover:hover];
    [c handleViewGizmoRelease:nil];
    assert(c->_view_gizmo_feedback.alpha == 1); // Active survives hover exit/control cancellation.
    hold.testState = UIGestureRecognizerStateEnded;
    [c handleViewGizmoHold:hold];
    assert(!c->_view_pie_visible && commands == 0 && c->_view_gizmo_feedback.alpha == 0);
    hold.testState = UIGestureRecognizerStateBegan;
    [c handleViewGizmoHold:hold];
    UIButton *choice = c->_view_pie_buttons[2];
    hold.point = choice.center;
    hold.testState = UIGestureRecognizerStateChanged;
    [c handleViewGizmoHold:hold];
    assert(c->_view_pie_hover_button == choice && choice.selected);
    hold.testState = UIGestureRecognizerStateEnded;
    [c handleViewGizmoHold:hold];
    assert(commands == 1 && lastCommand == 3 && !choice.selected);
    hold.testState = UIGestureRecognizerStateBegan;
    [c handleViewGizmoHold:hold];
    [c setViewPieHoverButton:choice];
    hold.point = CGPointMake(1, 1);
    hold.testState = UIGestureRecognizerStateEnded;
    [c handleViewGizmoHold:hold];
    assert(commands == 1); // Moving away must not commit the previous choice.
    hold.testState = UIGestureRecognizerStateBegan;
    [c handleViewGizmoHold:hold];
    hold.testState = UIGestureRecognizerStateCancelled;
    [c handleViewGizmoHold:hold];
    assert(commands == 1 && !c->_view_pie_visible);
    for (NSValue *size in @[[NSValue valueWithCGSize:CGSizeMake(1024, 768)],
                            [NSValue valueWithCGSize:CGSizeMake(951, 669)],
                            [NSValue valueWithCGSize:CGSizeMake(390, 844)],
                            [NSValue valueWithCGSize:CGSizeMake(844, 390)]]) {
      c->_view.frame = CGRectMake(0, 0, size.CGSizeValue.width, size.CGSizeValue.height);
      [c->_view layoutIfNeeded];
      [c setViewPieVisible:YES animated:NO];
      for (UIButton *b in c->_view_pie_buttons) {
        assert(CGRectContainsRect(c->_view.bounds, b.frame));
        assert(b.bounds.size.height == 44);
        for (UIButton *other in c->_view_pie_buttons) {
          assert(b == other || !CGRectIntersectsRect(b.frame, other.frame));
        }
      }
    }
    [UIView setAnimationsEnabled:YES];
    [c setViewPieVisible:NO animated:YES];
    [c setViewPieVisible:YES animated:YES];
    settle();
    for (UIButton *b in c->_view_pie_buttons) assert(!b.hidden && b.alpha == 1);
    [c cancelViewGizmoInteraction];
    settle();
    assert(!c->_view_pie_visible && c->_view_gizmo_feedback.alpha == 0);
    for (UIButton *b in c->_view_pie_buttons) assert(b.hidden);
    NSLog(@"PASS: stationary hold, hover/active feedback, selection, cancellation, resizing, interrupted reveal");
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "view_gizmo_test.mm"
            executable = Path(directory) / "view_gizmo_test"
            src.write_text(harness)
            build = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fblocks",
                "-fno-objc-arc", "-isysroot", sdk,
                "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation", "-framework", "QuartzCore",
                "-framework", "CoreGraphics",
                str(src), "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run(["xcrun", "simctl", "spawn", device, str(executable)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: stationary hold", result.stderr)


if __name__ == "__main__":
    unittest.main()
