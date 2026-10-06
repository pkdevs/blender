# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check original tool artwork through the installed native asset catalog."""
import ctypes
import json
from pathlib import Path
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
colors = probe.blender_device_probe_tool_colors
colors.restype = ctypes.c_void_p
theme = probe.blender_device_probe_theme
theme.argtypes = [ctypes.c_int]
theme.restype = ctypes.c_void_p
toolbar = probe.blender_device_probe_toolbar_label
toolbar.argtypes = [ctypes.c_int, ctypes.c_int]
toolbar.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/tool-colors-test.json'


def decode(ptr):
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, message):
    assert ok, message
    report['checks'].append(message)


def tests():
    decode(toolbar(4, 0))
    for name, style in [('light', 1), ('dark', 2)]:
        decode(theme(style))
        yield 1.0
        images = decode(colors())
        report[name] = images
        check(len(images) > 5 and all(i['visible'] > 0 for i in images), name + ': tool artwork visible')
        colored = [(index, item) for index, item in enumerate(images) if item['colored'] > 20]
        check(len(colored) >= 3, name + ': multiple colored tools')
        check(all(item['original'] for _, item in colored), name + ': colors bypass template tint')
        group = [g for i in images for g in i['group']]
        check(group and all(g['original'] and g['visible'] for g in group), name + ': grouped artwork original')
        index, original = colored[0]
        decode(toolbar(1, index))
        immediate = decode(colors())[index]
        check(immediate['selected'] and immediate['white'] and immediate['colored'] == 0,
              name + ': selected artwork is white immediately after the tap')
        yield 1.0
        selected = decode(colors())[index]
        report[name + '_selected'] = selected
        check(selected['selected'] and selected['original'] and selected['white'] and
              selected['visible'] > 0 and selected['colored'] == 0,
              name + ': selected tool is white on blue')
        other_index = next(i for i, _ in colored if i != index)
        decode(toolbar(1, other_index))
        yield 1.0
        deselected = decode(colors())[index]
        check(not deselected['selected'] and deselected['original'] and deselected['colored'] > 20,
              name + ': deselected tool restores Blender colors')
        report['stage'] = name
        yield 12.0
    light = {i['id']: i for i in report['light']}
    check(any(i['sum'] != light[i['id']]['sum'] for i in report['dark'] if i['id'] in light),
          'Artwork adapts to appearance')
    bpy.ops.object.mode_set(mode='EDIT')
    yield 2.0
    edit = decode(colors())
    report['edit'] = edit
    check(sum(i['colored'] > 20 for i in edit) > 5, 'Edit-mode modeling tools retain Blender colors')
    report['stage'] = 'edit'
    yield 12.0
    bpy.ops.object.mode_set(mode='OBJECT')
    decode(theme(0))
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
