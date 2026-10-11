# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Full-app group/command integration test, for a disposable factory-startup QA app only."""

import ctypes
import json
import traceback
from pathlib import Path

import bpy
from bl_ui.space_toolsystem_common import ToolSelectPanelHelper, native_toolbar_json


probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
probe.blender_device_probe_snapshot.restype = ctypes.c_void_p
probe.blender_device_probe_tool_pick.argtypes = [ctypes.c_char_p]
probe.blender_device_probe_tool_pick.restype = ctypes.c_bool
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
output = Path.home() / 'Documents/tool-groups-test.json'
report = {'status': 'running', 'checks': []}


def snapshot():
    pointer = probe.blender_device_probe_snapshot()
    try:
        return json.loads(ctypes.string_at(pointer))
    finally:
        free(pointer)


def check(condition, message):
    assert condition, message
    report['checks'].append(message)


def steps():
    yield 3.0
    for mode in ('OBJECT', 'EDIT', 'SCULPT', 'OBJECT'):
        bpy.ops.object.mode_set(mode=mode)
        yield 1.0
        native = snapshot()
        expected = json.loads(native_toolbar_json(bpy.context))
        check(native['tools'] == expected, mode + ': engine snapshot matches live Blender definitions')
        expected_buttons = [tool['id'] for tool in expected if tool is not None]
        check([tool['id'] for tool in native['nativeTools']] == expected_buttons,
              mode + ': exactly one native button per Blender toolbar slot')
        cls = ToolSelectPanelHelper._tool_class_from_space_type('VIEW_3D')
        expected_groups = [[tool.idname if tool else None for tool in group]
                           for group in cls.tools_from_context(bpy.context) if type(group) is tuple
                           and any(tool is not None for tool in group)]
        actual_groups = [[tool['id'] if tool else None for tool in group['children']]
                         for group in expected if group and 'children' in group]
        check(actual_groups == expected_groups, mode + ': all group members/order/separators match')

    for tool_id in ('builtin.select_circle', 'builtin.scale_cage', 'builtin.annotate_line'):
        check(probe.blender_device_probe_tool_pick(tool_id.encode()), tool_id + ': found in native flyout')
        yield 1.0
        active = bpy.context.workspace.tools.from_space_view3d_mode('OBJECT', create=False)
        check(active.idname == tool_id, tool_id + ': native flyout activated Blender tool')
        native = snapshot()
        check(tool_id in [tool['id'] for tool in native['nativeTools']],
              tool_id + ': selected tool is the visible group representative')

    bpy.ops.wm.tool_set_by_id(name='builtin.move')
    yield 1.0
    native = snapshot()
    ids = [tool['id'] for tool in native['nativeTools']]
    check(all(tool in ids for tool in ('builtin.select_circle', 'builtin.scale_cage', 'builtin.annotate_line')),
          'Each group remembers its own selected tool after switching away')
    report['status'] = 'passed'
    probe.blender_device_probe_tool_menu(b'builtin.select_circle')


sequence = steps()


def tick():
    try:
        window = bpy.context.window_manager.windows[0]
        area = next(area for area in window.screen.areas if area.type == 'VIEW_3D')
        region = next(region for region in area.regions if region.type == 'WINDOW')
        with bpy.context.temp_override(window=window, area=area, region=region):
            delay = next(sequence, None)
    except Exception:
        report['status'] = 'failed'
        report['error'] = traceback.format_exc()
        traceback.print_exc()
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=3.0)
