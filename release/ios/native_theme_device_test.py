# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the app accent on native controls in light and dark appearances."""
import ctypes
import json
from pathlib import Path
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
theme = probe.blender_device_probe_theme
theme.argtypes = [ctypes.c_int]
theme.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/native-theme-test.json'


def decode(ptr):
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def ui(**request):
    return decode(query(json.dumps(request).encode()))


def check(ok, message):
    assert ok, message
    report['checks'].append(message)


def tests():
    ui(action='hierarchy')
    for name, style in [('light', 1), ('dark', 2)]:
        decode(theme(style))
        yield 1.0
        # UIKit deliberately dims toolbar tint behind a modal popover.
        toolbar = decode(theme(-1))
        check(toolbar['window'] and toolbar['toolbar'], name + ': window and toolbar use blue')
        ui(action='shading_open', scope='shading')
        yield 1.5
        state = ui(scope='shading')
        fields = [field for section in state['properties']['sections'] for field in section['properties']]
        world = next(field for field in fields if field.get('property') == 'use_world_space_lighting')
        ui(action='toggle', scope='shading', key=world['key'], value=True)
        yield 0.5
        colors = decode(theme(-1))
        report[name] = colors
        for kind in ('switches', 'segments', 'selectedRows'):
            check(colors[kind] and all(colors[kind]), name + ': ' + kind + ' use blue')
        report['stage'] = name
        yield 20.0  # Leave a stable appearance for screenshot inspection.
        ui(action='shading_close', scope='shading')
        yield 2.0
    decode(theme(0))
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


bpy.app.timers.register(tick, first_interval=5.0)
