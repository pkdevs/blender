# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import sys
import tempfile
import unittest

import bpy


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

    def test_debug_view_evaluation_and_modifier_isolation(self):
        group, viewer = self.create_node_group()
        obj_a, modifier_a = self.create_object_with_modifier("Object A", group)
        obj_b, modifier_b = self.create_object_with_modifier("Object B", group)

        self.assertIsNone(viewer.inputs.get("Show"))
        modifier_a.show_debug_views = True
        modifier_b.show_debug_views = False

        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertEqual(self.evaluate(obj_b), 8)
        self.assertEqual(self.warning_messages(modifier_a), [])
        self.assertEqual(self.warning_messages(modifier_b), [])

        viewer.is_debug_view = True
        show_socket = viewer.inputs["Show"]
        show_socket.default_value = False

        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertEqual(self.warning_messages(modifier_a), [])

        show_socket.default_value = True

        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertEqual(self.evaluate(obj_b), 8)
        self.assertIn("Debug View evaluated", self.warning_messages(modifier_a))
        self.assertEqual(self.warning_messages(modifier_b), [])

    def test_show_socket_preserves_link(self):
        group, viewer = self.create_node_group()
        self.assertFalse(viewer.is_debug_view)
        viewer.is_debug_view = True
        show_socket = viewer.inputs["Show"]
        self.assertTrue(show_socket.default_value)
        show_socket.default_value = False
        boolean = group.nodes.new("FunctionNodeInputBool")
        group.links.new(boolean.outputs["Boolean"], show_socket)

        viewer.is_debug_view = False
        self.assertTrue(show_socket.is_unavailable)
        self.assertEqual(len(show_socket.links), 1)

        viewer.is_debug_view = True
        self.assertFalse(show_socket.is_unavailable)
        self.assertFalse(show_socket.default_value)
        self.assertEqual(len(show_socket.links), 1)
        self.assertEqual(show_socket.links[0].from_node, boolean)

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
                bpy.ops.object.geometry_nodes_debug_view_toggle(candidate_index=1),
                {'FINISHED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(candidate_index=0),
                {'FINISHED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(candidate_index=1),
                {'FINISHED'})
            # Re-enable both independently.
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(candidate_index=0),
                {'FINISHED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(candidate_index=1),
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
                bpy.ops.object.geometry_nodes_debug_view_toggle(
                    context_hash=(1, 2, 3, 4), candidate_index=0),
                {'CANCELLED'})
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(
                    node_tree_lineage_hash=(1, 2, 3, 4), candidate_index=0),
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
        self.evaluate(obj)
        with bpy.context.temp_override(
                object=obj, active_object=obj, modifier=modifier):
            # Initialize runtime state and intentionally disable the only original candidate.
            self.assertEqual(
                bpy.ops.object.geometry_nodes_debug_view_toggle(candidate_index=0),
                {'FINISHED'})

        bpy.ops.ed.undo_push()
        bpy.ops.ed.undo_push()

        modifier.node_group = replacement_group
        self.evaluate(obj)
        bpy.ops.ed.undo_push()

        self.assertEqual(bpy.ops.ed.undo(), {'FINISHED'})
        restored_modifier = bpy.data.objects["Object"].modifiers["Geometry Nodes"]
        self.assertEqual(restored_modifier.node_group.name, "Original Geometry Nodes")

        self.assertEqual(bpy.ops.ed.redo(), {'FINISHED'})
        redone_modifier = bpy.data.objects["Object"].modifiers["Geometry Nodes"]
        self.assertEqual(redone_modifier.node_group.name, "Replacement Geometry Nodes")

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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
