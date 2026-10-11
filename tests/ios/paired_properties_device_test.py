# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify native component groups against Blender's real panel layouts and RNA."""
import ctypes
import json
from pathlib import Path
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/paired-properties-test.json'


def ui(**request):
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def fields(panel):
    return next(s['properties'] for s in ui()['properties']['sections'] if s['id'] == panel)


def paired(a, b):
    check(a.get('group_key') and a.get('group_key') == b.get('group_key') and a['group'] == b['group'],
          a['property'] + ' and ' + b['property'] + ' share a stable component group')
    state = ui(action='scroll_property', key=a['key'])
    x, y = state['fieldFrames'][a['key']], state['fieldFrames'][b['key']]
    check(abs(x[1] - y[1]) < 1 and x[0] + x[2] <= y[0] and min(x[2], y[2]) > 60,
          a['group'] + ' controls are side by side without overlap')


def tests():
    ui(action='inspector')
    yield 1.0
    ui(action='tab', id='OUTPUT')
    yield 1.0
    props = {p['property']: p for p in fields('RENDER_PT_format') if 'property' in p}
    paired(props['resolution_x'], props['resolution_y'])
    paired(props['pixel_aspect_x'], props['pixel_aspect_y'])
    check(not props['resolution_percentage'].get('group'), 'Resolution percentage remains independent')
    report['format'] = props
    for name, value in [('pixel_aspect_x', 1.25), ('pixel_aspect_y', 1.5),
                        ('resolution_x', 1600), ('resolution_y', 900)]:
        other = name[:-1] + ('y' if name.endswith('x') else 'x')
        before = getattr(bpy.context.scene.render, other)
        ui(action='property', key='data.' + name + '.', value=value)
        yield 0.5
        check(abs(getattr(bpy.context.scene.render, name) - value) < 1e-5 and
              getattr(bpy.context.scene.render, other) == before, name + ' edits only its own component')
    yield 6.0
    bpy.types.Scene.native_pair_vector = bpy.props.FloatVectorProperty(name='Offset', size=2)
    for name, label in [('native_pair_min', 'Range Min'), ('native_pair_max', 'Range Max'),
                        ('native_separate_x', 'Separate X'), ('native_separate_y', 'Separate Y')]:
        setattr(bpy.types.Scene, name, bpy.props.FloatProperty(name=label))

    class SCENE_PT_native_pairs(bpy.types.Panel):
        bl_label = 'Component Groups'
        bl_space_type = 'PROPERTIES'
        bl_region_type = 'WINDOW'
        bl_context = 'scene'

        def draw(self, context):
            self.layout.use_property_split = True
            self.layout.prop(context.scene, 'native_pair_vector')
            col = self.layout.column(align=True)
            col.prop(context.scene, 'native_pair_min')
            col.prop(context.scene, 'native_pair_max')
            self.layout.column(align=True).prop(context.scene, 'native_separate_x')
            self.layout.column(align=True).prop(context.scene, 'native_separate_y')

    bpy.utils.register_class(SCENE_PT_native_pairs)
    ui(action='tab', id='SCENE')
    yield 1.0
    ui(action='section', id='SCENE_PT_native_pairs')
    yield 1.0
    props = fields('SCENE_PT_native_pairs')
    vector = [p for p in props if p.get('property') == 'native_pair_vector']
    paired(*vector)
    values = {p['property']: p for p in props if 'property' in p}
    paired(values['native_pair_min'], values['native_pair_max'])
    check(not values['native_separate_x'].get('group') and not values['native_separate_y'].get('group'),
          'Matching names in separate Blender layout groups do not merge')
    bpy.utils.unregister_class(SCENE_PT_native_pairs)
    for name in values:
        delattr(bpy.types.Scene, name)
    ui(action='tab', id='OUTPUT')
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
