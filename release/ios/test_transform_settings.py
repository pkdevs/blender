# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify native settings snapshots read the live Blender scene state."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class TransformSettingsTest(unittest.TestCase):
    def test_settings_snapshot_and_object_edit_proportional_state(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        start = source.index("int ios_orientation_value_for_id(")
        helpers = source[start:source.index("std::string ios_mesh_select_mode_string(", start)]
        harness = r'''
#include <cassert>
#include <iostream>
#include <string>
#define SET_FLAG_FROM_TEST(v, test, flag) ((test) ? (v |= flag) : (v &= ~flag))
enum { V3D_ORIENT_GLOBAL, V3D_ORIENT_LOCAL, V3D_ORIENT_NORMAL, V3D_ORIENT_GIMBAL,
       V3D_ORIENT_VIEW, V3D_ORIENT_CURSOR, V3D_ORIENT_PARENT };
enum { V3D_AROUND_CENTER_MEDIAN, V3D_AROUND_CENTER_BOUNDS, V3D_AROUND_CURSOR,
       V3D_AROUND_LOCAL_ORIGINS, V3D_AROUND_ACTIVE };
enum eContextObjectMode { CTX_MODE_OBJECT, CTX_MODE_EDIT_MESH };
using eSnapMode = int;
enum { SCE_SNAP_TO_INCREMENT = 1, SCE_SNAP_TO_GRID = 2, SCE_SNAP_TO_POINT = 4,
       SCE_SNAP_TO_EDGE_ENDPOINT = 8, SCE_SNAP_TO_EDGE = 16, SCE_SNAP_TO_FACE = 32,
       SCE_SNAP_TO_VOLUME = 64, SCE_SNAP = 1, SCE_ORIENT_DEFAULT = 0,
       PROP_EDIT_USE = 1, PROP_EDIT_CONNECTED = 2, PROP_EDIT_PROJECTED = 4 };
struct ToolSettings {
  int snap_flag = 0, snap_mode = SCE_SNAP_TO_INCREMENT, transform_pivot_point = V3D_AROUND_CENTER_MEDIAN;
  int proportional_edit = 0;
  bool proportional_objects = false;
};
struct TransformOrientationSlot { int type = V3D_ORIENT_GLOBAL; };
struct Scene { ToolSettings *toolsettings; TransformOrientationSlot orientation_slots[1]; };
static int BKE_scene_orientation_slot_get_index(const TransformOrientationSlot *slot) { return slot->type; }
''' + helpers + r'''
int main() {
  ToolSettings ts;
  Scene scene{&ts};
  assert(ios_transform_settings_json(nullptr, CTX_MODE_OBJECT) == "{}");
  for (const char *id : {"global", "local", "normal", "gimbal", "view", "cursor", "parent"}) {
    scene.orientation_slots[0].type = ios_orientation_value_for_id(id);
    assert(std::string(ios_orientation_id(scene.orientation_slots[0].type)) == id);
    std::cout << ios_transform_settings_json(&scene, CTX_MODE_OBJECT) << '\n';
  }
  assert(std::string(ios_orientation_id(99)) == "custom");
  for (const char *id : {"median", "bounds", "cursor", "individual", "active"}) {
    ts.transform_pivot_point = ios_pivot_value_for_id(id);
    assert(std::string(ios_pivot_id(ts.transform_pivot_point)) == id);
  }
  for (const char *id : {"increment", "grid", "vertex", "edge", "face", "volume"}) {
    ts.snap_mode = ios_snap_mode_for_id(id);
    ts.snap_flag = 0;
    assert(std::string(ios_snap_id(&ts)) == "off");
    ts.snap_flag = SCE_SNAP;
    assert(std::string(ios_snap_id(&ts)) == id);
  }
  ts.snap_mode = SCE_SNAP_TO_EDGE | SCE_SNAP_TO_FACE;
  assert(std::string(ios_snap_id(&ts)) == "mixed");
  ts.snap_mode = 1 << 20; // A target not exposed by the native menu.
  assert(std::string(ios_snap_id(&ts)) == "mixed");
  for (auto mode : {CTX_MODE_OBJECT, CTX_MODE_EDIT_MESH}) {
    for (const char *id : {"off", "on", "connected", "projected", "off"}) {
      ts.proportional_edit = 0;
      ts.proportional_objects = false;
      ios_proportional_set(&ts, mode, id);
      assert(std::string(ios_proportional_id(&ts, mode)) == id);
      auto other = mode == CTX_MODE_OBJECT ? CTX_MODE_EDIT_MESH : CTX_MODE_OBJECT;
      assert(std::string(ios_proportional_id(&ts, other)) == "off");
    }
  }
  // Read external/keyboard changes, rather than remembering the last native action.
  ts.proportional_objects = true;
  ts.proportional_edit = PROP_EDIT_CONNECTED;
  assert(std::string(ios_proportional_id(&ts, CTX_MODE_OBJECT)) == "connected");
  assert(std::string(ios_proportional_id(&ts, CTX_MODE_EDIT_MESH)) == "off");
  ts.proportional_edit |= PROP_EDIT_USE | PROP_EDIT_PROJECTED;
  assert(std::string(ios_proportional_id(&ts, CTX_MODE_EDIT_MESH)) == "connected");
  scene.orientation_slots[0].type = 99;
  std::cout << ios_transform_settings_json(&scene, CTX_MODE_EDIT_MESH) << '\n';
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "settings.cc"
            executable = Path(directory) / "settings"
            source_file.write_text(harness)
            result = subprocess.run([compiler, "-std=c++17", str(source_file), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            snapshots = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(snapshots), 8)
            self.assertEqual(snapshots[0], dict(orientation="global", pivot="median", snap="off", proportional="off"))
            self.assertEqual(snapshots[-1], dict(orientation="custom", pivot="active", snap="mixed", proportional="connected"))


if __name__ == "__main__":
    unittest.main()
