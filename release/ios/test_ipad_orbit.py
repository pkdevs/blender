# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Orbit direction must survive pole crossings and queued momentum updates."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.rindex(signature)
    return source[start:source.index("\n}", start) + 2]


class IPadOrbitTest(unittest.TestCase):
    def test_gesture_direction_and_event_coalescing(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        rotate = (ROOT / "source/blender/editors/space_view3d/view3d_navigate_view_rotate.cc").read_text()
        events = (ROOT / "source/blender/windowmanager/intern/wm_event_system.cc").read_text()
        dna = (ROOT / "source/blender/makesdna/DNA_view3d_types.h").read_text()
        start = dna.index("typedef struct View3D_Runtime {")
        runtime = dna[start:dna.index("} View3D_Runtime;", start) + len("} View3D_Runtime;")]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#define ELEM(a, b, c) ((a) == (b) || (a) == (c))
enum { MOUSEROTATE = 1, MOUSEPAN, VIEW_CONFIRM, VIEW_PASS,
       OPERATOR_FINISHED, OPERATOR_RUNNING_MODAL, WM_EVENT_SCROLL_INVERT = 1 };
using wmOperatorStatus = int;
using eV3D_OpEvent = int;
struct bContext {};
struct PointerRNA {};
''' + runtime + r'''
struct RegionView3D { bool is_persp = true; };
struct View3D { View3D_Runtime runtime{}; };
struct ViewOpsData {
  bool use_dyn_ofs = false, use_dyn_ofs_ortho_correction = false;
  RegionView3D *rv3d;
  View3D *v3d;
  float reverse;
};
struct wmEvent {
  int type = MOUSEROTATE, flag = 0, xy[2]{}, prev_xy[2]{};
  uint64_t trackpad_gesture_id = 0;
};
struct Runtime { struct { void *last = nullptr; } event_queue; };
struct wmWindow { Runtime *runtime; };
static std::vector<wmEvent *> queue;
static void wm_event_free_last(wmWindow *win) {
  delete queue.back(); queue.pop_back();
  win->runtime->event_queue.last = queue.empty() ? nullptr : queue.back();
}
static wmEvent *wm_event_add_intern(wmWindow *win, const wmEvent *event) {
  queue.push_back(new wmEvent(*event));
  win->runtime->event_queue.last = queue.back();
  return queue.back();
}
static void copy_v2_v2_int(int dst[2], const int src[2]) { dst[0] = src[0]; dst[1] = src[1]; }
static float applied_reverse;
static void viewrotate_apply(ViewOpsData *vod, const int *) { applied_reverse = vod->reverse; }
static void ED_view3d_camera_lock_autokey(View3D *, RegionView3D *, bContext *, bool, bool) {}
''' + function(rotate, "static float viewrotate_gesture_reverse(") + "\n" + function(
            rotate, "static wmOperatorStatus viewrotate_invoke_impl(") + "\n" + function(
            events, "static wmEvent *wm_event_add_trackpad(") + r'''
int main() {
  View3D view;
  RegionView3D region;
  wmEvent event;
  // A fresh ViewOpsData is made for EVERY update, both while touching and coasting.
  for (int gesture = 1; gesture <= 4; gesture++) {
    const float initial = gesture % 2 ? 1.0f : -1.0f;
    event.trackpad_gesture_id = gesture;
    for (int frame = 0; frame < 240; frame++) {
      const float recalculated = (frame / 20) % 2 ? -initial : initial;
      ViewOpsData vod{false, false, &region, &view, recalculated};
      assert(viewrotate_invoke_impl(nullptr, &vod, &event, nullptr) == OPERATOR_FINISHED);
#if APPLE_TARGET_IOS
      assert(applied_reverse == initial);
#else
      assert(applied_reverse == recalculated);
#endif
    }
  }
  // Unidentified events (desktop devices) retain their original per-event direction.
  event.trackpad_gesture_id = 0;
  for (float sign : {-1.0f, 1.0f}) {
    ViewOpsData vod{false, false, &region, &view, sign};
    viewrotate_invoke_impl(nullptr, &vod, &event, nullptr);
    assert(applied_reverse == sign);
  }
  // Separate viewports must not share direction state.
  View3D other;
  assert(viewrotate_gesture_reverse(other.runtime, &region, 4, 1.0f) == 1.0f);
  assert(viewrotate_gesture_reverse(view.runtime, &region, 4, -1.0f) == -1.0f);
  RegionView3D other_region;
  assert(viewrotate_gesture_reverse(view.runtime, &other_region, 4, 1.0f) == 1.0f);

  Runtime runtime;
  wmWindow win{&runtime};
  event.xy[0] = 100; event.xy[1] = 200;
  event.trackpad_gesture_id = 10;
  wm_event_add_trackpad(&win, &event, 3, -4);
  wm_event_add_trackpad(&win, &event, 5, -6);
  assert(queue.size() == 1);
  assert(queue[0]->prev_xy[0] == 92 && queue[0]->prev_xy[1] == 210);
  // The previous coast and a new flick may be queued together after a heavy frame.
  event.trackpad_gesture_id = 11;
  wm_event_add_trackpad(&win, &event, -7, 8);
  wm_event_add_trackpad(&win, &event, -9, 10);
  assert(queue.size() == 2);
  assert(queue[0]->trackpad_gesture_id == 10);
  assert(queue[1]->trackpad_gesture_id == 11);
  assert(queue[1]->prev_xy[0] == 116 && queue[1]->prev_xy[1] == 182);
  event.trackpad_gesture_id = 0;
  wm_event_add_trackpad(&win, &event, 1, 2);
  wm_event_add_trackpad(&win, &event, 3, 4);
  assert(queue.size() == 3);
  assert(queue[2]->prev_xy[0] == 96 && queue[2]->prev_xy[1] == 194);
  while (!queue.empty()) wm_event_free_last(&win);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "orbit_test.cc"
            executable = Path(directory) / "orbit_test"
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

    def test_native_identity_survives_release_and_momentum(self):
        native = (ROOT / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        pan = function(native, "- (void)handlePan:")
        momentum = function(native, "- (void)updateOrbitMomentum:")
        self.assertEqual(native.count("orbit_gesture_id = ++next_orbit_gesture_id"), 1)
        begin = pan.index("if (sender.state == UIGestureRecognizerStateBegan) {")
        end = pan.index("\n    }", begin)
        self.assertIn("orbit_gesture_id = ++next_orbit_gesture_id", pan[begin:end])
        for method in (pan, momentum):
            self.assertLess(method.index("event_info.gesture_id = orbit_gesture_id"),
                            method.index("[self generateUserInputEvents:event_info]"))
        generate = function(native, "- (void)generateUserInputEvents:")
        orbit = generate[generate.index("case UserInputEvent::EventTypes::ORBIT_GESTURE:"):]
        self.assertIn("event_info.gesture_id", orbit[:orbit.index("break;")])
        events = (ROOT / "source/blender/windowmanager/intern/wm_event_system.cc").read_text()
        self.assertIn("event.trackpad_gesture_id = pd->gesture_id;", events)
        self.assertIn("event.trackpad_gesture_id = 0;", events)


if __name__ == "__main__":
    unittest.main()
