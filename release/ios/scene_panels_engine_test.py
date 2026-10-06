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
edit(cube, 'rename', value='  Renamed Cube  ')
check(cube.name == 'Renamed Cube', 'Rename trims the native field')
edit(cube, 'rename', value='  ')
check(cube.name == 'Renamed Cube' and state()['error'], 'Blank names are rejected')
camera.select_set(True)
before = set(bpy.data.objects)
edit(cube, 'duplicate')
added = set(bpy.data.objects) - before
check(len(added) == 1 and not state()['error'], 'Duplicate targets only the swiped row despite multi-selection')
duplicate = added.pop()
check(duplicate.data != cube.data and duplicate.data.vertices[0].co == cube.data.vertices[0].co,
      'Duplicate uses Blender data duplication')
check(bpy.context.active_object == duplicate and duplicate.select_get() and not camera.select_get(),
      'Duplicate becomes the active selected object')
camera.select_set(True)
edit(duplicate, 'delete')
check(set(bpy.data.objects) == before and camera.select_get(), 'Delete targets only one row and preserves other selected objects')
edit(cube, 'visibility', value=True)
actions = next(row['actions'] for row in state()['hierarchy'] if row['id'] == str(cube.session_uid))
check('duplicate' not in actions and {'visibility', 'rename', 'delete'} <= set(actions),
      'Hidden rows expose only applicable actions')
edit(cube, 'duplicate')
check(state()['error'] and set(bpy.data.objects) == before, 'Hidden-object duplication is rejected')
edit(cube, 'visibility', value=False)
edit(cube, 'select')
bpy.ops.object.mode_set(mode='EDIT')
for action in ('delete', 'duplicate', 'rename'):
    edit(camera, action, value='Not renamed')
    check(state()['error'] and bpy.data.objects.get('Camera') == camera, 'Edit Mode guards ' + action)
bpy.ops.object.mode_set(mode='OBJECT')
# Scene-local deletion must retain an object linked into another scene.
other_scene = bpy.data.scenes.new('Other Scene')
other_scene.collection.objects.link(camera)
edit(camera, 'delete')
check(camera.name in other_scene.objects and camera.name not in bpy.context.scene.objects,
      'Delete respects Blender scene-local unlinking')
parent = bpy.context.view_layer.active_layer_collection
edit(cube, 'collection_new', value='  Native Collection  ')
collection = bpy.data.collections.get('Native Collection')
check(collection and collection.name in parent.collection.children and
      bpy.context.collection == collection, 'New collection is nested and becomes the active destination')
bpy.ops.mesh.primitive_cube_add()
check(bpy.context.object.name in collection.objects, 'New objects use the newly created collection')
count = len(bpy.data.collections)
edit(cube, 'collection_new', value='   ')
check(len(bpy.data.collections) == count and state()['error'], 'Blank collection names are rejected')
bpy.ops.object.mode_set(mode='EDIT')
edit(cube, 'collection_new', value='Not Added')
check(len(bpy.data.collections) == count and state()['error'], 'Collection creation is guarded in Edit Mode')
bpy.ops.object.mode_set(mode='OBJECT')
edit(cube, 'collection_new', value='Native Collection')
check(len(bpy.data.collections) == count + 1 and bpy.context.collection.name != collection.name,
      'Blender assigns unique collection names')
print('PASS: native scene panels ' + json.dumps(checks))
