# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise native hierarchy creation and Undo against the device's Blender scene."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
engine = ctypes.CDLL(None)
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
dialog = probe.blender_device_probe_dialog_snapshot
dialog.restype = ctypes.c_void_p
choose = probe.blender_device_probe_dialog_choose
choose.argtypes = [ctypes.c_char_p]
choose.restype = ctypes.c_bool
probe.blender_device_probe_dialog_start()
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
output = Path.home() / 'Documents/hierarchy-add-test.json'


def decode(ptr):
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def ui(**values):
    return decode(query(json.dumps(values).encode()))


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def menu(route):
    token = request(json.dumps(route).encode(), False)
    for _ in range(100):
        yield 0.1
        result = take(token)
        if result:
            return decode(result)
    raise AssertionError('Native Add menu timed out')


def tests():
    if not ui()['hierarchy']:
        ui(action='hierarchy')
    yield 0.8
    add = ui()['hierarchyAdd']
    check(add['primary'] and add['frame'][2:] == [44, 44], 'Hierarchy has a native 44-point plus menu')
    count = len(bpy.data.collections)
    ui(action='hierarchy_new_collection')
    yield 0.6
    check(decode(dialog())['class'] == 'UIAlertController', 'New Collection opens a native name dialog')
    ui(action='hierarchy_alert_text', value='   ')
    check(not choose(b'Create'), 'Blank collection names disable Create')
    check(any(action['title'] == 'Cancel' for action in decode(dialog())['actions']),
          'Collection dialog offers cancellation')
    ui(action='hierarchy_alert_close')
    yield 0.4
    check(len(bpy.data.collections) == count, 'Cancel does not create a collection')
    ui(action='hierarchy_new_collection')
    yield 0.6
    ui(action='hierarchy_alert_text', value='  Native Add Test  ')
    check(choose(b'Create'), 'Native Create action is connected')
    ui(action='hierarchy_alert_close')
    yield 0.8
    collection = bpy.data.collections.get('Native Add Test')
    check(collection and bpy.context.collection == collection, 'Created collection becomes the active destination')
    check(any(r['id'] == str(collection.session_uid) for r in ui()['rows']),
          'New collection appears in the native hierarchy')
    items = yield from menu({'root': 'VIEW3D_MT_add', 'path': []})
    mesh = next(item for item in items if item.get('menu') == 'VIEW3D_MT_mesh_add')
    check(mesh['icon'] == 'OUTLINER_OB_MESH', 'Add Object projects Blender categories and icons')
    meshes = yield from menu(mesh['route'])
    cube = next(item for item in meshes if item.get('operator') == 'MESH_OT_primitive_cube_add')
    check(cube['icon'] == 'MESH_CUBE', 'Native mesh submenu preserves the Blender Cube command and icon')
    objects = len(bpy.data.objects)
    request(json.dumps(cube['route']).encode(), True)
    yield 0.8
    check(len(bpy.data.objects) == objects + 1 and bpy.context.object.name in collection.objects,
          'Native Add Object creates a cube inside the new collection')
    check(str(bpy.context.object.session_uid) in ui()['selectedObjects'], 'Added object is selected in the hierarchy')
    enqueue(b'ED_OT_undo')
    yield 0.8
    check(len(bpy.data.objects) == objects, 'Object creation undoes in one step')
    enqueue(b'ED_OT_undo')
    yield 0.8
    check(len(bpy.data.collections) == count and not bpy.data.collections.get('Native Add Test'),
          'Collection creation undoes in one step')
    ui(action='hierarchy_add_open')
    report.update(passed=True)


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
