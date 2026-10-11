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

if __name__ == "__main__":
    unittest.main()
