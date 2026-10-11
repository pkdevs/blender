# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run in a temporary simulator app with ResponsiveDeviceProbe.dylib."""

import bpy
import ctypes
import json
import traceback
from pathlib import Path

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
gizmo = probe.blender_device_probe_view_gizmo
gizmo.argtypes = [ctypes.c_int, ctypes.c_int]
gizmo.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/view-gizmo-test.json'


def state(action=-1, item=-1):
    ptr = gizmo(action, item)
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def tests():
    s = state(4)
    check(s['enabled'] and s['holds'] == 1, 'Finger and Pencil stationary hold installed')
    s = state(0)
    check(s['feedbackAlpha'] == 1 and not s['visible'], 'Hover feedback before menu')
    yield 0.5
    s = state(1)
    check(s['visible'] and s['feedbackScale'] > 1, 'Stationary hold opens active menu')
    yield 0.5
    s = state(2)
    check(s['feedbackAlpha'] == 1, 'Active feedback survives hover exit')
    check(all(not b['hidden'] for b in s['buttons']), 'All eight view commands visible')
    x, y, w, h = s['safeArea']
    check(all(x <= b['frame'][0] and y <= b['frame'][1] and
              b['frame'][0] + b['frame'][2] <= x + w and
              b['frame'][1] + b['frame'][3] <= y + h
              for b in s['buttons']), 'Menu fits live viewport')
    state(5, 5)
    check(state()['buttons'][5]['selected'], 'Sliding onto Front highlights command')
    state(3, 5)
    yield 0.6
    region = next(a.spaces.active.region_3d for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    # Blender Front looks along -Y with Z up.
    direction = region.view_rotation @ __import__('mathutils').Vector((0, 0, -1))
    check(abs(direction.y - 1) < 0.01 and abs(direction.z) < 0.01,
          'Hold-slide-release executes real Blender Front view')
    s = state()
    check(not s['visible'] and s['feedbackAlpha'] == 0, 'Release clears active feedback')
    state(1)
    state(4)
    state(1)
    yield 0.6
    s = state()
    check(s['visible'] and all(not b['hidden'] for b in s['buttons']),
          'Interrupted hide does not hide reopened menu')
    report['active'] = s
    state(4)
    yield 0.5
    s = state()
    check(not s['visible'] and all(b['hidden'] for b in s['buttons']),
          'Cancellation clears the whole menu')
    state(1)  # Leave the active appearance available for a screenshot.
    report['passed'] = True


steps = tests()


def tick():
    try:
        delay = next(steps)
    except StopIteration:
        delay = None
    except Exception:
        report['error'] = traceback.format_exc()
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=3.0)
