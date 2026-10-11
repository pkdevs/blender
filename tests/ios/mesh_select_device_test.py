# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the native mesh selector against real Blender selection modes."""

import bpy
import ctypes
import json
import traceback
from pathlib import Path

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
select = probe.blender_device_probe_mesh_select
select.argtypes = [ctypes.c_int]
select.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/mesh-select-test.json'


def state(index=-1):
    ptr = select(index)
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, name):
    assert ok, name
    report['checks'].append(name)


def tests():
    check(state()['hidden'], 'Hidden outside Mesh Edit Mode')
    bpy.ops.object.mode_set(mode='EDIT')
    yield 0.6
    s = state()
    check(not s['hidden'] and s['segments'] == 3 and not s['momentary'],
          'Edit Mode shows three persistent native segments')
    check(s['nextToMode'], 'Selector immediately follows the mode picker')
    check(s['names'] == ['Vertex Select', 'Edge Select', 'Face Select'],
          'All segments have accessible names')
    for index, name in enumerate(('Vertex', 'Edge', 'Face')):
        check(state(index)['selected'] == index, f'{name} highlights immediately')
        yield 0.5
        check(tuple(bpy.context.tool_settings.mesh_select_mode) == tuple(i == index for i in range(3)),
              f'{name} selects exactly the correct Blender component mode')
        check(state()['selected'] == index, f'{name} indicator matches the live snapshot')
    bpy.context.tool_settings.mesh_select_mode = (False, True, False)
    yield 0.5
    check(state()['selected'] == 1, 'External Blender changes update the native indicator')
    bpy.ops.object.mode_set(mode='OBJECT')
    yield 0.5
    check(state()['hidden'], 'Leaving Edit Mode removes the selector')
    bpy.ops.object.mode_set(mode='EDIT')
    yield 0.5
    report['edit_state'] = state()
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
