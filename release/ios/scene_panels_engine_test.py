# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Hierarchy tests against real Blender. Native Properties is tested on the device."""
import importlib.util
import json
from pathlib import Path
import bpy

path = Path(__file__).resolve().parents[2] / 'scripts/startup/bl_ui/native_panels.py'
spec = importlib.util.spec_from_file_location('scene_panels_under_test', path)
panels = importlib.util.module_from_spec(spec)
spec.loader.exec_module(panels)
bpy.utils.register_class(panels.WM_OT_native_panel_edit)
checks = []


def check(ok, name):
    assert ok, name
    checks.append(name)


def state(mask=2):
    return json.loads(panels.snapshot_json(bpy.context, mask))


def edit(obj, action, **values):
    panels.apply_json(bpy.context, json.dumps(dict(id=str(obj.session_uid), action=action,
                                                 sequence=len(checks) + 1, **values)))


cube = bpy.context.active_object
camera = bpy.data.objects['Camera']
check(len([row for row in state()['hierarchy'] if row['kind'] == 'object']) == 3, 'Real scene objects')
check('hierarchy' not in state(0), 'Hidden hierarchy skips projection')
edit(camera, 'select')
check(bpy.context.active_object == camera and camera.select_get() and not cube.select_get(), 'Active object selection')
edit(cube, 'select', extend=True, value=True)
check(bpy.context.active_object == cube and camera.select_get() and cube.select_get(), 'Native multi-selection adds an active object')
edit(cube, 'select', extend=True, value=False)
check(bpy.context.active_object == camera and camera.select_get() and not cube.select_get(), 'Native deselection preserves remaining selection')
edit(cube, 'visibility', value=True)
check(cube.hide_get(), 'Hide object')
edit(cube, 'select')
check(bpy.context.active_object == camera and state()['error'], 'Hidden object cannot be selected')
edit(cube, 'visibility', value=False)
check(not cube.hide_get(), 'Show object')
cube.hide_select = True
edit(cube, 'select')
check(bpy.context.active_object == camera and state()['error'], 'Selection lock respected')
cube.hide_select = False
child = bpy.data.objects.new('Nested "quoted"', None)
bpy.context.collection.objects.link(child)
child.parent = cube
rows = state()['hierarchy']
parent = next(row for row in rows if row['id'] == str(cube.session_uid))
nested = next(row for row in rows if row['id'] == str(child.session_uid))
check(nested['parent'] == parent['key'] and parent['children'], 'Parent tree and disclosure state')
uid = str(child.session_uid)
bpy.data.objects.remove(child, do_unlink=True)
panels.apply_json(bpy.context, json.dumps(dict(id=uid, sequence=999, action='select')))
check(state()['ack'] == 999 and state()['error'], 'Deleted target is rejected and acknowledged')
edit(cube, 'select')
bpy.ops.object.mode_set(mode='EDIT')
edit(camera, 'select')
check(bpy.context.active_object == cube and state()['error'], 'Edit Mode guarded')
bpy.ops.object.mode_set(mode='OBJECT')
panels.apply_json(bpy.context, json.dumps(dict(id=str(camera.session_uid), sequence=1000, action='select')), True)
check(state()['error'] and bpy.context.active_object == cube, 'Native dialog blocks edits')
print('PASS: native scene panels ' + json.dumps(checks))
