# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native scene-panel round trips on a device. Real Pencil input stays enabled."""

import ctypes
import json
from pathlib import Path
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
engine = ctypes.CDLL(None)
engine.WM_apple_ios_ui_operator_enqueue.argtypes = [ctypes.c_char_p]
report = {'checks': []}
output = Path.home() / 'Documents/scene-panels-test.json'


def ui(**request):
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def tests():
    ui(action='inspector')
    ui(action='hierarchy')
    yield 1.0
    state = ui()
    check(state['inspector'] and state['hierarchy'], 'Both floating panels open on iPad')
    check(len(state['rows']) >= 4, 'Real Blender hierarchy appears')
    check(state['right'][0] < state['left'][0], 'Hierarchy is left and Properties is right')
    check({'OBJECT', 'SCENE', 'DATA'}.issubset({tab['id'] for tab in state['tabs']}),
          'Blender Properties context tabs are projected')
    check(state['expanded'] == [state['sections'][0]], 'Only the first Blender panel starts expanded')
    check('object.location.0' in state['fields'], 'Native inspector exposes the active object transform')
    camera = bpy.data.objects['Camera']
    ui(action='select', id=str(camera.session_uid))
    yield 1.0
    check(bpy.context.view_layer.objects.active == camera, 'Native hierarchy changes the ACTIVE Blender object')
    ui(action='tab', id='DATA')
    yield 1.0
    check('data.lens.' in ui()['fields'], 'Selection refreshes camera-specific native controls')
    old_lens = camera.data.lens
    ui(action='property', key='data.lens.', value='72')
    yield 1.0
    check(bpy.data.objects['Camera'].data.lens == 72, 'Native camera field edits real RNA')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(bpy.data.objects['Camera'].data.lens == old_lens, 'Native inspector edit is undoable')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_redo')
    yield 1.0
    check(bpy.data.objects['Camera'].data.lens == 72, 'Native inspector edit is redoable')
    def panel(identifier):
        return next(item for item in ui()['properties']['sections'] if item['id'] == identifier)

    def header(identifier, prop):
        return next(item for item in panel(identifier)['properties']
                    if item.get('header') and item.get('property') == prop)

    safe_id = 'DATA_PT_camera_safe_areas'
    switch = header(safe_id, 'show_safe_areas')
    check(safe_id not in ui()['expanded'] and switch['editable'],
          'Collapsed panel keeps its native enable switch available')
    ui(action='toggle', key=switch['key'], value=True)
    yield 1.0
    check(bpy.data.objects['Camera'].data.show_safe_areas,
          'Collapsed header switch enables the real Blender setting')
    check(safe_id not in ui()['expanded'], 'Enabling a feature does not expand its panel')
    ui(action='section', id=safe_id)
    yield 1.0
    check('DATA_PT_camera_safe_areas_center_cut' not in ui()['sections'],
          'Nested panels are sections, not outer disclosure panels')
    child_switch = header(safe_id, 'show_safe_center')
    check(child_switch.get('section') == 'Center-Cut Safe Areas',
          'Nested enable switch belongs to its inline section heading')
    ui(action='toggle', key=child_switch['key'], value=True)
    yield 1.0
    check(bpy.data.objects['Camera'].data.show_safe_center,
          'Nested section header switch updates Blender')
    ui(action='toggle', key=switch['key'], value=False)
    yield 1.0
    check(header(safe_id, 'show_safe_areas')['editable'] and
          not header(safe_id, 'show_safe_center')['editable'],
          'Disabled contents do not disable their parent enable switch')
    bpy.data.objects['Camera'].data.show_safe_center = False
    cube = bpy.data.objects['Cube']
    ui(action='select', id=str(cube.session_uid))
    ui(action='tab', id='OBJECT')
    yield 1.0
    ui(action='property', key='object.location.0', value='2')
    yield 1.0
    check(bpy.data.objects['Cube'].location.x == 2, 'Native transform field moves Blender object')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(bpy.data.objects['Cube'].location.x == 0, 'Transform undo restores scene')
    bpy.data.objects['Cube'].location.y = 1
    yield 1.0
    check(ui()['fields']['object.location.1'] == '1', 'External Blender edit updates the native inspector')
    bpy.data.objects['Cube'].location.y = 0
    yield 0.6
    report['state'] = ui()
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
