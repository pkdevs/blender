# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise native two-axis number scrubbing against live scene geometry."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
enqueue = ctypes.CDLL(None).WM_apple_ios_ui_operator_enqueue
enqueue.argtypes = [ctypes.c_char_p]
report = {'checks': []}
output = Path.home() / 'Documents/number-sensitivity-test.json'


def ui(surface='scene_panels', **request):
    query = getattr(probe, 'blender_device_probe_' + surface)
    query.argtypes = [ctypes.c_char_p]
    query.restype = ctypes.c_void_p
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def location_property():
    return next(p for section in ui()['properties']['sections'] for p in section['properties']
                if p.get('property') == 'location' and p.get('index') == 0)


def tests():
    ui(action='inspector')
    yield 1.0
    prop = location_property()
    start = bpy.context.object.location.x
    step = prop['dragStep']
    report['stage'] = 'properties-idle'
    yield 4.0
    ui(action='scrub_begin', key=prop['key'])
    # Vertical movement must never reinterpret previous horizontal movement.
    samples = ((20, 0, 20), (20, 80, 20), (40, 80, 30),
               (40, -80, 30), (50, -80, 50), (50, 0, 50), (60, 0, 60))
    for horizontal, vertical, expected in samples:
        ui(action='scrub_change', key=prop['key'], translation=horizontal, vertical=vertical)
        yield 0.25
        check(abs(bpy.context.object.location.x - start - expected * step) < 1e-5,
              f'Live Properties value at ({horizontal}, {vertical}) uses {expected} base steps')
    report['stage'] = 'properties-active'
    yield 4.0
    ui(action='scrub_finish', key=prop['key'])
    yield 0.5
    enqueue(b'ED_OT_undo')
    yield 0.7
    check(abs(bpy.context.object.location.x - start) < 1e-5,
          'One Undo restores the complete variable-sensitivity property drag')
    prop = location_property()
    ui(action='scrub_begin', key=prop['key'])
    ui(action='scrub_change', key=prop['key'], translation=20, vertical=-80)
    yield 0.3
    check(abs(bpy.context.object.location.x - start - 40 * step) < 1e-5,
          'Upward precision change doubles the live property adjustment')
    ui(action='scrub_cancel')
    yield 0.5
    check(abs(bpy.context.object.location.x - start) < 1e-5,
          'Cancel restores the property after changing sensitivity')

    enqueue(b'MESH_OT_primitive_cube_add')
    yield 1.0
    prop = next(p for p in ui('redo')['snapshot']['properties'] if p['property'] == 'size')
    count = len(bpy.data.objects)
    step = prop['dragStep']
    ui('redo', action='scrub_begin', key=prop['key'])
    for horizontal, vertical, expected in samples:
        ui('redo', action='scrub_change', key=prop['key'],
           translation=horizontal, vertical=vertical)
        yield 0.3
        check(abs(bpy.context.object.dimensions.x - 2 - expected * step) < 1e-4,
              f'Live redo geometry at ({horizontal}, {vertical}) uses {expected} base steps')
        check(len(bpy.data.objects) == count, 'Redo previews do not duplicate objects')
    report['stage'] = 'redo-active'
    yield 4.0
    ui('redo', action='scrub_finish', key=prop['key'], translation=64, vertical=80)
    yield 0.5
    check(abs(bpy.context.object.dimensions.x - 2 - 62 * step) < 1e-4,
          'Redo release preserves the precise final fine adjustment')
    ui('redo', action='scrub_begin', key=prop['key'])
    ui('redo', action='scrub_change', key=prop['key'], translation=10, vertical=-80)
    yield 0.3
    ui('redo', action='scrub_cancel', key=prop['key'])
    yield 0.5
    check(abs(bpy.context.object.dimensions.x - 2 - 62 * step) < 1e-4,
          'Redo cancellation restores geometry after changing sensitivity')
    enqueue(b'ED_OT_undo')
    yield 0.7
    check(len(bpy.data.objects) == count - 1, 'One Undo removes the adjusted operator')
    report.update(stage='complete', passed=True)


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
