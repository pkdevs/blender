# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check Pencil-origin command dispatch and deferred tool cancellation."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class IPadRadialBridgeTest(unittest.TestCase):
    def test_projected_menu_routes_and_modal_deferral(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        start = source.index("struct IOSMenuRequest {")
        state = source[start:source.index("using IOSUIMessage", start)]
        state += "GHOST_IOSMessageQueue<IOSMenuRequest> ios_ui_messages;\n"
        state += "GHOST_IOSMessageQueue<std::pair<uint64_t, std::string>> ios_menu_completed;\n"
        state += "std::map<uint64_t, std::string> ios_menu_results;\n"
        state += "std::atomic<uint64_t> ios_menu_next_identifier{0};\n"
        start = source.index('extern "C" uint64_t WM_apple_ios_ui_menu_request(')
        enqueue = source[start:source.index("/* UIKit only exchanges", start)]
        start = source.index("static void ios_ui_dispatch_menu(")
        dispatch = source[start:source.index("\n}\n", start) + 2]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include "GHOST_IOSMessageQueue.hh"
#define LISTBASE_FOREACH(type, name, list) for (type name : *(list))
enum { WM_HANDLER_TYPE_OP = 1, WM_HANDLER_TYPE_UI = 2 };
struct wmEventHandler { int type; };
struct wmEvent { int xy[2] = {}; };
struct WindowRuntime { std::vector<std::reference_wrapper<wmEventHandler>> modalhandlers; wmEvent *eventstate; };
struct wmWindow { WindowRuntime *runtime; };
struct bContext { wmWindow *win; bool region = true; };
struct IOSContextStore {};
static int contextDepth = 0;
static wmWindow *CTX_wm_window(bContext *C) { return C->win; }
static void *CTX_wm_area(bContext *C) { return C->region ? C : nullptr; }
static void *CTX_wm_region(bContext *C) { return CTX_wm_area(C); }
static IOSContextStore ios_context_push_view3d(bContext *) { contextDepth++; return {}; }
static void ios_context_pop(bContext *, IOSContextStore) { contextDepth--; }
static void wm_cursor_position_from_ghost_client_coords(wmWindow *, int *x, int *y) {
  *x *= 2; *y = (999 - *y) * 2;
}
static int calls, resultX, resultY;
static bool executed;
static std::string received;
namespace blender::ui {
static std::string native_menu_query(bContext *C, const std::string &route, bool execute) {
  calls++; received = route; executed = execute;
  resultX = C->win->runtime->eventstate->xy[0]; resultY = C->win->runtime->eventstate->xy[1];
  return "projected menu";
}
}
static std::mutex ios_ui_command_mutex;
''' + state + enqueue + dispatch + r'''
static void flush(bContext *C) {
  for (auto &request : ios_ui_messages.take()) {
    ios_ui_dispatch_menu(C, request);
  }
  assert(contextDepth == 0);
  for (auto &result : ios_menu_completed.take()) {
    ios_menu_results.emplace(result.first, std::move(result.second));
  }
}
int main() {
  wmEvent event;
  WindowRuntime runtime{{}, &event};
  wmWindow win{&runtime};
  bContext C{&win};
  const char *root = R"({"root":"context","path":[]})";
  const char *leaf = R"({"root":"VIEW3D_MT_object_context_menu","path":["origin","cursor"]})";
  auto id = WM_apple_ios_ui_menu_at_position_request(root, false, 120, 340);
  event.xy[0] = event.xy[1] = 888;
  flush(&C);
  assert(calls == 1 && !executed && received == root && ios_menu_results.at(id) == "projected menu");
  assert(resultX == 240 && resultY == 1318);
  wmEventHandler tool{WM_HANDLER_TYPE_OP};
  win.runtime->modalhandlers.push_back(std::ref(tool));
  id = WM_apple_ios_ui_menu_at_position_request(leaf, true, 45, 67);
  flush(&C);
  auto pending = ios_ui_messages.take();
  assert(calls == 1 && pending.size() == 1 && pending[0].identifier == id);
  ios_ui_messages.push(std::move(pending[0]));
  win.runtime->modalhandlers.clear();
  flush(&C);
  assert(calls == 2 && executed && received == leaf && !ios_menu_results.count(id));
  assert(resultX == 90 && resultY == 1864);
  win.runtime->modalhandlers.push_back(std::ref(tool));
  id = WM_apple_ios_ui_menu_at_position_request(root, false, 1, 2);
  flush(&C); flush(&C); flush(&C);
  assert(calls == 2 && ios_ui_messages.take().empty() && ios_menu_results.at(id) == "[]");
  id = WM_apple_ios_ui_menu_at_position_request(leaf, true, 1, 2);
  flush(&C); flush(&C); flush(&C);
  assert(calls == 2 && ios_ui_messages.take().empty() && !ios_menu_results.count(id));
  win.runtime->modalhandlers.clear();
  wmEventHandler ui{WM_HANDLER_TYPE_UI};
  win.runtime->modalhandlers.push_back(std::ref(ui));
  WM_apple_ios_ui_menu_at_position_request(leaf, true, 1, 2);
  flush(&C);
  assert(calls == 3 && executed && resultX == 2 && resultY == 1994);
  // The menu bar must not move the cursor or wait for modal cancellation.
  win.runtime->modalhandlers.push_back(std::ref(tool));
  WM_apple_ios_ui_menu_request(root, false);
  flush(&C);
  assert(calls == 4 && resultX == 2 && resultY == 1994);
  win.runtime->modalhandlers.clear();
  C.region = false;
  id = WM_apple_ios_ui_menu_at_position_request(root, false, 1, 2);
  flush(&C);
  assert(calls == 4 && ios_menu_results.at(id) == "[]");
  C.win = nullptr;
  id = WM_apple_ios_ui_menu_at_position_request(root, false, 1, 2);
  flush(&C);
  assert(calls == 4 && ios_menu_results.at(id) == "[]");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "menu_bridge.cc"
            executable = Path(directory) / "menu_bridge"
            source_file.write_text(harness)
            result = subprocess.run([compiler, "-std=c++17", "-I", str(root / "intern/ghost"),
                                    str(source_file), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_anchor_coordinates_and_modal_deferral(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        start = source.index("struct IOSOperatorCommand {")
        command = source[start:source.index("\n};", start) + 3]
        start = source.index('extern "C" void WM_apple_ios_ui_operator_at_position_enqueue(')
        enqueue = source[start:source.index("\n}", start) + 2]
        start = source.index("static void ios_ui_dispatch_operator(")
        dispatch = source[start:source.index("\n}\n", start) + 2]
        harness = r'''
#include <cassert>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include "GHOST_IOSMessageQueue.hh"
#define LISTBASE_FOREACH(type, name, list) for (type name : *(list))
enum { WM_HANDLER_TYPE_OP = 1, WM_HANDLER_TYPE_UI = 2 };
struct wmEventHandler { int type; };
struct wmEvent { int xy[2] = {}; };
struct WindowRuntime { std::vector<std::reference_wrapper<wmEventHandler>> modalhandlers; wmEvent *eventstate; };
struct wmWindow { WindowRuntime *runtime; };
struct bContext { wmWindow *win; bool region = true; };
struct IOSContextStore {};
static wmWindow *CTX_wm_window(bContext *C) { return C->win; }
static void *CTX_wm_area(bContext *C) { return C->region ? C : nullptr; }
static void *CTX_wm_region(bContext *C) { return CTX_wm_area(C); }
static IOSContextStore ios_context_push_view3d(bContext *) { return {}; }
static void ios_context_pop(bContext *, IOSContextStore) {}
static void wm_cursor_position_from_ghost_client_coords(wmWindow *, int *x, int *y) {
  *x *= 2; *y = (999 - *y) * 2;
}
static int calls, resultX, resultY;
static void ios_operator_invoke(bContext *C, const char *) {
  calls++; resultX = C->win->runtime->eventstate->xy[0]; resultY = C->win->runtime->eventstate->xy[1];
}
''' + command + r'''
static std::mutex ios_ui_command_mutex;
static GHOST_IOSMessageQueue<IOSOperatorCommand> ios_ui_messages;
''' + enqueue + dispatch + r'''
static void flush(bContext *C) {
  for (auto &command : ios_ui_messages.take()) {
    ios_ui_dispatch_operator(C, command);
  }
}
int main() {
  wmEvent event;
  WindowRuntime runtime{{}, &event};
  wmWindow win{&runtime};
  bContext C{&win};
  WM_apple_ios_ui_operator_at_position_enqueue(nullptr, 0, 0);
  WM_apple_ios_ui_operator_at_position_enqueue("", 0, 0);
  assert(ios_ui_messages.take().empty());
  WM_apple_ios_ui_operator_at_position_enqueue("TRANSFORM_OT_translate", 120, 340);
  event.xy[0] = event.xy[1] = 888; // Menu selection must not replace its opening point.
  flush(&C);
  assert(calls == 1 && resultX == 240 && resultY == 1318);
  wmEventHandler tool{WM_HANDLER_TYPE_OP};
  win.runtime->modalhandlers.push_back(std::ref(tool));
  WM_apple_ios_ui_operator_at_position_enqueue("MESH_OT_bevel", 45, 67);
  flush(&C);
  auto pending = ios_ui_messages.take();
  assert(calls == 1 && pending.size() == 1);
  ios_ui_messages.push(std::move(pending[0]));
  win.runtime->modalhandlers.clear(); // Queued Escape/release has now cancelled the previous tool.
  flush(&C);
  assert(calls == 2 && resultX == 90 && resultY == 1864);
  win.runtime->modalhandlers.push_back(std::ref(tool));
  WM_apple_ios_ui_operator_at_position_enqueue("MESH_OT_bevel", 45, 67);
  flush(&C); flush(&C); flush(&C);
  assert(calls == 2 && ios_ui_messages.take().empty()); // Never build an unbounded retry queue.
  win.runtime->modalhandlers.clear();
  wmEventHandler ui{WM_HANDLER_TYPE_UI};
  win.runtime->modalhandlers.push_back(std::ref(ui));
  WM_apple_ios_ui_operator_at_position_enqueue("VIEW3D_OT_view_selected", 1, 2);
  flush(&C);
  assert(calls == 3 && resultX == 2 && resultY == 1994);
  C.region = false;
  WM_apple_ios_ui_operator_at_position_enqueue("MESH_OT_bevel", 45, 67);
  flush(&C);
  assert(calls == 3 && ios_ui_messages.take().empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "radial_bridge.cc"
            executable = Path(directory) / "radial_bridge"
            source_file.write_text(harness)
            result = subprocess.run([compiler, "-std=c++17", "-I", str(root / "intern/ghost"),
                                    str(source_file), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
