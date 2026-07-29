# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything; its methods are accessed by run_blender_setup.py.

Tests for Info Editor / Reports functionality:
  - Filter property state (RNA round-trip only, see NOTE below)
  - Selection modes
  - Copying reports
  - Deleting reports
"""

import modules.ui_test_utils as ui


def _get_window_region(area):
    for region in area.regions:
        if region.type == "WINDOW":
            return region
    return None


def _setup_info_area():
    import bpy

    bpy.ops.wm.read_homefile(use_empty=True)
    yield

    e, t, window = ui.test_window()
    area = ui.largest_area(window.screen)
    area.type = 'INFO'

    # Perform operations to populate the Info editor with operator reports
    bpy.ops.mesh.primitive_cube_add()
    bpy.ops.transform.translate(value=(1.0, 2.0, 3.0))
    bpy.ops.object.shade_smooth()
    bpy.ops.object.modifier_add(type='BEVEL')
    yield
    
    return e, t, window, area


def _set_report_filters(space, *, debug=False, info=False,
                        warning=False, error=False, operator=False):
    space.show_report_debug = debug
    space.show_report_info = info
    space.show_report_warning = warning
    space.show_report_error = error
    space.show_report_operator = operator


def _select_reports(area, action, t):
    import bpy

    region = _get_window_region(area)

    with bpy.context.temp_override(area=area, region=region):
        result = bpy.ops.info.select_all(action=action)

    t.assertEqual(result, {'FINISHED'}, f"{action} selection should complete")
    yield


def _copy_reports(area, t):
    import bpy

    region = _get_window_region(area)

    result = {'CANCELLED'}
    with bpy.context.temp_override(area=area, region=region):
        if bpy.ops.info.report_copy.poll():
            result = bpy.ops.info.report_copy()

    t.assertEqual(result, {'FINISHED'}, "Copy reports should complete")
    yield


def _delete_reports(area, t):
    import bpy

    region = _get_window_region(area)

    result = {'CANCELLED'}
    with bpy.context.temp_override(area=area, region=region):
        if bpy.ops.info.report_delete.poll():
            result = bpy.ops.info.report_delete()

    t.assertEqual(result, {'FINISHED'}, "Delete reports should complete")
    yield


def test_info_report_filters():
    """
    Verify report filter properties.
    """
    import bpy

    e, t, window, area = yield from _setup_info_area()

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceInfo)

    filter_states = [
        ("all_on", dict(debug=True, info=True, warning=True, error=True, operator=True)),
        ("info_only", dict(info=True)),
        ("error_only", dict(error=True)),
        ("all_off", dict()),
    ]

    for case_name, filter_state in filter_states:
        _set_report_filters(space, **filter_state)

        for prop_name, expected_value in (
            ("show_report_debug", filter_state.get("debug", False)),
            ("show_report_info", filter_state.get("info", False)),
            ("show_report_warning", filter_state.get("warning", False)),
            ("show_report_error", filter_state.get("error", False)),
            ("show_report_operator", filter_state.get("operator", False)),
        ):
            t.assertEqual(
                getattr(space, prop_name),
                expected_value,
                f"{case_name}: {prop_name} did not round-trip",
            )

        yield

        # The current build does not expose the Info report list through
        # Python in a way that can be asserted reliably here, so this test
        # verifies the operators complete successfully for each filter state.
        yield from _select_reports(area, 'SELECT', t)
        yield from _copy_reports(area, t)


def test_info_report_selection_modes():
    """
    Verify selection modes.
    """
    import bpy

    e, t, window, area = yield from _setup_info_area()

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceInfo)

    _set_report_filters(
        space,
        debug=True,
        info=True,
        warning=True,
        error=True,
        operator=True,
    )

    yield
    for action in ("SELECT", "DESELECT"):
        yield from _select_reports(area, action, t)
        yield from _copy_reports(area, t)


def test_info_report_delete():
    """
    Verify deleting reports.
    """
    import bpy

    e, t, window, area = yield from _setup_info_area()

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceInfo)

    _set_report_filters(
        space,
        debug=True,
        info=True,
        warning=True,
        error=True,
        operator=True,
    )

    yield from _select_reports(area, 'SELECT', t)
    yield from _delete_reports(area, t)

    yield from _select_reports(area, 'SELECT', t)
    yield from _delete_reports(area, t)


def test_info_report_operator_poll():
    """
    Verify info operators are available.
    """
    import bpy

    e, t, window, area = yield from _setup_info_area()

    region = _get_window_region(area)

    with bpy.context.temp_override(area=area, region=region):
        t.assertTrue(
            bpy.ops.info.select_all.poll(),
            "select_all unavailable",
        )

        t.assertTrue(
            bpy.ops.info.report_copy.poll(),
            "report_copy unavailable",
        )
