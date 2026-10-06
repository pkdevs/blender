# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise the actual Blender shading panels through their native controls."""
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
output = Path.home() / 'Documents/shading-test.json'


def ui(**request):
    request.setdefault('scope', 'shading')
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def fields():
    return [p for section in ui()['properties'].get('sections', []) for p in section['properties']]


def prop(name):
    return next(p for p in fields() if p.get('property') == name)


def choose(name, value):
    return ui(action='enum', key=prop(name)['key'], value=value)


def toggle(name, value):
    return ui(action='toggle', key=prop(name)['key'], value=value)


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def tests():
    # The toolbar target opens the real UIKit popover, not a test-only replacement.
    ui(action='shading_open')
    yield 2.0
    report['initial'] = ui()
    shading = next(a.spaces.active.shading for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    check(ui()['shadingPresented'], 'Toolbar shading button presents native controls')
    check({p['value'] for p in prop('type')['options']} == {'WIREFRAME', 'SOLID', 'MATERIAL', 'RENDERED'},
          'Native mode picker exposes all four Blender shading modes')
    check(prop('light')['value'] == 'STUDIO' and prop('studio_light')['options'],
          'Solid mode includes Blender lighting and studio-light choices')
    choose('light', 'MATCAP')
    yield 0.8
    check(shading.light == 'MATCAP' and prop('studio_light')['options'], 'Native MatCap selection refreshes its choices')
    matcap = next(p['value'] for p in prop('studio_light')['options'] if p['value'] != shading.studio_light)
    choose('studio_light', matcap)
    yield 0.8
    check(shading.studio_light == matcap, 'Native lighting picker changes the actual MatCap')
    choose('light', 'STUDIO')
    yield 0.8
    toggle('use_world_space_lighting', True)
    yield 0.8
    check(shading.use_world_space_lighting and prop('studiolight_rotate_z')['editable'], 'World-space lighting enables rotation')
    choose('color_type', 'SINGLE')
    choose('background_type', 'VIEWPORT')
    yield 0.8
    check(prop('single_color')['type'] == 'COLOR' and prop('background_color')['type'] == 'COLOR',
          'Conditional object and background colors use native color controls')
    ui(action='color_open', key=prop('single_color')['key'])
    yield 0.8
    check(ui()['colorPresented'], 'Shading swatch presents the native color picker from its popover')
    ui(action='color_select', value=[0.2, 0.4, 0.6], continuous=False)
    ui(action='color_close')
    yield 0.8
    linear = [((value + 0.055) / 1.055) ** 2.4 for value in (0.2, 0.4, 0.6)]
    check(all(abs(a - b) < 1e-4 for a, b in zip(shading.single_color, linear)),
          'Native shading color edits reach Blender')
    toggle('show_xray', True)
    yield 0.8
    check(shading.show_xray and not prop('show_shadows')['editable'] and not prop('show_cavity')['editable'],
          'X-Ray follows Blender availability for shadows and cavity')
    toggle('show_xray', False)
    yield 0.8
    toggle('show_cavity', True)
    yield 0.8
    choose('cavity_type', 'BOTH')
    yield 0.8
    check({'cavity_ridge_factor', 'curvature_ridge_factor', 'matcap_ssao_samples', 'shadow_shift'} <=
          {p.get('property') for p in fields()}, 'Nested cavity and shadow popovers are included as native sections')
    ui(action='property', key='data.matcap_ssao_samples.', value=24)
    yield 0.8
    check(bpy.context.scene.display.matcap_ssao_samples == 24, 'Nested SSAO property edits target the real scene')
    toggle('show_cavity', False)
    yield 0.8
    check(prop('show_cavity')['editable'] and 'matcap_ssao_samples' not in {p.get('property') for p in fields()},
          'Disabling cavity removes dependent settings without disabling its enable switch')
    choose('type', 'WIREFRAME')
    yield 1.5
    names = {p.get('property') for p in fields()}
    check(shading.type == 'WIREFRAME' and 'show_xray_wireframe' in names and 'light' not in names and 'show_cavity' not in names,
          'Wireframe shows only its relevant Blender panels')
    choose('type', 'MATERIAL')
    yield 3.0
    check(shading.type == 'MATERIAL' and {'use_scene_lights', 'use_scene_world', 'render_pass', 'use_compositor'} <=
          {p.get('property') for p in fields()}, 'Material Preview includes scene lighting, render passes, and compositor')
    toggle('use_scene_world', True)
    yield 0.8
    check(shading.use_scene_world and 'studiolight_intensity' not in {p.get('property') for p in fields()},
          'Scene World hides studio environment controls')
    toggle('use_scene_world', False)
    yield 0.8
    check(prop('studiolight_intensity')['editable'], 'Disabling Scene World restores environment controls')
    choose('type', 'RENDERED')
    yield 3.0
    check(shading.type == 'RENDERED' and prop('use_scene_world_render'), 'Rendered mode uses its own scene lighting controls')
    toggle('use_scene_world_render', False)
    yield 1.0
    check(prop('studiolight_background_blur')['editable'], 'Rendered environment settings follow the engine and world toggle')
    choose('type', 'SOLID')
    yield 1.0
    report['state'] = ui()
    ui(action='shading_close')
    for _ in range(20):
        yield 0.25
        if not ui()['shadingPresented']:
            break
    check(not ui()['shadingPresented'], 'Shading popover dismisses cleanly')
    ui(action='shading_open')
    yield 1.0
    check(prop('type')['value'] == 'SOLID', 'Reopening shading restores the current Blender state')
    ui(scope='main', action='inspector')
    yield 1.0
    ui(scope='main', action='property', key='object.location.0', value=2.0)
    yield 0.8
    check(abs(bpy.context.object.location.x - 2.0) < 1e-6,
          'Properties edits work alongside the shading popover')
    choose('light', 'FLAT')
    yield 0.8
    check(shading.light == 'FLAT' and 'studio_light' not in {p.get('property') for p in fields()},
          'Shading acknowledgements and Flat lighting remain correct after a Properties edit')
    choose('light', 'STUDIO')
    ui(scope='main', action='inspector')
    bpy.context.view_layer.objects.active = None
    yield 0.8
    check(prop('type')['value'] == 'SOLID' and prop('studio_light')['editable'],
          'Shading remains available without an active object')
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
