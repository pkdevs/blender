# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_pyapi_region_active_item_index.py -- --verbose
import bpy

import os
import tempfile
import unittest

# Any identifier, the region API doesn't check that the list exists.
LISTTYPE_NAME = "test_list"

# Must match the value `direct_link_region` passes to `remove_least_recently_used_states`.
STATES_KEPT_ON_LOAD = 64


def properties_editor_find():
    """
    The window region of the first properties editor (with its screen & area),
    the region the custom properties panel is drawn in.
    """
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type != 'PROPERTIES':
                continue
            for region in area.regions:
                if region.type == 'WINDOW':
                    return screen, area, region
    raise RuntimeError("No properties editor in the default screens")


def properties_window_region():
    return properties_editor_find()[2]


def save_and_reload():
    with tempfile.TemporaryDirectory() as temp_dir:
        filepath = os.path.join(temp_dir, "test.blend")
        bpy.ops.wm.save_as_mainfile(filepath=filepath)
        bpy.ops.wm.open_mainfile(filepath=filepath, load_ui=True)


class TestRegionActiveUIIndex(unittest.TestCase):

    def setUp(self):
        # Start every test from the same file, this discards the region state.
        bpy.ops.wm.read_homefile(use_factory_startup=True)
        self.region = properties_window_region()

    def test_unknown(self):
        # An unknown list, or a list that stores its active item in the data: the first item.
        self.assertEqual(self.region.active_item_index_get(LISTTYPE_NAME, "object"), 0)
        self.assertEqual(self.region.active_item_index_get("OBJECT_UL_vgroups", ""), 0)

    def test_set_get(self):
        self.region.active_item_index_set(LISTTYPE_NAME, "object", 3)
        self.assertEqual(self.region.active_item_index_get(LISTTYPE_NAME, "object"), 3)
        # Only the matching instance is set.
        self.assertEqual(self.region.active_item_index_get(LISTTYPE_NAME, "object.data"), 0)
        self.assertEqual(self.region.active_item_index_get(LISTTYPE_NAME, ""), 0)

        self.region.active_item_index_set(LISTTYPE_NAME, "object", 0)
        self.assertEqual(self.region.active_item_index_get(LISTTYPE_NAME, "object"), 0)

    def test_properties_add(self):
        # The operator makes the new property active in the tree of the region it runs in.
        screen, area, region = properties_editor_find()
        ob = bpy.context.object
        # So the new property isn't the first, which is active by default.
        ob["existing"] = 1
        with bpy.context.temp_override(screen=screen, area=area, region=region):
            bpy.ops.wm.properties_add(data_path="object")
            self.assertEqual(ob.id_properties_active_get(bpy.context), "prop")

        # Each region has its own state, running from another region leaves this one unchanged.
        screen_other, area_other, region_other = next(
            (s, a, r) for s in bpy.data.screens for a in s.areas if a.type == 'VIEW_3D'
            for r in a.regions if r.type == 'UI'
        )
        with bpy.context.temp_override(screen=screen_other, area=area_other, region=region_other):
            bpy.ops.wm.properties_add(data_path="object")
            self.assertEqual(ob.id_properties_active_get(bpy.context), "prop1")
        with bpy.context.temp_override(screen=screen, area=area, region=region):
            self.assertEqual(ob.id_properties_active_get(bpy.context), "prop")

    def test_properties_remove(self):
        screen, area, region = properties_editor_find()
        ob = bpy.context.object
        ob["a"] = 1
        ob["b"] = 2
        ob["c"] = 3
        with bpy.context.temp_override(screen=screen, area=area, region=region):
            context = bpy.context
            ob.id_properties_active_set(context, "c")
            # Removing the active property activates the previous one.
            bpy.ops.wm.properties_remove(data_path="object", property_name="c")
            self.assertEqual(ob.id_properties_active_get(context), "b")
            # Removing another property leaves the active one.
            bpy.ops.wm.properties_remove(data_path="object", property_name="a")
            self.assertEqual(ob.id_properties_active_get(context), "b")
            # Removing the first property, the next one takes its place.
            bpy.ops.wm.properties_remove(data_path="object", property_name="b")
            self.assertIsNone(ob.id_properties_active_get(context))

    def test_id_properties_active(self):
        # The owner resolves the region state to a property name, see `bpy_struct.id_properties_active_get`.
        screen, area, region = properties_editor_find()
        ob = bpy.context.object
        ob["a"] = 1
        ob["b"] = 2
        # Without a region there is no state to read.
        self.assertIsNone(ob.id_properties_active_get(bpy.context))
        with bpy.context.temp_override(screen=screen, area=area, region=region):
            context = bpy.context
            # Nothing stored, the tree makes the first property active.
            self.assertEqual(ob.id_properties_active_get(context), "a")
            ob.id_properties_active_set(context, "b")
            self.assertEqual(ob.id_properties_active_get(context), "b")
            # Stored per type of data, the mesh doesn't share the index of the object.
            ob.data["c"] = 3
            ob.data["d"] = 4
            self.assertEqual(ob.data.id_properties_active_get(context), "c")
            with self.assertRaises(KeyError):
                ob.id_properties_active_set(context, "missing")
            with self.assertRaises(TypeError):
                context.scene.render.id_properties_active_get(context)
            # Removing the active property, the property that takes its place is active.
            del ob["b"]
            self.assertEqual(ob.id_properties_active_get(context), "a")
            del ob["a"]
            self.assertIsNone(ob.id_properties_active_get(context))

    def test_list_id_empty(self):
        self.region.active_item_index_set(LISTTYPE_NAME, "", 2)
        self.assertEqual(self.region.active_item_index_get(LISTTYPE_NAME, ""), 2)

    def test_file_reload_prune(self):
        # Twice as many states as are kept on load, the oldest are removed.
        for i in range(STATES_KEPT_ON_LOAD * 2):
            self.region.active_item_index_set(LISTTYPE_NAME, str(i), i)

        save_and_reload()

        region = properties_window_region()
        # The states set first were used least recently, so they were removed.
        for i in range(STATES_KEPT_ON_LOAD * 2):
            expect = i if (i >= STATES_KEPT_ON_LOAD) else 0
            self.assertEqual(region.active_item_index_get(LISTTYPE_NAME, str(i)), expect)

    def test_file_reload_kept(self):
        self.region.active_item_index_set(LISTTYPE_NAME, "object", 5)
        self.region.active_item_index_set(LISTTYPE_NAME, "object.data", 1)

        save_and_reload()

        region = properties_window_region()
        self.assertEqual(region.active_item_index_get(LISTTYPE_NAME, "object"), 5)
        self.assertEqual(region.active_item_index_get(LISTTYPE_NAME, "object.data"), 1)


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
