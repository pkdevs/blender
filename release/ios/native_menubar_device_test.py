# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run via --python-expr on the iPad, then fetch Documents/native-menubar-test.json.

Uses the real asynchronous native-menu bridge and Blender's live menu definitions.
The temporary operator and menu additions are removed on completion.
"""

import bpy
import ctypes
import json
import traceback
from pathlib import Path


engine = ctypes.CDLL(None)
request = engine.WM_apple_ios_ui_menu_request
request.argtypes = [ctypes.c_char_p, ctypes.c_bool]
request.restype = ctypes.c_uint64
positioned_request = engine.WM_apple_ios_ui_menu_at_position_request
positioned_request.argtypes = [ctypes.c_char_p, ctypes.c_bool, ctypes.c_int, ctypes.c_int]
positioned_request.restype = ctypes.c_uint64
take_result = engine.WM_apple_ios_ui_menu_take_result
take_result.argtypes = [ctypes.c_uint64]
take_result.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/native-menubar-test.json'
calls = []
enabled = True
initial_extension = bpy.context.scene.render.use_file_extension


class WM_OT_native_menu_probe(bpy.types.Operator):
    bl_idname = 'wm.native_menu_probe'
    bl_label = 'Native Menu Probe'
    number: bpy.props.IntProperty()
    text: bpy.props.StringProperty()
    flag: bpy.props.BoolProperty()

    @classmethod
    def poll(cls, context):
        return enabled

    def execute(self, context):
        calls.append((self.number, self.text, self.flag, context.area.type, context.region.type, 'EXEC'))
        return {'FINISHED'}

    def invoke(self, context, event):
        calls.append((self.number, self.text, self.flag, context.area.type, context.region.type, 'INVOKE'))
        return {'FINISHED'}


def append_probe(self, context):
    self.layout.separator()
    self.layout.operator_context = 'EXEC_REGION_WIN'
    op = self.layout.operator('wm.native_menu_probe', text='Native Probe Execute')
    op.number, op.text, op.flag = 73, 'Exact properties: "quoted"', True
    self.layout.operator_context = 'INVOKE_REGION_WIN'
    op = self.layout.operator('wm.native_menu_probe', text='Native Probe Invoke')
    op.number, op.text, op.flag = 19, 'Invoke properties', False
    self.layout.prop(context.scene.render, 'use_file_extension', text='Native Probe Toggle')


def route(item, execute=False):
    return item['route'], execute


def find(items, title):
    return next(item for item in items if item.get('title') == title)


def check(condition, name):
    assert condition, name
    report['checks'].append(name)
    output.write_text(json.dumps(report, indent=2))


def tests():
    global enabled
    roots = yield {'root': 'menubar', 'path': []}, False
    report['object_roots'] = [i.get('title') for i in roots]
    check({'File', 'Edit', 'Render', 'View', 'Select', 'Add', 'Object'} <= set(report['object_roots']),
          'Real object-mode menu roots')
    file = find(roots, 'File')
    file_items = yield route(file)
    report['file_items'] = file_items
    save_as = find(file_items, 'Save As...')
    save_copy = find(file_items, 'Save Copy...')
    check(save_as['operator'] == save_copy['operator'] and save_as['route'] != save_copy['route'],
          'Save As and Save Copy preserve different operator properties')
    imports = find(file_items, 'Import')
    import_items = yield route(imports)
    check(any(i.get('operator') for i in import_items), 'Real registered import operators')
    execute = find(file_items, 'Native Probe Execute')
    invoke = find(file_items, 'Native Probe Invoke')
    yield route(execute, True)
    check(calls[-1] == (73, 'Exact properties: "quoted"', True, 'VIEW_3D', 'WINDOW', 'EXEC'),
          'Exact typed properties and EXEC_REGION_WIN dispatch')
    yield route(invoke, True)
    check(calls[-1] == (19, 'Invoke properties', False, 'VIEW_3D', 'WINDOW', 'INVOKE'),
          'INVOKE_REGION_WIN dispatch')
    toggle = find(file_items, 'Native Probe Toggle')
    check(bool(toggle['checked']) == initial_extension, 'Live checkbox state')
    yield route(toggle, True)
    check(bpy.context.scene.render.use_file_extension != initial_extension, 'RNA property toggle dispatch')
    yield route(toggle, True)
    enabled = False
    disabled_items = yield route(file)
    check(find(disabled_items, 'Native Probe Execute')['disabled'], 'Live operator polling disables item')
    count = len(calls)
    yield route(execute, True)
    check(len(calls) == count, 'Stale enabled item re-polled on execution')
    enabled = True
    bpy.types.TOPBAR_MT_file.remove(append_probe)
    yield route(execute, True)
    check(len(calls) == count, 'Removed add-on command cannot dispatch stale route')
    context_items = yield {'root': 'context', 'path': []}, False
    report['object_context'] = context_items
    context_execute = find(context_items, 'Native Probe Execute')
    check(context_execute['route']['root'] == 'VIEW3D_MT_object_context_menu',
          'Radial root resolves to the actual object context menu')
    check(context_execute['action'] and not context_execute['submenu'],
          'Action metadata distinguishes real commands from labels')
    yield route(context_execute, True)
    check(calls[-1] == (73, 'Exact properties: "quoted"', True, 'VIEW_3D', 'WINDOW', 'EXEC'),
          'Radial command preserves Blender operator properties and invocation context')
    origin = find(context_items, 'Set Origin')
    origin_items = yield route(origin)
    check(origin['submenu'] and len([i for i in origin_items if i.get('action')]) > 1,
          'Blender Set Origin submenu projects its real choices')
    enabled = False
    disabled_context = yield {'root': 'context', 'path': []}, False
    check(find(disabled_context, 'Native Probe Execute')['disabled'], 'Radial command polling')
    count = len(calls)
    yield route(context_execute, True)
    check(len(calls) == count, 'Disabled radial command cannot execute a stale route')
    enabled = True
    bpy.ops.object.mode_set(mode='EDIT')
    yield route(context_execute, True)
    check(len(calls) == count, 'Object radial route rejected after switching to edit mode')
    for selection, title in [('VERT', 'Extrude Vertices'), ('EDGE', 'Extrude Edges'), ('FACE', 'Extrude Faces')]:
        bpy.context.scene.tool_settings.mesh_select_mode = tuple(
            selection == item for item in ('VERT', 'EDGE', 'FACE'))
        mesh_items = yield {'root': 'context', 'path': []}, False
        check(find(mesh_items, title)['route']['root'] == 'VIEW3D_MT_edit_mesh_context_menu',
              f'Radial mesh context follows {selection} selection mode')
    edit_roots = yield {'root': 'menubar', 'path': []}, False
    titles = [i.get('title') for i in edit_roots]
    report['edit_roots'] = titles
    check('Mesh' in titles and 'Object' not in titles, 'Menu roots follow mesh edit mode')
    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.mode_set(mode='SCULPT')
    sculpt_items = yield {'root': 'context', 'path': []}, False
    check(len(sculpt_items) == 1 and
          sculpt_items[0]['route']['root'] == 'VIEW3D_PT_sculpt_context_menu' and
          sculpt_items[0]['operator'] == 'WM_OT_call_panel',
          'Brush mode exposes the real contextual settings panel')
    bpy.ops.object.mode_set(mode='OBJECT')
    report['passed'] = True


generator = None
pending = None
executing = False


def tick():
    global generator, pending, executing
    try:
        if generator is None:
            bpy.utils.register_class(WM_OT_native_menu_probe)
            bpy.types.TOPBAR_MT_file.append(append_probe)
            bpy.types.VIEW3D_MT_object_context_menu.append(append_probe)
            generator = tests()
            query, executing = next(generator)
        else:
            if executing:
                result = None
            else:
                pointer = take_result(pending)
                if not pointer:
                    return 0.1
                result = json.loads(ctypes.string_at(pointer))
                free(pointer)
            query, executing = generator.send(result)
        if query['root'] == 'context' or query['root'].endswith('_context_menu'):
            pending = positioned_request(json.dumps(query).encode(), executing, 200, 300)
        else:
            pending = request(json.dumps(query).encode(), executing)
        return 0.3
    except StopIteration:
        pass
    except Exception:
        report['error'] = traceback.format_exc()
    try:
        bpy.types.TOPBAR_MT_file.remove(append_probe)
        bpy.types.VIEW3D_MT_object_context_menu.remove(append_probe)
        bpy.utils.unregister_class(WM_OT_native_menu_probe)
        bpy.context.scene.render.use_file_extension = initial_extension
        if bpy.context.object and bpy.context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
    except Exception:
        report['cleanup_error'] = traceback.format_exc()
    output.write_text(json.dumps(report, indent=2))
    return None


bpy.app.timers.register(tick, first_interval=4)
