# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-engine regression tests for the script-free Ellie template and rig controls."""
import importlib.util
import json
import math
from pathlib import Path
import tempfile
import time

import bpy
from bpy_extras import anim_utils

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('native_character_test', ROOT / 'scripts/startup/bl_ui/native_character.py')
controls = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controls)
spec = importlib.util.spec_from_file_location('native_templates_test', ROOT / 'scripts/startup/bl_ui/native_templates.py')
templates = importlib.util.module_from_spec(spec)
spec.loader.exec_module(templates)
for cls in controls.classes:
    bpy.utils.register_class(cls)
path = Path(__file__).resolve().parent / 'library/character-animation.blend'
bpy.ops.wm.open_mainfile(filepath=str(path), use_scripts=False)
rig = bpy.context.object
credit = json.loads(templates.attribution_json())
assert credit['license'] == 'CC-BY-4.0' and credit['author'] and credit['changes']
del bpy.context.scene['asset_attribution']
assert json.loads(templates.attribution_json()) == credit
bpy.context.scene['asset_attribution'] = '{invalid'
assert json.loads(templates.attribution_json()) == credit
rig['asset_attribution'] = '{}'
try:
    templates.attribution_json()
except ValueError:
    pass
else:
    raise AssertionError('Export must not silently drop damaged attribution')
rig['asset_attribution'] = json.dumps(credit)
bpy.context.scene['asset_attribution'] = json.dumps(credit)
assert templates.export_error() == ''
print('PASS: template export preserves credits, including an appended rig')
assert bpy.context.mode == 'POSE'
assert not bpy.data.texts and not bpy.data.libraries
assert not rig.animation_data.action
assert not bpy.app.handlers.frame_change_post and not bpy.app.handlers.depsgraph_update_post
assert all(driver.driver.is_valid for owner in bpy.data.user_map() if getattr(owner, 'animation_data', None)
           for driver in owner.animation_data.drivers)
assert all(driver.driver.is_simple_expression for owner in bpy.data.user_map()
           if getattr(owner, 'animation_data', None) for driver in owner.animation_data.drivers
           if driver.driver.type == 'SCRIPTED')
print('PASS: no scripts, external files, invalid drivers or scripted driver functions')


def mesh_positions(name):
    obj = bpy.data.objects[name].evaluated_get(bpy.context.evaluated_depsgraph_get())
    return [vertex.co.copy() for vertex in obj.data.vertices]


def moved(before, after):
    return max((a - b).length for a, b in zip(before, after)) > 0.001


head = rig.pose.bones['FK-Head']
before = mesh_positions('GEO-ellie_hair')
saved = head.matrix_basis.copy()
head.rotation_euler.y += 0.2
bpy.context.view_layer.update()
assert moved(before, mesh_positions('GEO-ellie_hair'))
head.matrix_basis = saved
print('PASS: mesh hair follows head without frame handlers')

hand = rig.pose.bones['IK-MSTR-Wrist.L']
before = mesh_positions('GEO-ellie_body')
saved = hand.matrix_basis.copy()
hand.location.x += 0.15
bpy.context.view_layer.update()
assert moved(before, mesh_positions('GEO-ellie_body'))
hand.matrix_basis = saved
print('PASS: IK hand control deforms the body')

for side, word in [('L', 'left'), ('R', 'right')]:
    for part, property_bone, end in [('arm', 'UpperArm', 'Wrist'), ('leg', 'Thigh', 'Foot')]:
        prop = f'ik_{word}_{part}'
        owner = 'PRP-' + property_bone + '.' + side
        info = controls.find_setting(rig, owner, prop)
        watched = ['IK-' + property_bone + '.' + side,
                   ('IK-Wrist.' if part == 'arm' else 'IK-Foot.') + side]
        bpy.context.view_layer.update()
        matrices = [rig.pose.bones[name].matrix.copy() for name in watched]
        mesh_before = mesh_positions('GEO-ellie_body')
        assert bpy.ops.pose.native_character_switch(bone=owner, prop=prop, value=0) == {'FINISHED'}
        assert rig.pose.bones[owner][prop] == 0
        print('SNAP OFF', part, side, max((a-b).length for a,b in zip(mesh_before, mesh_positions('GEO-ellie_body'))))
        assert bpy.ops.pose.native_character_switch(bone=owner, prop=prop, value=1) == {'FINISHED'}
        print('SNAP ON', part, side, max((a-b).length for a,b in zip(mesh_before, mesh_positions('GEO-ellie_body'))))
        for name, matrix in zip(watched, matrices):
            error = max(abs(a-b) for row_a, row_b in zip(matrix, rig.pose.bones[name].matrix)
                        for a, b in zip(row_a, row_b))
            assert error < 0.025, (name, error)
print('PASS: all four limbs switch IK/FK without pose jumps')

for bone, prop in controls.PARENT_SETTINGS:
    info = controls.find_setting(rig, bone, prop)
    names = json.loads(info['parent_names'])
    affected = json.loads(info['bones'])
    original = rig.pose.bones[bone][prop]
    matrices = [rig.pose.bones[name].matrix.copy() for name in affected]
    for index in range(len(names)):
        assert bpy.ops.pose.native_character_switch(bone=bone, prop=prop, value=index) == {'FINISHED'}
        for name, matrix in zip(affected, matrices):
            error = max(abs(a-b) for ra, rb in zip(matrix, rig.pose.bones[name].matrix) for a,b in zip(ra,rb))
            assert error < 0.025, (name, index, error)
    bpy.ops.pose.native_character_switch(bone=bone, prop=prop, value=int(original))
print('PASS: every parent-space choice preserves control transforms')

before = mesh_positions('GEO-ellie_head')
assert bpy.ops.pose.native_character_pose(action='face_angry') == {'FINISHED'}
bpy.context.view_layer.update()
assert moved(before, mesh_positions('GEO-ellie_head'))
assert bpy.ops.pose.native_character_pose(action='face_default') == {'FINISHED'}
print('PASS: native pose actions deform the face and restore it')

for bone in rig.pose.bones:
    bone.select = False
hand.select = True
rig.data.bones.active = hand.bone
area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
region = next(region for region in area.regions if region.type == 'WINDOW')
def insert_key():
    with bpy.context.temp_override(area=area, region=region):
        return bpy.ops.anim.keyframe_insert()
assert insert_key() == {'FINISHED'}
assert rig.animation_data.action
bag = anim_utils.action_get_channelbag_for_slot(rig.animation_data.action, rig.animation_data.action_slot)
assert bag and bag.fcurves
bpy.context.scene.frame_set(24)
hand.location.x += 0.1
assert insert_key() == {'FINISHED'}
bpy.context.scene.frame_set(1)
first = hand.location.copy()
bpy.context.scene.frame_set(24)
assert (hand.location - first).length > 0.05
print('PASS: first keyframe, second pose and evaluated animation')

start = time.perf_counter()
for frame in range(1, 49):
    bpy.context.scene.frame_set(frame)
    for value in rig.pose.bones['FK-Head'].matrix:
        assert all(math.isfinite(component) for component in value)
print('METRIC: evaluation milliseconds/frame', (time.perf_counter() - start) / 48 * 1000)
with tempfile.TemporaryDirectory() as directory:
    target = str(Path(directory) / 'character.blend')
    bpy.ops.wm.save_as_mainfile(filepath=target)
    bpy.ops.wm.open_mainfile(filepath=target, use_scripts=False)
    assert bpy.context.mode == 'POSE'
    assert bpy.context.object.animation_data.action
    assert json.loads(bpy.context.object['asset_attribution'])['license'] == 'CC-BY-4.0'
    bpy.context.scene.frame_set(24)
    assert (bpy.context.object.pose.bones['IK-MSTR-Wrist.L'].location - first).length > 0.05
print('PASS: save/reopen retains animation, mode and attribution')
print('PASS: Character Animation')
