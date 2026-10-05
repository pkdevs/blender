# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Minimal real-engine native undo/redo regression; run only in a factory QA scene."""
import bpy
import ctypes
import json
import traceback
from pathlib import Path

engine = ctypes.CDLL(None)
engine.WM_apple_ios_ui_operator_enqueue.argtypes = [ctypes.c_char_p]
engine.WM_apple_ios_ui_operator_enum_enqueue.argtypes = [ctypes.c_char_p] * 3
output = Path.home() / 'Documents/undo-device-test.json'
report = {'stage': 0}


def tick():
    try:
        stage = report['stage']
        report['objects'] = [(o.name, o.type, len(o.material_slots)) for o in bpy.context.scene.objects]
        if stage == 0:
            report['initial_count'] = len(bpy.context.scene.objects)
            engine.WM_apple_ios_ui_operator_enqueue(b'MESH_OT_primitive_cube_add')
        elif stage == 1:
            assert len(bpy.context.scene.objects) == report['initial_count'] + 1
            engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
        elif stage == 2:
            assert len(bpy.context.scene.objects) == report['initial_count']
            engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_redo')
        else:
            assert len(bpy.context.scene.objects) == report['initial_count'] + 1
            report['passed'] = True
        report['stage'] += 1
    except Exception:
        report['error'] = traceback.format_exc()
    output.write_text(json.dumps(report, indent=2))
    return None if report.get('passed') or report.get('error') else 1


bpy.app.timers.register(tick, first_interval=4)
