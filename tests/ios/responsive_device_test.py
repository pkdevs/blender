# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Full-app physical-device test, requiring the test-only native probe dylib.

Run with --python after copying this and native_menubar_device_test.py to Documents.
Uses the factory startup scene; does not save or modify the user's files.
"""

import bpy
import ctypes
import json
import time
import traceback
from pathlib import Path


documents = Path.home() / 'Documents'
output = documents / 'responsive-device-test.json'
report = {'stage': 'startup', 'checks': []}
output.write_text(json.dumps(report))
probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
probe.blender_device_probe_begin.restype = ctypes.c_bool
probe.blender_device_probe_end.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
calls = []
menu_deadline = time.monotonic() + 90


class WM_OT_responsive_device_probe(bpy.types.Operator):
    bl_idname = 'wm.responsive_device_probe'
    bl_label = 'Responsiveness Test'

    def execute(self, context):
        calls.append((context.area.type, context.region.type))
        return {'FINISHED'}


def save():
    output.write_text(json.dumps(report, indent=2))


def check(condition, description):
    assert condition, description
    report['checks'].append(description)
    save()


def recovered():
    try:
        check(len(calls) == 20, 'All 20 queued commands executed after the stall')
        check(all(c == ('VIEW_3D', 'WINDOW') for c in calls), 'Commands keep viewport context')
        check(bool(bpy.context.scene.objects), 'Live scene remains available')
        report['passed'] = True
        report['stage'] = 'complete'
    except Exception:
        report['error'] = traceback.format_exc()
    bpy.utils.unregister_class(WM_OT_responsive_device_probe)
    save()


def stall():
    try:
        if time.monotonic() > menu_deadline:
            raise TimeoutError('Native menu tests did not complete within 90 seconds')
        if not (documents / 'native-menubar-test.json').exists():
            return 0.5
        menu_report = json.loads((documents / 'native-menubar-test.json').read_text())
        if not menu_report.get('passed'):
            if 'error' in menu_report:
                raise RuntimeError(menu_report['error'])
            return 0.5
        report['menu_checks'] = menu_report['checks']
        bpy.utils.register_class(WM_OT_responsive_device_probe)
        check(probe.blender_device_probe_begin(), 'Native probe attached; Blender is off main')
        report['stage'] = 'engine-stalled'
        save()
        # Deliberately hold the actual Blender/Python thread busy, including its GIL.
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            pass
        check(not calls, 'Queued operators cannot run while the engine is blocked')
        pointer = probe.blender_device_probe_end()
        metrics = json.loads(ctypes.string_at(pointer))
        free(pointer)
        report['metrics'] = metrics
        check(metrics['ticks'] >= 120, 'Native display link continues throughout engine stall')
        check(metrics['maximum_gap_ms'] < 250, 'No main-thread freeze during engine stall')
        check(metrics['snapshot_reads'] >= 120, 'Native UI can read snapshots while engine is busy')
        check(metrics['toolbar_expanded'], 'Existing native toolbar expands while engine is busy')
        check(metrics['scroll_distance'] > 100, 'Existing native toolbar scrolls while engine is busy')
        check(metrics['queued_commands'] == 20, 'Native commands submit without waiting')
        report['stage'] = 'recovering'
        save()
        bpy.app.timers.register(recovered, first_interval=2)
    except Exception:
        report['error'] = traceback.format_exc()
        save()
    return None


# Result consumption belongs to UIKit after the threading refactor. The test-only
# adapter transfers its owned result back to this engine-thread test.
menu_source = (documents / 'native_menubar_device_test.py').read_text().replace(
    'take_result = engine.WM_apple_ios_ui_menu_take_result',
    'take_result = probe.blender_device_probe_menu_result')
(documents / 'native-menubar-test.json').unlink(missing_ok=True)
exec(compile(menu_source, 'native_menubar_device_test.py', 'exec'), {'probe': probe})
bpy.app.timers.register(stall, first_interval=5)
