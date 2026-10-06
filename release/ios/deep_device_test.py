# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Full Blender integration checks in an isolated, factory-startup device app.

Requires ResponsiveDeviceProbe.dylib, --enable-event-simulate and the existing
responsive/menu device scripts in Documents. Never run against a user's scene.
"""

import bpy
import ctypes
import json
import time
import traceback
from pathlib import Path

documents = Path.home() / 'Documents'
output = documents / 'deep-device-test.json'
report = {'checks': [], 'stage': 'waiting-for-menu-and-stall-tests'}
output.write_text(json.dumps(report))
probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
probe.blender_device_probe_snapshot.restype = ctypes.c_void_p
probe.blender_device_probe_axis.argtypes = [ctypes.c_char_p]
probe.blender_device_probe_axis.restype = ctypes.c_bool
probe.blender_device_probe_cancel_picker.restype = ctypes.c_bool
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
engine = ctypes.CDLL(None)
engine.WM_apple_ios_ui_tool_set_by_id.argtypes = [ctypes.c_char_p]
engine.WM_apple_ios_ui_command_enqueue.argtypes = [ctypes.c_int]
engine.WM_apple_ios_ui_setting_enqueue.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
engine.WM_apple_ios_ui_operator_enqueue.argtypes = [ctypes.c_char_p]
engine.WM_apple_ios_ui_operator_enum_enqueue.argtypes = [ctypes.c_char_p] * 3
deadline = time.monotonic() + 180
generator = None


def save():
    output.write_text(json.dumps(report, indent=2))


def check(condition, name):
    assert condition, name
    report['checks'].append(name)
    save()


def snapshot():
    pointer = probe.blender_device_probe_snapshot()
    result = json.loads(ctypes.string_at(pointer))
    free(pointer)
    return result


def viewport():
    window = bpy.context.window_manager.windows[0]
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    return dict(window=window, area=area, region=region)


def event(key):
    context = viewport()
    region = context['region']
    for value in ('PRESS', 'RELEASE'):
        context['window'].event_simulate(type=key, value=value,
            x=region.x + region.width // 2, y=region.y + region.height // 2)


def axes_absent(label):
    state = snapshot()
    check(not any(a.get('key') in ('X', 'Y', 'Z') for a in state['actions']), label + ': snapshot')
    check(not state['nativeAxes'], label + ': native buttons')


def steps():
    check({'File', 'Edit', 'View', 'Select', 'Add', 'Object'} <= set(snapshot()['nativeMenus']),
          'Visible native ellipsis exposes full Blender menus, including Add')
    axes_absent('Idle selection has no XYZ constraints')
    for tool in ('builtin.move', 'builtin.rotate', 'builtin.scale', 'builtin.select_box'):
        engine.WM_apple_ios_ui_tool_set_by_id(tool.encode())
        yield 0.6
        check(snapshot()['active'] == tool, f'Tool change reaches native snapshot: {tool}')
        axes_absent(f'Idle {tool} has no XYZ constraints')
    for name in ('translate', 'rotate', 'resize'):
        with bpy.context.temp_override(**viewport()):
            result = getattr(bpy.ops.transform, name)('INVOKE_DEFAULT')
        check('RUNNING_MODAL' in result, f'{name} starts a live transform')
        yield 0.6
        check([a['key'] for a in snapshot()['nativeAxes']] == ['X', 'Y', 'Z'],
              f'{name} shows XYZ while applicable')
        for key in ('X', 'Y', 'Z'):
            check(probe.blender_device_probe_axis(key.encode()), f'{name}: native {key} button exists')
            yield 0.4
            check(next(a['selected'] for a in snapshot()['nativeAxes'] if a['key'] == key),
                  f'{name}: native {key} toggle turns on')
            probe.blender_device_probe_axis(key.encode())
            yield 0.4
            check(not any(a['selected'] for a in snapshot()['nativeAxes']),
                  f'{name}: native {key} toggle turns off')
        event('ESC')
        yield 0.6
        axes_absent(f'Cancelled {name} removes XYZ')
    with bpy.context.temp_override(**viewport()):
        bpy.ops.transform.translate('INVOKE_DEFAULT')
    yield 0.5
    event('RET')
    yield 0.6
    axes_absent('Confirmed transform removes XYZ')
    for command, mode in ((1, 'mesh_edit'), (0, 'objectmode')):
        engine.WM_apple_ios_ui_command_enqueue(command)
        yield 0.8
        check(snapshot()['mode'] == mode, f'Mode change updates native UI: {mode}')
        check(('Mesh' if mode == 'mesh_edit' else 'Object') in snapshot()['nativeMenus'],
              f'Visible menu roots follow {mode}')
        axes_absent(f'{mode} idle state hides XYZ')
    count = len(bpy.context.scene.objects)
    engine.WM_apple_ios_ui_operator_enum_enqueue(b'OBJECT_OT_duplicate', b'', b'')
    yield 0.5
    check(len(bpy.context.scene.objects) == count + 1, 'Duplicate creates a new scene object')
    check(snapshot()['canUndo'], 'Native Undo enables after an edit')
    for operator, expected in ((b'ED_OT_undo', count), (b'ED_OT_redo', count + 1), (b'ED_OT_undo', count)):
        engine.WM_apple_ios_ui_operator_enqueue(operator)
        yield 0.8
        check(len(bpy.context.scene.objects) == expected, f'Native {operator.decode()} restores scene state')
    for command, shading in ((20, 'wireframe'), (21, 'solid'), (22, 'material'), (21, 'solid')):
        engine.WM_apple_ios_ui_command_enqueue(command)
        yield 1.5
        check(snapshot()['shading'] == shading, f'Shading reaches native snapshot: {shading}')
    engine.WM_apple_ios_ui_tool_set_by_id(b'builtin.move')
    yield 0.5
    for orientation in (b'local', b'global'):
        engine.WM_apple_ios_ui_setting_enqueue(b'orientation', orientation)
        yield 0.5
        check(snapshot()['settings']['orientation'] == orientation.decode(),
              f'Orientation state follows native command: {orientation.decode()}')
    path = documents / 'deep-qa-scene.blend'
    bpy.context.scene['deep_qa_marker'] = 'round-trip'
    bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)
    check(path.exists() and path.stat().st_size > 0, 'Real .blend save writes a nonempty scene')
    yield 0.5
    with bpy.context.temp_override(**viewport()):
        bpy.ops.wm.open_mainfile('INVOKE_DEFAULT')
    picker_deadline = time.monotonic() + 10
    while snapshot()['presented'] != 'UIDocumentPickerViewController' and time.monotonic() < picker_deadline:
        yield 0.1
    check(snapshot()['presented'] == 'UIDocumentPickerViewController', 'Open uses native Files picker')
    check(probe.blender_device_probe_cancel_picker(), 'Native Files picker cancels without loading')
    dismissal_deadline = time.monotonic() + 10
    while snapshot()['presented'] and time.monotonic() < dismissal_deadline:
        yield 0.1
    check(not snapshot()['presented'], 'Cancelled Files picker leaves no modal UI')
    bpy.context.scene['deep_qa_marker'] = 'modified'
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False)
    yield 2
    check(bpy.context.scene.get('deep_qa_marker') == 'round-trip', 'Real .blend load restores scene data')
    check('Add' in snapshot()['nativeMenus'], 'Native menus recover after file load')
    axes_absent('File load does not leave XYZ constraints behind')
    engine.WM_apple_ios_ui_tool_set_by_id(b'builtin.select_box')
    yield 0.5
    report['passed'] = True
    report['stage'] = 'complete'


def tick():
    global generator
    try:
        if time.monotonic() > deadline:
            raise TimeoutError('Full device integration checks exceeded 180 seconds')
        if generator is None:
            result_path = documents / 'responsive-device-test.json'
            if not result_path.exists():
                return 0.5
            result = json.loads(result_path.read_text())
            if 'error' in result:
                raise RuntimeError(result['error'])
            if not result.get('passed'):
                return 0.5
            report['responsiveness'] = result
            report['stage'] = 'functional-checks'
            generator = steps()
        return next(generator)
    except StopIteration:
        pass
    except Exception:
        report['error'] = traceback.format_exc()
    save()
    return None


(documents / 'responsive-device-test.json').unlink(missing_ok=True)
exec(compile((documents / 'responsive_device_test.py').read_text(),
             'responsive_device_test.py', 'exec'), {})
bpy.app.timers.register(tick, first_interval=1, persistent=True)
