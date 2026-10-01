# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


import math
import unittest
import bpy
import sys


class TestObjectEdit(unittest.TestCase):

    def setUp(self):
        if bpy.context.object and bpy.context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        for mesh in [mesh for mesh in bpy.data.meshes]:
            bpy.data.meshes.remove(mesh)
        for ob in [ob for ob in bpy.data.objects]:
            bpy.data.objects.remove(ob)
        for pointcloud in [pointcloud for pointcloud in bpy.data.pointclouds]:
            bpy.data.pointclouds.remove(pointcloud)
        for node_group in [node_group for node_group in bpy.data.node_groups]:
            bpy.data.node_groups.remove(node_group)

    def _add_gaussian_splat_store_attribute(self, tree, name, data_type, value):
        node = tree.nodes.new("GeometryNodeStoreNamedAttribute")
        node.data_type = data_type
        node.domain = 'POINT'
        node.inputs["Name"].default_value = name
        node.inputs["Value"].default_value = value
        return node

    def _add_gaussian_splat_object(self):
        # Build a single-splat gaussian splat point cloud with a Geometry Nodes modifier:
        # one point -> Set Point Cloud Type (gaussian splat) -> Store Named Attribute x5,
        # then apply the modifier. This avoids depending on any file fixture.
        pointcloud = bpy.data.pointclouds.new("GSplatPointCloud")
        ob = bpy.data.objects.new("GSplat", pointcloud)
        bpy.context.collection.objects.link(ob)
        bpy.context.view_layer.objects.active = ob

        modifier = ob.modifiers.new("GSplat", 'NODES')
        tree = bpy.data.node_groups.new("GSplatTree", 'GeometryNodeTree')
        modifier.node_group = tree

        tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
        group_output = tree.nodes.new("NodeGroupOutput")

        points = tree.nodes.new("GeometryNodePoints")
        points.inputs["Count"].default_value = 1
        points.inputs["Position"].default_value = (0.0, 0.0, 0.0)

        set_type = tree.nodes.new("GeometryNodePointsSetType")
        set_type.inputs["Type"].default_value = "3D Gaussian Splat"

        # NodeSocketRotation stores its default as an Euler; (0, 0, 0) is the identity quaternion.
        rotation = self._add_gaussian_splat_store_attribute(
            tree, "rotation", 'QUATERNION', (0.0, 0.0, 0.0))
        scale = self._add_gaussian_splat_store_attribute(
            tree, "scale", 'FLOAT_VECTOR', (1.0, 2.0, 3.0))
        sh_0 = self._add_gaussian_splat_store_attribute(
            tree, "radiance:sh_0", 'FLOAT_VECTOR', (1.0, 1.0, 1.0))
        sh_1 = self._add_gaussian_splat_store_attribute(
            tree, "radiance:sh_1", 'FLOAT_VECTOR', (2.0, 2.0, 2.0))
        sh_2 = self._add_gaussian_splat_store_attribute(
            tree, "radiance:sh_2", 'FLOAT_VECTOR', (3.0, 3.0, 3.0))

        tree.links.new(points.outputs["Points"], set_type.inputs["Points"])
        tree.links.new(set_type.outputs["Points"], rotation.inputs["Geometry"])
        tree.links.new(rotation.outputs["Geometry"], scale.inputs["Geometry"])
        tree.links.new(scale.outputs["Geometry"], sh_0.inputs["Geometry"])
        tree.links.new(sh_0.outputs["Geometry"], sh_1.inputs["Geometry"])
        tree.links.new(sh_1.outputs["Geometry"], sh_2.inputs["Geometry"])
        tree.links.new(sh_2.outputs["Geometry"], group_output.inputs["Geometry"])

        bpy.ops.object.modifier_apply(modifier=modifier.name)
        return ob

    def test_transform_apply_gaussian_splat(self):
        ob = self._add_gaussian_splat_object()

        # The setup must have produced exactly what the test needs, or a broken fixture could
        # pass silently.
        data = ob.data
        self.assertEqual(ob.type, 'POINTCLOUD')
        self.assertEqual(data.type, 'GAUSSIAN_SPLAT')
        self.assertEqual(len(data.attributes["position"].data), 1)
        self.assertEqual(data.attributes["rotation"].data_type, 'QUATERNION')
        self.assertEqual(data.attributes["rotation"].domain, 'POINT')
        self.assertEqual(data.attributes["scale"].data_type, 'FLOAT_VECTOR')
        self.assertEqual(data.attributes["scale"].domain, 'POINT')
        for i in range(3):
            attr = data.attributes[f"radiance:sh_{i}"]
            self.assertEqual(attr.data_type, 'FLOAT_VECTOR')
            self.assertEqual(attr.domain, 'POINT')

        ob.rotation_euler = (0.0, 0.0, math.pi / 2)
        ob.scale = (2.0, 2.0, 2.0)
        bpy.ops.object.select_all(action='DESELECT')
        ob.select_set(True)
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

        self.assertEqual(tuple(ob.rotation_euler), (0.0, 0.0, 0.0))
        for value in ob.scale:
            self.assertAlmostEqual(value, 1.0, delta=1e-5)

        self._assert_gaussian_splat_transformed(ob.data, 0)

    def _assert_gaussian_splat_transformed(self, data, index):
        # The splat of #_add_gaussian_splat_object, rotated 90 degrees about Z and scaled by 2.
        rotation_value = tuple(data.attributes["rotation"].data[index].value)
        # Quaternion of 90 degrees about Z, in (w, x, y, z) order. #to_quaternion() may return
        # either sign, so compare up to sign.
        expected_rotation = (math.cos(math.pi / 4), 0.0, 0.0, math.sin(math.pi / 4))
        dot = sum(a * b for a, b in zip(rotation_value, expected_rotation))
        self.assertAlmostEqual(abs(dot), 1.0, delta=1e-5)

        scale_value = tuple(data.attributes["scale"].data[index].vector)
        for value, expected in zip(scale_value, (2.0, 4.0, 6.0)):
            self.assertAlmostEqual(value, expected, delta=1e-5)

        # Rotating the band-1 basis (-y, z, -x) by 90 degrees about Z maps coefficients
        # (c0, c1, c2) to (c2, c1, -c0).
        expected_sh = ((3.0, 3.0, 3.0), (2.0, 2.0, 2.0), (-1.0, -1.0, -1.0))
        for i, expected in enumerate(expected_sh):
            sh_value = tuple(data.attributes[f"radiance:sh_{i}"].data[index].vector)
            for value, expected_value in zip(sh_value, expected):
                self.assertAlmostEqual(value, expected_value, delta=1e-5)

    def test_join_gaussian_splats(self):
        active = self._add_gaussian_splat_object()
        other = self._add_gaussian_splat_object()
        other.rotation_euler = (0.0, 0.0, math.pi / 2)
        other.scale = (2.0, 2.0, 2.0)
        bpy.ops.object.select_all(action='DESELECT')
        active.select_set(True)
        other.select_set(True)
        bpy.context.view_layer.objects.active = active
        bpy.ops.object.join()

        data = active.data
        self.assertEqual(data.type, 'GAUSSIAN_SPLAT')
        self.assertEqual(len(data.attributes["position"].data), 2)
        # The splat of the active object is unchanged.
        self.assertEqual(tuple(data.attributes["rotation"].data[0].value), (1.0, 0.0, 0.0, 0.0))
        self.assertEqual(tuple(data.attributes["scale"].data[0].vector), (1.0, 2.0, 3.0))
        self.assertEqual(tuple(data.attributes["radiance:sh_2"].data[0].vector), (3.0, 3.0, 3.0))
        self._assert_gaussian_splat_transformed(data, 1)

    def test_auto_smooth_detection(self):
        bpy.ops.mesh.primitive_cube_add()
        ob = bpy.context.object
        bpy.ops.object.shade_auto_smooth(use_auto_smooth=True)
        bpy.ops.object.shade_auto_smooth(use_auto_smooth=True)
        bpy.ops.object.shade_auto_smooth(use_auto_smooth=True)
        bpy.ops.object.shade_auto_smooth(use_auto_smooth=True)
        self.assertEqual(len(ob.modifiers), 1)
        bpy.ops.object.shade_flat()
        self.assertEqual(len(ob.modifiers), 0)
        bpy.ops.object.shade_auto_smooth(use_auto_smooth=True)
        bpy.ops.object.shade_smooth()
        self.assertEqual(len(ob.modifiers), 0)


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
