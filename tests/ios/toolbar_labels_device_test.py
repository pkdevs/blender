# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native toolbar regression checks in a temporary instrumented iPad build."""

import bpy
import ctypes
import json
import traceback
from pathlib import Path

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
toolbar = probe.blender_device_probe_toolbar_label
toolbar.argtypes = [ctypes.c_int, ctypes.c_int]
toolbar.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/toolbar-labels-test.json'


def state(action=-1, index=-1):
    ptr = toolbar(action, index)
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(condition, label):
    assert condition, label
    report['checks'].append(label)


def active():
    return bpy.context.workspace.tools.from_space_view3d_mode('OBJECT').idname


def tests():
    s = state(0)
    move = next(i for i, t in enumerate(s['tools']) if t['id'] == 'builtin.move')
    select = 0
    width = s['rail'][2]
    s = state(1, move)
    check(s['active'] == 'builtin.move' and not s['expanded'] and s['docked'],
          'Tap selects in docked icon toolbar without expanding')
    check(s['rail'][2] == width and s['label'] == 'Move' and s['alpha'] == 1,
          'Tap shows the name without changing toolbar width')
    yield 0.35
    check(active() == 'builtin.move', 'Native tap changes the real Blender active tool')
    s = state()
    check(s['alpha'] == 1, 'Snapshot refresh does not erase selection confirmation')
    yield 1.0
    check(state()['alpha'] == 0, 'Tap confirmation fades automatically')
    s = state(2, move)
    check(s['label'] == 'Move' and s['alpha'] == 1 and not s['transient'],
          'Hover shows the pointed tool name')
    x, y, w, h = s['bubble']
    check(x > s['rail'][0] + s['rail'][2] and x + w <= s['viewport'][2] and
          y >= 0 and y + h <= s['viewport'][3], 'Label is beside the rail and inside the viewport')
    yield 1.2
    check(state()['alpha'] == 1, 'Stationary hover keeps its label visible')
    state(3)
    yield 0.25
    check(state()['alpha'] == 0, 'Leaving hover dismisses the label')
    state(1, select)
    state(2, move)
    yield 1.2
    check(state()['alpha'] == 1 and state()['label'] == 'Move',
          'Older tap expiry cannot dismiss a newer hover')
    state(3)
    s = state(4)
    width = s['rail'][2]
    s = state(1, move)
    check(s['expanded'] and s['rail'][2] == width,
          'Tap in the labeled toolbar preserves the expanded layout')
    yield 0.35
    check(active() == 'builtin.move', 'Labeled toolbar tap executes the tool')
    state(0)
    state(2, move)
    report['hover'] = state()
    report['passed'] = True


steps = tests()


def tick():
    try:
        delay = next(steps)
    except StopIteration:
        delay = None
    except Exception:
        report['error'] = traceback.format_exc()
        report['failure_state'] = state()
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=5.0)
