# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Test the native sculpt brush rail and sculpt top-bar settings on an iPad."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
rail_query = probe.blender_device_probe_brush_rail
rail_query.argtypes = [ctypes.c_int, ctypes.c_int]
rail_query.restype = ctypes.c_void_p
tool_query = probe.blender_device_probe_toolbar_label
tool_query.argtypes = [ctypes.c_int, ctypes.c_int]
tool_query.restype = ctypes.c_void_p
panels_query = probe.blender_device_probe_scene_panels
panels_query.argtypes = [ctypes.c_char_p]
panels_query.restype = ctypes.c_void_p
engine = ctypes.CDLL(None)
snapshot_query = probe.blender_device_probe_snapshot
snapshot_query.restype = ctypes.c_void_p
enum_operator = engine.WM_apple_ios_ui_operator_enum_enqueue
enum_operator.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
command = engine.WM_apple_ios_ui_command_enqueue
command.argtypes = [ctypes.c_int]
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/brush-rail-test.json'
SET_MODE_OBJECT, SET_MODE_SCULPT = 0, 2


def take(ptr):
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def rail(action=0, index=-1):
    return take(rail_query(action, index))


def tool_fields():
    ui = take(panels_query(json.dumps({'scope': 'tool'}).encode()))
    return {p.get('property') or p.get('label'): p for section in ui['properties'].get('sections', [])
            for p in section['properties']}


def stacked(state):
    """The brush rail sits directly above the tool rail in the same column, fully on screen."""
    brush, tool, view = state['rail'], state['toolRail'], state['viewport']
    return (abs(brush[0] - tool[0]) < 0.5 and brush[1] + brush[3] <= tool[1] + 0.5 and
            brush[1] >= view[1] and tool[1] + tool[3] <= view[1] + view[3])


def check(ok, title):
    assert ok, title
    report['checks'].append(title)


def active_brush():
    return bpy.context.tool_settings.sculpt.brush


def wait_for(condition, message, timeout=20.0):
    for _ in range(int(timeout / 0.2)):
        yield 0.2
        if condition():
            return
    raise AssertionError(message)


def tests():
    check(not rail()['available'], 'Object mode has no brush rail')
    command(SET_MODE_SCULPT)
    yield from wait_for(lambda: bpy.context.mode == 'SCULPT', 'Sculpt mode was not entered')
    # Asset libraries load asynchronously; previews follow a few at a time.
    yield from wait_for(lambda: rail()['available'] and len(rail()['brushes']) > 10,
                        'Sculpt mode did not list its brushes')
    yield from wait_for(lambda: all(b['centerAlpha'] > 200 for b in rail()['brushes']),
                        'Brush previews did not load', timeout=30.0)
    state = rail()
    brushes = state['brushes']
    check(stacked(state), 'The compact brush rail sits above the tool rail in the same column')
    check(all(b['cornerAlpha'] == 0 and b['centerAlpha'] > 200 for b in brushes),
          'Every brush image is cropped to a circle')
    check(sum(b['visible'] for b in brushes) == 1, 'The compact rail shows only the active brush')

    for area in bpy.context.screen.areas:
        if area.type == 'VIEW_3D':
            shelves = [r for r in area.regions if r.type in {'ASSET_SHELF', 'ASSET_SHELF_HEADER'}]
            check(all(r.height <= 1 or r.width <= 1 for r in shelves),
                  "Blender's own brush asset shelf stays hidden")

    state = rail(2)
    check(state['docked'] and all(b['visible'] for b in state['brushes']),
          'Docking the rail lists all brushes')
    check(stacked(state), 'The docked brush list does not overlap the tool rail')
    take(tool_query(0, -1))
    state = rail()
    check(stacked(state) and state['rail'][3] > 64 and state['toolRail'][3] > 64,
          'Brush and tool lists share the column without overlapping')
    take(tool_query(5, -1))
    state = rail()
    check(stacked(state), 'Collapsing the tool list keeps the rails stacked')
    target = next(i for i, b in enumerate(state['brushes'])
                  if b['id'] != state['active'] and 'Clay' in b['name'])
    name = state['brushes'][target]['name']
    rail(1, target)
    yield from wait_for(lambda: active_brush() and active_brush().name == name,
                        'Tapping a native brush did not activate it')
    check(True, 'Tapping a native brush activates it in Blender')
    yield from wait_for(lambda: rail()['active'] == rail()['brushes'][target]['id'],
                        'The rail did not follow the active brush')
    check(rail()['brushes'][target]['selected'], 'The listed active brush is highlighted')
    state = rail(3)
    check(state['expanded'] and state['rail'][2] > 100 and stacked(state),
          'Expanding shows brush names without overlapping the tool rail')
    state = rail(4)
    check(not state['docked'] and sum(b['visible'] for b in state['brushes']) == 1 and stacked(state),
          'Collapsing returns to the compact rail')
    active_index = next(i for i, b in enumerate(state['brushes']) if b['visible'])
    state = rail(1, active_index)
    check(state['expanded'] and all(b['visible'] for b in state['brushes']) and stacked(state),
          'Tapping the collapsed brush rail opens it')
    target = next(i for i, b in enumerate(state['brushes'])
                  if b['id'] != state['active'] and 'Draw' in b['name'])
    name = state['brushes'][target]['name']
    state = rail(1, target)
    check(not state['expanded'] and sum(b['visible'] for b in state['brushes']) == 1,
          'Picking from a tap-opened brush rail closes it')
    yield from wait_for(lambda: active_brush() and active_brush().name == name,
                        'Picking from the tap-opened rail did not activate the brush')

    fields = tool_fields()
    check({'size', 'strength'} <= fields.keys() or {'unprojected_size', 'strength'} <= fields.keys(),
          'The top bar exposes brush radius and strength')
    check(fields.get('size', fields.get('unprojected_size', {})).get('toolbar_label') == 'Radius',
          'Radius uses its sculpt caption')
    check('Dyntopo' in fields and 'use_mesh_mirror_x' in fields,
          'The top bar exposes Dyntopo and X mirroring')
    check(fields['Dyntopo'].get('checkbox'), 'Dyntopo is projected as a toggle')
    take(panels_query(json.dumps({'scope': 'tool', 'action': 'toggle',
                                  'key': fields['Dyntopo']['key'], 'value': True}).encode()))
    yield from wait_for(lambda: bpy.context.sculpt_object.use_dynamic_topology_sculpting,
                        'The native Dyntopo switch did not enable Dyntopo')
    check(True, 'The native Dyntopo switch enables Dyntopo')
    yield from wait_for(lambda: 'detail_size' in tool_fields() or
                        'constant_detail_resolution' in tool_fields() or 'detail_percent' in tool_fields(),
                        'Dyntopo detail did not appear in the top bar')
    check(True, 'Enabling Dyntopo reveals its detail setting')
    detail = next(f for name, f in tool_fields().items()
                  if name in {'detail_size', 'constant_detail_resolution', 'detail_percent'})
    check(detail.get('slider') and detail.get('toolbar_label') == 'Detail',
          'Dyntopo detail is a native slider')
    strip = rail()['strip']
    check(strip and strip['frame'][2] <= strip['window'] and strip['frame'][0] >= 0 and
          strip['frame'][0] + strip['frame'][2] <= strip['window'],
          'The sculpt settings strip fits inside the top bar')
    check(strip['content'] >= strip['frame'][2] - 0.5,
          'Settings beyond the visible width are reachable by scrolling the strip')
    report['strip'] = strip
    take(panels_query(json.dumps({'scope': 'tool', 'action': 'toggle',
                                  'key': tool_fields()['Dyntopo']['key'], 'value': False}).encode()))
    yield from wait_for(lambda: not bpy.context.sculpt_object.use_dynamic_topology_sculpting,
                        'The native Dyntopo switch did not disable Dyntopo')
    check(True, 'The native Dyntopo switch disables Dyntopo again')

    actions = {a['title']: a for a in take(snapshot_query())['actions']}
    check({'Invert Mask', 'Clear Mask', 'Reveal'} <= actions.keys() and
          not {'Undo', 'Redo', 'Frame'} & actions.keys(),
          'Sculpt actions offer mask and visibility operations instead of Undo and Frame')

    def mask_values():
        mask = bpy.context.sculpt_object.data.attributes.get('.sculpt_mask')
        return [v.value for v in mask.data] if mask else []

    invert = actions['Invert Mask']
    enum_operator(invert['operator'].encode(), invert['property'].encode(), invert['value'].encode())
    yield from wait_for(lambda: mask_values() and min(mask_values()) > 0.99,
                        'Invert Mask did not mask the whole mesh')
    check(True, 'Invert Mask action masks the unmasked mesh')
    clear = actions['Clear Mask']
    enum_operator(clear['operator'].encode(), clear['property'].encode(), clear['value'].encode())
    yield from wait_for(lambda: not mask_values() or max(mask_values()) < 0.01,
                        'Clear Mask did not clear the mask')
    check(True, 'Clear Mask action clears the mask')

    command(SET_MODE_OBJECT)
    yield from wait_for(lambda: not rail()['available'], 'The brush rail did not hide outside sculpt mode')
    check(True, 'Leaving sculpt mode hides the brush rail')
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
