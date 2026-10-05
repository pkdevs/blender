# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise the native bridge's undo availability and execution-time recheck."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class IPadUndoTest(unittest.TestCase):
    def test_undo_history_and_modal_guard(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        methods = []
        for signature in ("bool ios_undo_available(", "void ios_operator_invoke("):
            start = source.index(signature)
            methods.append(source[start:source.index("\n}", start) + 2])
        harness = r'''
#include <cassert>
#include <cstring>
#include <vector>
#define STREQ(a, b) (std::strcmp(a, b) == 0)
#define LISTBASE_FOREACH(type, name, list) for (type name : *(list))
enum { WM_HANDLER_TYPE_OP = 1, WM_HANDLER_TYPE_UI = 2 };
struct wmEventHandler { int type; };
struct wmWindow { std::vector<wmEventHandler *> modalhandlers; };
struct bContext { wmWindow *win; bool history; };
namespace blender::wm { enum class OpCallContext { InvokeRegionWin }; }
static int calls = 0;
static wmWindow *CTX_wm_window(bContext *C) { return C->win; }
static bool WM_operator_name_poll(bContext *C, const char *name) {
  return STREQ(name, "ED_OT_undo") ? C->history : true;
}
static void WM_operator_name_call(bContext *, const char *name, blender::wm::OpCallContext,
                                  void *, void *) {
  ++calls;
}
''' + "\n".join(methods) + r'''
int main() {
  wmWindow win;
  bContext C{&win, false};
  assert(!ios_undo_available(&C));
  ios_operator_invoke(&C, "ED_OT_undo");
  assert(calls == 0);
  C.history = true;
  assert(ios_undo_available(&C));
  ios_operator_invoke(&C, "ED_OT_undo");
  assert(calls == 1);
  wmEventHandler tool{WM_HANDLER_TYPE_OP};
  win.modalhandlers.push_back(&tool);
  assert(!ios_undo_available(&C));
  // History was available when the UI snapshot was made, but a tool began before dispatch.
  ios_operator_invoke(&C, "ED_OT_undo");
  assert(calls == 1);
  ios_operator_invoke(&C, "OTHER_OT_operator");
  assert(calls == 2);
  win.modalhandlers.clear();
  wmEventHandler ui{WM_HANDLER_TYPE_UI};
  win.modalhandlers.push_back(&ui);
  assert(ios_undo_available(&C));
  C.history = false;
  ios_operator_invoke(&C, "ED_OT_undo");
  assert(calls == 2);
  C.win = nullptr;
  C.history = true;
  assert(!ios_undo_available(&C));
  ios_operator_invoke(&C, "ED_OT_undo");
  assert(calls == 2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "undo_test.cc"
            executable = Path(directory) / "undo_test"
            source_file.write_text(harness)
            result = subprocess.run([compiler, "-std=c++17", str(source_file), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
