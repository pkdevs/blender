# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check moving native selection and hover-only paint previews on a real iPad."""
import ctypes
import json
from pathlib import Path
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
toolbar = probe.blender_device_probe_toolbar_label
toolbar.argtypes = [ctypes.c_int, ctypes.c_int]
toolbar.restype = ctypes.c_void_p
hover = probe.blender_device_probe_pencil_hover
hover.argtypes = [ctypes.c_int, ctypes.c_double, ctypes.c_double]
snapshot = ctypes.CDLL(None).WM_apple_ios_ui_pencil_hover_copy_json
snapshot.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/selection-brush-test.json'
draws = [0]
cursor_handle = None


def decode(ptr):
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def tool(action=5, index=0):
    return decode(toolbar(action, index))


def check(ok, label):
    assert ok, label
    report['checks'].append(label)


def count_draw(*_args):
    draws[0] += 1


def tests():
    global cursor_handle
    state = tool(4)
    indices = {item['id']: i for i, item in enumerate(state['tools'])}
    tool(1, indices['builtin.cursor'])
    yield 0.4
    before = tool()
    tool(1, indices['builtin.move'])
    yield 0.04
    moving = tool()
    check(moving['selectionIdentity'] == before['selectionIdentity'], 'Selection reuses one native glass pill')
    check(moving['selection'] != before['selection'] and moving['selectionAnimations'],
          'Selection movement starts immediately')
    check(moving['selectionPresentation'] != moving['selection'], 'Glass selection interpolates between tools')
    tool(1, indices['builtin.rotate'])
    yield 0.03
    tool(1, indices['builtin.scale'])
    yield 0.4
    settled = tool()
    check(settled['active'] == 'builtin.scale' and settled['selectionPresentation'] == settled['selection'],
          'Rapid taps retarget and settle on the latest tool')
    check(settled['selectionIdentity'] == before['selectionIdentity'], 'Rapid taps do not create extra highlights')

    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    cursor_handle = bpy.types.WindowManager.draw_cursor_add(count_draw, (), 'VIEW_3D', 'WINDOW')
    hover(0, 0.5, 0.5)
    yield 0.3
    for mode, name in [('OBJECT', 'select'), ('SCULPT', 'paint')]:
        with bpy.context.temp_override(area=area, region=region):
            bpy.ops.object.mode_set(mode=mode)
            if mode == 'OBJECT':
                bpy.ops.wm.tool_set_by_id(name='builtin.select_circle')
        yield 0.6
        if mode == 'SCULPT':
            with bpy.context.temp_override(area=area, region=region):
                bpy.ops.brush.asset_activate(asset_library_type='ESSENTIALS',
                    relative_asset_identifier='brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw')
            yield 0.6
            check(bpy.context.scene.tool_settings.sculpt.brush is not None, 'Sculpt preview uses a real brush asset')
        baseline = draws[0]
        area.tag_redraw()
        yield 0.3
        check(draws[0] == baseline, name + ': cursor callbacks remain hidden without hover')
        hover(1, 0.5, 0.5)
        yield 0.3
        check(decode(snapshot())['cursorVisible'] and draws[0] > baseline,
              name + ': Pencil hover draws brush previews')
        report['stage'] = name + '-hover'
        yield 3.0
        hover(0, 0.5, 0.5)
        yield 0.3
        check(not decode(snapshot())['cursorVisible'], name + ': leaving hover range hides the cursor')
        baseline = draws[0]
        area.tag_redraw()
        yield 0.3
        check(draws[0] == baseline, name + ': later viewport redraws cannot resurrect the cursor')
        report['stage'] = name + '-out'
        yield 3.0
        hover(1, 0.5, 0.5)
        yield 0.2
        hover(1, 0.02, 0.97)
        yield 0.3
        check(not decode(snapshot())['cursorVisible'], name + ': hovering native controls hides the preview')
        hover(1, 0.5, 0.5)
        yield 0.2
        hover(2, 0.5, 0.5)
        yield 0.3
        check(not decode(snapshot())['cursorVisible'], name + ': cancelled Pencil input clears the preview')
    with bpy.context.temp_override(area=area, region=region):
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.types.WindowManager.draw_cursor_remove(cursor_handle)
    cursor_handle = None
    report['passed'] = True


steps = tests()


def tick():
    global cursor_handle
    try:
        delay = next(steps)
    except StopIteration:
        delay = None
    except Exception:
        report['error'] = traceback.format_exc()
        if cursor_handle:
            bpy.types.WindowManager.draw_cursor_remove(cursor_handle)
            cursor_handle = None
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=5.0)
