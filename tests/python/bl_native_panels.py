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


def collection_row(coll):
    return next(row for row in state()['hierarchy'] if row['kind'] == 'collection' and row['id'] == str(coll.session_uid))


def edit_collection(coll, action, **values):
    panels.apply_json(bpy.context, json.dumps(dict(id=str(coll.session_uid), kind='collection', action=action,
                                                 sequence=len(checks) + 1, **values)))


check(collection_row(bpy.context.scene.collection)['actions'] == [],
      'The scene collection cannot be hidden, renamed or deleted')
check(collection_row(collection)['actions'] == ['visibility', 'rename', 'delete', 'move'],
      'Collections offer swipe actions and can be dragged')
edit_collection(collection, 'visibility', value=True)
check(collection_row(collection)['hidden'] and
      panels._layer_collection(bpy.context.view_layer.layer_collection, collection).hide_viewport,
      'Hide uses the collection viewport toggle')
edit_collection(collection, 'visibility', value=False)
edit_collection(collection, 'rename', value='  Props  ')
check(collection.name == 'Props', 'Rename a collection')
inner = bpy.data.collections.new('Inner')
collection.children.link(inner)
contents = set(collection.objects)
holder = parent.collection
edit_collection(collection, 'delete')
check('Props' not in bpy.data.collections and contents <= set(holder.objects) and inner.name in holder.children,
      'Delete keeps objects and child collections in the parent, like the Outliner')

# Dragging rows: objects into collections or onto other objects, collections into collections.
def request(action, ids, target, kind='object'):
    panels.apply_json(bpy.context, json.dumps(dict(action=action, kind=kind, ids=ids, target=target,
                                                 sequence=len(checks) + 1)))
    return state()['error']


scene_collection = bpy.context.scene.collection
shelf = bpy.data.collections.new('Shelf')
scene_collection.children.link(shelf)
bpy.context.view_layer.update()
check(collection_row(shelf)['accepts'] and collection_row(scene_collection)['accepts'],
      'Collections accept dropped rows')
stand = bpy.data.objects.new('Stand', None)
stand.location = (1, 2, 3)
scene_collection.objects.link(stand)
lamp = bpy.data.objects['Light']
lamp.location = (4, 1, 6)
bpy.context.view_layer.update()
before = lamp.matrix_world.copy()
error = request('move', [str(lamp.session_uid)], str(shelf.session_uid))
check(not error and list(lamp.users_collection) == [shelf], 'Dropping an object on a collection moves it there')
error = request('parent', [str(lamp.session_uid)], str(stand.session_uid))
check(not error and lamp.parent == stand and all(abs(a - b) < 1e-5 for a, b in zip(lamp.matrix_world.translation, before.translation)),
      'Dropping an object on another parents it in place')
rows = state()['hierarchy']
stand_rows = [row for row in rows if row['id'] == str(stand.session_uid)]
lamp_row = next(row for row in rows if row['id'] == str(lamp.session_uid))
check(lamp_row['parent'] in {row['key'] for row in stand_rows},
      'A parented object lists under its parent, sharing its collection')
error = request('parent', [str(stand.session_uid)], str(lamp.session_uid))
check(error and stand.parent is None, 'An object cannot be parented to its own child')
error = request('move', [str(lamp.session_uid)], str(shelf.session_uid))
check(not error and lamp.parent is None and list(lamp.users_collection) == [shelf] and
      all(abs(a - b) < 1e-5 for a, b in zip(lamp.matrix_world.translation, before.translation)),
      'Moving a child out of its parent unparents it in place')
error = request('move', [str(lamp.session_uid), str(stand.session_uid)], str(scene_collection.session_uid))
check(not error and scene_collection in lamp.users_collection and scene_collection in stand.users_collection
      and lamp.name not in shelf.objects, 'Several dragged objects move together')
bpy.ops.ed.undo_push(message='Before nesting')
box = bpy.data.collections.new('Box')
scene_collection.children.link(box)
bpy.context.view_layer.update()
error = request('move', [str(box.session_uid)], str(shelf.session_uid), kind='collection')
check(not error and box.name in shelf.children and box.name not in scene_collection.children,
      'Dropping a collection on another nests it')
error = request('move', [str(shelf.session_uid)], str(box.session_uid), kind='collection')
check(error and shelf.name in scene_collection.children, 'A collection cannot move into its own child')

# The selection's swipe and menu actions, as one step each.
both = [str(lamp.session_uid), str(stand.session_uid)]
request_ids = lambda action, **values: panels.apply_json(bpy.context, json.dumps(dict(
    action=action, ids=both, sequence=len(checks) + 1, **values)))
request_ids('visibility', value=True)
check(lamp.hide_get() and stand.hide_get(), 'Hide several objects')
request_ids('visibility', value=False)
count = len(bpy.data.objects)
request_ids('duplicate')
check(len(bpy.data.objects) == count + 2 and not state()['error'], 'Duplicate several objects')
request_ids('delete')
check('Light' not in bpy.data.objects and 'Stand' not in bpy.data.objects and not state()['error'],
      'Delete several objects')
print('PASS: native scene panels ' + json.dumps(checks))
