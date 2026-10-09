# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise template creation, native document switching and export on a disposable device app."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/TemplatesDeviceProbe.dylib'))
query = probe.blender_device_probe_templates
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/templates-test.json'


def ui(**request):
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(condition, title):
    assert condition, title
    report['checks'].append(title)
    output.write_text(json.dumps(report, indent=2))


def opened(identifier):
    for _ in range(150):
        yield 0.2
        state = ui()
        if not state['opening'] and not state['creating'] and bpy.context.scene.get('blender_ipad_template') == identifier:
            return
    raise AssertionError('Timed out opening ' + identifier + ': ' + json.dumps(ui()))


def tests():
    yield 4
    check(len(ui()['entries']) >= 5, 'Device catalog includes all bundled templates')
    ui(action='bootstrap')
    yield from opened('general')
    check(bool(ui()['document']), 'Native UIDocument opens a copied seed')
    for identifier, mode, timeline in [('sculpt', 'SCULPT', False),
                                        ('drawing', 'PAINT_GREASE_PENCIL', False),
                                        ('animation-2d', 'PAINT_GREASE_PENCIL', True),
                                        ('animation-3d', 'OBJECT', True)]:
        previous = ui()['document']
        ui(action='new')
        yield 0.6
        check(ui()['choosing'], 'Native chooser opens for ' + identifier)
        ui(action='choose', id=identifier)
        yield from opened(identifier)
        yield 1
        check(ui()['document'] != previous, identifier + ': system browser creates an independent document')
        check(bpy.context.mode == mode, identifier + ': Blender restores the intended mode')
        check(ui()['toolbar'].get('objectMode') == mode and ui()['toolbar'].get('modes'),
              identifier + ': native mode controls follow the loaded document')
        check(ui()['timeline'] == timeline, identifier + ': native timeline follows its presentation preset')
        if identifier == 'sculpt':
            check(len(bpy.context.object.data.vertices) > 20000 and bpy.context.tool_settings.sculpt.brush is not None,
                  'Sculpt template has a usable dense mesh and active brush')
        if mode == 'PAINT_GREASE_PENCIL':
            check(bpy.context.tool_settings.gpencil_paint.brush is not None and len(bpy.context.object.data.layers) >= 2,
                  identifier + ': Grease Pencil resolves its brush and layers on iPad')
    original_path = bpy.data.filepath
    bpy.context.scene['template_export_test'] = 'retained'
    original_dirty = bpy.data.is_dirty
    ui(action='save')
    for _ in range(150):
        yield 0.2
        if ui()['result'] != 'pending':
            break
    check(ui()['result'] == 'ok', 'Personal template exports with a generated preview: ' + ui()['result'])
    check(bpy.data.filepath == original_path and bpy.data.is_dirty == original_dirty and
          bpy.context.scene['template_export_test'] == 'retained',
          'Export preserves the live document, contents and dirty state')
    check(any(e['personal'] and e['title'] == 'Device Test Template' for e in ui()['entries']),
          'Export appears in My Templates')
    ui(action='new')
    yield 0.7
    check(ui()['choosing'], 'Final native chooser remains available for inspection')
    report['passed'] = True


steps = tests()


def tick():
    try:
        return next(steps)
    except StopIteration:
        output.write_text(json.dumps(report, indent=2))
    except Exception:
        report['error'] = traceback.format_exc()
        report['ui'] = ui()
        output.write_text(json.dumps(report, indent=2))
    return None


bpy.app.timers.register(tick, first_interval=1, persistent=True)
