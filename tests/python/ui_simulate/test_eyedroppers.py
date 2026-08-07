# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, its methods are accessed for tests by ``run_blender_setup.py``.

Regression tests for the Object/Bone eyedropper icons that appear on pointer-type search fields
(e.g. a constraint's *Target* object field, or a *Bone* sub-target field).

Tests verify:
- The eyedropper modal operator starts (``UI_OT_eyedropper_id`` / ``UI_OT_eyedropper_bone``) when
  the ``E`` key is pressed while the cursor is over the appropriate Properties-editor field.
- The eyedropper can be cancelled with ``Escape`` without crashing.
- Both cases work regardless of the 3D Viewport shading mode (``SOLID`` and ``WIREFRAME``).
"""

import modules.ui_test_utils as ui


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _build_scene():
    """Create a minimal scene: a mesh cube with a COPY_LOCATION constraint
    that targets a newly-created armature. Returns (mesh_obj, armature_obj)."""
    import bpy

    bpy.ops.wm.read_homefile(use_empty=True)

    # Mesh cube – this will receive the constraint.
    bpy.ops.mesh.primitive_cube_add(location=(0.0, 0.0, 0.0))
    mesh_obj = bpy.context.view_layer.objects.active
    mesh_obj.name = "Constrained"

    # Armature – used as constraint target so the Bone subtarget field appears.
    bpy.ops.object.armature_add(location=(3.0, 0.0, 0.0))
    arm_obj = bpy.context.view_layer.objects.active
    arm_obj.name = "TargetArm"

    # Add COPY_LOCATION on the cube, target the armature.
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = mesh_obj
    mesh_obj.select_set(True)
    con = mesh_obj.constraints.new("COPY_LOCATION")
    con.target = arm_obj  # Makes the Bone subtarget field visible in the UI

    return mesh_obj, arm_obj


def _set_viewport_shading(window, shading_type):
    """Set the VIEW_3D shading type in the given window (if a VIEW_3D area exists)."""
    import bpy
    for area in window.screen.areas:
        if area.type == 'VIEW_3D':
            with bpy.context.temp_override(window=window, area=area):
                area.spaces.active.shading.type = shading_type
            return area
    return None


def _eyedropper_cursor_positions(area, field):
    """Return candidate cursor positions for an eyedropper-enabled field within region bounds."""
    region = next(r for r in area.regions if r.type == 'WINDOW')
    top = region.y + region.height

    if field == "target":
        row_y = top - 84
        x_fractions = (0.28, 0.34, 0.40, 0.46, 0.52, 0.58)
    elif field == "bone":
        row_y = top - 120
        x_fractions = (0.58, 0.64, 0.70, 0.76, 0.82, 0.88)
    else:
        raise ValueError(f"Unknown field: {field!r}")

    x_positions = [region.x + int(region.width * f) for f in x_fractions]
    y_positions = (row_y - 12, row_y, row_y + 12)

    return [(x, y) for y in y_positions for x in x_positions]


def _invoke_eyedropper_from_positions(e, expected_idnames, cursor_positions, ops_before):
    """Try several cursor positions until the expected eyedropper starts cleanly."""
    import bpy

    last_idname = None
    for cx, cy in cursor_positions:
        e.cursor_position_set(cx, cy, move=True)
        yield

        yield e.e()
        yield from ui.idle_until(
            lambda: len(bpy.context.window_manager.operators) > ops_before,
            timeout=1.0,
        )

        ops_after = bpy.context.window_manager.operators
        last_idname = ops_after[-1].bl_idname if ops_after else None

        if last_idname in expected_idnames:
            return last_idname

        # Cancel unexpected modal operator before trying the next position
        if ops_after and len(ops_after) > ops_before:
            yield e.esc()
            yield from ui.idle_until(
                lambda: len(bpy.context.window_manager.operators) <= ops_before,
                timeout=1.0,
            )

    return last_idname


def _eyedropper_test(
    shading,
    expected_operator,
    cursor_positions_fn,
    precondition_fn=None,
):
    import bpy

    mesh_obj, arm_obj = _build_scene()
    yield

    e, t, window = ui.test_window()

    _set_viewport_shading(window, shading)
    yield

    area = ui.largest_area(window.screen)
    area.type = 'PROPERTIES'
    yield

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceProperties)

    bpy.context.view_layer.objects.active = mesh_obj
    yield

    space.context = 'CONSTRAINT'
    yield

    area.tag_redraw()
    yield

    con = mesh_obj.constraints[0]

    if precondition_fn is not None:
        precondition_fn(t, con)

    cursor_positions = cursor_positions_fn(area)

    ops_before = len(bpy.context.window_manager.operators)
    last_idname = yield from _invoke_eyedropper_from_positions(
        e,
        {expected_operator},
        cursor_positions,
        ops_before,
    )

    t.assertEqual(
        last_idname,
        expected_operator,
        f"Expected {expected_operator} to start in {shading} mode, but got {last_idname!r}",
    )

    # Clean up activated eyedropper
    yield e.esc()
    yield from ui.idle_until(
        lambda: len(bpy.context.window_manager.operators) <= ops_before,
        timeout=1.0,
    )

    # Ensure dataset wasn't modified on escape cancel
    t.assertEqual(con.target, arm_obj)
    t.assertEqual(con.subtarget, "")


# ---------------------------------------------------------------------------
# Test Cases
# ---------------------------------------------------------------------------


def object_eyedropper():
    """Test object eyedropper across SOLID and WIREFRAME shading modes."""
    for shading in ('SOLID', 'WIREFRAME'):
        yield from _eyedropper_test(
            shading=shading,
            expected_operator="UI_OT_eyedropper_id",
            cursor_positions_fn=lambda area: _eyedropper_cursor_positions(area, "target"),
        )


def _bone_precondition(t, con):
    t.assertIsNotNone(con.target)
    t.assertEqual(con.target.type, 'ARMATURE')


def bone_eyedropper():
    """Test bone eyedropper across SOLID and WIREFRAME shading modes."""
    for shading in ('SOLID', 'WIREFRAME'):
        yield from _eyedropper_test(
            shading=shading,
            expected_operator="UI_OT_eyedropper_bone",
            cursor_positions_fn=lambda area: _eyedropper_cursor_positions(area, "bone"),
            precondition_fn=_bone_precondition,
        )


def eyedropper_properties_redraw():
    """Test ensuring Properties redraws cleanly after viewport shading changes."""
    import bpy

    mesh_obj, _ = _build_scene()
    yield

    _, t, window = ui.test_window()

    area = ui.largest_area(window.screen)
    area.type = 'PROPERTIES'
    yield

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceProperties)

    bpy.context.view_layer.objects.active = mesh_obj
    yield

    space.context = 'CONSTRAINT'
    yield

    for shading in ('SOLID', 'WIREFRAME'):
        v3d_area = _set_viewport_shading(window, shading)
        yield

        if v3d_area:
            v3d_area.tag_redraw()
        area.tag_redraw()
        yield

    t.assertIsNotNone(mesh_obj.constraints[0].target, "Constraint target lost during draw test")
