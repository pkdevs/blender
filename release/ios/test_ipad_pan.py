# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression checks for iPad pan binding and simultaneous gesture routing."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class IPadPanTest(unittest.TestCase):
    def test_pan_dispatch_and_delta(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/editors/space_view3d/view3d_navigate_view_move.cc").read_text()

        def function(signature):
            start = source.index(signature)
            return source[start:source.index("\n}", start) + 2]

        harness = r'''
#include <cassert>
enum { MOUSEPAN = 1, MOUSEMOVE, WM_EVENT_MULTITOUCH_TWO_FINGERS = 8,
       VIEW_CONFIRM, VIEW_PASS, OPERATOR_FINISHED, OPERATOR_RUNNING_MODAL,
       OPERATOR_PASS_THROUGH };
using eV3D_OpEvent = int;
using wmOperatorStatus = int;
struct bContext {};
struct wmOperator {};
struct PointerRNA {};
struct wmEvent { int type, flag, xy[2], prev_xy[2]; };
struct ViewOpsData { int v3d = 0, rv3d = 0; };
static int mx, my, applications = 0, invocations = 0;
static int ViewOpsType_move;
static void viewmove_apply(ViewOpsData *, int x, int y) {
  mx = x; my = y; ++applications;
}
static void ED_view3d_camera_lock_autokey(int, int, bContext *, bool, bool) {}
''' + function("static wmOperatorStatus viewmove_invoke_impl(") + r'''
static wmOperatorStatus view3d_navigate_invoke_impl(
    bContext *C, wmOperator *, const wmEvent *event, const int *) {
  ++invocations;
  ViewOpsData vod;
  return viewmove_invoke_impl(C, &vod, event, nullptr);
}
''' + function("static wmOperatorStatus viewmove_invoke(") + r'''
int main() {
  wmEvent event{MOUSEPAN, WM_EVENT_MULTITOUCH_TWO_FINGERS, {300, 200}, {280, 215}};
  assert(viewmove_invoke(nullptr, nullptr, &event) == OPERATOR_FINISHED);
  assert(applications == 1 && invocations == 1);
  assert(mx == 320 && my == 185);
  // Unsupported input must pass through on iPad, not report a successful pan.
  event.flag = 0;
#if APPLE_TARGET_IOS
  assert(viewmove_invoke(nullptr, nullptr, &event) == OPERATOR_PASS_THROUGH);
  assert(applications == 1 && invocations == 1);
  event.type = MOUSEMOVE;
  event.flag = WM_EVENT_MULTITOUCH_TWO_FINGERS;
  assert(viewmove_invoke(nullptr, nullptr, &event) == OPERATOR_PASS_THROUGH);
  assert(applications == 1 && invocations == 1);
#else
  assert(viewmove_invoke(nullptr, nullptr, &event) == OPERATOR_FINISHED);
  assert(applications == 2 && invocations == 2);
  event.type = MOUSEMOVE;
  assert(viewmove_invoke(nullptr, nullptr, &event) == OPERATOR_RUNNING_MODAL);
#endif
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "pan_dispatch_test.cc"
            executable = Path(directory) / "pan_dispatch_test"
            source_file.write_text(harness)
            for ios in (0, 1):
                with self.subTest(ios=ios):
                    result = subprocess.run([
                        compiler, "-std=c++17", "-DWITH_APPLE_CROSSPLATFORM",
                        f"-DAPPLE_TARGET_IOS={ios}", str(source_file), "-o", str(executable),
                    ], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(executable)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_binding_and_gesture_pairs(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        keymaps = (root / "source/blender/windowmanager/intern/wm_keymap.cc").read_text()
        start = keymaps.index("static void wm_keymap_ensure_ios_gestures(")
        binding = keymaps[start:keymaps.index("\n}", start) + 2]
        start = keymaps.index("if (wm_keymap_update_flag & WM_KEYMAP_UPDATE_RECONFIGURE)")
        reconfigure = keymaps[start:]
        self.assertLess(reconfigure.index("wm_keymap_ensure_ios_gestures("),
                        reconfigure.index("wmKeyConfig *kc_active"))
        self.assertLess(reconfigure.index("wm_keymap_ensure_ios_gestures("),
                        reconfigure.index("wm_keymap_patch_update("))
        native = (root / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        start = native.index("/* Pan, pinch, and twist")
        start = native.index("{", start)
        delegate_body = native[start:native.index("\n}", start) + 2]
        region = (root / "source/blender/editors/space_view3d/space_view3d.cc").read_text()
        region = region[region.index("static void view3d_main_region_init("):]
        self.assertLess(region.index('"3D View iPad Gestures"'), region.index('"Paint Face Mask'))
        harness = r'''
#include <cassert>
#include <cstring>
using BOOL = bool;
enum { MOUSEPAN = 10, KM_ANY = -1, SPACE_VIEW3D = 1, RGN_TYPE_WINDOW = 0 };
struct KeyMapItem_Params { int type, value, modifier, keymodifier, direction; };
struct wmKeyMap { int items = 0; };
struct wmKeyConfig {};
static wmKeyMap map;
static KeyMapItem_Params captured;
static int calls = 0;
static wmKeyConfig keyconf;
static bool BLI_listbase_is_empty(int *items) { return *items == 0; }
static wmKeyMap *WM_keymap_ensure(wmKeyConfig *, const char *name, int space, int region)
{
  assert(std::strcmp(name, "3D View iPad Gestures") == 0);
  assert(space == SPACE_VIEW3D && region == RGN_TYPE_WINDOW);
  return &map;
}
static void WM_keymap_add_item(wmKeyMap *km, const char *op, const KeyMapItem_Params *params)
{
  assert(std::strcmp(op, "VIEW3D_OT_move") == 0);
  captured = *params;
  ++km->items;
  ++calls;
}
''' + binding + r'''
static const int pan2f_gesture_recognizer = 1;
static const int zoom_gesture_recognizer = 2;
static const int rotation_gesture_recognizer = 3;
static BOOL simultaneous(int gestureRecognizer, int otherGestureRecognizer)
''' + delegate_body + r'''
int main()
{
  wm_keymap_ensure_ios_gestures(&keyconf);
  wm_keymap_ensure_ios_gestures(&keyconf);
  assert(calls == 1);
  assert(captured.type == MOUSEPAN && captured.value == KM_ANY);
  assert(captured.modifier == KM_ANY && captured.keymodifier == 0);
  // Preset loading clears ALL default keymap items, not just those defined by Python.
  // Reconfigure must restore the binding before copying the defaults into the user map.
  for (int reload = 1; reload <= 3; reload++) {
    map.items = 0;
    wm_keymap_ensure_ios_gestures(&keyconf);
    const wmKeyMap user_map = map;
    assert(user_map.items == 1 && calls == reload + 1);
    wm_keymap_ensure_ios_gestures(&keyconf);
    assert(map.items == 1 && calls == reload + 1);
  }
  for (int first = 1; first <= 5; ++first) {
    for (int second = 1; second <= 5; ++second) {
      assert(simultaneous(first, second) ==
             (first != second && first <= 3 && second <= 3));
    }
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "pan_test.cc"
            executable = Path(directory) / "pan_test"
            source.write_text(harness)
            for ios in (0, 1):
                with self.subTest(ios=ios):
                    subprocess.run([
                        compiler, "-std=c++17", "-DWITH_APPLE_CROSSPLATFORM",
                        f"-DAPPLE_TARGET_IOS={ios}", str(source), "-o", str(executable),
                    ], check=True, capture_output=True, text=True)
                    subprocess.run([str(executable)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
