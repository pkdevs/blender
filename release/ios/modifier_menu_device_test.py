# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify native Properties menus against actual modifier creation and Undo."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
engine = ctypes.CDLL(None)
request = engine.WM_apple_ios_ui_menu_request
request.argtypes = [ctypes.c_char_p, ctypes.c_bool]
request.restype = ctypes.c_uint64
take = probe.blender_device_probe_menu_result
take.argtypes = [ctypes.c_uint64]
take.restype = ctypes.c_void_p
enqueue = engine.WM_apple_ios_ui_operator_enqueue
enqueue.argtypes = [ctypes.c_char_p]
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/modifier-menu-test.json'


def consume(pointer):
    try:
        return json.loads(ctypes.string_at(pointer))
    finally:
        free(pointer)


def ui(**values):
    query = probe.blender_device_probe_scene_panels
    query.argtypes = [ctypes.c_char_p]
    query.restype = ctypes.c_void_p
    return consume(query(json.dumps(values).encode()))


def menu(route, execute=False):
    token = request(json.dumps(route).encode(), execute)
    if execute:
        yield 0.3
        return []
    for _ in range(100):
        yield 0.1
        result = take(token)
        if result:
            return consume(result)
    raise AssertionError('Native menu request timed out')


def check(ok, name):
    assert ok, name
    report['checks'].append(name)


def tests():
    ui(action='inspector')
    yield 0.8
    ui(action='tab', id='MODIFIER')
    yield 0.8
    state = ui()
    add = next(p for section in state['properties']['sections'] for p in section['properties']
               if p.get('menu') == 'OBJECT_MT_modifier_add')
    check(add['type'] == 'MENU' and add['tab'] == 'MODIFIER',
          'Blender call-menu button is projected as a native Properties menu')
    check(state['nativeMenus'][add['key']]['primary'] and state['nativeMenus'][add['key']]['icon'],
          'Add Modifier uses an icon-bearing UIKit primary menu')
    root = {'root': add['menu'], 'path': [], 'propertiesTab': add['tab']}
    items = yield from menu(root)
    check({'Edit', 'Generate', 'Deform', 'Normals', 'Physics'} <= {i.get('title') for i in items},
          'Native menu contains Blender mesh modifier categories')
    generate = next(i for i in items if i.get('menu') == 'OBJECT_MT_modifier_add_generate')
    check(generate['route']['propertiesTab'] == 'MODIFIER', 'Submenu keeps the Properties context')
    choices = yield from menu(generate['route'])
    bevel = next(i for i in choices if i.get('title') == 'Bevel')
    check(bevel['icon'] == 'MOD_BEVEL' and bevel['operator'] == 'OBJECT_OT_modifier_add',
          'Bevel preserves Blender icon and actual operator')
    check(any(i.get('operator') == 'OBJECT_OT_modifier_add_node_group' for i in choices),
          'Geometry Nodes asset modifiers remain available')
    check(not any(i.get('operator') == 'WM_OT_call_menu' for i in items + choices),
          'Native categories never invoke a Blender popup')
    count = len(bpy.context.object.modifiers)
    yield from menu(bevel['route'], True)
    yield 0.5
    check(len(bpy.context.object.modifiers) == count + 1 and bpy.context.object.modifiers[-1].type == 'BEVEL',
          'Native menu adds the real Bevel modifier')
    enqueue(b'ED_OT_undo')
    yield 0.7
    check(len(bpy.context.object.modifiers) == count, 'Modifier addition is undone in one step')

    # Each snapshot constructs and destroys instanced panels. Keep processing events
    # between snapshots to expose any animation handler retaining a retired panel.
    for title in ('Bevel', 'Mirror', 'Subdivision Surface'):
        choice = next(i for i in choices if i.get('title') == title)
        for repeat in range(3):
            yield from menu(choice['route'], True)
            yield 0.6
            check(len(bpy.context.object.modifiers) == count + 1,
                  f'{title} addition {repeat + 1} survives repeated Properties snapshots')
            ui(action='inspector')
            ui(action='inspector')
            yield 0.3
            enqueue(b'ED_OT_undo')
            yield 0.6
            check(len(bpy.context.object.modifiers) == count,
                  f'{title} Undo {repeat + 1} safely retires its native property panels')

    cube = bpy.context.object
    cube.select_set(False)
    camera = bpy.data.objects['Camera']
    camera.select_set(True)
    bpy.context.view_layer.objects.active = camera
    yield from menu(bevel['route'], True)
    yield 0.3
    check(not camera.modifiers, 'Stale modifier route cannot add a modifier to an unsupported object')
    camera.select_set(False)
    cube.select_set(True)
    bpy.context.view_layer.objects.active = cube
    yield 0.7
    ui(action='tab', id='MODIFIER')
    yield 0.5
    ui(action='native_menu_open', key=add['key'])
    report.update(passed=True, stage='native-menu-open')


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
