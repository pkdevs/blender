# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check per-axis hover animation and the native redraw handoff."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class IPadNavigationTest(unittest.TestCase):
    def test_native_view_commands_invoke_smooth_view_without_changing_gestures(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()

        def function(name):
            start = source.index(f"void {name}(")
            return source[start:source.index("\n}\n", start) + 2]

        harness = r'''
#include <cassert>
#include <string>
#include <string_view>
template<typename T, typename... V> bool elem(T value, V... values) {
  return ((value == values) || ...);
}
#define ELEM(...) elem(__VA_ARGS__)
namespace blender::wm { enum class OpCallContext { ExecRegionWin, InvokeRegionWin }; }
using blender::wm::OpCallContext;
struct bContext {};
struct PointerRNA { std::string type; float angle = 0; bool align = false, relative = false; };
struct wmOperatorType {};
static bool allowed = true;
static int calls = 0, allocated = 0;
static OpCallContext context;
static PointerRNA properties;
static bool WM_operator_name_poll(bContext *, const char *) { return allowed; }
static wmOperatorType *WM_operatortype_find(const char *, bool) { static wmOperatorType type; return &type; }
static void WM_operator_properties_create_ptr(PointerRNA *, wmOperatorType *) { allocated++; }
static void WM_operator_properties_free(PointerRNA *) { allocated--; }
static void RNA_enum_set_identifier(bContext *, PointerRNA *p, const char *, const char *value) { p->type = value; }
static void RNA_boolean_set(PointerRNA *p, const char *name, bool value) {
  if (std::string_view(name) == "relative") p->relative = value; else p->align = value;
}
static void RNA_float_set(PointerRNA *p, const char *, float value) { p->angle = value; }
static void WM_operator_name_call(bContext *, const char *, OpCallContext c, PointerRNA *p, void *) {
  context = c; calls++; properties = p ? *p : PointerRNA{};
}
static void WM_operator_name_call_ptr(bContext *C, wmOperatorType *, OpCallContext c, PointerRNA *p, void *) {
  WM_operator_name_call(C, "", c, p, nullptr);
}
''' + '\n'.join(function(name) for name in (
            "ios_operator_call", "ios_operator_call_with_enum_property", "ios_view_axis",
            "ios_view_orbit", "ios_view_roll_angle")) + r'''
int main() {
  for (const char *name : {"VIEW3D_OT_view_selected", "VIEW3D_OT_view_all", "VIEW3D_OT_localview",
                           "VIEW3D_OT_view_camera", "VIEW3D_OT_view_persportho",
                           "VIEW3D_OT_view_axis", "VIEW3D_OT_view_orbit"}) {
    std::string ownedName(name); // Native messages own their strings; pointer identity is irrelevant.
    ios_operator_call(nullptr, ownedName.c_str());
    assert(context == OpCallContext::InvokeRegionWin);
    ios_operator_call_with_enum_property(nullptr, ownedName.c_str(), "", "");
    assert(context == OpCallContext::InvokeRegionWin);
  }
  for (const char *axis : {"FRONT", "BACK", "LEFT", "RIGHT", "TOP", "BOTTOM"}) {
    ios_view_axis(nullptr, axis);
    assert(context == OpCallContext::InvokeRegionWin && properties.type == axis);
    assert(!properties.align && !properties.relative && allocated == 0);
  }
  for (const char *direction : {"ORBITLEFT", "ORBITRIGHT", "ORBITUP", "ORBITDOWN"}) {
    ios_view_orbit(nullptr, direction);
    assert(context == OpCallContext::InvokeRegionWin && properties.type == direction);
  }
  ios_operator_call_with_enum_property(nullptr, "VIEW3D_OT_view_axis", "type", "TOP");
  assert(context == OpCallContext::InvokeRegionWin && properties.type == "TOP");
  ios_view_roll_angle(nullptr, 0.25f);
  assert(context == OpCallContext::ExecRegionWin && properties.angle == 0.25f);
  ios_operator_call_with_enum_property(nullptr, "OBJECT_OT_duplicate", "", "");
  assert(context == OpCallContext::ExecRegionWin);
  ios_operator_call_with_enum_property(nullptr, "MESH_OT_select_all", "action", "SELECT");
  assert(context == OpCallContext::ExecRegionWin && properties.type == "SELECT");
  const int previous = calls;
  allowed = false;
  ios_view_axis(nullptr, "TOP");
  ios_view_orbit(nullptr, "ORBITLEFT");
  ios_operator_call(nullptr, "VIEW3D_OT_view_selected");
  assert(calls == previous && allocated == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "view_commands.cc"
            executable = Path(directory) / "view_commands"
            code.write_text(harness)
            result = subprocess.run(["c++", "-std=c++17", str(code), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pencil_taps_retarget_axes_without_hover_and_preserve_drag_routing(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        events = (root / "source/blender/windowmanager/intern/wm_event_system.cc").read_text()
        start = events.index("  bool handle_highlight = false;", events.index("static eHandlerActionFlag wm_handlers_do_gizmo_handler("))
        decision = events[start:events.index("  /* There is no need to handle this event", start)]
        axis_source = (root / "source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc").read_text()
        start = axis_source.index("static int gizmo_axis_test_select(")
        hit_test = axis_source[start:axis_source.index("\n}", start) + 2]
        harness = r'''
#include <cassert>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#define WITH_APPLE_CROSSPLATFORM 1
#define APPLE_TARGET_IOS TEST_IOS
#define ISMOUSE_BUTTON(type) ((type) == LEFTMOUSE)
#define ELEM(value, a, b, c) ((value) == (a) || (value) == (b) || (value) == (c))
enum { LEFTMOUSE = 1, MOUSEMOVE, KM_PRESS, KM_RELEASE, KM_CLICK, KM_DBL_CLICK, KM_PRESS_DRAG, KM_NOTHING };
struct wmEvent { int type, val; struct { bool is_motion_absolute; } tablet; };
struct wmGizmo { float matrix_basis[4][4] = {}, matrix_offset[4][4] = {}; float scale_final = 100; };
struct bContext {};
static void sub_v2_v2(float *a, const float *b) { a[0] -= b[0]; a[1] -= b[1]; }
static void mul_v2_fl(float *a, float b) { a[0] *= b; a[1] *= b; }
static float len_squared_v2(const float *a) { return a[0]*a[0] + a[1]*a[1]; }
static float len_squared_v2v2(const float *a, const float *b) {
  return (a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]);
}
struct Decision { bool highlight, keymap; };
static Decision decide(const wmEvent *event, bool modal = false, bool is_event_modifier = false) {
  wmGizmo active;
  struct { wmGizmo *gz_modal; } prev{modal ? &active : nullptr};
  const bool is_event_drag = event->val == KM_PRESS_DRAG;
''' + decision + r'''
  return {handle_highlight, handle_keymap};
}
''' + hit_test + r'''
int main() {
  wmGizmo gizmo;
  const float axes[3][3] = {{0.7071068f, -0.4082483f, 0.5773503f},
                            {-0.7071068f, -0.4082483f, 0.5773503f},
                            {0.0f, 0.8164966f, 0.5773503f}};
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) gizmo.matrix_offset[i][j] = axes[i][j];
  gizmo.matrix_basis[3][0] = 780;
  gizmo.matrix_basis[3][1] = 110;
  for (int part = 1; part <= 6; part++) {
    const int axis = (part - 1) / 2;
    const int sign = part % 2 ? -1 : 1;
    const int mval[2] = {int(780 + axes[axis][0] * sign * 80),
                         int(110 + axes[axis][1] * sign * 80)};
    assert(gizmo_axis_test_select(nullptr, &gizmo, mval) == part);
    for (int value : {KM_PRESS, KM_CLICK, KM_DBL_CLICK}) {
      wmEvent tap{LEFTMOUSE, value, {true}};
      Decision d = decide(&tap);
      assert(d.keymap);
      assert(d.highlight == bool(TEST_IOS));
      if (TEST_IOS) {
        // Both a cold tap and a stale highlight must resolve to the touched axis.
        for (int oldPart : {-1, part % 6 + 1}) {
          int highlighted = oldPart;
          if (d.highlight) highlighted = gizmo_axis_test_select(nullptr, &gizmo, mval);
          assert(highlighted == part);
        }
      }
      Decision modal = decide(&tap, true);
      assert(!modal.highlight && modal.keymap); // Do not switch an active drag to another axis.
      tap.tablet.is_motion_absolute = false;
      d = decide(&tap);
      assert(!d.highlight && d.keymap); // Mouse behavior is unchanged.
    }
  }
  wmEvent hover{MOUSEMOVE, KM_NOTHING, {true}};
  Decision d = decide(&hover);
  assert(d.highlight && !d.keymap); // Hover must never invoke a view change.
  wmEvent drag{LEFTMOUSE, KM_PRESS_DRAG, {true}};
  d = decide(&drag);
  assert(d.highlight && d.keymap); // Existing drag-part routing remains enabled.
  const int outside[2] = {0, 0};
  assert(gizmo_axis_test_select(nullptr, &gizmo, outside) == -1);
  // When aligned with an axis, tapping its center flips to the opposite view.
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) gizmo.matrix_offset[i][j] = i == j;
  const int center[2] = {780, 110};
  assert(gizmo_axis_test_select(nullptr, &gizmo, center) == 5);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "gizmo_tap_test.cc"
            executable = Path(directory) / "gizmo_tap_test"
            source_file.write_text(harness)
            for ios in (0, 1):
                result = subprocess.run([compiler, "-std=c++17", f"-DTEST_IOS={ios}",
                                         str(source_file), "-o", str(executable)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hover_growth_retargeting_exit_and_redraw(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc").read_text()
        start = source.index("struct NavigateRotateGizmo {")
        animation = source[start:source.index("\n#endif", start)]
        bridge = (root / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        start = bridge.index('extern "C" void WM_apple_ios_ui_navigation_hover_set(')
        bridge_methods = bridge[start:bridge.index('static void ios_ui_dispatch_operator(', start)]
        native = (root / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        self.assertIn('view_gizmo_drag.allowedTouchTypes = @[ @(UITouchTypeDirect) ]', native)
        self.assertIn('ios_navigation_redraw_pending.exchange(false)', bridge)
        harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <initializer_list>
struct wmGizmo {};
static std::atomic<bool> ios_navigation_hover_active{false};
static std::atomic<bool> ios_navigation_redraw_pending{false};
''' + animation + bridge_methods + r'''
int main() {
  for (int part = 1; part <= 6; part++) {
    NavigateRotateGizmo axis{};
    double now = 1.0;
    assert(!gizmo_axis_hover_update(&axis, now, -1));
    for (int frame = 0; frame < 60; frame++) {
      const float previous = axis.hover[part - 1];
      now += 1.0 / 120.0;
      gizmo_axis_hover_update(&axis, now, part);
      assert(axis.hover[part - 1] >= previous && axis.hover[part - 1] <= 1.0f);
      if (frame == 0) assert(axis.hover[part - 1] > 0.0f && axis.hover[part - 1] < 0.5f);
      for (int i = 0; i < 6; i++) if (i != part - 1) assert(axis.hover[i] == 0.0f);
    }
    assert(!gizmo_axis_hover_update(&axis, now, part)); // Idle hover must not redraw forever.
    const int next = part % 6 + 1;
    now += 1.0 / 60.0;
    assert(gizmo_axis_hover_update(&axis, now, next));
    assert(axis.hover[part - 1] > 0.0f && axis.hover[part - 1] < 1.0f);
    assert(axis.hover[next - 1] > 0.0f && axis.hover[next - 1] < 1.0f);
    for (int frame = 0; frame < 60; frame++) {
      now += 1.0 / 120.0;
      gizmo_axis_hover_update(&axis, now, -1);
    }
    for (float value : axis.hover) assert(value == 0.0f);
    assert(!gizmo_axis_hover_update(&axis, now, -1));
  }
  // Equivalent elapsed time behaves the same at 60 and 120 Hz.
  NavigateRotateGizmo slow{}, fast{};
  gizmo_axis_hover_update(&slow, 1.0, -1);
  gizmo_axis_hover_update(&fast, 1.0, -1);
  for (int i = 1; i <= 12; i++) gizmo_axis_hover_update(&slow, 1.0 + i / 60.0, 3);
  for (int i = 1; i <= 24; i++) gizmo_axis_hover_update(&fast, 1.0 + i / 120.0, 3);
  assert(std::abs(slow.hover[2] - fast.hover[2]) < 0.0001f);
  WM_apple_ios_ui_navigation_hover_set(true);
  assert(WM_apple_ios_ui_navigation_hover_active());
  assert(ios_navigation_redraw_pending.exchange(false));
  WM_apple_ios_ui_navigation_hover_set(true);
  assert(!ios_navigation_redraw_pending.exchange(false));
  WM_apple_ios_ui_navigation_redraw_request();
  assert(ios_navigation_redraw_pending.exchange(false));
  WM_apple_ios_ui_navigation_hover_set(false);
  assert(!WM_apple_ios_ui_navigation_hover_active());
  assert(ios_navigation_redraw_pending.exchange(false));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "navigation_test.cc"
            executable = Path(directory) / "navigation_test"
            source_file.write_text(harness)
            result = subprocess.run([compiler, "-std=c++17", str(source_file), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
