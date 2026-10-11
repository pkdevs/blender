# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run only in a separate factory-startup QA app with ResponsiveDeviceProbe.dylib."""

import bpy
import ctypes
import json
import time
import traceback
from pathlib import Path

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
probe.blender_device_probe_dialog_start()
probe.blender_device_probe_dialog_snapshot.restype = ctypes.c_void_p
probe.blender_device_probe_dialog_choose.argtypes = [ctypes.c_char_p]
probe.blender_device_probe_dialog_choose.restype = ctypes.c_bool
probe.blender_device_probe_cancel_picker.restype = ctypes.c_bool
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
engine = ctypes.CDLL(None)
engine.WM_apple_ios_ui_operator_enqueue.argtypes = [ctypes.c_char_p]
output = Path.home() / 'Documents/native-dialogs-test.json'
report = {'checks': []}
deadline = time.monotonic() + 120


def check(condition, label):
    assert condition, label
    report['checks'].append(label)
    output.write_text(json.dumps(report, indent=2))


def snapshot():
    pointer = probe.blender_device_probe_dialog_snapshot()
    try:
        return json.loads(ctypes.string_at(pointer))
    finally:
        free(pointer)


def choose(title):
    check(probe.blender_device_probe_dialog_choose(title.encode()), f'Native action exists: {title}')


def invoke(operator, **properties):
    window = bpy.context.window_manager.windows[0]
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    with bpy.context.temp_override(window=window, area=area, region=region):
        return operator('INVOKE_DEFAULT', **properties)


class WM_OT_native_warning_probe(bpy.types.Operator):
    bl_idname = 'wm.native_warning_probe'
    bl_label = 'Native warning test'

    def execute(self, context):
        self.report({'ERROR'}, 'QA native save failure')
        return {'CANCELLED'}


def steps():
    bpy.utils.register_class(WM_OT_native_warning_probe)
    yield 1
    count = len(bpy.context.scene.objects)
    invoke(bpy.ops.object.delete, confirm=True)
    yield 1
    state = snapshot()
    check(state['class'] == 'UIAlertController' and 'Delete' in state['title'],
          'Object Delete uses a native alert')
    check(next(a['style'] for a in state['actions'] if a['title'] == 'Delete') == 2,
          'Delete has native destructive styling')
    check(len(bpy.context.scene.objects) == count, 'Delete does nothing before confirmation')
    choose('Cancel')
    yield 1
    check(len(bpy.context.scene.objects) == count and not snapshot()['class'],
          'Delete cancellation preserves the scene and dismisses the alert')
    invoke(bpy.ops.object.delete, confirm=True)
    yield 1
    choose('Delete')
    yield 1
    check(len(bpy.context.scene.objects) == count - 1, 'Confirmed Delete executes the real operator')
    engine.WM_apple_ios_ui_operator_enqueue(b'ED_OT_undo')
    yield 1
    check(len(bpy.context.scene.objects) == count, 'Native-confirmed Delete is undoable')

    bpy.context.preferences.view.use_save_prompt = True
    bpy.context.scene['native_dialog_marker'] = 'unsaved'
    invoke(bpy.ops.wm.open_mainfile)
    yield 1
    state = snapshot()
    check(state['class'] == 'UIAlertController' and 'Save changes' in state['title'],
          'Open with unsaved changes uses a native Save warning')
    check({a['title'] for a in state['actions']} == {'Save', "Don't Save", 'Cancel'},
          'Save warning preserves all three choices')
    choose('Cancel')
    yield 1
    check(bpy.context.scene.get('native_dialog_marker') == 'unsaved' and not snapshot()['class'],
          'Cancelling Open preserves unsaved data')
    invoke(bpy.ops.wm.open_mainfile)
    yield 1
    choose("Don't Save")
    yield 1.5
    check(snapshot()['class'] == 'UIDocumentPickerViewController',
          'Discard continues to the native Open picker')
    check(probe.blender_device_probe_cancel_picker(), 'Open picker can still cancel')
    yield 1

    path = Path.home() / 'Documents/native-dialog-roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)
    bpy.context.scene['native_dialog_marker'] = 'save-before-open'
    invoke(bpy.ops.wm.open_mainfile)
    yield 1
    choose('Save')
    yield 2
    check(snapshot()['class'] == 'UIDocumentPickerViewController',
          'Successful Save continues to native Open only after writing')
    probe.blender_device_probe_cancel_picker()
    yield 1
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False)
    yield 1
    check(bpy.context.scene.get('native_dialog_marker') == 'save-before-open',
          'Save confirmation wrote the changed scene')

    engine.WM_apple_ios_ui_operator_enqueue(b'WM_OT_native_warning_probe')
    yield 1
    state = snapshot()
    check(state['class'] == 'UIAlertController' and 'QA native save failure' in state['message'],
          'Operator errors use native alerts')
    choose('OK')
    yield 1
    check(not snapshot()['class'], 'Duplicate report paths do not show duplicate alerts')
    image = bpy.data.images.new('Native warning image', width=8, height=8)
    image.pixels[0] = 0.5
    bpy.context.scene['native_dialog_marker'] = 'image-warning'
    invoke(bpy.ops.wm.open_mainfile)
    yield 1
    state = snapshot()
    check(state['class'] == 'WMIOSSwiftDialogSheet' and len(state['toggles']) > 0,
          'Modified-image save choices use native switches')
    choose('Cancel')
    yield 1
    check(bpy.context.scene.get('native_dialog_marker') == 'image-warning',
          'Cancelling the native options sheet preserves edits')
    bpy.data.images.remove(image)
    bpy.utils.unregister_class(WM_OT_native_warning_probe)
    report['passed'] = True


generator = steps()


def tick():
    try:
        if time.monotonic() > deadline:
            raise TimeoutError('Native dialog integration checks timed out')
        return next(generator)
    except StopIteration:
        pass
    except Exception:
        report['error'] = traceback.format_exc()
    output.write_text(json.dumps(report, indent=2))
    return None


bpy.app.timers.register(tick, first_interval=1, persistent=True)
