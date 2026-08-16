# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything; its methods are accessed by run_blender_setup.py.
"""

import modules.ui_test_utils as ui


def test_region_split():
    _, t, window = ui.test_window()
    screen = window.screen
    initial_count = len(screen.areas)

    ui.split_area(ui.largest_area(screen), "VERTICAL", 0.5)
    yield

    t.assertEqual(len(screen.areas), initial_count + 1, "Area split did not create a new area")


def test_region_join():
    _, t, window = ui.test_window()
    screen = window.screen
    initial_count = len(screen.areas)

    old_areas, source = ui.split_area_pair(screen)
    yield

    target = ui.get_new_area(screen, old_areas)

    t.assertEqual(len(screen.areas), initial_count + 1, "Split did not create a new area")
    ui.join_area(source, target)
    yield

    t.assertEqual(len(screen.areas), initial_count, "Join did not remove the split area")


def test_region_swap():
    _, t, window = ui.test_window()
    screen = window.screen

    old_areas, source = ui.split_area_pair(screen)
    yield

    target = ui.get_new_area(screen, old_areas)

    t.assertEqual(len(screen.areas), len(old_areas) + 1, "Split did not create a new area")

    source.type = "VIEW_3D"
    target.type = "OUTLINER"

    ui.swap_area(source, target)
    yield

    editors = {
        area.type
        for area in screen.areas
    }

    t.assertTrue(
        {"VIEW_3D", "OUTLINER"}.issubset(editors),
        "Swap removed one of the editor areas",
    )
