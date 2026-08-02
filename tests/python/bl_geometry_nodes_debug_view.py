# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import pathlib
import sys
import tempfile
import unittest

import bpy


args = None


class GeometryNodesDebugViewTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def create_node_group(self):
        group = bpy.data.node_groups.new("Geometry Nodes", "GeometryNodeTree")
        group.interface.new_socket(
            name="Geometry", in_out='OUTPUT', socket_type="NodeSocketGeometry")

        output = group.nodes.new("NodeGroupOutput")
        normal_geometry = group.nodes.new("GeometryNodeMeshCube")
        debug_geometry = group.nodes.new("GeometryNodeMeshIcoSphere")
        warning = group.nodes.new("GeometryNodeWarning")
        warning.inputs["Show"].default_value = True
        warning.inputs["Message"].default_value = "Debug View evaluated"

        switch = group.nodes.new("GeometryNodeSwitch")
        switch.input_type = 'GEOMETRY'
        group.links.new(warning.outputs["Show"], switch.inputs["Switch"])
        group.links.new(debug_geometry.outputs["Mesh"], switch.inputs["True"])

        viewer = group.nodes.new("GeometryNodeViewer")
        viewer.viewer_items.new('GEOMETRY', "Geometry")
        group.links.new(switch.outputs["Output"], viewer.inputs["Geometry"])
        group.links.new(normal_geometry.outputs["Mesh"], output.inputs["Geometry"])
        return group, viewer

    def create_object_with_modifier(self, name, group):
        mesh = bpy.data.meshes.new(name + " Mesh")
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(obj)
        modifier = obj.modifiers.new("Geometry Nodes", 'NODES')
        modifier.node_group = group
        obj.modifiers.active = modifier
        self.assertFalse(modifier.show_debug_views)
        return obj, modifier

    def add_debug_view(self, group, label, geometry_node_type, warning_message):
        debug_geometry = group.nodes.new(geometry_node_type)
        warning = group.nodes.new("GeometryNodeWarning")
        warning.inputs["Show"].default_value = True
        warning.inputs["Message"].default_value = warning_message

        switch = group.nodes.new("GeometryNodeSwitch")
        switch.input_type = 'GEOMETRY'
        group.links.new(warning.outputs["Show"], switch.inputs["Switch"])
        group.links.new(debug_geometry.outputs[0], switch.inputs["True"])

        viewer = group.nodes.new("GeometryNodeViewer")
        viewer.label = label
        viewer.viewer_items.new('GEOMETRY', "Geometry")
        viewer.is_debug_view = True
        group.links.new(switch.outputs["Output"], viewer.inputs["Geometry"])
        return viewer

    def evaluate(self, obj):
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_obj = obj.evaluated_get(depsgraph)
        return len(evaluated_obj.data.vertices)

    def warning_messages(self, modifier):
        return [warning.message for warning in modifier.node_warnings]

    def assert_debug_view_enabled(self, obj, modifier, candidate_index, expected):
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        self.evaluate(obj)
        with bpy.context.temp_override(
                object=obj, active_object=obj, modifier=modifier):
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(
                    candidate_index=candidate_index,
                    expected_enabled=int(expected)),
                {'FINISHED'})

    def test_debug_view_evaluation_and_modifier_isolation(self):
        group, viewer = self.create_node_group()
        obj_a, modifier_a = self.create_object_with_modifier("Object A", group)
        obj_b, modifier_b = self.create_object_with_modifier("Object B", group)

        self.assertIsNone(viewer.inputs.get("Show"))
        modifier_a.show_debug_views = True
        modifier_b.show_debug_views = False

        self.assertEqual(self.evaluate(obj_b), 8)
        self.assertEqual(self.warning_messages(modifier_a), [])
        self.assertEqual(self.warning_messages(modifier_b), [])

        viewer.is_debug_view = True
        self.assertEqual(self.evaluate(obj_b), 8)
        self.assertIn("Debug View evaluated", self.warning_messages(modifier_a))
        self.assertEqual(self.warning_messages(modifier_b), [])

    def test_modifier_and_object_isolation(self):
        group, viewer = self.create_node_group()
        viewer.is_debug_view = True

        obj_a, modifier_a_first = self.create_object_with_modifier("Object A", group)
        modifier_a_second = obj_a.modifiers.new("Geometry Nodes Second", 'NODES')
        modifier_a_second.node_group = group
        obj_b, modifier_b = self.create_object_with_modifier("Object B", group)

        modifier_a_first.show_debug_views = False
        modifier_a_second.show_debug_views = True
        modifier_b.show_debug_views = False

        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertEqual(self.evaluate(obj_b), 8)
        self.assertEqual(self.warning_messages(modifier_a_first), [])
        self.assertIn("Debug View evaluated", self.warning_messages(modifier_a_second))
        self.assertEqual(self.warning_messages(modifier_b), [])

        modifier_a_first.show_debug_views = True
        modifier_a_second.show_debug_views = False
        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertIn("Debug View evaluated", self.warning_messages(modifier_a_first))
        self.assertEqual(self.warning_messages(modifier_a_second), [])

    def test_runtime_state_isolation_copy_and_modifier_reorder(self):
        group, viewer = self.create_node_group()
        viewer.is_debug_view = True
        obj, first_modifier = self.create_object_with_modifier("Object", group)
        first_modifier.show_debug_views = True
        self.assert_debug_view_enabled(obj, first_modifier, 0, True)
        with bpy.context.temp_override(
                object=obj, active_object=obj, modifier=first_modifier):
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(candidate_index=0),
                {'FINISHED'})
        self.assert_debug_view_enabled(obj, first_modifier, 0, False)

        second_modifier = obj.modifiers.new("Second Geometry Nodes", 'NODES')
        second_modifier.node_group = group
        second_modifier.show_debug_views = True
        obj.modifiers.active = second_modifier
        self.assert_debug_view_enabled(obj, second_modifier, 0, True)
        obj.modifiers.active = first_modifier
        self.assert_debug_view_enabled(obj, first_modifier, 0, False)

        obj.modifiers.move(1, 0)
        obj.modifiers.active = second_modifier
        self.assert_debug_view_enabled(obj, second_modifier, 0, True)
        obj.modifiers.active = first_modifier
        self.assert_debug_view_enabled(obj, first_modifier, 0, False)

        with bpy.context.temp_override(object=obj, active_object=obj):
            self.assertEqual(
                bpy.ops.object.modifier_copy(modifier=first_modifier.name),
                {'FINISHED'})
        copied_modifier = obj.modifiers[-1]
        obj.modifiers.active = copied_modifier
        self.assert_debug_view_enabled(obj, copied_modifier, 0, True)

        copied_obj = obj.copy()
        copied_obj.name = "Copied Object"
        bpy.context.collection.objects.link(copied_obj)
        copied_object_modifier = copied_obj.modifiers[first_modifier.name]
        copied_obj.modifiers.active = copied_object_modifier
        self.assert_debug_view_enabled(copied_obj, copied_object_modifier, 0, True)

        shared_obj, shared_modifier = self.create_object_with_modifier("Shared Object", group)
        shared_modifier.show_debug_views = True
        self.assert_debug_view_enabled(shared_obj, shared_modifier, 0, True)
        obj.modifiers.active = first_modifier
        self.assert_debug_view_enabled(obj, first_modifier, 0, False)

    def test_disabled_modifier_does_not_evaluate_debug_view(self):
        group, viewer = self.create_node_group()
        viewer.is_debug_view = True
        obj, modifier = self.create_object_with_modifier("Object", group)
        modifier.show_debug_views = True
        modifier.show_viewport = False

        self.assertEqual(self.evaluate(obj), 0)
        self.assertEqual(self.warning_messages(modifier), [])

    def test_multiple_debug_view_toggles_do_not_reevaluate(self):
        group, first_viewer = self.create_node_group()
        first_viewer.label = "First"
        first_viewer.is_debug_view = True
        second_viewer = self.add_debug_view(
            group, "Second", "GeometryNodeMeshCylinder", "Second Debug View evaluated")
        obj, modifier = self.create_object_with_modifier("Object", group)
        modifier.show_debug_views = True
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)

        self.assertEqual(self.evaluate(obj), 8)
        warnings_before = self.warning_messages(modifier)
        self.assertIn("Debug View evaluated", warnings_before)
        self.assertIn("Second Debug View evaluated", warnings_before)

        with bpy.context.temp_override(
                object=obj, active_object=obj, modifier=modifier):
            # The first toggle initializes the state with candidate zero enabled,
            # then independently enables candidate one.
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(candidate_index=1),
                {'FINISHED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(candidate_index=0),
                {'FINISHED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(candidate_index=1),
                {'FINISHED'})
            # Re-enable both independently.
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(candidate_index=0),
                {'FINISHED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(candidate_index=1),
                {'FINISHED'})

        bpy.context.view_layer.update()
        self.assertEqual(len(obj.evaluated_get(
            bpy.context.evaluated_depsgraph_get()).data.vertices), 8)
        self.assertEqual(self.warning_messages(modifier), warnings_before)

    def test_debug_view_operator_rejects_incomplete_and_stale_identities(self):
        group, viewer = self.create_node_group()
        viewer.is_debug_view = True
        obj, modifier = self.create_object_with_modifier("Object", group)
        modifier.show_debug_views = True
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        self.evaluate(obj)

        with bpy.context.temp_override(
                object=obj, active_object=obj, modifier=modifier):
            # An incomplete stable identity must not fall through to the test-only index.
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

    def test_node_group_replacement_and_viewer_deletion_memfile_undo(self):
        original_group, original_viewer = self.create_node_group()
        replacement_group, replacement_viewer = self.create_node_group()
        original_group.name = "Original Geometry Nodes"
        replacement_group.name = "Replacement Geometry Nodes"
        original_viewer.is_debug_view = True
        replacement_viewer.is_debug_view = True
        # Creation order deliberately gives the unrelated trees equivalent node/context
        # structure. Raw integer identifier equality is covered directly by the C++ test because
        # node identifiers are intentionally not exposed through RNA.
        self.assertEqual(
            [node.bl_idname for node in original_group.nodes],
            [node.bl_idname for node in replacement_group.nodes],
        )

        obj, modifier = self.create_object_with_modifier("Object", original_group)
        modifier.show_debug_views = True
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        self.assert_debug_view_enabled(obj, modifier, 0, True)

        bpy.ops.ed.undo_push()
        bpy.ops.ed.undo_push()

        modifier.node_group = replacement_group
        self.assert_debug_view_enabled(obj, modifier, 0, False)
        bpy.ops.ed.undo_push()

        self.assertEqual(bpy.ops.ed.undo(), {'FINISHED'})
        restored_obj = bpy.data.objects["Object"]
        restored_modifier = restored_obj.modifiers["Geometry Nodes"]
        self.assertEqual(restored_modifier.node_group.name, "Original Geometry Nodes")
        self.assert_debug_view_enabled(restored_obj, restored_modifier, 0, True)

        self.assertEqual(bpy.ops.ed.redo(), {'FINISHED'})
        redone_obj = bpy.data.objects["Object"]
        redone_modifier = redone_obj.modifiers["Geometry Nodes"]
        self.assertEqual(redone_modifier.node_group.name, "Replacement Geometry Nodes")
        self.assert_debug_view_enabled(redone_obj, redone_modifier, 0, False)
        with bpy.context.temp_override(
                object=redone_obj, active_object=redone_obj, modifier=redone_modifier):
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_test_toggle(candidate_index=0),
                {'FINISHED'})
        self.assert_debug_view_enabled(redone_obj, redone_modifier, 0, True)

        bpy.ops.ed.undo_push()
        replacement_tree = redone_modifier.node_group
        replacement_tree.nodes.remove(
            next(node for node in replacement_tree.nodes if node.bl_idname == "GeometryNodeViewer"))
        bpy.ops.ed.undo_push()
        self.assertFalse(any(
            node.bl_idname == "GeometryNodeViewer" for node in replacement_tree.nodes))
        self.assertEqual(bpy.ops.ed.undo(), {'FINISHED'})
        restored_tree = bpy.data.node_groups["Replacement Geometry Nodes"]
        self.assertTrue(any(
            node.bl_idname == "GeometryNodeViewer" for node in restored_tree.nodes))
        viewer_restored_obj = bpy.data.objects["Object"]
        viewer_restored_modifier = viewer_restored_obj.modifiers["Geometry Nodes"]
        self.assert_debug_view_enabled(
            viewer_restored_obj, viewer_restored_modifier, 0, True)

    def test_debug_view_settings_persist(self):
        group, viewer = self.create_node_group()
        obj, modifier = self.create_object_with_modifier("Object", group)
        viewer.label = "Surface"
        viewer.is_debug_view = True
        show_socket = viewer.inputs["Show"]
        show_socket.default_value = False
        boolean = group.nodes.new("FunctionNodeInputBool")
        group.links.new(boolean.outputs["Boolean"], show_socket)

        inactive_viewer = group.nodes.new("GeometryNodeViewer")
        inactive_viewer.label = "Inactive"
        inactive_viewer.viewer_items.new('GEOMETRY', "Geometry")
        inactive_viewer.is_debug_view = True
        group.links.new(boolean.outputs["Boolean"], inactive_viewer.inputs["Show"])
        inactive_viewer.is_debug_view = False
        modifier.show_debug_views = True

        with tempfile.TemporaryDirectory() as temp_dir:
            filepath = os.path.join(temp_dir, "debug_view_persistence.blend")
            bpy.ops.wm.save_as_mainfile(filepath=filepath)
            bpy.ops.wm.open_mainfile(filepath=filepath)

            loaded_group = bpy.data.node_groups["Geometry Nodes"]
            loaded_viewer = next(
                node for node in loaded_group.nodes if node.label == "Surface")
            loaded_modifier = bpy.data.objects["Object"].modifiers["Geometry Nodes"]
            loaded_show_socket = loaded_viewer.inputs["Show"]
            loaded_inactive_viewer = next(
                node for node in loaded_group.nodes if node.label == "Inactive")

            self.assertEqual(loaded_viewer.label, "Surface")
            self.assertTrue(loaded_viewer.is_debug_view)
            self.assertFalse(loaded_show_socket.default_value)
            self.assertEqual(len(loaded_show_socket.links), 1)
            self.assertEqual(loaded_show_socket.links[0].from_node.bl_idname,
                             "FunctionNodeInputBool")
            self.assertFalse(loaded_inactive_viewer.is_debug_view)
            self.assertIsNone(loaded_inactive_viewer.inputs.get("Show"))
            loaded_inactive_viewer.is_debug_view = True
            loaded_inactive_show_socket = loaded_inactive_viewer.inputs["Show"]
            self.assertEqual(len(loaded_inactive_show_socket.links), 1)
            self.assertEqual(loaded_inactive_show_socket.links[0].from_node.bl_idname,
                             "FunctionNodeInputBool")
            self.assertTrue(loaded_modifier.show_debug_views)


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
        self.assertIn("NESTED_DEBUG_EVALUATED", warnings)

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
        self.assertIn("NESTED_DEBUG_EVALUATED", warnings)

        root_b_show.default_value = True
        self.assertEqual(self.evaluate(), 8)


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
