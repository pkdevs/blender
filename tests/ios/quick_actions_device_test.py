# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise quick-action transitions in a disposable factory-startup QA app."""

import ctypes
import json
import traceback
from pathlib import Path

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
read = probe.blender_device_probe_quick_actions
read.argtypes = [ctypes.c_int]
read.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/quick-actions-test.json'


def state(taps=0):
    ptr = read(taps)
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, name):
    assert ok, name
    report['checks'].append(name)


def tests():
    s = state()
    check(not s['collapsed'] and s['icon'] and s['count'] > 0, 'Starts with actions and a close icon')
    s = state(1)
    check(s['collapsed'] and not s['interactive'], 'Close immediately disables hidden actions')
    yield 0.6
    s = state()
    check(s['hidden'] and s['alpha'] == 0 and s['accessibilityHidden'],
          'Collapsed actions cannot intercept touches or accessibility focus')
    check(s['title'] == 'Actions' and not s['icon'] and 44 <= s['toggle'][2] <= 97,
          'Collapsed control is a small text-only Actions pill')
    bpy.ops.object.mode_set(mode='EDIT')
    yield 0.6
    check(state()['collapsed'] and state()['hidden'], 'Changing modes keeps actions collapsed')
    state(1)
    yield 0.6
    s = state()
    check(not s['collapsed'] and not s['hidden'] and s['interactive'] and s['icon'],
          'Tapping Actions restores the current contextual commands')
    state(4)
    yield 0.6
    s = state()
    check(not s['collapsed'] and not s['hidden'] and s['alpha'] == 1,
          'Rapid reversals do not leave an invisible or unresponsive row')
    bpy.ops.object.mode_set(mode='OBJECT')
    yield 0.6
    state(1)
    yield 0.6
    report['collapsed_state'] = state()
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


bpy.app.timers.register(tick, first_interval=4.0)
