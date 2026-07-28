# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, its methods are accessed for tests by ``run_blender_setup.py``.

Regression tests for View3D click selection across viewport shading modes.
"""

import modules.ui_test_utils as ui


def _view3d_area_region(window):
    area = ui.largest_area(window.screen)
    area.type = 'VIEW_3D'
    region = next(region for region in area.regions if region.type == 'WINDOW')
    return area, region


def _view3d_object_calc_screen_space_location(window, obj):
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    _, region = _view3d_area_region(window)
    rv3d = region.data

    co = location_3d_to_region_2d(region, rv3d, obj.matrix_world.translation)
    if co is None:
        raise RuntimeError(f"Object {obj.name} is not visible in the View3D")

    return int(region.x + co[0]), int(region.y + co[1])


def _setup_scene_objects():
    import bpy

    bpy.ops.wm.read_homefile(use_empty=True)

    bpy.ops.mesh.primitive_cube_add(location=(-3.0, 0.0, 0.0))
    cube = bpy.context.view_layer.objects.active
    cube.name = "SelectionCube"

    bpy.ops.mesh.primitive_uv_sphere_add(location=(3.0, 0.0, 0.0))
    sphere = bpy.context.view_layer.objects.active
    sphere.name = "SelectionSphere"

    return cube, sphere


def _setup_view3d(window, shading_type):
    import bpy

    area, region = _view3d_area_region(window)

    with bpy.context.temp_override(window=window, area=area, region=region):
        area.spaces.active.shading.type = shading_type
        bpy.ops.view3d.view_axis(type='FRONT')
        bpy.ops.view3d.view_all(center=False)

    return area


def _click_object(e, obj):
    location = _view3d_object_calc_screen_space_location(e.window, obj)
    e.cursor_position_set(*location, move=True)
    yield
    e.ctrl.leftmouse.tap()
    yield


def _assert_only_object_selected(t, selected_obj, deselected_obj):
    import bpy

    t.assertEqual(bpy.context.view_layer.objects.active, selected_obj)
    t.assertTrue(selected_obj.select_get())
    t.assertFalse(deselected_obj.select_get())
    t.assertEqual({obj.name for obj in bpy.context.selected_objects}, {selected_obj.name})


def _test_view3d_selection_in_shading(shading_type):
    import bpy

    cube, sphere = _setup_scene_objects()
    yield

    e, t, window = ui.test_window()

    area = _setup_view3d(window, shading_type)
    t.assertEqual(area.spaces.active.shading.type, shading_type)
    yield

    bpy.ops.object.select_all(action='DESELECT')
    yield

    # Verify that clicking another object replaces the active selection.
    yield from _click_object(e, sphere)
    _assert_only_object_selected(t, sphere, cube)

    yield from _click_object(e, cube)
    _assert_only_object_selected(t, cube, sphere)


def test_view3d_selection_solid():
    yield from _test_view3d_selection_in_shading('SOLID')


def test_view3d_selection_wireframe():
    yield from _test_view3d_selection_in_shading('WIREFRAME')


def test_view3d_selection_material():
    yield from _test_view3d_selection_in_shading('MATERIAL')


def test_view3d_selection_rendered():
    yield from _test_view3d_selection_in_shading('RENDERED')
