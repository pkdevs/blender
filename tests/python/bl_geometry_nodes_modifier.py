# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import pathlib
import tempfile
import unittest

import bpy


class GeometryNodesModifierLayerSelectionTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.context.scene.frame_set(1)
        self._tempdir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self._tempdir.cleanup()

    @staticmethod
    def create_grease_pencil_object():
        grease_pencil = bpy.data.grease_pencils.new("Layer Selection Test")
        obj = bpy.data.objects.new("Layer Selection Test", grease_pencil)
        bpy.context.scene.collection.objects.link(obj)

        for name, point_count in (("Layer A", 2), ("Layer B", 3)):
            layer = grease_pencil.layers.new(name)
            frame = layer.frames.new(1)
            frame.drawing.add_strokes([point_count])

        selection_attribute = grease_pencil.attributes.new(
            "select_a", type='BOOLEAN', domain='LAYER')
        for index, layer in enumerate(grease_pencil.layers):
            selection_attribute.data[index].value = layer.name == "Layer A"
        return obj

    @staticmethod
    def create_node_group():
        tree = bpy.data.node_groups.new("Layer Selection Test", 'GeometryNodeTree')
        tree.is_modifier = True
        geometry_input = tree.interface.new_socket(
            "Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
        tree.interface.new_socket(
            "Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
        selection_input = tree.interface.new_socket(
            "Selection", in_out='INPUT', socket_type='NodeSocketBool')
        selection_input.layer_selection_field = True

        group_input = tree.nodes.new("NodeGroupInput")
        group_output = tree.nodes.new("NodeGroupOutput")
        group_output.is_active_output = True
        separate = tree.nodes.new("GeometryNodeSeparateGeometry")
        separate.domain = 'LAYER'

        geometry_output = next(
            socket for socket in group_input.outputs
            if socket.identifier == geometry_input.identifier)
        selection_output = next(
            socket for socket in group_input.outputs
            if socket.identifier == selection_input.identifier)
        tree.links.new(geometry_output, separate.inputs["Geometry"])
        tree.links.new(selection_output, separate.inputs["Selection"])
        tree.links.new(separate.outputs["Selection"], group_output.inputs["Geometry"])
        return tree, selection_input, separate, group_output

    @staticmethod
    def evaluated_layers(obj):
        obj.update_tag(refresh={'DATA'})
        bpy.context.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        result = []
        for layer in evaluated.data.layers:
            frame = layer.current_frame()
            point_count = 0 if frame is None else sum(
                len(stroke.points) for stroke in frame.drawing.strokes)
            result.append((layer.name, point_count))
        return sorted(result)

    @staticmethod
    def use_separate_output(tree, separate, group_output, output_name):
        output_socket = group_output.inputs["Geometry"]
        for link in list(output_socket.links):
            tree.links.remove(link)
        tree.links.new(separate.outputs[output_name], output_socket)

    def test_layer_selection_input(self):
        obj = self.create_grease_pencil_object()
        tree, selection_input, separate, group_output = self.create_node_group()
        modifier = obj.modifiers.new("GeometryNodes", 'NODES')
        modifier.node_group = tree

        input_properties = getattr(modifier.properties.inputs, selection_input.identifier)
        self.assertEqual(input_properties.type, 'LAYER')
        input_properties.layer_name = "Layer A"
        self.assertEqual(self.evaluated_layers(obj), [("Layer A", 2)])

        self.use_separate_output(tree, separate, group_output, "Inverted")
        self.assertEqual(self.evaluated_layers(obj), [("Layer B", 3)])
        self.use_separate_output(tree, separate, group_output, "Selection")

        # Changing the interface flag does not reset the stored enum. Normal Boolean value and
        # attribute inputs must still work after changing a Layer Selection input back to normal.
        selection_input.layer_selection_field = False
        input_properties = getattr(modifier.properties.inputs, selection_input.identifier)
        self.assertEqual(input_properties.type, 'LAYER')
        input_properties.value = True
        self.assertEqual(
            self.evaluated_layers(obj), [("Layer A", 2), ("Layer B", 3)])
        input_properties.value = False
        self.assertEqual(self.evaluated_layers(obj), [])

        input_properties.type = 'ATTRIBUTE'
        input_properties.attribute_name = "select_a"
        self.assertEqual(self.evaluated_layers(obj), [("Layer A", 2)])

        # Reproduce the stale property state created by Blender 5.2 before this fix.
        input_properties.type = 'VALUE'
        selection_input.layer_selection_field = True
        input_properties = getattr(modifier.properties.inputs, selection_input.identifier)
        self.assertEqual(input_properties.type, 'VALUE')
        input_properties.layer_name = "Layer B"
        self.assertEqual(self.evaluated_layers(obj), [("Layer B", 3)])

        filepath = pathlib.Path(self._tempdir.name) / "layer_selection.blend"
        bpy.ops.wm.save_as_mainfile(filepath=str(filepath))
        bpy.ops.wm.open_mainfile(filepath=str(filepath))

        obj = bpy.data.objects["Layer Selection Test"]
        modifier = obj.modifiers["GeometryNodes"]
        tree = modifier.node_group
        selection_input = next(
            item for item in tree.interface.items_tree
            if item.item_type == 'SOCKET' and item.name == "Selection")
        input_properties = getattr(modifier.properties.inputs, selection_input.identifier)
        self.assertEqual(input_properties.type, 'VALUE')
        self.assertEqual(input_properties.layer_name, "Layer B")
        self.assertEqual(self.evaluated_layers(obj), [("Layer B", 3)])


if __name__ == "__main__":
    import sys

    sys.argv = [__file__] + (
        sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    )
    unittest.main()
