# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native scene-panel round trips on a device. Real Pencil input stays enabled."""

import ctypes
import json
import sys
from pathlib import Path
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
engine = ctypes.CDLL(None)
engine.WM_apple_ios_ui_operator_enqueue.argtypes = [ctypes.c_char_p]
report = {'checks': []}
output = Path.home() / 'Documents/scene-panels-test.json'


def ui(**request):
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def tests():
    ui(action='inspector')
    ui(action='hierarchy')
    yield 1.0
    state = ui()
    check(state['inspector'] and state['hierarchy'], 'Both floating panels open on iPad')
    check(len(state['rows']) >= 4, 'Real Blender hierarchy appears')
    check(state['right'][0] < state['left'][0], 'Hierarchy is left and Properties is right')
    check({'OBJECT', 'SCENE', 'DATA'}.issubset({tab['id'] for tab in state['tabs']}),
          'Blender Properties context tabs are projected')
    check(state['expanded'] == [state['sections'][0]], 'Only the first Blender panel starts expanded')
    check('object.location.0' in state['fields'], 'Native inspector exposes the active object transform')
    transform_identity = state['fieldIdentities']['object.location.0']
    ui(action='section', id='OBJECT_PT_transform')
    check(ui()['sectionRows']['OBJECT_PT_transform'] == 0, 'Property section collapses immediately without waiting for Blender')
    yield 0.7
    ui(action='section', id='OBJECT_PT_transform')
    reopened = ui()
    check(reopened['sectionRows']['OBJECT_PT_transform'] > 0 and
          reopened['fieldIdentities']['object.location.0'] == transform_identity,
          'Cached property section reopens immediately with its original controls')
    yield 0.7
    ui(action='section', id='OBJECT_PT_display')
    yield 0.7
    check(ui()['fieldIdentities']['object.location.0'] == transform_identity,
          'Expanding another section preserves existing native controls')
    ui(action='section', id='OBJECT_PT_display')
    yield 0.7
    for _ in range(10):
        ui(action='section', id='OBJECT_PT_transform')
    check(ui()['sectionRows']['OBJECT_PT_transform'] > 0, 'Rapid animated section toggles leave the table consistent')
    yield 0.7
    for _ in range(6):
        ui(action='inspector')
    yield 0.4
    check(ui()['inspector'], 'Interrupted floating-panel animations settle open correctly')
    def transform_fields():
        return next(section['properties'] for section in ui()['properties']['sections']
                    if section['id'] == 'OBJECT_PT_transform')

    transforms = transform_fields()
    def rotation_heading():
        return next(item for item in transform_fields() if item.get('group_menu'))

    heading = rotation_heading()
    check(heading['group'] == 'Rotation' and heading['key'] in ui()['groupHeadings'],
          'Rotation label is a native long-press menu target')
    check(not any(item.get('property') == 'rotation_mode' for item in transforms),
          'Rotation mode moves into the heading instead of a separate row')
    options = ui(action='group_menu', key=heading['key'])['groupMenu']
    check({item['id'] for item in options} == {'XYZ', 'XZY', 'YXZ', 'YZX', 'ZXY', 'ZYX', 'QUATERNION', 'AXIS_ANGLE'} and
          [item['id'] for item in options if item['checked']] == ['XYZ'],
          'Heading menu exposes all Blender rotation modes with the current mode checked')
    ui(action='group_option', key=heading['key'], value='ZYX')
    yield 1.0
    check(bpy.data.objects['Cube'].rotation_mode == 'ZYX', 'Native Rotation heading changes the Blender rotation mode')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(bpy.data.objects['Cube'].rotation_mode == 'XYZ', 'Rotation heading edits are undoable')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_redo')
    yield 1.0
    check(bpy.data.objects['Cube'].rotation_mode == 'ZYX', 'Rotation heading edits are redoable')
    for order in ['XZY', 'YXZ', 'YZX', 'ZXY', 'XYZ']:
        ui(action='group_option', key=rotation_heading()['key'], value=order)
        yield 0.7
        options = ui(action='group_menu', key=rotation_heading()['key'])['groupMenu']
        check(bpy.data.objects['Cube'].rotation_mode == order and
              [item['id'] for item in options if item['checked']] == [order],
              'Native heading selects and checks Euler order ' + order)
    check(not any(item.get('property') in {'lock_location', 'lock_rotation', 'lock_scale'}
                  for item in transforms), 'Transform locks do not create separate toggle rows')
    for name, lock_name in [('location', 'lock_location'), ('rotation_euler', 'lock_rotation'),
                            ('scale', 'lock_scale')]:
        fields = [item for item in transforms if item.get('property') == name]
        check(len(fields) == 3 and all(item['lock']['property'] == lock_name and
              item['lock']['index'] == item['index'] for item in fields),
              name + ' fields carry their own Blender axis locks')
    location_x = next(item for item in transforms if item.get('property') == 'location' and item['index'] == 0)
    check(ui(action='lock_menu', key=location_x['key'])['lockMenu'] == 'Lock',
          'Native property menu offers Lock for an unlocked axis')
    ui(action='property_lock', key=location_x['key'])
    yield 1.0
    check(tuple(bpy.data.objects['Cube'].lock_location) == (True, False, False),
          'Native Lock action changes only the corresponding Blender axis')
    check(ui(action='lock_menu', key=location_x['key'])['lockMenu'] == 'Unlock',
          'Native property menu updates to Unlock after the snapshot arrives')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(not any(bpy.data.objects['Cube'].lock_location), 'Native property locking is undoable')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_redo')
    yield 1.0
    check(bpy.data.objects['Cube'].lock_location[0], 'Native property locking is redoable')
    ui(action='property_lock', key=location_x['key'])
    yield 1.0
    check(not any(bpy.data.objects['Cube'].lock_location), 'Native Unlock action clears the Blender lock')
    for mode, prop in [('QUATERNION', 'rotation_quaternion'), ('AXIS_ANGLE', 'rotation_axis_angle')]:
        ui(action='group_option', key=rotation_heading()['key'], value=mode)
        yield 1.0
        check(bpy.data.objects['Cube'].rotation_mode == mode and rotation_heading()['group'] == 'Rotation',
              'Native Rotation heading switches to ' + mode)
        fields = [item for item in transform_fields() if item.get('property') == prop]
        check(len(fields) == 4 and all(item['lock']['property'] ==
              ('lock_rotation_w' if item['index'] == 0 else 'lock_rotation') and
              item['lock'].get('index', -1) == item['index'] - 1 for item in fields),
              mode + ' rotation locks map W and XYZ correctly')
        first = next(item for item in fields if item['index'] == 0)
        ui(action='property_lock', key=first['key'])
        yield 1.0
        check(bpy.data.objects['Cube'].lock_rotation_w and not any(bpy.data.objects['Cube'].lock_rotation),
              mode + ' native W lock leaves XYZ independent')
        ui(action='property_lock', key=first['key'])
        yield 1.0
        check(not bpy.data.objects['Cube'].lock_rotation_w, mode + ' native W unlock works')
    ui(action='group_option', key=rotation_heading()['key'], value='XYZ')
    yield 1.0

    def curves(owner, path):
        data = owner.animation_data
        if not data or not data.action or not data.action.layers:
            return []
        bag = data.action.layers[0].strips[0].channelbag(data.action_slot)
        return [curve for curve in bag.fcurves if curve.data_path == path] if bag else []

    def location_curves():
        return curves(bpy.data.objects['Cube'], 'location')

    def animation_titles(key):
        return [item['title'] for item in ui(action='property_menu', key=key)['propertyMenu']]

    key = location_x['key']
    check({'Lock', 'Insert Keyframes', 'Insert Single Keyframe'}.issubset(animation_titles(key)),
          'Native long-press menu combines lock and Blender single/all keyframe actions')
    ui(action='property_animation', key=key, operation='insert_single')
    yield 1.0
    check(len(location_curves()) == 1 and location_curves()[0].array_index == 0 and
          len(location_curves()[0].keyframe_points) == 1,
          'Insert Single Keyframe keys only the pressed axis')
    check({'Replace Single Keyframe', 'Delete Single Keyframe', 'Clear Single Keyframes'}.issubset(animation_titles(key)),
          'Keyed properties offer Blender replace, delete and clear actions')
    ui(action='property', key='object.location.0', value=3)
    yield 1.0
    ui(action='property_animation', key=key, operation='insert_single')
    yield 1.0
    check(location_curves()[0].keyframe_points[0].co.y == 3,
          'Replace Keyframe captures the edited property value')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(location_curves()[0].keyframe_points[0].co.y == 0, 'Native keyframe replacement is undoable')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_redo')
    yield 1.0
    check(location_curves()[0].keyframe_points[0].co.y == 3, 'Native keyframe replacement is redoable')
    bpy.context.scene.frame_set(10)
    yield 1.0
    check('Insert Single Keyframe' in animation_titles(key) and 'Delete Single Keyframe' not in animation_titles(key),
          'Menu refreshes for an animated property without a key on the current frame')
    ui(action='property_animation', key=key, operation='insert_single')
    yield 1.0
    check(len(location_curves()[0].keyframe_points) == 2, 'Native insertion uses the current Blender frame')
    ui(action='property_animation', key=key, operation='delete_single')
    yield 1.0
    check(len(location_curves()[0].keyframe_points) == 1 and location_curves()[0].keyframe_points[0].co.x == 1,
          'Delete Keyframe removes only the key on the current frame')
    ui(action='property_animation', key=key, operation='insert_all')
    yield 1.0
    check({curve.array_index for curve in location_curves()} == {0, 1, 2},
          'Insert Keyframes keys all XYZ components')
    ui(action='property_animation', key=key, operation='clear_single')
    yield 1.0
    check({curve.array_index for curve in location_curves()} == {1, 2},
          'Clear Single Keyframes removes the pressed axis curve and preserves the others')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check({curve.array_index for curve in location_curves()} == {0, 1, 2},
          'Clearing property animation is undoable')
    ui(action='property_animation', key=key, operation='clear_all')
    yield 1.0
    check(not location_curves(), 'Clear Keyframes removes animation for the entire property')
    bpy.context.scene.frame_set(1)
    bpy.data.objects['Cube'].location = (0, 0, 0)
    driver = bpy.data.objects['Cube'].driver_add('scale', 0)
    driver.driver.expression = '1.0'
    yield 1.0
    scale_x = next(item for item in transform_fields() if item.get('property') == 'scale' and item['index'] == 0)
    check(not scale_x.get('animation'), 'Driven properties do not offer inappropriate keyframe actions')
    bpy.data.objects['Cube'].driver_remove('scale', 0)
    yield 1.0
    camera = bpy.data.objects['Camera']
    ui(action='select', id=str(camera.session_uid))
    yield 1.0
    check(bpy.context.view_layer.objects.active == camera, 'Native hierarchy changes the ACTIVE Blender object')
    ui(action='tab', id='DATA')
    yield 1.0
    check('data.lens.' in ui()['fields'], 'Selection refreshes camera-specific native controls')
    old_lens = camera.data.lens
    ui(action='property', key='data.lens.', value='72')
    yield 1.0
    check(bpy.data.objects['Camera'].data.lens == 72, 'Native camera field edits real RNA')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(bpy.data.objects['Camera'].data.lens == old_lens, 'Native inspector edit is undoable')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_redo')
    yield 1.0
    check(bpy.data.objects['Camera'].data.lens == 72, 'Native inspector edit is redoable')
    lens = next(item for section in ui()['properties']['sections'] for item in section['properties']
                if item.get('property') == 'lens')
    check('Insert Keyframe' in animation_titles(lens['key']), 'Scalar property offers native Insert Keyframe')
    ui(action='property_animation', key=lens['key'], operation='insert_all')
    yield 1.0
    check(len(curves(bpy.data.objects['Camera'].data, 'lens')) == 1, 'Native keying targets the property data owner')
    ui(action='property_animation', key=lens['key'], operation='delete_all')
    yield 1.0
    check(not curves(bpy.data.objects['Camera'].data, 'lens'), 'Native scalar Delete Keyframe removes its last key')
    def panel(identifier):
        return next(item for item in ui()['properties']['sections'] if item['id'] == identifier)

    def header(identifier, prop):
        return next(item for item in panel(identifier)['properties']
                    if item.get('header') and item.get('property') == prop)

    safe_id = 'DATA_PT_camera_safe_areas'
    switch = header(safe_id, 'show_safe_areas')
    check(safe_id not in ui()['expanded'] and switch['editable'],
          'Collapsed panel keeps its native enable switch available')
    ui(action='toggle', key=switch['key'], value=True)
    yield 1.0
    check(bpy.data.objects['Camera'].data.show_safe_areas,
          'Collapsed header switch enables the real Blender setting')
    check(safe_id not in ui()['expanded'], 'Enabling a feature does not expand its panel')
    ui(action='section', id=safe_id)
    yield 1.0
    check('DATA_PT_camera_safe_areas_center_cut' not in ui()['sections'],
          'Nested panels are sections, not outer disclosure panels')
    safe_slider = next(item for item in panel(safe_id)['properties']
                       if item.get('property') == 'title' and item.get('index') == 0)
    check('slider' in safe_slider and safe_slider['key'] in ui()['sliders'],
          'Blender vector sliders project as native UISlider controls')
    old_safe = bpy.context.scene.safe_areas.title[0]
    ui(action='slider_begin', key=safe_slider['key'])
    ui(action='slider_change', key=safe_slider['key'], value=0.6)
    yield 0.5
    check(bpy.context.scene.safe_areas.title[0] == old_safe,
          'Slider drag updates native feedback without intermediate undo entries')
    ui(action='slider_finish', key=safe_slider['key'])
    yield 1.0
    expected_safe = safe_slider['slider']['min'] + 0.6 * (safe_slider['slider']['max'] - safe_slider['slider']['min'])
    check(abs(bpy.context.scene.safe_areas.title[0] - expected_safe) < 1e-5,
          'Slider release updates the real Blender property')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(abs(bpy.context.scene.safe_areas.title[0] - old_safe) < 1e-5,
          'A complete slider drag is one undo step')
    ui(action='slider_begin', key=safe_slider['key'])
    ui(action='slider_change', key=safe_slider['key'], value=0.8)
    ui(action='slider_cancel', key=safe_slider['key'])
    yield 0.5
    check(abs(bpy.context.scene.safe_areas.title[0] - old_safe) < 1e-5,
          'Cancelled slider drag leaves Blender unchanged')
    child_switch = header(safe_id, 'show_safe_center')
    check(child_switch.get('section') == 'Center-Cut Safe Areas',
          'Nested enable switch belongs to its inline section heading')
    ui(action='toggle', key=child_switch['key'], value=True)
    yield 1.0
    check(bpy.data.objects['Camera'].data.show_safe_center,
          'Nested section header switch updates Blender')
    ui(action='toggle', key=switch['key'], value=False)
    yield 1.0
    check(header(safe_id, 'show_safe_areas')['editable'] and
          not header(safe_id, 'show_safe_center')['editable'],
          'Disabled contents do not disable their parent enable switch')
    bpy.data.objects['Camera'].data.show_safe_center = False
    ui(action='tab', id='RENDER')
    yield 1.0
    sampling_id = 'RENDER_PT_eevee_sampling'
    if sampling_id not in ui()['expanded']:
        ui(action='section', id=sampling_id)
    yield 1.0
    shadows_key = header(sampling_id, 'use_shadows')['key']
    ui(action='scroll_property', key=shadows_key)
    yield 0.5
    for enabled in (False, True, False):
        ui(action='toggle', key=shadows_key, value=enabled)
        yield 0.7
        switch_state = ui()['switches'][shadows_key]
        check(bpy.context.scene.eevee.use_shadows == enabled and switch_state['on'] == enabled,
              f'Native Shadows switch sets the Blender value to {enabled}')
        check(switch_state['enabled'] and switch_state['hittable'],
              f'Shadows switch remains enabled and touchable when its value is {enabled}')
        rays = next(item for item in panel(sampling_id)['properties']
                    if item.get('property') == 'shadow_ray_count')
        check(bool(rays['editable']) == enabled,
              f'Shadow settings follow the master switch when its value is {enabled}')
    ui(action='section', id=sampling_id)
    yield 0.7
    ui(action='section', id=sampling_id)
    yield 0.7
    ui(action='scroll_property', key=shadows_key)
    yield 0.5
    switch_state = ui()['switches'][shadows_key]
    check(switch_state['enabled'] and switch_state['hittable'] and not switch_state['on'],
          'Reopening Sampling preserves the available Shadows enable switch')
    ui(action='toggle', key=shadows_key, value=True)
    yield 0.7
    check(bpy.context.scene.eevee.use_shadows,
          'Shadows can be re-enabled after reopening the disabled panel')
    cube = bpy.data.objects['Cube']
    ui(action='select', id=str(cube.session_uid))
    ui(action='tab', id='OBJECT')
    yield 1.0
    ui(action='property', key='object.location.0', value='2')
    yield 1.0
    check(bpy.data.objects['Cube'].location.x == 2, 'Native transform field moves Blender object')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(bpy.data.objects['Cube'].location.x == 0, 'Transform undo restores scene')
    bpy.data.objects['Cube'].location.y = 1
    yield 1.0
    check(ui()['fields']['object.location.1'] == '1', 'External Blender edit updates the native inspector')
    bpy.data.objects['Cube'].location.y = 0
    yield 0.6
    ui(action='section', id='OBJECT_PT_display')
    yield 1.0
    color = next(item for item in panel('OBJECT_PT_display')['properties'] if item.get('property') == 'color')
    check(color['type'] == 'COLOR' and color['alpha'] and len(color['value']) == 4,
          'RGBA property projects as one native color swatch')
    original_color = tuple(bpy.data.objects['Cube'].color)
    ui(action='property_animation', key=color['key'], operation='insert_all')
    yield 1.0
    check(len(curves(bpy.data.objects['Cube'], 'color')) == 4, 'Color swatch menu keys the whole RGBA property')
    ui(action='property_animation', key=color['key'], operation='clear_all')
    yield 1.0
    check(not curves(bpy.data.objects['Cube'], 'color'), 'Color swatch menu can clear RGBA animation')
    ui(action='color_open', key=color['key'])
    yield 1.0
    check(ui()['colorPresented'] and ui()['colorAlpha'], 'RGBA swatch presents the native alpha-enabled picker')
    ui(action='color_select', value=[0.5, 0.25, 0.75, 0.4], continuous=True)
    yield 0.4
    check(tuple(bpy.data.objects['Cube'].color) == original_color,
          'Picker drag previews locally without flooding Blender undo')
    ui(action='color_select', value=[0.5, 0.25, 0.75, 0.4], continuous=False)
    yield 1.0
    def linear(value):
        return value / 12.92 if value < 0.04045 else ((value + 0.055) / 1.055) ** 2.4
    expected = [linear(0.5), linear(0.25), linear(0.75), 0.4]
    check(all(abs(a - b) < 1e-5 for a, b in zip(bpy.data.objects['Cube'].color, expected)),
          'Picker commits RGBA atomically with correct scene-linear conversion')
    ui(action='color_close')
    yield 0.5
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    check(all(abs(a - b) < 1e-5 for a, b in zip(bpy.data.objects['Cube'].color, original_color)),
          'One undo restores the complete original color')
    ui(action='select', id=str(bpy.data.objects['Light'].session_uid))
    ui(action='tab', id='DATA')
    yield 1.0
    colors = [item for section in ui()['properties']['sections'] for item in section['properties']
              if item.get('type') == 'COLOR']
    if not colors:
        for identifier in ui()['sections']:
            if identifier not in ui()['expanded'] and 'light' in identifier.lower():
                ui(action='section', id=identifier)
        yield 1.0
        colors = [item for section in ui()['properties']['sections'] for item in section['properties']
                  if item.get('type') == 'COLOR']
    light_color = next(item for item in colors if item['property'] == 'color')
    check(not light_color['alpha'] and len(light_color['value']) == 3,
          'RGB light color uses a swatch without an alpha channel')
    ui(action='color_open', key=light_color['key'])
    yield 1.0
    check(ui()['colorPresented'] and not ui()['colorAlpha'], 'RGB picker hides the opacity control')
    ui(action='color_select', value=[0.25, 0.5, 0.75], continuous=False)
    yield 1.0
    check(all(abs(a - linear(b)) < 1e-5 for a, b in zip(bpy.data.objects['Light'].data.color, [0.25, 0.5, 0.75])),
          'Native RGB picker edits the real light color')
    ui(action='color_close')
    yield 0.5
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1.0
    bpy.types.Scene.native_picker_gamma_test = bpy.props.FloatVectorProperty(
        name='Display Color', size=3, subtype='COLOR_GAMMA', default=(0.1, 0.2, 0.3), min=0, max=1)

    class SCENE_PT_native_picker_test(bpy.types.Panel):
        bl_label = 'Color Picker Test'
        bl_space_type = 'PROPERTIES'
        bl_region_type = 'WINDOW'
        bl_context = 'scene'

        def draw(self, context):
            self.layout.prop(context.scene, 'native_picker_gamma_test')

    bpy.utils.register_class(SCENE_PT_native_picker_test)
    ui(action='tab', id='SCENE')
    yield 1.0
    ui(action='section', id='SCENE_PT_native_picker_test')
    yield 1.0
    gamma = panel('SCENE_PT_native_picker_test')['properties'][0]
    ui(action='color_open', key=gamma['key'])
    yield 1.0
    ui(action='color_select', value=[0.2, 0.4, 0.6], continuous=False)
    yield 1.0
    check(all(abs(a - b) < 1e-5 for a, b in zip(bpy.context.scene.native_picker_gamma_test, [0.2, 0.4, 0.6])),
          'Display-referred color properties are not incorrectly linearized')
    ui(action='color_close')
    yield 0.5
    bpy.utils.unregister_class(SCENE_PT_native_picker_test)
    del bpy.types.Scene.native_picker_gamma_test
    ui(action='select', id=str(bpy.data.objects['Cube'].session_uid))
    ui(action='tab', id='OBJECT')
    yield 1.0
    if '--native-color-preview' in sys.argv:
        color = next(item for item in panel('OBJECT_PT_display')['properties'] if item.get('property') == 'color')
        ui(action='color_open', key=color['key'])
        yield 1.0
        check(ui()['colorPresented'], 'Native picker is visible on the physical iPad')
    bpy.types.Scene.native_slider_factor = bpy.props.FloatProperty(
        name='Amount', subtype='FACTOR', default=0.25, min=0, max=2, soft_min=0, soft_max=1)
    bpy.types.Scene.native_slider_count = bpy.props.IntProperty(
        name='Count', default=2, min=0, max=10)
    bpy.types.Scene.native_slider_angle = bpy.props.FloatProperty(
        name='Angle', subtype='ANGLE', default=0, min=0, max=3.141592653589793)

    class SCENE_PT_native_slider_test(bpy.types.Panel):
        bl_label = 'Native Sliders'
        bl_space_type = 'PROPERTIES'
        bl_region_type = 'WINDOW'
        bl_context = 'scene'

        def draw(self, context):
            self.layout.prop(context.scene, 'native_slider_factor', slider=True)
            self.layout.prop(context.scene, 'native_slider_count', slider=True)
            self.layout.prop(context.scene, 'native_slider_angle', slider=True)

    bpy.utils.register_class(SCENE_PT_native_slider_test)
    ui(action='tab', id='SCENE')
    yield 1.0
    ui(action='section', id='SCENE_PT_native_slider_test')
    yield 1.0
    props = {item['property']: item for item in panel('SCENE_PT_native_slider_test')['properties']}
    check(all('slider' in item for item in props.values()),
          'Float, integer and angle properties retain Blender slider metadata')
    count = props['native_slider_count']
    ui(action='slider_begin', key=count['key'])
    ui(action='slider_change', key=count['key'], value=0.56)
    ui(action='slider_finish', key=count['key'])
    yield 1.0
    check(bpy.context.scene.native_slider_count == 6, 'Native integer slider rounds to whole values')
    angle = props['native_slider_angle']
    ui(action='slider_begin', key=angle['key'])
    ui(action='slider_change', key=angle['key'], value=0.25)
    ui(action='slider_finish', key=angle['key'])
    yield 1.0
    check(abs(bpy.context.scene.native_slider_angle - 3.141592653589793 / 4) < 1e-5,
          'Native angle slider writes radians and displays degrees')
    ui(action='property', key='data.native_slider_factor.', value=1.5)
    yield 1.0
    check(abs(bpy.context.scene.native_slider_factor - 1.5) < 1e-5,
          'Precise numeric entry can exceed the slider soft range within RNA hard limits')
    if '--native-slider-preview' not in sys.argv:
        bpy.utils.unregister_class(SCENE_PT_native_slider_test)
        del bpy.types.Scene.native_slider_factor
        del bpy.types.Scene.native_slider_count
        del bpy.types.Scene.native_slider_angle
        ui(action='tab', id='OBJECT')
        yield 1.0
    else:
        ui(action='scroll_section', id='SCENE_PT_native_slider_test')
        yield 1.0
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
