# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Drive the native hover handler through the real engine and Metal viewport."""
import ctypes
import json
from pathlib import Path
import traceback

import bpy
import bmesh
from bpy_extras.view3d_utils import location_3d_to_region_2d
from mathutils import Quaternion, Vector

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
move = probe.blender_device_probe_pencil_hover
move.argtypes = [ctypes.c_int, ctypes.c_double, ctypes.c_double]
engine = ctypes.CDLL(None)
snapshot = engine.WM_apple_ios_ui_pencil_hover_copy_json
snapshot.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
output = Path.home() / 'Documents/pencil-hover-test.json'
report = {'checks': []}
output.write_text(json.dumps(report))


def state():
    ptr = snapshot()
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(ok, message):
    assert ok, message + ': ' + str(state())
    report['checks'].append(message)


def tests():
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    rv3d = space.region_3d
    rv3d.view_rotation = Quaternion((1, 0, 0, 0))
    rv3d.view_location = Vector((0, 0, 0))
    rv3d.view_distance = 8
    rv3d.view_perspective = 'ORTHO'
    space.show_gizmo = False
    space.shading.type = 'SOLID'
    space.shading.show_xray = False
    cube = bpy.data.objects['Cube']
    uid = cube.session_uid
    selected = [o.session_uid for o in bpy.context.selected_objects]
    active = bpy.context.view_layer.objects.active.session_uid

    def hover(point):
        xy = location_3d_to_region_2d(region, rv3d, Vector(point))
        assert xy is not None
        move(1, xy.x / region.width, xy.y / region.height)

    yield 2.0
    hover((0, 0, 1))
    yield 1.0
    check(state()['kind'] == 1 and state()['object'] == uid, 'Object hover identifies the real cube')
    report['stage'] = 'object'
    yield 6.0
    report['stage'] = None
    check([o.session_uid for o in bpy.context.selected_objects] == selected and
          bpy.context.view_layer.objects.active.session_uid == active, 'Hover never changes selection or active object')
    move(0, 0.5, 0.5)
    yield 0.3
    check(state()['kind'] == 0, 'Leaving hover range clears object feedback')
    report['stage'] = 'cleared'
    yield 3.0
    report['stage'] = None
    hover((0, 0, 1))
    yield 0.3
    move(2, 0, 0)
    yield 0.3
    check(state()['kind'] == 0, 'Pencil stroke cancellation clears hover')
    hover((0, 0, 1))
    yield 0.3
    move(1, 0.02, 0.97)
    yield 0.3
    check(state()['kind'] == 0, 'Native top toolbar excludes viewport hover')
    hover((4, 3, 0))
    yield 0.3
    check(state()['kind'] == 0, 'Empty space clears object hover')
    hover((0, 0, 1))
    yield 0.3
    cube.hide_set(True)
    yield 0.5
    check(state()['kind'] == 0, 'Hiding the hovered object invalidates stationary hover')
    cube.hide_set(False)
    yield 0.5
    check(state()['object'] == uid, 'Unhiding recomputes a stationary hover')
    with bpy.context.temp_override(area=area, region=region):
        bpy.ops.object.mode_set(mode='EDIT')
    yield 0.5
    for name, mode, point, kind in [
        ('vertex', (True, False, False), (-1, -1, 1), 2),
        ('edge', (False, True, False), (0, -1, 1), 3),
        ('face', (False, False, True), (0, 0, 1), 4),
    ]:
        bpy.context.tool_settings.mesh_select_mode = mode
        yield 0.5
        bm = bmesh.from_edit_mesh(cube.data)
        before = ([v.select for v in bm.verts], [e.select for e in bm.edges], [f.select for f in bm.faces],
                  list(bm.select_history))
        hover(point)
        yield 0.5
        hit = state()
        check(hit['kind'] == kind and hit['object'] == uid and hit['index'] >= 0,
              name + ' hover follows Blender selection mode')
        elements = {2: bm.verts, 3: bm.edges, 4: bm.faces}[kind]
        elements.ensure_lookup_table()
        elem = elements[hit['index']]
        verts = [elem] if kind == 2 else list(elem.verts)
        check(all(v.co.z > 0.9 for v in verts), name + ' picking chooses visible front geometry')
        check(before == ([v.select for v in bm.verts], [e.select for e in bm.edges], [f.select for f in bm.faces],
                         list(bm.select_history)), name + ' hover leaves mesh selection and history unchanged')
        report['stage'] = name
        yield 6.0
        report['stage'] = None
        move(0, 0.5, 0.5)
        yield 0.3
        check(state()['kind'] == 0, name + ' clears on hover exit')
    hover((0, 0, 1))
    yield 0.3
    space.overlay.show_overlays = False
    yield 0.3
    check(state()['kind'] == 0, 'Disabling overlays clears component feedback')
    space.overlay.show_overlays = True
    space.shading.show_xray = True
    yield 0.5
    check(state()['kind'] == 4, 'X-Ray mode still resolves a face hover')
    space.shading.show_xray = False
    bpy.context.tool_settings.mesh_select_mode = (True, False, False)
    bm = bmesh.from_edit_mesh(cube.data)
    occluded = bm.verts.new((0, 0, 0))
    bmesh.update_edit_mesh(cube.data, loop_triangles=True, destructive=True)
    hover((0, 0, 0))
    yield 0.5
    check(state()['kind'] == 0, 'Solid mode does not highlight an occluded interior vertex')
    space.shading.show_xray = True
    yield 0.5
    check(state()['kind'] == 2, 'X-Ray allows the same occluded vertex to be highlighted')
    occluded.hide_set(True)
    bmesh.update_edit_mesh(cube.data, loop_triangles=False, destructive=False)
    yield 0.5
    check(state()['kind'] == 0, 'Hidden mesh components never highlight, even in X-Ray')
    bm.verts.remove(occluded)
    bmesh.update_edit_mesh(cube.data, loop_triangles=True, destructive=True)
    yield 0.5
    check(state()['kind'] == 0, 'Topology deletion while hovering cannot retain a stale vertex')
    space.shading.show_xray = False
    move(0, 0.5, 0.5)
    with bpy.context.temp_override(area=area, region=region):
        bpy.ops.object.mode_set(mode='OBJECT')
    second = cube.copy()
    second.data = cube.data.copy()
    bpy.context.collection.objects.link(second)
    second.location.x = 3
    second.select_set(True)
    with bpy.context.temp_override(area=area, region=region):
        bpy.ops.object.mode_set(mode='EDIT')
    yield 0.5
    hover((2, -1, 1))
    yield 0.5
    check(state()['kind'] == 2 and state()['object'] == second.session_uid,
          'Multi-object Edit Mode identifies the component owner correctly')
    with bpy.context.temp_override(area=area, region=region):
        bpy.ops.object.mode_set(mode='OBJECT')
    second.select_set(False)
    hover((3, 0, 1))
    yield 0.5
    check(state()['kind'] == 1 and state()['object'] == second.session_uid and not second.select_get() and
          bpy.context.view_layer.objects.active == cube, 'Unselected object hover preserves the active object')
    report['stage'] = 'unselected'
    yield 6.0
    report['stage'] = None
    for shading in ['WIREFRAME', 'SOLID', 'MATERIAL']:
        space.shading.type = shading
        yield 1.0
        # In wireframe, only the actual wires are pickable, not the empty face interior.
        hover((3, -1, 1) if shading == 'WIREFRAME' else (3, 0, 1))
        yield 0.5
        check(state()['object'] == second.session_uid, shading + ' viewport shading supports hover')
    space.shading.type = 'SOLID'
    bpy.data.objects.remove(second, do_unlink=True)
    yield 0.5
    hover((0, 0, 1))
    yield 0.3
    bpy.data.objects.remove(cube, do_unlink=True)
    yield 0.5
    check(state()['kind'] == 0, 'Deleting hovered geometry safely clears its highlight')
    space.show_gizmo = True
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
