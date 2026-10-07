# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Quick action choices against real Blender scenes. Runs in background mode, so actions that need
a 3D viewport (X-Ray, Extrude) are covered on the device instead."""
import importlib.util
import json
from pathlib import Path
import bpy

path = Path(__file__).resolve().parents[2] / 'scripts/startup/bl_ui/native_actions.py'
spec = importlib.util.spec_from_file_location('native_actions_under_test', path)
actions = importlib.util.module_from_spec(spec)
spec.loader.exec_module(actions)
checks = []


def check(ok, name):
    assert ok, name
    checks.append(name)


def titles(tool="builtin.select_box"):
    return [item["title"] for item in json.loads(actions.actions_json(bpy.context, tool))]


icons = bpy.types.UILayout.bl_rna.functions['prop'].parameters['icon'].enum_items.keys()
for action in actions.ACTIONS:
    check(action.operator().idname(), f"{action.op} exists")
    check(not action.icon or action.icon in icons, f"{action.icon} is a Blender icon")
check(not any("clear" in action.op and "_clear" in action.op for action in actions.ACTIONS if
              action.op.startswith("object.")), "No esoteric object transform clearing")

cube = bpy.data.objects['Cube']
bpy.ops.object.select_all(action='DESELECT')
check(titles()[:1] == ["Select All"] and "Delete" not in titles(), "Nothing selected offers Select All only")
cube.select_set(True)
bpy.context.view_layer.objects.active = cube
# Hide needs a 3D viewport, which background mode does not have.
check(titles()[:2] == ["Duplicate", "Delete"], "A selected object leads with Duplicate and Delete")
check("Shade Smooth" in titles() and "Join" not in titles() and "Parent" not in titles(),
      "One mesh offers shading, not Join or Parent")
check("Shade Smooth" not in titles("builtin.move"), "Shading stays with the selection tools")
check("Apply Scale" not in titles(), "Apply Scale only when the scale is not applied")
cube.scale = (2, 1, 1)
check("Apply Scale" in titles("builtin.scale"), "Unapplied scale offers Apply Scale")
cube.scale = (1, 1, 1)
bpy.ops.mesh.primitive_uv_sphere_add(location=(3, 0, 0))
sphere = bpy.context.active_object
cube.select_set(True)
check("Join" in titles() and "Parent" in titles(), "Two meshes offer Join and Parent")
light = bpy.data.objects['Light']
light.select_set(True)
check("Join" not in titles(), "Join needs objects of one type")
bpy.ops.object.select_all(action='DESELECT')
bpy.ops.object.camera_add()
camera_rule = actions._BY_ID["object.active_camera"].when
check(camera_rule(actions._State(bpy.context, "builtin.select_box")),
      "A camera that is not the scene camera can become active")
bpy.context.scene.camera = bpy.context.active_object
check(not camera_rule(actions._State(bpy.context, "builtin.select_box")), "The scene camera is already active")

bpy.ops.object.select_all(action='DESELECT')
cube.select_set(True)
bpy.context.view_layer.objects.active = cube
bpy.ops.object.mode_set(mode='EDIT')
bpy.context.tool_settings.mesh_select_mode = (True, False, False)
bpy.ops.mesh.select_all(action='DESELECT')
check(titles()[:1] == ["Select All"] and "Delete" not in titles(), "Edit Mode with nothing selected")
bpy.ops.mesh.select_all(action='SELECT')
vertex = titles()
check(vertex[:2] == ["Connect", "Merge"] and "Delete" in vertex and "Inset" not in vertex,
      "Vertices offer Connect and Merge first, no face tools: " + str(vertex))
bpy.context.tool_settings.mesh_select_mode = (False, True, False)
edge = titles()
check(edge[:2] == ["Bevel", "Bridge Loops"] and "Dissolve" in edge and "Connect" not in edge,
      "Edges offer Bevel and Bridge: " + str(edge))
bpy.context.tool_settings.mesh_select_mode = (False, False, True)
face = titles()
check(face[:2] == ["Extrude", "Inset"] and "Separate" in face and "Bevel" not in face and "Merge" not in face,
      "Faces offer Extrude, Inset and Separate: " + str(face))
import bmesh
mesh = bmesh.from_edit_mesh(cube.data)
for face in mesh.faces:
    face.select_set(face.index == 0)
mesh.select_flush_mode()
bmesh.update_edit_mesh(cube.data)
faces = len(cube.data.polygons)
check(actions.run(bpy.context, "mesh.inset"), "Run Inset")
bpy.ops.object.mode_set(mode='OBJECT')
check(len(cube.data.polygons) > faces, "Inset changed the mesh")
check(not actions.run(bpy.context, "mesh.inset"), "A stale action does not run outside its context")

bpy.ops.object.mode_set(mode='SCULPT')
check(titles("builtin_brush.Draw")[:2] == ["Invert Mask", "Clear Mask"], "Sculpt keeps its mask actions")
bpy.ops.object.mode_set(mode='OBJECT')

bpy.ops.object.armature_add()
bpy.ops.object.mode_set(mode='POSE')
check(titles()[:2] == ["Clear Transforms", "Copy Pose"], "Pose Mode offers pose actions")
print('PASS: native quick actions ' + json.dumps(checks))
