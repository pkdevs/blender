# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise the production live-transform constraint API and modal-handler lookup."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class AxisConstraintTest(unittest.TestCase):
    def test_toggle_state_orientation_macros_and_lifecycle(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/editors/transform/transform_ops.cc").read_text()
        start = source.index("static TransInfo *modal_constraint_data(")
        methods = source[start:source.index("static void transform_cancel(", start)]
        bridge = (root / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        start = bridge.index("wmOperator *ios_constraint_operator(")
        lookup = bridge[start:bridge.index("\n}", start) + 2]
        harness = r'''
#include <cassert>
#include <string>
#include <vector>
#define ELEM(a, b, c) ((a) == (b) || (a) == (c))
#define IFACE_(s) s
#define LISTBASE_FOREACH(type, name, list) for (type name : *(list))
enum { SPACE_VIEW3D = 1, T_NO_CONSTRAINT = 1, T_2D_EDIT = 2,
       TRANS_STARTING = 0, TRANS_RUNNING = 1, TRANS_CONFIRM = 2, TRANS_CANCEL = 3,
       CON_APPLY = 1, CON_AXIS0 = 2, CON_AXIS1 = 4, CON_AXIS2 = 8,
       MOD_CONSTRAINT_SELECT_AXIS = 1, MOD_CONSTRAINT_SELECT_PLANE = 2,
       O_DEFAULT = 0, O_SCENE = 1, O_LOCAL = 2, TREDRAW_HARD = 3,
       WM_HANDLER_TYPE_OP = 1, WM_HANDLER_TYPE_UI = 2 };
struct bContext {};
struct TransInfo {
  int spacetype = SPACE_VIEW3D, flag = 0, state = TRANS_RUNNING;
  struct { int mode = 0; } con;
  int modifiers = 0, orient_curr = O_DEFAULT, redraw = 0;
  bContext *context = nullptr;
};
struct wmOperatorType { struct { void *first = nullptr; } macro; void (*modal)(); };
struct wmOperator { wmOperatorType *type; void *customdata; wmOperator *opm = nullptr; };
struct wmEventHandler { int type; };
struct wmEventHandler_Op : wmEventHandler { wmOperator *op; };
struct wmWindow { std::vector<wmEventHandler *> modalhandlers; };
static void transform_modal() {}
static void unrelated_modal() {}
static int applies = 0, gizmos = 0;
static std::string constraint_text;
static void transform_orientations_current_set(TransInfo *t, int orientation) {
  assert(t->context);
  t->orient_curr = orientation;
}
static void stopConstraint(TransInfo *t) {
  assert(t->context);
  // Blender retains the axis bits, but clears CON_APPLY.
  t->con.mode &= ~CON_APPLY;
  t->orient_curr = O_DEFAULT;
}
static void setUserConstraint(TransInfo *t, int mode, const char *text) {
  assert(t->context);
  t->con.mode = mode | CON_APPLY;
  constraint_text = text;
}
static void transform_gizmo_3d_model_from_constraint_and_mode_set(TransInfo *) { ++gizmos; }
static void transformApply(bContext *C, TransInfo *t) {
  assert(t->redraw == TREDRAW_HARD && t->context == C);
  ++applies;
  t->redraw = 0;
  t->context = nullptr;
}
namespace blender::ed::transform {
''' + methods + "}\n" + lookup + r'''
using namespace blender::ed::transform;
int main() {
  bContext C;
  wmOperatorType type{{}, transform_modal}, other{{}, unrelated_modal};
  TransInfo t;
  wmOperator op{&type, &t};
  int axes = -1;
  assert(modal_constraint_get(&op, axes) && axes == 0);
  // Test every axis in every constraint state, including keyboard-created planes.
  for (int mask = 0; mask < 8; mask++) {
    for (int axis = 0; axis < 3; axis++) {
      t.con.mode = mask ? CON_APPLY | (mask << 1) : 0;
      t.orient_curr = mask ? O_LOCAL : O_DEFAULT;
      t.modifiers = MOD_CONSTRAINT_SELECT_AXIS | MOD_CONSTRAINT_SELECT_PLANE;
      assert(modal_constraint_get(&op, axes) && axes == mask);
      const int expected = (mask ^ (1 << axis)) == 7 ? 0 : (mask ^ (1 << axis));
      assert(modal_constraint_toggle(&C, &op, axis));
      assert(modal_constraint_get(&op, axes) && axes == expected);
      assert(t.modifiers == 0 && !t.context);
      assert(t.orient_curr == (expected ? (mask ? O_LOCAL : O_SCENE) : O_DEFAULT));
    }
  }
  assert(applies == 24 && gizmos == applies);
  // X on -> X off, without an intermediate switch to local coordinates.
  t.con.mode = 0;
  t.orient_curr = O_DEFAULT;
  assert(modal_constraint_toggle(&C, &op, 0));
  assert(modal_constraint_get(&op, axes) && axes == 1);
  assert(modal_constraint_toggle(&C, &op, 0));
  assert(modal_constraint_get(&op, axes) && axes == 0);
  assert(t.con.mode & CON_AXIS0); // No false highlight from inactive, retained bits.
  // Planar toggle and description (XY then remove X -> Y).
  modal_constraint_toggle(&C, &op, 0);
  modal_constraint_toggle(&C, &op, 1);
  assert(modal_constraint_get(&op, axes) && axes == 3);
  assert(constraint_text == "locking %s Z");
  modal_constraint_toggle(&C, &op, 0);
  assert(modal_constraint_get(&op, axes) && axes == 2);
  assert(constraint_text == "along %s Y");
  // Macro parent/child back-pointers must not recurse forever.
  wmOperatorType macro_type{{&type}, unrelated_modal};
  wmOperator macro{&macro_type, nullptr, &op};
  op.opm = &macro;
  assert(modal_constraint_get(&macro, axes) && axes == 2);
  modal_constraint_toggle(&C, &macro, 1);
  assert(modal_constraint_get(&macro, axes) && axes == 0);
  wmEventHandler_Op handler{{WM_HANDLER_TYPE_OP}, &macro};
  wmEventHandler ui{WM_HANDLER_TYPE_UI};
  wmWindow win{{&ui, &handler}};
  assert(ios_constraint_operator(&win) == &macro);
  assert(!ios_constraint_operator(nullptr));
  const int before = applies;
  assert(!modal_constraint_toggle(&C, &op, -1));
  assert(!modal_constraint_toggle(&C, &op, 3));
  for (int flag : {T_NO_CONSTRAINT, T_2D_EDIT}) {
    t.flag = flag;
    assert(!modal_constraint_get(&op, axes));
    assert(!modal_constraint_toggle(&C, &op, 0));
    assert(!ios_constraint_operator(&win));
  }
  t.flag = 0;
  for (int state : {TRANS_CONFIRM, TRANS_CANCEL}) {
    t.state = state;
    assert(!modal_constraint_toggle(&C, &op, 0));
  }
  t.state = TRANS_RUNNING;
  t.spacetype = -1;
  assert(!modal_constraint_toggle(&C, &op, 0));
  t.spacetype = SPACE_VIEW3D;
  op.type = &other;
  assert(!modal_constraint_get(&op, axes));
  assert(!modal_constraint_toggle(&C, &macro, 0));
  op.type = &type;
  op.customdata = nullptr; // Confirm/cancel between publishing state and draining the queue.
  assert(!modal_constraint_toggle(&C, &macro, 0));
  assert(!ios_constraint_operator(&win));
  assert(!modal_constraint_toggle(&C, nullptr, 0));
  macro.opm = nullptr;
  assert(!modal_constraint_get(&macro, axes));
  assert(applies == before);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "axis_constraints.cc"
            executable = Path(directory) / "axis_constraints"
            source_file.write_text(harness)
            result = subprocess.run([compiler, "-std=c++17", str(source_file), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
