# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify live native edits against Blender geometry, cancellation, and undo."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
engine = ctypes.CDLL(None)
enqueue = engine.WM_apple_ios_ui_operator_enqueue
enqueue.argtypes = [ctypes.c_char_p]
setting = engine.WM_apple_ios_ui_setting_enqueue
setting.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
report = {'checks': []}
output = Path.home() / 'Documents/live-adjustments-test.json'


def ui(surface='scene_panels', **request):
    query = getattr(probe, 'blender_device_probe_' + surface)
    query.argtypes = [ctypes.c_char_p]
    query.restype = ctypes.c_void_p
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def property_named(name, index=None):
    return next(p for section in ui()['properties']['sections'] for p in section['properties']
                if p.get('property') == name and (index is None or p.get('index') == index))


def tests():
    ui(action='inspector')
    yield 1.0
    location = property_named('location', 0)
    start = bpy.context.object.location.x
    report['stage'] = 'before-move'
    yield 4.0
    ui(action='scrub_begin', key=location['key'])
    for distance in (10, 30, 50):
        ui(action='scrub_change', key=location['key'], translation=distance)
        yield 0.25
        check(abs(bpy.context.object.location.x - start - distance * location['dragStep']) < 1e-5,
              f'Viewport object moves during the drag at {distance} points')
        check(ui()['scrubs'][location['key']]['active'], 'Preview snapshots preserve the active gesture')
    report['stage'] = 'live-move'
    yield 4.0
    ui(action='scrub_finish', key=location['key'])
    yield 0.4
    enqueue(b'ED_OT_undo')
    yield 0.7
    check(abs(bpy.context.object.location.x - start) < 1e-5, 'One Undo restores the entire number drag')
    location = property_named('location', 0)
    ui(action='scrub_begin', key=location['key'])
    ui(action='scrub_change', key=location['key'], translation=-30)
    yield 0.3
    check(bpy.context.object.location.x < start, 'Cancelled drag first previews in the real scene')
    ui(action='scrub_cancel')
    yield 0.4
    check(abs(bpy.context.object.location.x - start) < 1e-5, 'Cancellation restores the real scene')

    # Native color drags change the scene while the system picker stays open.
    ui(action='section', id='OBJECT_PT_display')
    yield 0.7
    color = property_named('color')
    original_color = tuple(bpy.context.object.color)
    ui(action='color_open', key=color['key'])
    yield 0.5
    ui(action='color_select', value=[0.2, 0.4, 0.6, 0.8], continuous=True)
    yield 0.4
    check(abs(bpy.context.object.color[3] - 0.8) < 1e-5 and ui()['colorPresented'],
          'Color picker previews in Blender before the gesture finishes')
    ui(action='color_select', value=[0.3, 0.5, 0.7, 0.9], continuous=False)
    ui(action='color_close')
    yield 0.5
    enqueue(b'ED_OT_undo')
    yield 0.7
    check(all(abs(a - b) < 1e-5 for a, b in zip(bpy.context.object.color, original_color)),
          'One Undo restores a continuous color gesture')

    ui(scope='shading', action='shading_open')
    yield 0.7
    shading_state = ui(scope='shading')
    shadows = next(p for section in shading_state['properties']['sections'] for p in section['properties']
                   if p.get('property') == 'show_shadows')
    ui(scope='shading', action='toggle', key=shadows['key'], value=True)
    yield 0.5
    shading_state = ui(scope='shading')
    shadow = next(p for section in shading_state['properties']['sections'] for p in section['properties']
                  if p.get('property') == 'shadow_intensity')
    shading = next(a.spaces.active.shading for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    original_shadow = shading.shadow_intensity
    check(shading_state['sliders'][shadow['key']]['enabled'], 'Native shadows switch enables the slider')
    ui(scope='shading', action='slider_begin', key=shadow['key'])
    ui(scope='shading', action='slider_change', key=shadow['key'], value=0.8)
    yield 0.4
    check(abs(shading.shadow_intensity - 0.8) < 1e-5, 'Shading slider changes the viewport while held')
    ui(scope='shading', action='slider_cancel', key=shadow['key'])
    yield 0.4
    check(abs(shading.shadow_intensity - original_shadow) < 1e-5, 'Slider cancellation restores viewport shading')
    ui(scope='shading', action='shading_close')
    yield 0.3

    # Exercise the last-value queue under an engine-side producer burst.
    request = dict(property_named('location', 0), id=ui()['properties']['id'],
                   action='property', interaction='live-burst', phase='update')
    for index in range(200):
        request.update(value=index / 100, sequence=1000 + index)
        setting(b'native_properties_edit', json.dumps(request).encode())
    request.update(value=2.125, phase='end', sequence=1200)
    setting(b'native_properties_edit', json.dumps(request).encode())
    yield 0.7
    check(abs(bpy.context.object.location.x - 2.125) < 1e-5, 'Coalesced burst preserves the final precise value')
    enqueue(b'ED_OT_undo')
    yield 0.7
    check(abs(bpy.context.object.location.x - start) < 1e-5, 'A coalesced burst creates one undo step')

    enqueue(b'MESH_OT_primitive_cube_add')
    yield 1.0
    ui('redo', action='expand')
    yield 0.3
    redo = ui('redo')['snapshot']
    size = next(p for p in redo['properties'] if p['property'] == 'size')
    count = len(bpy.data.objects)
    ui('redo', action='scrub_begin', key=size['key'])
    for distance in (20, 60, 100):
        ui('redo', action='scrub_change', key=size['key'], translation=distance)
        yield 0.3
        check(abs(bpy.context.object.dimensions.x - 2 - distance * size['dragStep']) < 1e-4,
              f'Redo scrub updates geometry before release at {distance} points')
        check(not ui('redo')['snapshot'].get('error') and len(bpy.data.objects) == count,
              'Live redo replaces geometry without errors or extra objects')
    report['stage'] = 'live-redo'
    yield 4.0
    ui('redo', action='scrub_finish', key=size['key'], translation=103)
    yield 0.5
    check(abs(bpy.context.object.dimensions.x - 2 - 103 * size['dragStep']) < 1e-4,
          'Redo release keeps the exact final value')
    ui('redo', action='scrub_begin', key=size['key'])
    ui('redo', action='scrub_change', key=size['key'], translation=200)
    yield 0.3
    ui('redo', action='scrub_cancel', key=size['key'])
    yield 0.5
    check(abs(bpy.context.object.dimensions.x - 2 - 103 * size['dragStep']) < 1e-4,
          'Redo cancellation restores the starting geometry')
    enqueue(b'ED_OT_undo')
    yield 0.7
    check(len(bpy.data.objects) == count - 1, 'Undo removes the adjusted operator in one step')
    report['stage'] = 'complete'
    report['passed'] = True


steps = tests()


def tick():
    try:
        delay = next(steps)
    except StopIteration:
        delay = None
    except Exception:
        report['error'] = traceback.format_exc()
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=5.0)
