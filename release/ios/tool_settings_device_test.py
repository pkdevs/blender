# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Test active-tool controls against real annotation RNA on an iPad."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
engine = ctypes.CDLL(None)
select = engine.WM_apple_ios_ui_tool_set_by_id
select.argtypes = [ctypes.c_char_p]
edit = engine.WM_apple_ios_ui_setting_enqueue
edit.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
operator = engine.WM_apple_ios_ui_operator_enqueue
operator.argtypes = [ctypes.c_char_p]
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/tool-settings-test.json'


def ui(**request):
    ptr = query(json.dumps(dict(scope='tool', **request)).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def fields():
    return {p.get('property'): p for section in ui()['properties'].get('sections', [])
            for p in section['properties']}


def tool(identifier):
    select(identifier.encode())
    for _ in range(60):
        yield 0.2
        if ui()['properties'].get('tool') == identifier:
            return
    raise AssertionError('Tool settings did not follow ' + identifier)


def check(ok, title):
    assert ok, title
    report['checks'].append(title)


def tests():
    yield from tool('builtin.annotate')
    props = fields()
    check({'annotation_stroke_placement_view3d', 'use_stabilizer', 'stabilizer_radius',
           'stabilizer_factor'} <= props.keys(), 'Annotate exposes Blender placement and stabilization settings')
    check(props['annotation_stroke_placement_view3d'].get('toolbar_label') == 'Placement' and
          props['stabilizer_radius'].get('toolbar_label') == 'Radius',
          'Toolbar captions use Blender layout labels, not selected enum values')
    ui(action='toggle', key=props['use_stabilizer']['key'], value=True)
    yield 0.7
    current = bpy.context.workspace.tools.from_space_view3d_mode(bpy.context.mode, create=False)
    check(current.operator_properties('gpencil.annotate').use_stabilizer,
          'Native stabilization switch changes the active tool operator settings')
    check(ui()['sliders'][props['stabilizer_radius']['key']]['enabled'],
          'Enabling stabilization enables its native sliders')
    radius_key = props['stabilizer_radius']['key']
    old_radius = current.operator_properties('gpencil.annotate').stabilizer_radius
    ui(action='slider_begin', key=radius_key)
    ui(action='slider_change', key=radius_key, value=0.8)
    yield 0.5
    check(current.operator_properties('gpencil.annotate').stabilizer_radius != old_radius,
          'Dragging the native radius slider updates Blender live')
    ui(action='slider_finish', key=radius_key)
    yield 0.3
    ui(action='enum', key=props['annotation_stroke_placement_view3d']['key'], value='SURFACE')
    yield 0.7
    check(bpy.context.scene.tool_settings.annotation_stroke_placement_view3d == 'SURFACE',
          'Native placement menu updates Blender')
    check('use_annotation_stroke_endpoints' in fields(),
          'Surface placement reveals Blender conditional controls')
    yield from tool('builtin.annotate_line')
    props = fields()
    check({'arrowstyle_start', 'arrowstyle_end'} <= props.keys() and 'use_stabilizer' not in props,
          'Annotate Line replaces stabilization with line-end styles')
    arrow = props['arrowstyle_end']
    value = next(option['value'] for option in arrow['options'] if option['value'] != arrow['value'])
    ui(action='enum', key=arrow['key'], value=value)
    yield 0.7
    current = bpy.context.workspace.tools.from_space_view3d_mode(bpy.context.mode, create=False)
    check(current.operator_properties('gpencil.annotate').arrowstyle_end == value,
          'Native line-style menu edits the actual tool')
    yield from tool('builtin.annotate_polygon')
    check('annotation_stroke_placement_view3d' in fields() and 'arrowstyle_end' not in fields(),
          'Polygon annotation shows only its applicable settings')
    yield from tool('builtin.annotate_eraser')
    radius = fields()['grease_pencil_eraser_radius']
    original = bpy.context.preferences.edit.grease_pencil_eraser_radius
    value = min(radius['max'], max(radius['min'], original + 3))
    semantic = f"{radius['owner']}.{radius['property']}."
    ui(action='property', key=semantic, value=value)
    yield 0.7
    check(bpy.context.preferences.edit.grease_pencil_eraser_radius == value,
          'Native eraser radius field updates Blender preferences')
    # Restore the user preference before testing rejection of a stale tool control.
    ui(action='property', key=semantic, value=original)
    yield 0.5
    stale = dict(radius, id=ui()['properties']['id'], value=value)
    yield from tool('builtin.move')
    edit(b'native_properties_edit', json.dumps(stale).encode())
    yield 0.5
    check(bpy.context.preferences.edit.grease_pencil_eraser_radius == original,
          'A stale annotation control cannot edit after changing tools')
    yield from tool('builtin.annotate')
    ui(action='enum', key=fields()['annotation_stroke_placement_view3d']['key'], value='CURSOR')
    yield 0.5
    operator(b'GPENCIL_OT_annotation_add')
    yield 0.7
    color = fields()['color']
    ui(action='color_open', key=color['key'])
    yield 0.7
    check(ui()['colorPresented'], 'Annotation color opens the native color picker')
    ui(action='color_select', value=[0.2, 0.5, 0.8, 1.0], continuous=False)
    yield 0.5
    check(fields()['color']['value'] != color['value'], 'Native color picker changes the annotation layer')
    ui(action='color_close')
    yield 0.7
    ui(action='tool_popover')
    yield 0.7
    check(ui()['toolPresented'], 'Compact tool settings open a native popover')
    ui(action='tool_popover_close')
    yield 0.7
    report.update(passed=True)


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
