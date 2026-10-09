# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import modules.ui_test_utils as ui


def test_outliner_vertex_group_rename_updates_modifier_reference():
    import bpy

    bpy.ops.wm.read_homefile(use_empty=True)
    e, t, window = ui.test_window()
    area = ui.largest_area(window.screen)
    area.type = 'OUTLINER'
    yield

    space = area.spaces.active
    space.display_mode = 'VIEW_LAYER'

    mesh = bpy.data.meshes.new("Mesh")
    obj = bpy.data.objects.new("Object", mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.vertex_groups.new(name="Group")
    obj.vertex_groups.new(name="Sentinel")
    modifier = obj.modifiers.new(name="Modifier", type='MASK')
    modifier.vertex_group = "Group"
    obj.vertex_groups.active_index = 1
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    space.filter_text = "Group"
    yield

    x = area.x + min(100, area.width // 3)
    for y in range(area.y + area.height - 40, area.y + 40, -10):
        e.cursor_position_set(x, y, move=True)
        yield e.leftmouse()
        yield
        if obj.vertex_groups.active_index == 0:
            break
    else:
        t.fail("Could not activate the vertex group row in the Outliner")

    t.assertEqual(obj.vertex_groups.active.name, "Group")
    yield e.f2()
    yield
    yield e.ctrl.a()
    yield e.text("RenamedGroup")
    yield e.ret()
    yield

    t.assertEqual(obj.vertex_groups[0].name, "RenamedGroup")
    t.assertEqual(modifier.vertex_group, "RenamedGroup")
