# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Timeline keyframes against real Blender: what the native timeline lists and how keys are selected, moved and deleted."""
import importlib.util
import json
from pathlib import Path
import bpy

path = Path(__file__).resolve().parents[2] / 'scripts/startup/bl_ui/native_timeline.py'
spec = importlib.util.spec_from_file_location('native_timeline_under_test', path)
timeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timeline)
for cls in timeline.classes:
    bpy.utils.register_class(cls)
checks = []


def check(ok, name):
    assert ok, name
    checks.append(name)


def frames():
    return [key["f"] for key in json.loads(timeline.keys_json(bpy.context))]


cube = bpy.data.objects['Cube']
camera = bpy.data.objects['Camera']
check(frames() == [], 'An unanimated scene has no keys')
for frame, x in ((1, 0.0), (10, 5.0), (20, 9.0)):
    cube.location.x = x
    cube.keyframe_insert('location', frame=frame)
camera.keyframe_insert('location', frame=30)
bpy.ops.object.select_all(action='DESELECT')
cube.select_set(True)
bpy.context.view_layer.objects.active = cube
check(frames() == [1, 10, 20], 'Only the selected objects are listed, like Only Show Selected')
camera.select_set(True)
check(frames() == [1, 10, 20, 30], 'Every selected object adds its keys')
camera.select_set(False)


def selected():
    return [key["f"] for key in json.loads(timeline.keys_json(bpy.context)) if key["s"]]


check(bpy.ops.anim.native_key_select(frames="10") == {'FINISHED'} and selected() == [10],
      'Selecting a frame selects only its keys')
check(bpy.ops.anim.native_key_move(delta=5) == {'FINISHED'} and frames() == [1, 15, 20] and selected() == [15],
      'Moving moves the selected keys')
bpy.context.scene.frame_set(15)
check(abs(cube.location.x - 5.0) < 1e-6, 'Moved keys keep their values')
check(bpy.ops.anim.native_key_move(delta=5) == {'FINISHED'} and frames() == [1, 20],
      'A key moved onto another key replaces it')
bpy.context.scene.frame_set(20)
check(abs(cube.location.x - 5.0) < 1e-6, 'The moved key wins, as when moving keys in Blender')
check(bpy.ops.anim.native_key_select(frames="1,20") == {'FINISHED'} and selected() == [1, 20],
      'A box selects several frames')
check(bpy.ops.anim.native_key_move(delta=2) == {'FINISHED'} and frames() == [3, 22],
      'Selected keys move together')
check(bpy.ops.anim.native_key_select(frames="") == {'FINISHED'} and selected() == [],
      'Selecting nothing deselects everything')
check(bpy.ops.anim.native_key_move(delta=1) == {'CANCELLED'}, 'Nothing selected moves nothing')
bpy.ops.anim.native_key_select(frames="3")
check(bpy.ops.anim.native_key_select(frames="22", extend=True) == {'FINISHED'} and selected() == [3, 22],
      'Extending keeps the selection')
bpy.ops.anim.native_key_select(frames="22")
check(bpy.ops.anim.native_key_delete() == {'FINISHED'} and frames() == [3], 'Deleting removes the selected keys')
bpy.ops.anim.native_key_select(frames="3")
check(bpy.ops.anim.native_key_delete() == {'FINISHED'} and frames() == []
      and len(cube.animation_data.action.layers[0].strips[0].channelbags[0].fcurves) == 0,
      'Curves left without keys are removed')
check(bpy.ops.anim.native_key_delete() == {'CANCELLED'}, 'Nothing selected deletes nothing')
check(len(camera.animation_data.action.layers[0].strips[0].channelbags[0].fcurves[0].keyframe_points) == 1,
      'Unselected objects are untouched')
print('PASS: native timeline keys ' + json.dumps(checks))
