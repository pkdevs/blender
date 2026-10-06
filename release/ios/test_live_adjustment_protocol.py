# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production preview/commit undo boundaries without a GPU or device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LiveAdjustmentProtocolTest(unittest.TestCase):
    def test_preview_commit_cancel_and_interruption(self):
        source = (ROOT / 'source/blender/editors/interface/interface_native_properties.cc').read_text()
        state = source[source.index('struct NativePropertyInteraction'):source.index('bool set_property(')]
        start = source.index('  if (push_undo) {', source.index('bool set_property('))
        commit = source[start:source.index('  WM_event_add_notifier', start)]
        start = source.index('  if (execute) {', source.index('std::string native_properties_query('))
        guard = source[start:source.index('  const bool shading', start)]
        guard = guard.replace('return "";', 'return false;')
        harness = r'''
#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
struct bContext {};
struct Request {
  std::map<std::string, std::string> values;
  std::optional<std::string> lookup_str(const char *key) const {
    auto it = values.find(key);
    return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
};
static double current = 0;
static std::vector<double> history{0};
static void ED_undo_push(bContext *, const char *) { history.push_back(current); }
static bool RNA_struct_undo_check(int) { return true; }
static const char *RNA_property_ui_name(int) { return "Location"; }
''' + state + r'''
static bool edit(bContext *C, const Request &request, double value) {
  bool execute = true, push_undo = true;
  auto *query = &request;
''' + guard + r'''
  current = value;
  struct Pointer { bool owner_id = true; int type = 0; } pointer, *ptr = &pointer;
  int prop = 0;
''' + commit + r'''
  return true;
}
static void reset() { current = 0; history = {0}; native_property_interaction = {}; }
static void undo() { history.pop_back(); current = history.back(); }
static Request phase(const char *token, const char *name) {
  return {{{"interaction", token}, {"phase", name}}};
}
int main() {
  bContext C;
  for (int i = 1; i <= 100; ++i) { assert(edit(&C, phase("a", "update"), i)); }
  assert(current == 100 && history.size() == 1);
  assert(edit(&C, phase("a", "end"), 100.125));
  assert(history.size() == 2 && current == 100.125);
  undo(); assert(current == 0);
  reset();
  edit(&C, phase("a", "update"), 3);
  edit(&C, phase("a", "cancel"), 0);
  assert(current == 0 && history.size() == 1);
  assert(!edit(&C, phase("a", "update"), 9));
  reset();
  assert(!edit(&C, phase("a", "cancel"), 9));
  assert(current == 0 && history.size() == 1);
  edit(&C, phase("a", "end"), 7);
  assert(current == 7 && history.size() == 2);
  reset();
  edit(&C, phase("a", "update"), 2);
  edit(&C, phase("b", "update"), 4);
  edit(&C, phase("b", "end"), 5);
  assert(history == std::vector<double>({0, 2, 5}));
  undo(); assert(current == 2);
  reset();
  edit(&C, phase("a", "update"), 2);
  finish_property_interaction(&C);
  assert(!edit(&C, phase("a", "end"), 10));
  assert(current == 2 && history.size() == 2);
  edit(&C, Request{}, 4);
  assert(history == std::vector<double>({0, 2, 4}));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / 'live-protocol.cc'
            executable = Path(directory) / 'live-protocol'
            code.write_text(harness)
            result = subprocess.run(['xcrun', 'clang++', '-std=c++17', str(code), '-o', str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
