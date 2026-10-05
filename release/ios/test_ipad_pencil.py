# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise native Pencil routing and Blender's real click/drag dispatch."""

import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.rindex(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


class IPadPencilTest(unittest.TestCase):
    def test_click_and_box_drag_use_distinct_blender_bindings(self):
        keyconfig = runpy.run_path(str(ROOT / "scripts/presets/keyconfig/keymap_data/blender_default.py"))
        params = keyconfig["Params"](select_mouse="LEFT")
        keymaps = {name: data["items"] for name, _, data in keyconfig["generate_keymaps"](params)}

        def plain_binding(name, operator):
            return next((event, properties) for op, event, properties in keymaps[name]
                        if op == operator and event.get("type") == "LEFTMOUSE"
                        and not any(event.get(key) for key in ("shift", "ctrl", "alt", "oskey")))

        event, properties = plain_binding("3D View", "view3d.select")
        self.assertEqual(event["value"], "CLICK")
        self.assertFalse(dict(properties["properties"]).get("select_passthrough", False))
        event, _ = plain_binding("3D View Tool: Select Box", "view3d.select_box")
        self.assertEqual(event["value"], "CLICK_DRAG")

    def test_native_pencil_event_lifecycle(self):
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
        source = (ROOT / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        # The modal radial view shields the viewport, so hover must observe their common window.
        self.assertIn("[self addGestureRecognizer:hover_gesture_recognizer]", source)
        self.assertNotIn("[window->getView() addGestureRecognizer:hover_gesture_recognizer]", source)
        header = (ROOT / "intern/ghost/intern/GHOST_WindowIOS.hh").read_text()
        harness = Path(__file__).with_name("pencil_input_test.mm").read_text()
        harness = '#import "GHOST_IOSPencilMenu.hh"\n' + harness
        pointer_methods = []
        for signature in ("  void updatePointerPosition(", "  GHOST_TSuccess getPointerPosition("):
            start = header.index(signature)
            pointer_methods.append(header[start:header.index("\n  }", start) + 4])
        harness = harness.replace("// POINTER_METHODS", "\n".join(pointer_methods))
        start = source.index("typedef struct UserInputEvent {")
        harness = harness.replace("// INPUT_EVENT", source[start:source.index("} UserInputEvent;", start) + 17])
        methods = "\n".join(function(source, signature) for signature in (
            "- (void)generateUserInputEvents:",
            "- (BOOL)touchBeganInBlenderViewport:",
            "- (BOOL)isBlenderPencilInputView:",
            "- (void)updateTabletDataFromPencilTouch:",
            "- (CGPoint)getScaledPencilPoint:",
            "- (void)sendPencilCursorEvent:",
            "- (void)sendPencilButtonEvent:",
            "- (void)handleNavigationTap:",
            "- (void)cancelPencilStroke\n",
            "- (void)setFingerToolEnabled:",
            "- (void)sendEvent:",
            "- (void)applicationWillResignActive:",
            "- (void)handleHover:",
            "- (void)togglePencilMenuForInteraction:",
            "- (void)pencilInteraction:(UIPencilInteraction *)interaction didReceiveTap:",
            "- (void)pencilInteraction:(UIPencilInteraction *)interaction\n      didReceiveSqueeze:",
        ))
        harness = harness.replace("// NATIVE_METHODS", methods)
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "native_pencil_test.mm"
            executable = Path(directory) / "native_pencil_test"
            source_file.write_text(harness)
            build = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fblocks", "-fno-objc-arc",
                "-I", str(ROOT / "intern/ghost/intern"),
                "-isysroot", sdk.stdout.strip(), "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation", "-framework", "CoreGraphics",
                "-framework", "QuartzCore", str(ROOT / "intern/ghost/intern/GHOST_IOSPencilMenu.mm"),
                str(source_file), "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run(["xcrun", "simctl", "spawn", device, str(executable)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: Pencil event ordering", result.stderr)

    def test_taps_dispatch_clicks_and_real_drags_keep_their_own_origin(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        source = (ROOT / "source/blender/windowmanager/intern/wm_event_system.cc").read_text()
        query = (ROOT / "source/blender/windowmanager/intern/wm_event_query.cc").read_text()
        methods = "\n".join(function(source, signature) for signature in (
            "static void wm_event_prev_values_set(",
            "static void wm_event_prev_click_set(",
            "static void wm_event_state_update_and_click_set_ex(",
        ))
        self.assertNotIn("force_immediate_press_drag", source)
        preferences = (ROOT / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        start = preferences.index("  U.drag_threshold_mouse =")
        preferences = preferences[start:preferences.index("  U.flag", start)]
        queries = "\n".join(function(query, signature) for signature in (
            "int WM_event_drag_threshold(", "bool WM_event_drag_test_with_delta(",
            "bool WM_event_drag_test(", "void WM_event_drag_start_xy(",
        ))
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>
#define WITH_APPLE_CROSSPLATFORM 1
#define APPLE_TARGET_IOS 1
#define BLI_assert assert
#define ELEM(value, a, b) ((value) == (a) || (value) == (b))
#define ISKEYBOARD_OR_BUTTON(type) ((type) == LEFTMOUSE)
#define ISMOUSE_BUTTON(type) ((type) == LEFTMOUSE)
#define ISMOUSE_MOTION(type) ((type) == MOUSEMOVE)
#define ISKEYMODIFIER(type) false
#define ISMOUSE_WHEEL(type) false
#define ISMOUSE_GESTURE(type) false
#define UNPACK2(xy) (xy)[0], (xy)[1]
#define CLOG_DEBUG(...) ((void)0)
enum { LEFTMOUSE = 1, MOUSEMOVE, KM_PRESS, KM_RELEASE, KM_DBL_CLICK, KM_NOTHING, KM_PRESS_DRAG, KM_CLICK };
using eWM_EventFlag = int;
using wmEventType = int;
using wmEventModifierFlag = int;
using eHandlerActionFlag = int;
constexpr int WM_HANDLER_CONTINUE = 0, WM_HANDLER_BREAK = 1, WM_HANDLER_MODAL = 2;
constexpr int WM_EVENT_IS_REPEAT = 1, WM_EVENT_FORCE_DRAG_THRESHOLD = 2;
static float UI_SCALE_FAC = 1;
static struct { int drag_threshold_mouse, drag_threshold_tablet, drag_threshold, move_threshold; } U;
struct wmEvent {
  bool tablet = true;
  int type = 0, val = 0, flag = 0, modifier = 0, keymodifier = 0, direction = 0;
  int prev_type = 0, prev_val = 0, prev_press_type = 0;
  int prev_press_modifier = 0, prev_press_keymodifier = 0;
  int xy[2] = {}, prev_xy[2] = {}, prev_press_xy[2] = {};
};
struct wmWindow {
  bool event_queue_check_click = false, event_queue_check_drag = false;
  bool event_queue_check_drag_handled = false;
};
struct bContext { wmWindow *win; };
struct wmEventHandler {};
template<typename T> struct ListBaseT {};
using ListBase = ListBaseT<wmEventHandler>;
static wmWindow *CTX_wm_window(bContext *C) { return C->win; }
static void wm_event_handler_return_value_check(bContext *, wmEvent *, int) {}
static void copy_v2_v2_int(int *dst, const int *src) { dst[0] = src[0]; dst[1] = src[1]; }
static void sub_v2_v2v2_int(int *dst, const int *a, const int *b) { dst[0] = a[0]-b[0]; dst[1] = a[1]-b[1]; }
static bool wm_event_is_double_click(const wmEvent *, uint64_t, uint64_t) { return false; }
static bool WM_event_is_tablet(const wmEvent *event) { return event->tablet; }
static int WM_event_drag_direction(wmEvent *) { return 1; }
''' + methods + "\n" + queries + "\n" + function(source, "static bool wm_action_not_handled(") + r'''
static std::vector<wmEvent> clicks, drags;
static bool press_tool = false;
static int presses;
// Stand in for the editor/tool keymaps; click selection and drag tools are distinct bindings.
static int wm_handlers_do_intern(bContext *, wmWindow *, wmEvent *event, ListBase *) {
  if (event->val == KM_CLICK) { clicks.push_back(*event); return WM_HANDLER_BREAK; }
  if (event->val == KM_PRESS_DRAG) { drags.push_back(*event); return WM_HANDLER_BREAK; }
  if (press_tool && event->val == KM_PRESS) { presses++; return WM_HANDLER_BREAK; }
  return WM_HANDLER_CONTINUE;
}
''' + function(source, "static eHandlerActionFlag wm_handlers_do(") + r'''
static void dispatch(bContext *C, wmEvent event) {
  wm_handlers_do(C, &event, nullptr);
  if (C->win->event_queue_check_drag_handled) {
    C->win->event_queue_check_drag = false;
    C->win->event_queue_check_drag_handled = false;
  }
}
int main() {
''' + preferences + r'''
  for (float scale : {1.0f, 2.0f}) {
    UI_SCALE_FAC = scale;
    for (bool tablet : {true, false}) {
      wmWindow win;
      bContext C{&win};
      wmEvent state;
      state.tablet = tablet;
      uint64_t press_time = 0;
      std::vector<wmEvent> queue;
      auto enqueue = [&](int type, int val, int x, int y) {
        state.xy[0] = x; state.xy[1] = y;
        wmEvent event = state;
        event.type = type; event.val = val;
        if (type == LEFTMOUSE) {
          wm_event_state_update_and_click_set_ex(&event, 1000, &state, &press_time, false, false);
        } else {
          event.prev_type = state.type; event.prev_val = state.val;
        }
        queue.push_back(event);
      };
      const int threshold = int(3 * scale);
      clicks.clear(); drags.clear();
      // No motion, sub-threshold jitter, exact threshold, then an actual drag.
      const int delta[] = {0, 1, threshold, threshold + 1};
      const int starts[][2] = {{110, 730}, {810, 120}, {45, 60}, {900, 1000}};
      for (int i = 0; i < 4; i++) {
        const int x = starts[i][0], y = starts[i][1];
        state.modifier = i; state.keymodifier = i + 10;
        enqueue(LEFTMOUSE, KM_PRESS, x, y);
        enqueue(MOUSEMOVE, KM_NOTHING, x, y); // Stationary cursor update is not a drag.
        enqueue(MOUSEMOVE, KM_NOTHING, x + delta[i], y - delta[i]);
        enqueue(LEFTMOUSE, KM_RELEASE, x + delta[i], y - delta[i]);
      }
      // Queue all strokes first: later eventstate must never overwrite an earlier origin.
      for (const wmEvent &event : queue) dispatch(&C, event);
      assert(clicks.size() == 3 && drags.size() == 1);
      for (int i = 0; i < 3; i++) {
        assert(clicks[i].xy[0] == starts[i][0] && clicks[i].xy[1] == starts[i][1]);
      }
      int origin[2];
      WM_event_drag_start_xy(&drags[0], origin);
      assert(origin[0] == starts[3][0] && origin[1] == starts[3][1]);
      assert(drags[0].xy[0] == starts[3][0] + threshold + 1);
      assert(drags[0].modifier == 3 && drags[0].keymodifier == 13);
      assert(!win.event_queue_check_click && !win.event_queue_check_drag);
      // Press-based tools (paint/sculpt) must still start on contact, not wait for a drag.
      queue.clear(); press_tool = true; presses = 0;
      enqueue(LEFTMOUSE, KM_PRESS, 500, 500);
      dispatch(&C, queue.back());
      assert(presses == 1 && !win.event_queue_check_click && !win.event_queue_check_drag);
      press_tool = false;
    }
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "pencil_test.cc"
            executable = Path(directory) / "pencil_test"
            source_file.write_text(harness)
            build = subprocess.run([compiler, "-std=c++17", str(source_file), "-o", str(executable)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
