# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise native hierarchy swipe handlers against Blender on a physical device."""
import ctypes
import json
from pathlib import Path
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
dialog = probe.blender_device_probe_dialog_snapshot
dialog.restype = ctypes.c_void_p
choose = probe.blender_device_probe_dialog_choose
choose.argtypes = [ctypes.c_char_p]
choose.restype = ctypes.c_bool
probe.blender_device_probe_dialog_start()
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
engine = ctypes.CDLL(None)
engine.WM_apple_ios_ui_operator_enqueue.argtypes = [ctypes.c_char_p]
report = {'checks': []}
output = Path.home() / 'Documents/hierarchy-swipe-test.json'


def decode(ptr):
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def ui(**request):
    return decode(query(json.dumps(request).encode()))


def swipe(obj, title=None, leading=False):
    return ui(action='hierarchy_swipe', id=str(obj.session_uid), title=title, leading=leading)['swipeActions']


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def tests():
    ui(action='hierarchy')
    yield 1.0
    cube = bpy.data.objects['Cube']
    camera = bpy.data.objects['Camera']
    camera.select_set(True)
    bpy.context.view_layer.objects.active = camera
    yield 0.5
    check([a['title'] for a in swipe(cube, leading=True)] == ['Duplicate', 'Rename'],
          'Leading native swipe offers Duplicate and Rename')
    actions = swipe(cube)
    check([a['title'] for a in actions] == ['Delete', 'Hide'] and actions[0]['destructive'],
          'Trailing native swipe offers destructive Delete and Hide')
    check(not any(a['fullSwipe'] for a in actions + swipe(cube, leading=True)),
          'Full swipes never perform accidental edits')
    before = set(bpy.data.objects)
    swipe(cube, 'Duplicate', True)
    yield 1.0
    added = set(bpy.data.objects) - before
    check(len(added) == 1, 'Native duplicate affects one row despite other selected objects')
    duplicate = added.pop()
    check(duplicate.data != cube.data and bpy.context.active_object == duplicate,
          'Duplicate uses Blender data copying and becomes active')
    check(str(duplicate.session_uid) in ui()['selectedObjects'], 'Native outline selects the new duplicate')
    swipe(duplicate, 'Rename', True)
    yield 0.8
    check(decode(dialog())['class'] == 'UIAlertController', 'Rename presents an Apple native alert')
    ui(action='hierarchy_alert_text', value='   ')
    check(not choose(b'Rename'), 'Blank names disable the native Rename action')
    ui(action='hierarchy_alert_text', value='Swipe Copy')
    check(choose(b'Rename'), 'Native rename confirmation is connected')
    ui(action='hierarchy_alert_close')
    yield 0.8
    check(duplicate.name == 'Swipe Copy', 'Native text field renames the targeted object')
    swipe(duplicate, 'Delete')
    yield 0.8
    alert = decode(dialog())
    check(alert['class'] == 'UIAlertController' and 'Swipe Copy' in alert['title'] and
          {a['title'] for a in alert['actions']} == {'Cancel', 'Delete'},
          'Delete asks for native confirmation naming the object')
    check(choose(b'Cancel'), 'Cancel completes the native swipe without executing Delete')
    ui(action='hierarchy_alert_close')
    yield 0.4
    check(bpy.data.objects.get('Swipe Copy') == duplicate, 'Cancelling delete leaves the object unchanged')
    camera.select_set(True)
    cube.select_set(True)
    swipe(duplicate, 'Delete')
    yield 0.8
    check(choose(b'Delete'), 'Native delete confirmation is connected')
    ui(action='hierarchy_alert_close')
    yield 1.0
    check(not bpy.data.objects.get('Swipe Copy') and bpy.data.objects.get('Camera') and bpy.data.objects.get('Cube'),
          'Confirmed delete removes only the swiped object')
    check(camera.select_get() and cube.select_get(), 'Deleting a row preserves other selected objects')
    engine.WM_apple_ios_ui_operator_enqueue(b'ed.undo')
    yield 1.0
    check(bpy.data.objects.get('Swipe Copy') is not None, 'One undo restores the deleted object')
    engine.WM_apple_ios_ui_operator_enqueue(b'ed.undo')
    yield 1.0
    check(bpy.data.objects.get('Cube.001') is not None and not bpy.data.objects.get('Swipe Copy'),
          'One undo reverses the rename')
    engine.WM_apple_ios_ui_operator_enqueue(b'ed.undo')
    yield 1.0
    check(bpy.data.objects.get('Cube.001') is None, 'One undo reverses the duplicate')
    cube = bpy.data.objects['Cube']
    swipe(cube, 'Hide')
    yield 0.8
    check(cube.hide_get() and [a['title'] for a in swipe(cube)] == ['Delete', 'Show'],
          'Hide updates the native action to Show')
    check([a['title'] for a in swipe(cube, leading=True)] == ['Rename'],
          'Hidden objects do not offer unavailable Duplicate')
    swipe(cube, 'Show')
    yield 0.8
    check(not cube.hide_get(), 'Show restores viewport visibility')
    ui(action='hierarchy_search', value='Camera')
    yield 0.5
    swipe(bpy.data.objects['Camera'], 'Duplicate', True)
    yield 0.8
    check(bpy.data.objects.get('Camera.001') is not None, 'Filtered hierarchy swipes resolve the actual row identity')
    ui(action='hierarchy_search', value='')
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
        report['state'] = ui()
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=5.0)
