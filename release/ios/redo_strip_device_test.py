# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise native last-operation controls against the real Blender undo stack."""
import ctypes
import json
from pathlib import Path
import traceback
import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_redo
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
engine = ctypes.CDLL(None)
enqueue = engine.WM_apple_ios_ui_operator_enqueue
enqueue.argtypes = [ctypes.c_char_p]
setting = engine.WM_apple_ios_ui_setting_enqueue
setting.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
command = engine.WM_apple_ios_ui_command_enqueue
command.argtypes = [ctypes.c_int]
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/redo-strip-test.json'
output.write_text(json.dumps(report))


def ui(**request):
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, label):
    state = ui()
    assert not state.get('alert'), label + ': ' + str(state)
    assert not state['snapshot'].get('error'), label + ': ' + str(state)
    assert ok, label + ': ' + str(state)
    report['checks'].append(label)


def prop(name, index=None):
    return next(p for p in ui()['snapshot']['properties']
                if p['property'] == name and (index is None or p.get('index') == index))


def tests():
    yield 1.0
    check(not ui()['visible'], 'No strip at startup')
    enqueue(b'MESH_OT_primitive_cube_add')
    yield 1.5
    state = ui()
    check(state['visible'] and state['snapshot']['operator'] == 'MESH_OT_primitive_cube_add',
          'Native Add Cube automatically opens its actual redo properties')
    check(state['frame'][3] == 60 and state['frame'][2] > 300, 'Strip is thin and fits the physical iPad')
    check(all(name in {p['property'] for p in state['snapshot']['properties']}
              for name in ['size', 'location', 'rotation']), 'Actual operator layout exposes dimensions and transform components')
    identity = state['snapshot']['id']
    count = len(bpy.data.objects)
    ui(action='edit', key=prop('size')['key'], value=4)
    yield 1.0
    check(abs(bpy.context.object.dimensions.x - 4) < 1e-5 and len(bpy.data.objects) == count,
          'Native numeric edit repeats Add Cube without creating another object')
    ui(action='edit', key=prop('size')['key'], value=3)
    yield 1.0
    check(abs(bpy.context.object.dimensions.x - 3) < 1e-5 and ui()['snapshot']['id'] == identity,
          'Repeated edits preserve the operator identity and replace the result')
    ui(action='edit', key=prop('location', 0)['key'], value=2)
    yield 1.0
    check(abs(bpy.context.object.location.x - 2) < 1e-5 and abs(bpy.context.object.dimensions.x - 3) < 1e-5,
          'Vector component edit preserves other adjusted properties')
    uv = prop('calc_uvs')
    ui(action='edit', key=uv['key'], value=False)
    yield 1.0
    check(len(bpy.context.object.data.uv_layers) == 0, 'Native switch adjusts generated UVs through real redo')
    report['stage'] = 'cube'
    yield 8.0
    ui(action='done')
    yield 0.5
    check(not ui()['visible'] and len(bpy.data.objects) == count, 'Checkmark dismisses without undoing the operation')
    yield 0.8
    check(not ui()['visible'], 'Engine snapshots do not reopen a dismissed operation')
    enqueue(b'ED_OT_undo')
    yield 1.0
    check(len(bpy.data.objects) == count - 1, 'One Undo removes the whole adjusted operation, not an intermediate edit')
    enqueue(b'ED_OT_redo')
    yield 1.0
    check(len(bpy.data.objects) == count and abs(bpy.context.object.dimensions.x - 3) < 1e-5 and
          abs(bpy.context.object.location.x - 2) < 1e-5, 'Redo restores the final adjusted result')
    enqueue(b'MESH_OT_primitive_cylinder_add')
    yield 1.5
    check(ui()['visible'] and ui()['snapshot']['id'] != identity, 'Next operator opens a fresh strip')
    new_identity = ui()['snapshot']['id']
    before = tuple(bpy.context.object.dimensions)
    setting(b'native_redo_edit', json.dumps({'id': identity, 'key': uv['key'], 'value': True}).encode())
    yield 0.5
    check(ui()['snapshot']['id'] == new_identity and tuple(bpy.context.object.dimensions) == before,
          'Stale messages from an earlier operation cannot edit the new one')
    ui(action='edit', key=prop('vertices')['key'], value=12)
    yield 1.0
    check(len(bpy.context.object.data.vertices) == 24, 'Integer control changes cylinder topology')
    ui(action='edit', key=prop('end_fill_type')['key'], value='NOTHING')
    yield 1.0
    check(len(bpy.context.object.data.polygons) == 12, 'Native enum adjusts the cylinder cap type')
    report['stage'] = 'cylinder'
    yield 8.0
    ui(action='done')
    yield 0.3
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    command(1)
    yield 0.5
    import bmesh
    original_faces = len(bmesh.from_edit_mesh(bpy.context.object.data).faces)
    enqueue(b'MESH_OT_subdivide')
    yield 1.0
    check(ui()['visible'] and ui()['snapshot']['operator'] == 'MESH_OT_subdivide', 'Edit Mode operator exposes its own controls')
    ui(action='edit', key=prop('number_cuts')['key'], value=2)
    yield 1.0
    check(prop('number_cuts')['value'] == 2 and
          len(bmesh.from_edit_mesh(bpy.context.object.data).faces) == original_faces * 9,
          'Edit Mode adjustment replaces subdivision with the requested topology')
    check(not any(r.type == 'HUD' and r.width > 1 and r.height > 1 for r in area.regions),
          'Desktop redo panel does not appear beneath the native strip')
    before_z = max(v.co.z for v in bmesh.from_edit_mesh(bpy.context.object.data).verts)
    with bpy.context.temp_override(area=area, region=region):
        bpy.ops.mesh.extrude_region_move('EXEC_DEFAULT', True, TRANSFORM_OT_translate={'value': (0, 0, 1)})
    yield 1.0
    check(ui()['snapshot']['operator'] == 'MESH_OT_extrude_region_move' and prop('value', 2)['value'] == 1,
          'Macro operators expose the transform child properties')
    vertex_count = len(bmesh.from_edit_mesh(bpy.context.object.data).verts)
    ui(action='edit', key=prop('value', 2)['key'], value=3)
    yield 1.0
    mesh = bmesh.from_edit_mesh(bpy.context.object.data)
    check(abs(max(v.co.z for v in mesh.verts) - before_z - 3) < 1e-5 and len(mesh.verts) == vertex_count,
          'Macro adjustment redoes the whole extrusion without duplicating geometry')
    report['passed'] = True
    report['stage'] = 'extrude'


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
