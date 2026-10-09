# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Adapt the publicly released Ellie rig without executing its embedded Python.

blender --background --factory-startup --disable-autoexec --python build_character.py
The upstream file is kept separately in sources; only library is installed in the app.
"""
import hashlib
import json
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / 'sources/ellie.blend'
LIBRARY = ROOT / 'library'
SOURCE_SHA256 = '3dd235218725a9e68890073bbeacb4820f9b34db65d44f80c31bb88696a3c54c'
CREDIT = {
    'title': 'Ellie',
    'author': 'Blender Studio / Blender Foundation',
    'license': 'CC-BY-4.0',
    'licenseURL': 'https://creativecommons.org/licenses/by/4.0/',
    'sourceURL': 'https://studio.blender.org/characters/ellie/v1/',
    'sourceSHA256': SOURCE_SHA256,
    'changes': ('Adapted for Blender 5.1 and iPad: organized control collections, repaired finger '
                'pole drivers, used the original rigged mesh hair in viewport and render, removed '
                'legacy scripts and unused particle hair, and added native rig controls and template staging.'),
}


def build():
    if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise RuntimeError('Ellie source differs from the reviewed public release')
    bpy.ops.wm.read_factory_settings(use_empty=True)
    with bpy.data.libraries.load(str(SOURCE), link=False) as (source, target):
        target.collections = ['CH-ellie']
        # Facial and hand poses are independent assets, not dependencies of the rig.
        target.actions = [name for name in source.actions if name.startswith(
            ('face_', 'eyemask_', 'hand_', 'hands_', 'mouth_', 'Mouth_', 'ponytail_', 'pose_'))]
    character = target.collections[0]
    bpy.context.scene.collection.children.link(character)
    rig = bpy.data.objects['RIG-Ellie']
    rig['blender_character_rig'] = 'ellie-v1'
    rig['asset_attribution'] = json.dumps(CREDIT, ensure_ascii=True)
    bpy.context.scene['asset_attribution'] = json.dumps(CREDIT, ensure_ascii=True)

    # Mesh hair is an original, armature-deformed alternative provided with Ellie.
    # The particle version depends on handlers in the old document, so do not ship it.
    for obj in list(bpy.data.objects):
        if obj.name.startswith('META-') or obj.particle_systems or '.scalp-' in obj.name:
            bpy.data.objects.remove(obj, do_unlink=True)
    for obj in bpy.data.objects:
        if obj.type == 'MESH' and obj.name in {'GEO-ellie_hair', 'GEO-ellie_hair_cut'}:
            obj.hide_render = obj.hide_viewport
            driver = obj.driver_add('hide_render').driver
            driver.type = 'SCRIPTED'
            driver.expression = 'hairstyle != ' + ('1' if obj.name.endswith('_cut') else '0')
            variable = driver.variables.new()
            variable.name = 'hairstyle'
            variable.type = 'SINGLE_PROP'
            variable.targets[0].id = rig
            variable.targets[0].data_path = 'pose.bones["Properties_Character_Ellie"]["Hairstyle"]'
        if obj != rig:
            obj.hide_select = True
    settings = rig.pose.bones['Properties_Character_Ellie']
    settings['Particle Hair'] = 0
    settings['Quality'] = 1
    # The public rig references these missing properties in twenty finger drivers.
    for side, word in [('L', 'left'), ('R', 'right')]:
        owner = rig.pose.bones['PRP-UpperArm.' + side]
        for finger in ('index', 'middle', 'ring', 'pinky', 'thumb'):
            key = f'ik_pole_follow_{word}_finger_{finger}1'
            owner[key] = 0.0
            owner.id_properties_ui(key).update(min=0, max=1, default=0)
    for curve in rig.animation_data.drivers:
        curve.driver.expression = curve.driver.expression

    # Preserve the authored layer membership and names using modern bone collections.
    layers = rig.data['rigify_layers']
    for collection in rig.data.collections:
        collection.is_visible = False
        if collection.name.startswith('Layer '):
            index = int(collection.name.split()[-1]) - 1
            name = layers[index].get('name', '')
            if name and not name.startswith('$'):
                collection.name = 'Controls: ' + name
                collection['character_control_group'] = True
                collection.is_visible = name == 'IK'
    # Head, neck and gaze should be available without the dense facial controls.
    basics = rig.data.collections.new('Controls: Head')
    basics['character_control_group'] = True
    for name in ('FK-Head', 'FK-Neck', 'MSTR-TGT-Eyes'):
        basics.assign(rig.data.bones[name])
    basics.is_visible = True
    for bone in rig.pose.bones:
        bone.select = False
        # Remove obsolete pointers to document-provided UI machinery.
        for key in ('bone_gizmo', 'enable_bone_gizmo'):
            if key in bone:
                del bone[key]
    for key in ('script', 'hair_script', 'rigify_rig_ui', 'rigify_target_rig'):
        if key in rig.data:
            del rig.data[key]
    for text in list(bpy.data.texts):
        bpy.data.texts.remove(text)
    bpy.data.orphans_purge(do_recursive=True)
    for library in list(bpy.data.libraries):
        bpy.data.libraries.remove(library)
    bpy.context.view_layer.objects.active = rig
    rig.hide_set(False)
    rig.select_set(True)
    rig.show_in_front = True
    rig.animation_data.action = None
    bpy.ops.object.mode_set(mode='POSE')
    # A relaxed, welcoming pose using the author's supplied pose assets.
    for name in ('pose_wave', 'ponytail_default', 'face_default'):
        rig.pose.apply_pose_from_action(bpy.data.actions[name], evaluation_time=1)
    rig.data.bones.active = rig.data.bones['MSTR-Spine_Torso']
    rig.pose.bones['MSTR-Spine_Torso'].select = True

    scene = bpy.context.scene
    scene['blender_ipad_template'] = 'character-animation'
    scene.frame_start, scene.frame_end = 1, 120
    scene.frame_set(1)
    scene.render.fps = 24
    scene.tool_settings.use_keyframe_insert_auto = False
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 32
    scene.world = bpy.data.worlds.new('Studio')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.18, 0.18, 0.18, 1)
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.5
    bpy.context.view_layer.update()
    focus = Vector((0, 0, 0.78))
    camera_data = bpy.data.cameras.new('Camera')
    camera = bpy.data.objects.new('Camera', camera_data)
    scene.collection.objects.link(camera)
    camera.location = (2.5, -6, 1.9)
    camera.rotation_euler = (focus - camera.location).to_track_quat('-Z', 'Y').to_euler()
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 2.7
    camera.hide_set(True)
    scene.camera = camera
    for name, location, energy, size in (
        ('Key', (2, -4, 4), 450, 4), ('Fill', (-3, -2, 2), 200, 3), ('Rim', (1, 3, 3), 350, 3),
    ):
        data = bpy.data.lights.new(name, 'AREA')
        data.energy, data.shape, data.size = energy, 'DISK', size
        obj = bpy.data.objects.new(name, data)
        scene.collection.objects.link(obj)
        obj.location = location
        obj.rotation_euler = (focus - obj.location).to_track_quat('-Z', 'Y').to_euler()
        obj.hide_set(True)
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                space = area.spaces.active
                space.shading.type = 'SOLID'
                space.shading.light = 'STUDIO'
                space.shading.color_type = 'MATERIAL'
                space.overlay.show_relationship_lines = False
                space.region_3d.view_location = focus
                space.region_3d.view_distance = 3.2
                space.region_3d.view_rotation = camera.rotation_euler.to_quaternion()
                space.region_3d.view_perspective = 'PERSP'
    scene.render.resolution_x, scene.render.resolution_y = 640, 420
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    bpy.ops.file.pack_all()
    assert not bpy.data.texts and not bpy.data.libraries
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=str(LIBRARY / 'character-animation.blend'), compress=True)
    scene.render.filepath = str(LIBRARY / 'character-animation.png')
    bpy.ops.render.render(write_still=True)
    return dict(id='character-animation', title='Character Animation',
                file='character-animation.blend', preview='character-animation.png',
                minimumBlender=[5, 1, 0], presentation=dict(timeline=True), attribution=CREDIT)


if __name__ == '__main__':
    entry = build()
    path = LIBRARY / 'manifest.json'
    manifest = json.loads(path.read_text())
    manifest['templates'] = [item for item in manifest['templates'] if item['id'] != entry['id']] + [entry]
    path.write_text(json.dumps(manifest, indent=2) + '\n')
