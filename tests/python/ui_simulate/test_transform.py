# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, its methods are accessed for tests by ``run_blender_setup.py``.

Tests for interactive transform, driven by simulated key events.
"""
import modules.ui_test_utils as ui


def _view3d_startup_area_maximized(e):
    yield e.shift.f5()                  # 3D Viewport.
    yield e.ctrl.alt.space()            # Full-screen.


def _assert_location(t, ob, location):
    for axis, (value, expected) in enumerate(zip(ob.location, location)):
        t.assertAlmostEqual(value, expected, places=5, msg="axis {:d}".format(axis))


def view3d_translate_numeric_input_axis_constraint():
    """Typed values with a single axis constraint are applied to that axis."""
    e, t, window = ui.test_window()
    yield from _view3d_startup_area_maximized(e)

    cube = window.view_layer.objects.active
    t.assertEqual(cube.name, "Cube")
    _assert_location(t, cube, (0.0, 0.0, 0.0))

    yield e.g().y().two().ret()
    _assert_location(t, cube, (0.0, 2.0, 0.0))

    yield e.g().z().minus().one().ret()
    _assert_location(t, cube, (0.0, 2.0, -1.0))


def view3d_translate_numeric_input_plane_constraint():
    """
    Typed values with a plane constraint are applied to both constrained axes,
    in axis order (see #163442).
    """
    e, t, window = ui.test_window()
    yield from _view3d_startup_area_maximized(e)

    cube = window.view_layer.objects.active
    t.assertEqual(cube.name, "Cube")
    _assert_location(t, cube, (0.0, 0.0, 0.0))

    # YZ plane: first value is Y, second is Z.
    yield e.g().shift.x().one().tab().two().ret()
    _assert_location(t, cube, (0.0, 1.0, 2.0))

    # XZ plane: first value is X, second is Z.
    yield e.g().shift.y().three().tab().four().ret()
    _assert_location(t, cube, (3.0, 1.0, 6.0))

    # XY plane: first value is X, second is Y.
    yield e.g().shift.z().minus().three().tab().minus().one().ret()
    _assert_location(t, cube, (0.0, 0.0, 6.0))

    # A single typed value on a plane constraint only moves the first axis.
    yield e.g().shift.x().five().ret()
    _assert_location(t, cube, (0.0, 5.0, 6.0))
