# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Covers object selection across viewport shading modes, as well as element
selection in edit, pose and paint modes (mesh vertices/edges/faces, armature
edit & pose bones, grease pencil points, curves points and weight-paint
vertices).

The test functions are generators invoked by ``run_blender_setup.py``, they do not run when this file is imported directly.
"""

import modules.ui_test_utils as ui

# Shading modes object selection is tested against.
SHADING_TYPES = ('SOLID', 'WIREFRAME', 'MATERIAL', 'RENDERED')


# -----------------------------------------------------------------------------
# View3D helpers
# -----------------------------------------------------------------------------


def _setup_view3d(window, *, shading_type='SOLID', view_axis='FRONT'):
    """Switch the largest area to View3D, set shading/axis, return (area, region).

    This is the single helper that mutates the area type to VIEW_3D.
    """
    import bpy
    area = ui.largest_area(window.screen)
    area.type = 'VIEW_3D'
    region = next(r for r in area.regions if r.type == 'WINDOW')
    with bpy.context.temp_override(window=window, area=area, region=region):
        area.spaces.active.shading.type = shading_type
        bpy.ops.view3d.view_axis(type=view_axis)
        bpy.ops.view3d.view_all(center=False)
    return area, region


def _deselect_all(operator_path):
    """Deselect everything, ``operator_path`` e.g. ``'mesh.select_all'``."""
    import bpy
    op = bpy.ops
    for part in operator_path.split('.'):
        op = getattr(op, part)
    op(action='DESELECT')


def _view3d_click_at(e, co_world, *, ctrl=False):
    """Send a click at a world-space location in the View3D."""
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    area = next(a for a in e.window.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    co = location_3d_to_region_2d(region, region.data, co_world)
    if co is None:
        raise RuntimeError(f"Point {co_world} is not visible in the View3D")
    location = int(region.x + co[0]), int(region.y + co[1])

    e.cursor_position_set(*location, move=True)
    yield
    if ctrl:
        e.ctrl.leftmouse.tap()
    else:
        e.leftmouse.tap()
    yield


def _assert_only_object_selected(t, selected_obj):
    import bpy
    t.assertEqual(bpy.context.view_layer.objects.active, selected_obj)
    t.assertTrue(selected_obj.select_get())
    t.assertEqual({obj.name for obj in bpy.context.selected_objects}, {selected_obj.name})


def _setup_selection_test(
        setup_data, *, post_setup=None, shading_type='SOLID', view_axis='FRONT'):
    """Create objects, set up View3D, return ``(obj, e, t, window, area)``.

    ``post_setup`` runs after yielding the initial scene setup. This is needed
    for operators that require the window context restored after loading a
    startup file. Mode changes and deselect are left to the caller.
    """
    obj = setup_data()
    yield
    e, t, window = ui.test_window()
    if post_setup is not None:
        post_setup(obj)
    area, _region = _setup_view3d(window, shading_type=shading_type, view_axis=view_axis)
    return obj, e, t, window, area


# -----------------------------------------------------------------------------
# Object selection
# -----------------------------------------------------------------------------


def _setup_scene_objects():
    """Two objects placed either side of the origin."""
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)

    bpy.ops.mesh.primitive_cube_add(location=(-3.0, 0.0, 0.0))
    cube = bpy.context.view_layer.objects.active
    cube.name = "SelectionCube"

    bpy.ops.mesh.primitive_uv_sphere_add(location=(3.0, 0.0, 0.0))
    sphere = bpy.context.view_layer.objects.active
    sphere.name = "SelectionSphere"

    return cube, sphere


def _test_view3d_selection_in_shading(shading_type):
    obj, e, t, _window, area = yield from _setup_selection_test(
        _setup_scene_objects, shading_type=shading_type)
    cube, sphere = obj
    t.assertEqual(area.spaces.active.shading.type, shading_type)

    import bpy
    bpy.ops.object.select_all(action='DESELECT')
    yield

    yield from _view3d_click_at(e, sphere.matrix_world.translation, ctrl=True)
    _assert_only_object_selected(t, sphere)

    yield from _view3d_click_at(e, cube.matrix_world.translation, ctrl=True)
    _assert_only_object_selected(t, cube)


def _setup_depth_cycling_objects():
    """Three overlapping cubes ordered from nearest to farthest in front view."""
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)

    objects = []
    for name, y in (('DepthNear', -2.0), ('DepthMiddle', 0.0), ('DepthFar', 2.0)):
        bpy.ops.mesh.primitive_cube_add(location=(0.0, y, 0.0))
        obj = bpy.context.view_layer.objects.active
        obj.name = name
        objects.append(obj)
    return objects


def test_view3d_selection_depth_cycling():
    """Repeated clicks at a stationary cursor select nearest to farthest."""
    objects, e, t, _window, _area = yield from _setup_selection_test(_setup_depth_cycling_objects)

    import bpy
    bpy.ops.object.select_all(action='DESELECT')
    yield

    for obj in (*objects, objects[0]):
        yield from _view3d_click_at(e, (0.0, 0.0, 0.0))
        _assert_only_object_selected(t, obj)


def test_view3d_selection_all_shading_modes():
    for shading_type in SHADING_TYPES:
        yield from _test_view3d_selection_in_shading(shading_type)


# -----------------------------------------------------------------------------
# Mesh edit mode selection
# -----------------------------------------------------------------------------


def _setup_plane():
    """A default plane, a single 2x2 face with 4 corner vertices at the origin."""
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)

    bpy.ops.mesh.primitive_plane_add(location=(0.0, 0.0, 0.0))
    return bpy.context.view_layer.objects.active


def _mesh_set_select_mode(select_type):
    """Switch the mesh selection domain, clearing the selection."""
    import bpy
    bpy.ops.mesh.select_mode(type=select_type)
    bpy.ops.mesh.select_all(action='DESELECT')
    yield


def _mesh_edit_selected(obj, domain):
    import bmesh
    bm = bmesh.from_edit_mesh(obj.data)
    elements = {'VERT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
    return {element.index for element in elements if element.select}


def _mesh_edit_active_vertex(obj):
    import bmesh
    bm = bmesh.from_edit_mesh(obj.data)
    active = bm.select_history.active
    return None if active is None else active.index


def test_view3d_mesh_edit_selection():
    obj, e, t, window, _area = yield from _setup_selection_test(_setup_plane)

    import bpy
    bpy.ops.object.mode_set(mode='EDIT')
    yield

    # The default plane has vertex 0 at (-1, -1), edge 1 through (0, -1)
    # and a single face at the origin.
    yield from _mesh_set_select_mode('VERT')
    yield from _view3d_click_at(e, (-1.0, -1.0, 0.0))
    t.assertEqual(_mesh_edit_selected(obj, 'VERT'), {0})
    t.assertEqual(_mesh_edit_active_vertex(obj), 0)

    yield from _mesh_set_select_mode('EDGE')
    yield from _view3d_click_at(e, (0.0, -1.0, 0.0))
    t.assertEqual(_mesh_edit_selected(obj, 'EDGE'), {1})

    yield from _mesh_set_select_mode('FACE')
    # Face picking samples the select-buffer at a single pixel, so reframe
    # the view to force a fresh select-buffer after switching selection domain.
    _setup_view3d(window, view_axis='TOP')
    yield
    yield from _view3d_click_at(e, (0.4, 0.4, 0.0))
    t.assertEqual(_mesh_edit_selected(obj, 'FACE'), {0})


# -----------------------------------------------------------------------------
# Armature edit & pose mode selection
# -----------------------------------------------------------------------------


def _setup_armature():
    """An armature with a single default bone at the origin."""
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)

    bpy.ops.object.armature_add(location=(0.0, 0.0, 0.0))
    return bpy.context.view_layer.objects.active


def _armature_edit_bones_setup(obj):
    """Give the armature two named bones: ``Bone1`` at X=0 and ``Bone2`` at X=2."""
    import bpy
    bpy.ops.object.mode_set(mode='EDIT')
    bones = obj.data.edit_bones
    bones[0].name = 'Bone1'
    bones['Bone1'].head = (0.0, 0.0, 0.0)
    bones['Bone1'].tail = (0.0, 0.0, 1.0)
    bone2 = bones.new('Bone2')
    bone2.head = (2.0, 0.0, 0.0)
    bone2.tail = (2.0, 0.0, 1.0)
    # Leave edit mode so the rest-pose bones (used for the object bounds by
    # ``view3d.view_all``) include both bones, allowing the view to frame them.
    bpy.ops.object.mode_set(mode='OBJECT')


def _armature_edit_selected_bones(obj):
    return {bone.name for bone in obj.data.edit_bones if bone.select}


def _armature_pose_selected_bones(obj):
    return {bone.name for bone in obj.pose.bones if bone.select}


def _test_view3d_armature_selection(mode, deselect_operator, get_selected, check_active):
    """Click the body of each bone and assert the resulting selection.
    Clicking a bone head only selects the head, so the tests click the bodies.
    """
    obj, e, t, _window, _area = yield from _setup_selection_test(
        _setup_armature, post_setup=_armature_edit_bones_setup)

    import bpy
    bpy.ops.object.mode_set(mode=mode)
    yield

    _deselect_all(deselect_operator)
    yield

    for bone_name, co_world in (('Bone1', (0.0, 0.0, 0.5)), ('Bone2', (2.0, 0.0, 0.5))):
        yield from _view3d_click_at(e, co_world)
        t.assertEqual(get_selected(obj), {bone_name})
        if check_active:
            t.assertEqual(obj.data.edit_bones.active.name, bone_name)


def test_view3d_armature_edit_selection():
    yield from _test_view3d_armature_selection(
        mode='EDIT',
        deselect_operator='armature.select_all',
        get_selected=_armature_edit_selected_bones,
        check_active=True,
    )


def test_view3d_armature_pose_selection():
    yield from _test_view3d_armature_selection(
        mode='POSE',
        deselect_operator='pose.select_all',
        get_selected=_armature_pose_selected_bones,
        check_active=False,
    )


# -----------------------------------------------------------------------------
# Point selection in edit mode (grease pencil & curves)
# -----------------------------------------------------------------------------


def _link_object(name, data):
    """Link a data-block as a new active, selected object."""
    import bpy
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj


def _set_points_along_x(points, spacing):
    """Position the points of a line along X at ``spacing`` intervals.

    ``points`` is an indexable point sequence. Attribute data (e.g. a curves
    ``position`` attribute) assigns its values directly.
    """
    for i in range(len(points)):
        co = (float(i) * spacing, 0.0, 0.0)
        point = points[i]
        if hasattr(point, 'position'):
            point.position = co
        else:
            points[i] = co


def _attribute_selected(obj, attribute_name):
    """Indices of the selected values in a boolean attribute (e.g. ``'.select_vert'``)."""
    attribute = obj.data.attributes[attribute_name]
    return {i for i, value in enumerate(attribute.data) if value.value}


def _grease_pencil_selected_points(obj):
    stroke = obj.data.layers[0].frames[0].drawing.strokes[0]
    return {i for i, point in enumerate(stroke.points) if point.select}


def _setup_grease_pencil_edit():
    """A grease pencil object with a single 3-point stroke along X."""
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)

    gp = bpy.data.grease_pencils.new("SelectionGP")
    obj = _link_object("SelectionGP", gp)

    layer = gp.layers.new("Layer1")
    frame = layer.frames.new(0)
    frame.drawing.add_strokes([3])
    stroke = frame.drawing.strokes[0]
    _set_points_along_x(stroke.points, spacing=0.5)
    for point in stroke.points:
        point.radius = 0.05
    return obj


def _setup_curves_edit():
    """A curves object with a single 3-point curve along X."""
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)

    curves = bpy.data.hair_curves.new('SelectionCurves')
    obj = _link_object('SelectionCurves', curves)

    curves.add_curves([3])
    _set_points_along_x(curves.points, spacing=1.0)
    return obj


def _test_view3d_point_selection(setup_data, *, deselect_operator, get_selected, click_coords):
    """Click points in edit mode and assert the resulting selection."""
    obj, e, t, _window, _area = yield from _setup_selection_test(setup_data)

    import bpy
    bpy.ops.object.mode_set(mode='EDIT')
    yield

    _deselect_all(deselect_operator)
    yield

    for co_world, expected in click_coords:
        yield from _view3d_click_at(e, co_world)
        t.assertEqual(get_selected(obj), expected)


def test_view3d_grease_pencil_point_selection():
    yield from _test_view3d_point_selection(
        _setup_grease_pencil_edit,
        deselect_operator='grease_pencil.select_all',
        get_selected=_grease_pencil_selected_points,
        click_coords=(((0.0, 0.0, 0.0), {0}), ((1.0, 0.0, 0.0), {2})),
    )


def test_view3d_curves_point_selection():
    yield from _test_view3d_point_selection(
        _setup_curves_edit,
        deselect_operator='curves.select_all',
        get_selected=lambda obj: _attribute_selected(obj, '.selection'),
        click_coords=(((0.0, 0.0, 0.0), {0}), ((2.0, 0.0, 0.0), {2})),
    )


# -----------------------------------------------------------------------------
# Weight paint vertex selection
# -----------------------------------------------------------------------------


def test_view3d_weight_paint_vertex_selection():
    import bpy
    obj, e, t, window, _area = yield from _setup_selection_test(_setup_plane)

    bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
    yield

    obj.data.use_paint_mask_vertex = True
    # With vertex selection enabled the brush does not select, so switch to
    # the select tool for LMB to run ``view3d.select``. Assigning the tool
    # requires the View3D area in the context.
    area, _ = _setup_view3d(window)
    with bpy.context.temp_override(window=window, area=area):
        bpy.ops.wm.tool_set_by_id(name="builtin.select")
    yield

    _deselect_all('paint.vert_select_all')
    yield

    # Click the corner vertex at (-1, -1).
    yield from _view3d_click_at(e, (-1.0, -1.0, 0.0))
    t.assertEqual(_attribute_selected(obj, '.select_vert'), {0})
