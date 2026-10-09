# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Application-owned controls for the migrated Ellie template.

The rig's authored UI data is data only: no document scripts or operator names
are executed. These ordinary Blender panels also project into native Properties.
"""
import json

import bpy
from bpy.props import IntProperty, StringProperty
from bpy.types import Menu, Operator, Panel


def character(context):
    obj = context.object
    return obj if obj and obj.type == 'ARMATURE' and obj.get('blender_character_rig') == 'ellie-v1' else None


def settings(rig, group):
    data = rig.data.get('ui_data', {}).get(group, {})
    for section, rows in data.items():
        if not hasattr(rows, 'items'):
            continue
        for row in rows.values():
            for title, info in row.items():
                if hasattr(info, 'get') and 'prop_bone' in info and 'prop_id' in info:
                    yield section, title, info


def find_setting(rig, bone, prop):
    for group in rig.data.get('ui_data', {}):
        for _, _, info in settings(rig, group):
            if info['prop_bone'] == bone and info['prop_id'] == prop:
                return info
    raise ValueError('This rig setting is no longer available')


def _key_transform(bone):
    rotation = ('rotation_quaternion' if bone.rotation_mode == 'QUATERNION' else
                'rotation_axis_angle' if bone.rotation_mode == 'AXIS_ANGLE' else 'rotation_euler')
    for path in ('location', rotation, 'scale'):
        bone.keyframe_insert(path, group=bone.name)


def switch_setting(context, rig, info, value):
    """Preserve pose while switching the authored parent or IK/FK mapping.

    Uses CloudRig's published mapping and pole placement, but Blender 5.1 APIs.
    """
    owner = rig.pose.bones[info['prop_bone']]
    prop = info['prop_id']
    previous = owner[prop]
    if previous == value:
        return
    mapping = json.loads(info.get('map_on' if value else 'map_off', '[]'))
    if not mapping:
        mapping = [(name, name) for name in json.loads(info.get('bones', '[]'))]
    if len(mapping) > 128 or any(a not in rig.pose.bones or b not in rig.pose.bones for a, b in mapping):
        raise ValueError('Invalid control mapping')
    context.view_layer.update()
    targets = [(rig.pose.bones[a], rig.pose.bones[b].matrix.copy(), rig.pose.bones[b].scale.copy())
               for a, b in mapping]
    pole_name = info.get('ik_pole')
    if value and pole_name:
        first, last = rig.pose.bones[info['fk_first']], rig.pose.bones[info['fk_last']]
        pole = rig.pose.bones[pole_name]
        matrix = pole.matrix.copy()
        matrix.translation = first.tail + (first.vector - last.vector).normalized() * (
            first.vector.length + last.vector.length) / 2
        targets.append((pole, matrix, pole.scale.copy()))
    old_transforms = [(bone, bone.matrix_basis.copy()) for bone, _, _ in targets]
    roll = rig.pose.bones.get('ROLL-Foot.' + owner.name[-1]) if value and prop.endswith('_leg') else None
    if roll:
        old_transforms.append((roll, roll.matrix_basis.copy()))
    try:
        # The FK foot already includes heel/toe roll. Leaving the old IK roll in
        # place would apply it twice when the foot is snapped back to IK.
        if roll:
            roll.matrix_basis.identity()
        owner[prop] = type(previous)(value)
        rig.update_tag()
        context.view_layer.update()
        for bone, matrix, scale in targets:
            location = bone.location.copy()
            rotation = ('rotation_quaternion' if bone.rotation_mode == 'QUATERNION' else
                        'rotation_axis_angle' if bone.rotation_mode == 'AXIS_ANGLE' else 'rotation_euler')
            angles = tuple(getattr(bone, rotation))
            # Account for the additive constraints used on Ellie's FK foot controls.
            before = rig.convert_space(pose_bone=bone, matrix=bone.matrix_basis,
                                       from_space='LOCAL', to_space='POSE')
            bone.matrix = matrix - (bone.matrix - before)
            for index, locked in enumerate(bone.lock_location):
                if (locked and 'parent_names' not in info) or bone.bone.use_connect:
                    bone.location[index] = location[index]
            locks = tuple(bone.lock_rotation)
            if len(angles) == 4:
                locks = (bone.lock_rotation_w,) + locks if bone.lock_rotations_4d else (all(locks),) * 4
            for index, locked in enumerate(locks):
                if locked and 'parent_names' not in info:
                    getattr(bone, rotation)[index] = angles[index]
            bone.scale = scale
            context.view_layer.update()
            if 'parent_names' in info:
                # Armature constraints (notably the eye target) are multiplicative,
                # unlike the additive FK controls. Solve in pre-constraint space.
                for _ in range(8):
                    if max(abs(a - b) for ra, rb in zip(bone.matrix, matrix)
                           for a, b in zip(ra, rb)) < 1e-5:
                        break
                    local = rig.convert_space(pose_bone=bone, matrix=bone.matrix_basis,
                                              from_space='LOCAL', to_space='POSE')
                    bone.matrix = local @ bone.matrix.inverted_safe() @ matrix
                    context.view_layer.update()
    except Exception:
        owner[prop] = previous
        for bone, matrix in old_transforms:
            bone.matrix_basis = matrix
        rig.update_tag()
        context.view_layer.update()
        raise
    if context.scene.tool_settings.use_keyframe_insert_auto:
        owner.keyframe_insert('[' + json.dumps(prop) + ']', group=owner.name)
        for bone, _, _ in targets:
            _key_transform(bone)
        if roll:
            _key_transform(roll)
    for collection in rig.data.collections:
        if collection.name in {'Controls: IK', 'Controls: FK'}:
            collection.is_visible = True


class POSE_OT_native_character_switch(Operator):
    bl_idname = 'pose.native_character_switch'
    bl_label = 'Switch Rig Control'
    bl_options = {'UNDO', 'INTERNAL'}
    bone: StringProperty()
    prop: StringProperty()
    value: IntProperty(min=0, max=32)

    @classmethod
    def poll(cls, context):
        return character(context) is not None and context.mode == 'POSE'

    def execute(self, context):
        rig = character(context)
        try:
            info = find_setting(rig, self.bone, self.prop)
            names = json.loads(info.get('texts', '[]'))
            limit = len(names) - 1 if names else 1
            if not 0 <= self.value <= limit:
                raise ValueError('Invalid setting value')
            switch_setting(context, rig, info, self.value)
        except (KeyError, TypeError, ValueError, RuntimeError) as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        return {'FINISHED'}


class POSE_OT_native_character_pose(Operator):
    bl_idname = 'pose.native_character_pose'
    bl_label = 'Apply Character Pose'
    bl_options = {'UNDO', 'INTERNAL'}
    action: StringProperty()

    @classmethod
    def poll(cls, context):
        return character(context) is not None and context.mode == 'POSE'

    def execute(self, context):
        rig = character(context)
        action = bpy.data.actions.get(self.action)
        if not action or not action.asset_data or action.library:
            self.report({'ERROR'}, 'Pose asset is no longer available')
            return {'CANCELLED'}
        selected = [bone for bone in rig.pose.bones if bone.select]
        try:
            for bone in selected:
                bone.select = False
            rig.pose.apply_pose_from_action(action, evaluation_time=1)
            if context.scene.tool_settings.use_keyframe_insert_auto:
                paths = {curve.data_path for layer in action.layers for strip in layer.strips
                         for bag in strip.channelbags for curve in bag.fcurves}
                for bone in rig.pose.bones:
                    if any(path.startswith(bone.path_from_id() + '.') for path in paths):
                        _key_transform(bone)
        finally:
            for bone in selected:
                bone.select = True
        return {'FINISHED'}


class CharacterPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'object'
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return character(context) is not None


class OBJECT_PT_character_controls(CharacterPanel, Panel):
    bl_label = 'Character Controls'
    bl_order = -10
    bl_options = set()

    def draw(self, context):
        rig = character(context)
        for collection in rig.data.collections:
            if collection.get('character_control_group'):
                self.layout.prop(collection, 'is_visible', text=collection.name.removeprefix('Controls: '))


class OBJECT_PT_character_appearance(CharacterPanel, Panel):
    bl_label = 'Character Appearance'

    def draw(self, context):
        owner = character(context).pose.bones['Properties_Character_Ellie']
        for key in ('Quality', 'Hairstyle', 'Corrective Smooth', 'Shrinkwrap Mesh'):
            value = owner[key]
            names = owner.get('$' + key)
            title = key + ': ' + names[int(value)] if names and 0 <= value < len(names) else key
            self.layout.prop(owner, '[' + json.dumps(key) + ']', text=title, slider=True)


def draw_group(self, context):
    rig = character(context)
    previous = None
    for section, title, info in settings(rig, self.character_group):
        if section and section != previous:
            self.layout.label(text=section)
        previous = section
        owner = rig.pose.bones.get(info['prop_bone'])
        if not owner or info['prop_id'] not in owner:
            continue
        prop = info['prop_id']
        if 'parent_names' in info:
            menu = PARENT_MENUS.get((info['prop_bone'], prop))
            if menu:
                names = json.loads(info['parent_names'])
                index = int(owner[prop])
                label = names[index] if 0 <= index < len(names) else 'Unknown'
                self.layout.menu(menu, text=title + ': ' + label)
        elif 'map_on' in info:
            op = self.layout.operator('pose.native_character_switch',
                text=title + (': IK' if owner[prop] else ': FK'), icon='FILE_REFRESH')
            op.bone, op.prop, op.value = owner.name, prop, int(not owner[prop])
        else:
            row = self.layout.row(align=True)
            row.prop(owner, '[' + json.dumps(prop) + ']', text=title, slider=True)
            if info.get('bones') and not info.get('texts'):
                op = row.operator('pose.native_character_switch', text='Switch', icon='FILE_REFRESH')
                op.bone, op.prop, op.value = owner.name, prop, int(not owner[prop])


def draw_parent(self, context):
    rig = character(context)
    if not rig:
        return
    bone, prop = self.character_setting
    info = find_setting(rig, bone, prop)
    for index, name in enumerate(json.loads(info['parent_names'])):
        op = self.layout.operator('pose.native_character_switch', text=name,
            icon='RADIOBUT_ON' if rig.pose.bones[bone][prop] == index else 'RADIOBUT_OFF')
        op.bone, op.prop, op.value = bone, prop, index


PARENT_SETTINGS = [('PRP-UpperArm.' + side, 'ik_parents_' + word + '_arm')
                   for side, word in [('L', 'left'), ('R', 'right')]] + [
                  ('PRP-Thigh.' + side, 'ik_parents_' + word + '_leg')
                   for side, word in [('L', 'left'), ('R', 'right')]] + [
                  ('PRP-Head', 'parents_MSTR-TGT-Eyes')] + [
                  (name, 'parents_' + name) for name in (
                      'Fannypack_Zipper_Slider', 'P-Empty1.L', 'P-Empty1.R',
                      'P-Empty2.L', 'P-Empty2.R', 'P-Grab.L', 'P-Grab.R', 'P-Binoculars_Empty1')]
PARENT_MENUS = {setting: 'NATIVE_MT_character_parent_' + str(index)
                for index, setting in enumerate(PARENT_SETTINGS)}


class NATIVE_MT_character_poses(Menu):
    bl_label = 'Pose Library'

    def draw(self, context):
        for action in sorted(bpy.data.actions, key=lambda item: item.name.casefold()):
            if action.asset_data and not action.library:
                op = self.layout.operator('pose.native_character_pose',
                    text=action.name.replace('_', ' ').title(), icon='POSE_HLT')
                op.action = action.name


class OBJECT_PT_character_poses(CharacterPanel, Panel):
    bl_label = 'Pose Library'

    def draw(self, context):
        self.layout.menu('NATIVE_MT_character_poses', icon='POSE_HLT')


classes = (
    POSE_OT_native_character_switch,
    POSE_OT_native_character_pose,
    OBJECT_PT_character_controls,
    OBJECT_PT_character_appearance,
    NATIVE_MT_character_poses,
    OBJECT_PT_character_poses,
) + tuple(type('OBJECT_PT_character_' + identifier, (CharacterPanel, Panel), {
    '__module__': __name__, 'bl_label': title, 'character_group': group, 'draw': draw_group,
}) for identifier, title, group in (
    ('ikfk', 'IK / FK', 'FK/IK Switch'), ('ik', 'Inverse Kinematics', 'IK'),
    ('fk', 'Forward Kinematics', 'FK'), ('face', 'Face', 'Face'),
    ('fingers', 'Fingers', 'Finger IK'), ('clothes', 'Clothes', 'Clothes'),
    ('spaces', 'Parent Spaces', 'Space Switch'),
)) + tuple(type(name, (Menu,), {
    '__module__': __name__, 'bl_label': 'Parent Space', 'character_setting': setting, 'draw': draw_parent,
}) for setting, name in PARENT_MENUS.items())
