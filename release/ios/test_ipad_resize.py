# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Guard live resizing and exercise the production drawable-size callback chain."""

import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class IPadResizeTest(unittest.TestCase):
    def test_dynamic_scene_resize_configuration(self):
        with (ROOT / "release/ios/Blender.app/Info.plist").open("rb") as source:
            info = plistlib.load(source)
        # On iPadOS 27 this legacy flag defers scene-size changes until release.
        self.assertFalse(info.get("UIRequiresFullScreen", False))
        self.assertTrue(info.get("UILaunchStoryboardName"))
        self.assertEqual(set(info["UISupportedInterfaceOrientations"]), {
            "UIInterfaceOrientationPortrait", "UIInterfaceOrientationPortraitUpsideDown",
            "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight",
        })
        scenes = info["UIApplicationSceneManifest"]["UISceneConfigurations"]
        self.assertEqual(scenes["UIWindowSceneSessionRoleApplication"][0]["UISceneDelegateClassName"],
                         "IOSSceneDelegate")

    def test_every_intermediate_size_reaches_blender(self):
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
        window = (ROOT / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        system = (ROOT / "intern/ghost/intern/GHOST_SystemIOS.mm").read_text()

        def method(source, signature):
            start = source.index(signature)
            return source[start:source.index("\n}\n", start) + 2]

        scale = method(window, "- (void)updateMTKViewDrawableScale\n")
        resize = method(system, "- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:")
        bounds = method(window, "void GHOST_WindowIOS::getClientBounds(")
        geometry = method(window, "void GHOST_WindowIOS::publishGeometry(CGSize size)")
        harness = r'''
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#include <vector>
#include <atomic>
#define GHOST_ASSERT(condition, message) NSCAssert(condition, @"%s", message)
struct GHOST_Rect { double l_, t_, r_, b_; };
class GHOST_SystemIOS;
struct TestSurface { void resize(CGSize) {} };
class GHOST_WindowIOS {
 public:
  MTKView *metal_view_;
  std::atomic<float> scale_{1};
  std::atomic<uint64_t> drawable_size_{0};
  TestSurface *surface = nullptr;
  GHOST_SystemIOS *system_ios_;
  void publishGeometry(CGSize size);
  bool getValid() const { return true; }
  void getClientBounds(GHOST_Rect &bounds) const;
};
''' + bounds + r'''
enum { GHOST_kEventWindowSize = 1 };
struct GHOST_Event {
  int type;
  GHOST_WindowIOS *window;
  GHOST_Event(int, int type, GHOST_WindowIOS *window) : type(type), window(window) {}
};
class GHOST_ISystem {
 public:
  static GHOST_ISystem *getSystem();
};
class GHOST_SystemIOS : public GHOST_ISystem {
 public:
  GHOST_WindowIOS *current_active_window_ = nullptr;
  std::vector<GHOST_Rect> sizes;
  std::vector<GHOST_Event *> events;
  int getMilliSeconds() { return 0; }
  void pushEvent(GHOST_Event *event) {
    NSCAssert(event->type == GHOST_kEventWindowSize, @"Wrong resize event");
    events.push_back(event);
  }
  void dispatchEvents() {
    for (GHOST_Event *event : events) {
      GHOST_Rect bounds;
      event->window->getClientBounds(bounds);
      sizes.push_back(bounds);
      delete event;
    }
    events.clear();
  }
};
static GHOST_SystemIOS ghost;
GHOST_ISystem *GHOST_ISystem::getSystem() { return &ghost; }
''' + geometry + r'''
@interface ResizeRenderer : NSObject <MTKViewDelegate> {
@public GHOST_WindowIOS *_window;
}
@end
@implementation ResizeRenderer
- (void)drawInMTKView:(MTKView *)view {}
''' + resize + r'''
@end
@interface ResizeController : NSObject {
@public MTKView *_view;
}
@end
@implementation ResizeController
''' + scale + r'''
@end
int main() {
  @autoreleasepool {
    MTKView *view = [[[MTKView alloc] initWithFrame:CGRectMake(0, 0, 1000, 700)] autorelease];
    view.autoResizeDrawable = YES;
    view.paused = YES;
    GHOST_WindowIOS window{view};
    window.system_ios_ = &ghost;
    ghost.current_active_window_ = &window;
    ResizeRenderer *renderer = [[[ResizeRenderer alloc] init] autorelease];
    renderer->_window = &window;
    view.delegate = renderer;
    ResizeController *controller = [[[ResizeController alloc] init] autorelease];
    controller->_view = view;
    const CGSize sizes[] = {{1000, 700}, {975, 680}, {950, 660}, {840, 630},
                            {640, 480}, {480, 640}, {600, 800}, {900, 700}, {1000, 700}};
    for (CGSize size : sizes) {
      view.bounds = CGRectMake(0, 0, size.width, size.height);
      [controller updateMTKViewDrawableScale];
      // The command-line harness has no display loop to deliver MetalKit's callback.
      [renderer mtkView:view drawableSizeWillChange:view.drawableSize];
      ghost.dispatchEvents();
      NSCAssert(!ghost.sizes.empty(), @"No drawable resize notification");
      const GHOST_Rect &bounds = ghost.sizes.back();
      NSCAssert(bounds.r_ == size.width * view.contentScaleFactor &&
                bounds.b_ == size.height * view.contentScaleFactor,
                @"Blender did not see intermediate drawable dimensions");
      NSCAssert(CGSizeEqualToSize(view.drawableSize, CGSizeMake(bounds.r_, bounds.b_)),
                @"Viewport and drawable disagree");
    }
    NSCAssert(ghost.sizes.size() >= 9, @"Resize updates deferred until the end");
    ghost.current_active_window_ = nullptr;
    renderer->_window = nullptr;
    [renderer mtkView:view drawableSizeWillChange:CGSizeMake(500, 500)];
    NSLog(@"PASS: intermediate resize, Retina dimensions, portrait, landscape, restore");
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "resize.mm"
            executable = Path(directory) / "resize"
            source.write_text(harness)
            result = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17",
                "-fno-objc-arc", "-isysroot", sdk,
                "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "MetalKit", "-framework", "QuartzCore",
                "-framework", "Foundation",
                str(source), "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(["xcrun", "simctl", "spawn", device, str(executable)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: intermediate resize", result.stderr)


if __name__ == "__main__":
    unittest.main()
