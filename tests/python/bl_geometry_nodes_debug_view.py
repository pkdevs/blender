# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import sys
import tempfile
import unittest

import bpy


args = None


class GeometryNodesDebugViewLifecycleFixtureTest(unittest.TestCase):
    def setUp(self):
        if args is None or args.testdir is None:
            self.skipTest("Debug View fixture test directory was not provided")

        fixture_path = args.testdir / "persistent_debug_view_lifecycle.blend"
        self.assertTrue(fixture_path.exists(), f"Missing fixture: {fixture_path}")
        bpy.ops.wm.open_mainfile(filepath=str(fixture_path))

        self.obj = bpy.data.objects["Debug View Lifecycle Object"]
        self.modifier = self.obj.modifiers["Debug View Lifecycle Modifier"]
        self.original_group = bpy.data.node_groups["Debug View Original Group"]
        self.replacement_group = bpy.data.node_groups["Debug View Replacement Group"]
        self.original_viewer = self.original_group.nodes["Original Debug"]
        self.replacement_viewer = self.replacement_group.nodes["Replacement Debug"]

    def evaluate(self, obj):
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_obj = obj.evaluated_get(depsgraph)
        return len(evaluated_obj.data.vertices)

    def test_node_group_replacement_and_viewer_deletion_memfile_undo(self):
        self.assertEqual(self.obj.name, "Debug View Lifecycle Object")
        self.assertEqual(self.modifier.name, "Debug View Lifecycle Modifier")
        self.assertEqual(self.original_group.name, "Debug View Original Group")
        self.assertEqual(self.replacement_group.name, "Debug View Replacement Group")
        self.assertEqual(self.original_viewer.name, "Original Debug")
        self.assertEqual(self.replacement_viewer.name, "Replacement Debug")
        self.assertEqual(self.modifier.node_group, self.original_group)
        self.assertEqual(self.evaluate(self.obj), 8)

        bpy.ops.ed.undo_push()
        bpy.ops.ed.undo_push()

        self.modifier.node_group = self.replacement_group
        self.assertEqual(self.evaluate(self.obj), 8)
        bpy.ops.ed.undo_push()

        self.assertEqual(self.modifier.node_group.name, "Debug View Replacement Group")
        self.assertEqual(bpy.ops.ed.undo(), {'FINISHED'})
        restored_obj = bpy.data.objects["Debug View Lifecycle Object"]
        restored_modifier = restored_obj.modifiers["Debug View Lifecycle Modifier"]
        self.assertEqual(restored_modifier.node_group.name, "Debug View Original Group")
        self.assertEqual(self.evaluate(restored_obj), 8)

        self.assertEqual(bpy.ops.ed.redo(), {'FINISHED'})
        redone_obj = bpy.data.objects["Debug View Lifecycle Object"]
        redone_modifier = redone_obj.modifiers["Debug View Lifecycle Modifier"]
        self.assertEqual(redone_modifier.node_group.name, "Debug View Replacement Group")
        self.assertEqual(self.evaluate(redone_obj), 8)

        bpy.ops.ed.undo_push()
        replacement_tree = redone_modifier.node_group
        replacement_tree.nodes.remove(replacement_tree.nodes["Replacement Debug"])
        bpy.ops.ed.undo_push()
        self.assertIsNone(replacement_tree.nodes.get("Replacement Debug"))

        self.assertEqual(bpy.ops.ed.undo(), {'FINISHED'})
        restored_tree = bpy.data.node_groups["Debug View Replacement Group"]
        self.assertIsNotNone(restored_tree.nodes.get("Replacement Debug"))


class GeometryNodesDebugViewIsolationFixtureTest(unittest.TestCase):
    def setUp(self):
        if args is None or args.testdir is None:
            self.skipTest("Debug View fixture test directory was not provided")

        fixture_path = args.testdir / "persistent_debug_view_isolation.blend"
        self.assertTrue(fixture_path.exists(), f"Missing fixture: {fixture_path}")
        bpy.ops.wm.open_mainfile(filepath=str(fixture_path))

        self.group = bpy.data.node_groups["Persistent Debug View Isolation Group"]
        self.viewer = self.group.nodes["Isolation Debug View"]
        self.obj = bpy.data.objects["Debug View Isolation Object"]
        self.first_modifier = self.obj.modifiers["Debug View Isolation First"]
        self.second_modifier = self.obj.modifiers["Debug View Isolation Second"]
        self.shared_obj = bpy.data.objects["Debug View Shared Object"]
        self.shared_modifier = self.shared_obj.modifiers["Debug View Shared Modifier"]

    def evaluate(self, obj):
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_obj = obj.evaluated_get(depsgraph)
        return len(evaluated_obj.data.vertices)

    def warning_messages(self, modifier):
        return [warning.message for warning in modifier.node_warnings]

    def test_debug_view_evaluation_and_modifier_isolation(self):
        self.assertFalse(self.viewer.is_debug_view)
        self.assertIsNone(self.viewer.inputs.get("Show"))
        self.first_modifier.show_debug_views = True
        self.second_modifier.show_debug_views = False

        self.assertEqual(self.evaluate(self.obj), 8)
        self.assertEqual(self.warning_messages(self.first_modifier), [])
        self.assertEqual(self.warning_messages(self.second_modifier), [])

        self.viewer.is_debug_view = True
        self.assertEqual(self.evaluate(self.obj), 8)
        self.assertIn(
            "ISOLATION_DEBUG_EVALUATED", self.warning_messages(self.first_modifier))
        self.assertEqual(self.warning_messages(self.second_modifier), [])

    def test_modifier_and_object_isolation(self):
        self.assertEqual(self.first_modifier.node_group, self.group)
        self.assertEqual(self.second_modifier.node_group, self.group)
        self.assertEqual(self.shared_modifier.node_group, self.group)

        self.viewer.is_debug_view = True
        self.first_modifier.show_debug_views = False
        self.second_modifier.show_debug_views = True
        self.shared_modifier.show_debug_views = False

        self.assertEqual(self.evaluate(self.obj), 8)
        self.assertEqual(self.evaluate(self.shared_obj), 8)
        self.assertEqual(self.warning_messages(self.first_modifier), [])
        self.assertIn(
            "ISOLATION_DEBUG_EVALUATED", self.warning_messages(self.second_modifier))
        self.assertEqual(self.warning_messages(self.shared_modifier), [])

        self.first_modifier.show_debug_views = True
        self.second_modifier.show_debug_views = False
        self.assertEqual(self.evaluate(self.obj), 8)
        self.assertIn(
            "ISOLATION_DEBUG_EVALUATED", self.warning_messages(self.first_modifier))
        self.assertEqual(self.warning_messages(self.second_modifier), [])

        self.assertEqual(self.evaluate(self.shared_obj), 8)
        self.assertEqual(self.warning_messages(self.shared_modifier), [])


class GeometryNodesDebugViewFixtureTest(unittest.TestCase):
    def setUp(self):
        if args is None or args.testdir is None:
            self.skipTest("Debug View fixture test directory was not provided")

        fixture_path = args.testdir / "persistent_debug_view.blend"
        self.assertTrue(fixture_path.exists(), f"Missing fixture: {fixture_path}")
        bpy.ops.wm.open_mainfile(filepath=str(fixture_path))

        self.obj = bpy.data.objects["Persistent Debug View Object"]
        self.modifier = self.obj.modifiers["Persistent Debug View Modifier"]
        self.root_group = bpy.data.node_groups["Persistent Debug View Root"]
        self.nested_group = bpy.data.node_groups["Nested Debug Group"]

    def evaluate(self):
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_obj = self.obj.evaluated_get(depsgraph)
        return len(evaluated_obj.data.vertices)

    def warning_messages(self):
        return [warning.message for warning in self.modifier.node_warnings]

    def test_disabled_modifier_does_not_evaluate_debug_view(self):
        self.modifier.show_debug_views = True
        self.modifier.show_viewport = False

        self.assertEqual(self.evaluate(), 0)
        self.assertEqual(self.warning_messages(), [])

    def test_debug_view_operator_rejects_incomplete_and_stale_identities(self):
        self.modifier.show_debug_views = True
        bpy.context.view_layer.objects.active = self.obj
        self.obj.select_set(True)
        self.evaluate()

        with bpy.context.temp_override(
                object=self.obj, active_object=self.obj, modifier=self.modifier):
            # An incomplete stable identity must be rejected.
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(context_hash=(1, 2, 3, 4)),
                {'CANCELLED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(
                    node_tree_lineage_hash=(1, 2, 3, 4)),
                {'CANCELLED'})
            # A complete but stale identity cannot resolve to an available candidate.
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(
                    context_hash=(1, 2, 3, 4),
                    node_tree_lineage_hash=(5, 6, 7, 8),
                    viewer_node_id=9),
                {'CANCELLED'})

    def test_debug_view_settings_persist(self):
        self.modifier.show_debug_views = True

        with tempfile.TemporaryDirectory() as temp_dir:
            filepath = pathlib.Path(temp_dir) / "debug_view_persistence.blend"
            bpy.ops.wm.save_as_mainfile(filepath=str(filepath))
            self.assertTrue(filepath.exists())
            self.modifier.show_debug_views = False
            self.assertFalse(self.modifier.show_debug_views)
            bpy.ops.wm.open_mainfile(filepath=str(filepath))

            loaded_obj = bpy.data.objects.get("Persistent Debug View Object")
            self.assertIsNotNone(loaded_obj)
            loaded_modifier = loaded_obj.modifiers.get("Persistent Debug View Modifier")
            self.assertIsNotNone(loaded_modifier)
            loaded_root_group = bpy.data.node_groups.get("Persistent Debug View Root")
            self.assertIsNotNone(loaded_root_group)
            loaded_nested_group = bpy.data.node_groups.get("Nested Debug Group")
            self.assertIsNotNone(loaded_nested_group)

            self.assertEqual(loaded_modifier.node_group, loaded_root_group)
            self.assertEqual(loaded_modifier.node_group.name, "Persistent Debug View Root")
            self.assertTrue(loaded_modifier.show_debug_views)

            root_a = loaded_root_group.nodes["Root Debug A"]
            root_b = loaded_root_group.nodes["Root Debug B"]
            nested = loaded_nested_group.nodes["Nested Debug"]

            self.assertEqual(root_a.label, "Root Debug A")
            self.assertTrue(root_a.is_debug_view)
            self.assertEqual(root_b.label, "Root Debug B")
            self.assertTrue(root_b.is_debug_view)
            self.assertEqual(nested.label, "Nested Debug")
            self.assertTrue(nested.is_debug_view)

            root_a_show = root_a.inputs["Show"]
            self.assertFalse(root_a_show.default_value)
            self.assertEqual(len(root_a_show.links), 1)
            root_a_show_link = root_a_show.links[0]
            self.assertTrue(root_a_show_link.is_valid)
            self.assertEqual(root_a_show_link.to_node, root_a)
            self.assertEqual(root_a_show_link.to_socket, root_a_show)
            self.assertEqual(root_a_show_link.from_node.name, "Root Debug A Show Source")
            self.assertEqual(root_a_show_link.from_node.bl_idname, "FunctionNodeBooleanMath")
            self.assertEqual(root_a_show_link.from_socket.name, "Boolean")

            root_b_show = root_b.inputs["Show"]
            self.assertEqual(len(root_b_show.links), 0)
            self.assertTrue(root_b_show.default_value)
            nested_show = nested.inputs["Show"]
            self.assertEqual(len(nested_show.links), 0)
            self.assertTrue(nested_show.default_value)

            self.obj = loaded_obj
            self.modifier = loaded_modifier
            self.root_group = loaded_root_group
            self.nested_group = loaded_nested_group
            self.assertEqual(self.evaluate(), 8)

    def test_fixture_smoke(self):
        self.assertEqual(len(bpy.data.objects), 1)
        self.assertEqual(self.modifier.node_group, self.root_group)

        root_a = self.root_group.nodes["Root Debug A"]
        root_b = self.root_group.nodes["Root Debug B"]
        nested = self.nested_group.nodes["Nested Debug"]
        self.assertEqual(root_a.label, "Root Debug A")
        self.assertEqual(root_b.label, "Root Debug B")
        self.assertEqual(nested.label, "Nested Debug")
        self.assertTrue(root_a.is_debug_view)
        self.assertTrue(root_b.is_debug_view)
        self.assertTrue(nested.is_debug_view)

        root_a_show = root_a.inputs["Show"]
        root_b_show = root_b.inputs["Show"]
        nested_show = nested.inputs["Show"]
        self.assertEqual(len(root_a_show.links), 1)
        root_a_show_link = root_a_show.links[0]
        root_a_show_source = root_a_show_link.from_node
        self.assertEqual(root_a_show_source.name, "Root Debug A Show Source")
        self.assertEqual(root_a_show_source.bl_idname, "FunctionNodeBooleanMath")
        self.assertEqual(root_a_show_link.from_socket.name, "Boolean")
        self.assertFalse(root_a_show.default_value)
        self.assertEqual(len(root_b_show.links), 0)
        self.assertTrue(root_b_show.default_value)
        self.assertEqual(len(nested_show.links), 0)
        self.assertTrue(nested_show.default_value)

        self.assertFalse(self.modifier.show_debug_views)
        self.assertEqual(self.evaluate(), 8)
        self.assertEqual(self.warning_messages(), [])

        self.modifier.show_debug_views = True
        self.assertEqual(self.evaluate(), 8)
        warnings = self.warning_messages()
        self.assertIn("ROOT_DEBUG_A_EVALUATED", warnings)
        self.assertIn("ROOT_DEBUG_B_EVALUATED", warnings)
        self.assertNotIn("NESTED_DEBUG_EVALUATED", warnings)

        root_a.is_debug_view = False
        self.assertTrue(root_a_show.is_unavailable)
        self.assertEqual(len(root_a_show.links), 1)
        self.assertEqual(root_a_show.links[0].from_node, root_a_show_source)
        self.assertEqual(self.evaluate(), 8)

        root_a.is_debug_view = True
        self.assertFalse(root_a_show.is_unavailable)
        self.assertFalse(root_a_show.default_value)
        self.assertEqual(len(root_a_show.links), 1)
        self.assertEqual(root_a_show.links[0].from_node, root_a_show_source)
        self.assertEqual(self.evaluate(), 8)

        root_b_show.default_value = False
        self.assertEqual(self.evaluate(), 8)
        warnings = self.warning_messages()
        self.assertNotIn("ROOT_DEBUG_B_EVALUATED", warnings)
        self.assertIn("ROOT_DEBUG_A_EVALUATED", warnings)
        self.assertNotIn("NESTED_DEBUG_EVALUATED", warnings)

        root_b_show.default_value = True
        self.assertEqual(self.evaluate(), 8)

    def test_viewer_show_gates_data_evaluation(self):
        self.modifier.show_debug_views = True
        viewer = self.root_group.nodes["Root Debug B"]
        show_socket = viewer.inputs["Show"]

        show_socket.default_value = False
        self.assertEqual(self.evaluate(), 8)
        self.assertNotIn("ROOT_DEBUG_B_EVALUATED", self.warning_messages())

        show_socket.default_value = True
        self.assertEqual(self.evaluate(), 8)
        self.assertIn("ROOT_DEBUG_B_EVALUATED", self.warning_messages())

    def test_group_input_show_gates_nested_viewer_data(self):
        interface_socket = self.nested_group.interface.new_socket(
            name="Show Debug", in_out='INPUT', socket_type='NodeSocketBool')
        interface_socket.default_value = False

        group_input = self.nested_group.nodes["Nested Group Input"]
        viewer = self.nested_group.nodes["Nested Debug"]
        self.nested_group.links.new(group_input.outputs["Show Debug"], viewer.inputs["Show"])

        group_node = next(
            node for node in self.root_group.nodes
            if node.bl_idname == "GeometryNodeGroup" and node.node_tree == self.nested_group)
        self.modifier.show_debug_views = True

        group_node.inputs["Show Debug"].default_value = False
        self.assertEqual(self.evaluate(), 8)
        warnings = self.warning_messages()
        self.assertNotIn("NESTED_DEBUG_EVALUATED", warnings)

        group_node.inputs["Show Debug"].default_value = True
        self.assertEqual(self.evaluate(), 8)
        self.assertIn("NESTED_DEBUG_EVALUATED", self.warning_messages())

    def test_two_explicit_group_boundaries_and_removed_exposure(self):
        nested_interface_socket = self.nested_group.interface.new_socket(
            name="Show Debug", in_out='INPUT', socket_type='NodeSocketBool')
        nested_interface_socket.default_value = True
        self.nested_group.links.new(
            self.nested_group.nodes["Nested Group Input"].outputs["Show Debug"],
            self.nested_group.nodes["Nested Debug"].inputs["Show"])

        middle_group = bpy.data.node_groups.new("Middle Debug Group", 'GeometryNodeTree')
        middle_group.interface.new_socket(
            name="Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
        middle_interface_socket = middle_group.interface.new_socket(
            name="Show Nested Debug", in_out='INPUT', socket_type='NodeSocketBool')
        middle_interface_socket.default_value = True
        middle_group.interface.new_socket(
            name="Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')

        middle_input = middle_group.nodes.new("NodeGroupInput")
        middle_output = middle_group.nodes.new("NodeGroupOutput")
        nested_group_node = middle_group.nodes.new("GeometryNodeGroup")
        nested_group_node.node_tree = self.nested_group
        middle_group.links.new(
            middle_input.outputs["Geometry"], nested_group_node.inputs["Geometry"])
        middle_group.links.new(
            middle_input.outputs["Show Nested Debug"], nested_group_node.inputs["Show Debug"])
        middle_group.links.new(
            nested_group_node.outputs["Geometry"], middle_output.inputs["Geometry"])

        root_nested_group_node = self.root_group.nodes["Nested Debug Group"]
        self.root_group.nodes.remove(root_nested_group_node)
        middle_group_node = self.root_group.nodes.new("GeometryNodeGroup")
        middle_group_node.node_tree = middle_group
        self.root_group.links.new(
            self.root_group.nodes["Normal Cube"].outputs["Mesh"],
            middle_group_node.inputs["Geometry"])
        self.root_group.links.new(
            middle_group_node.outputs["Geometry"],
            self.root_group.nodes["Root Group Output"].inputs["Geometry"])

        self.modifier.show_debug_views = True
        middle_group_node.inputs["Show Nested Debug"].default_value = True
        self.assertEqual(self.evaluate(), 8)
        self.assertIn("NESTED_DEBUG_EVALUATED", self.warning_messages())

        for link in list(middle_group.links):
            if (link.from_node == middle_input and
                    link.from_socket.name == "Show Nested Debug"):
                middle_group.links.remove(link)

        self.assertEqual(self.evaluate(), 8)
        self.assertNotIn("NESTED_DEBUG_EVALUATED", self.warning_messages())


class GeometryNodesDebugViewSimulationZoneFixtureTest(unittest.TestCase):
    def setUp(self):
        if args is None or args.testdir is None:
            self.skipTest("Debug View fixture test directory was not provided")

        fixture_path = args.testdir / "persistent_debug_view_simulation_zone.blend"
        self.assertTrue(fixture_path.exists(), f"Missing fixture: {fixture_path}")
        bpy.ops.wm.open_mainfile(filepath=str(fixture_path))

        self.obj = bpy.data.objects["Persistent Debug View Simulation Object"]
        self.modifier = self.obj.modifiers["Persistent Debug View Simulation Modifier"]
        self.root_group = bpy.data.node_groups["Persistent Debug View Simulation Root"]
        self.nested_group = bpy.data.node_groups["Simulation Debug Nested Group"]
        self.viewer = self.nested_group.nodes["Simulation Persistent Debug"]
        self.marker = self.nested_group.nodes["Simulation Debug Marker"]

    def evaluate(self):
        bpy.context.scene.frame_set(1)
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_obj = self.obj.evaluated_get(depsgraph)
        return len(evaluated_obj.data.vertices)

    def warning_messages(self):
        return [warning.message for warning in self.modifier.node_warnings]

    def test_nested_debug_view_is_available_and_evaluates_through_simulation(self):
        self.assertEqual(len(bpy.data.objects), 1)
        self.assertEqual(len(self.obj.modifiers), 1)
        self.assertEqual(
            len([node for node in self.root_group.nodes
                 if node.bl_idname == "GeometryNodeSimulationOutput"]), 1)
        self.assertEqual(
            len([node for node in self.root_group.nodes if node.bl_idname == "GeometryNodeGroup"]), 1)
        self.assertTrue(self.viewer.is_debug_view)
        self.assertEqual(self.marker.inputs["Message"].default_value,
                         "SIMULATION_DEBUG_BRANCH_EVALUATED")

        self.assertFalse(self.modifier.show_debug_views)
        self.assertEqual(self.evaluate(), 8)
        self.assertEqual(self.warning_messages(), [])
        with bpy.context.temp_override(
                object=self.obj, active_object=self.obj, modifier=self.modifier):
            self.assertFalse(bpy.ops.object.geometry_nodes_debug_view_toggle.poll())

        self.modifier.show_debug_views = True
        self.assertEqual(self.evaluate(), 8)
        self.assertIn("SIMULATION_DEBUG_BRANCH_EVALUATED", self.warning_messages())
        with bpy.context.temp_override(
                object=self.obj, active_object=self.obj, modifier=self.modifier):
            self.assertTrue(bpy.ops.object.geometry_nodes_debug_view_toggle.poll())

        self.modifier.show_debug_views = False
        self.assertEqual(self.evaluate(), 8)
        self.assertEqual(self.warning_messages(), [])

    def test_reload_starts_from_deterministic_initial_state(self):
        self.assertFalse(self.modifier.show_debug_views)
        self.assertEqual(self.evaluate(), 8)
        self.assertEqual(self.warning_messages(), [])
        self.assertTrue(self.viewer.is_debug_view)
        self.assertEqual(self.marker.inputs["Message"].default_value,
                         "SIMULATION_DEBUG_BRANCH_EVALUATED")


def main():
    global args
    import argparse

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', type=pathlib.Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
