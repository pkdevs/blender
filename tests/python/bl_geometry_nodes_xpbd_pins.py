# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_geometry_nodes_xpbd_pins.py
#
# Hard pins (compliance <= 0) in the XPBD solver are kinematic: no constraint
# may move them, and a pin driven along an animated target keeps both the
# target's position and its velocity, damping included.
import math
import os
import unittest

import bpy

CLOTH_ASSETS = os.path.join(bpy.utils.system_resource('DATAFILES'),
                            "assets", "nodes", "geometry_nodes_dynamics_assets.blend")


def load_group(name):
    if name not in bpy.data.node_groups:
        with bpy.data.libraries.load(CLOTH_ASSETS, link=False) as (data_from, data_to):
            assert name in data_from.node_groups, name
            data_to.node_groups = [name]
    return bpy.data.node_groups[name]


def find_socket(node, name, in_out='INPUT', socket_type=None):
    sockets = node.inputs if in_out == 'INPUT' else node.outputs
    for socket in sockets:
        if socket.name == name and (socket_type is None or socket.type == socket_type):
            return socket
    raise KeyError(f"socket {name!r} not found on {node.name}")


def clear_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)


def build_pinned_cloth(moving=False, bendiness=1e6, damping=5.0):
    """A cloth grid pinned along the x = -1 edge through a stored attribute,
    so the pin selection stays stable when the input geometry is animated."""
    cloth = load_group("Cloth Dynamics (Experimental)")
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=16, y_subdivisions=16, size=2.0)
    obj = bpy.context.object
    mesh = obj.data
    selection = mesh.attributes.new("pin_sel", 'FLOAT', 'POINT')
    selection.data.foreach_set(
        "value", [1.0 if vert.co.x < -0.99 else 0.0 for vert in mesh.vertices])

    tree = bpy.data.node_groups.new("Test Cloth", 'GeometryNodeTree')
    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    group_input = tree.nodes.new('NodeGroupInput')
    group_output = tree.nodes.new('NodeGroupOutput')
    cloth_node = tree.nodes.new('GeometryNodeGroup')
    cloth_node.node_tree = cloth
    link = tree.links.new

    geometry_out = group_input.outputs[0]
    if moving:
        # Move the whole input at 1 m/s in +x; the pin targets are the
        # animated input positions.
        time_node = tree.nodes.new('GeometryNodeInputSceneTime')
        combine = tree.nodes.new('ShaderNodeCombineXYZ')
        transform = tree.nodes.new('GeometryNodeTransform')
        link(time_node.outputs["Seconds"], combine.inputs["X"])
        link(combine.outputs["Vector"], find_socket(transform, "Translation"))
        link(geometry_out, find_socket(transform, "Geometry"))
        geometry_out = find_socket(transform, "Geometry", 'OUTPUT', 'GEOMETRY')

    pin_attr = tree.nodes.new('GeometryNodeInputNamedAttribute')
    pin_attr.data_type = 'FLOAT'
    pin_attr.inputs["Name"].default_value = "pin_sel"
    link(pin_attr.outputs["Attribute"], find_socket(cloth_node, "Pin Group", socket_type='VALUE'))
    link(geometry_out, find_socket(cloth_node, "Geometry", socket_type='GEOMETRY'))
    link(find_socket(cloth_node, "Geometry", 'OUTPUT', 'GEOMETRY'), group_output.inputs[0])

    find_socket(cloth_node, "Substeps", socket_type='INT').default_value = 20
    find_socket(cloth_node, "Constraint Steps", socket_type='INT').default_value = 15
    find_socket(cloth_node, "Gravity", socket_type='BOOLEAN').default_value = True
    find_socket(cloth_node, "Mass", socket_type='VALUE').default_value = 4.0 / len(mesh.vertices)
    find_socket(cloth_node, "Linear Damping", socket_type='VALUE').default_value = damping
    find_socket(cloth_node, "Stretchiness", socket_type='VALUE').default_value = 0.0
    find_socket(cloth_node, "Bendiness", socket_type='VALUE').default_value = bendiness

    modifier = obj.modifiers.new("Cloth", 'NODES')
    modifier.node_group = tree
    return obj


def run_frames(count):
    scene = bpy.context.scene
    scene.frame_start = 1
    scene.frame_end = count
    for frame in range(1, count + 1):
        scene.frame_set(frame)


def evaluated_points(obj, attributes=()):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mesh = obj.evaluated_get(depsgraph).to_mesh()
    coords = [tuple(vert.co) for vert in mesh.vertices]
    values = {}
    for name in attributes:
        attribute = mesh.attributes.get(name)
        values[name] = [tuple(item.vector) for item in attribute.data] if attribute else None
    obj.evaluated_get(depsgraph).to_mesh_clear()
    return coords, values


class HardPinTest(unittest.TestCase):
    def setUp(self):
        clear_scene()

    def test_stiff_network_cannot_move_hard_pin(self):
        # Stiff cross edge bending (Bendiness 0.5) pushes hard against the
        # pinned edge; the pins have to stay exactly at their targets anyway.
        obj = build_pinned_cloth(bendiness=0.5)
        rest = [tuple(vert.co) for vert in obj.data.vertices]
        pins = [i for i, coord in enumerate(rest) if coord[0] < -0.99]
        run_frames(100)
        coords, _ = evaluated_points(obj)
        worst = max(math.dist(coords[i], rest[i]) for i in pins)
        self.assertLess(worst, 1e-6)

    def test_animated_pin_keeps_position_and_velocity_under_damping(self):
        # A pin target moving at 1 m/s: the pinned points follow it exactly
        # and their velocity is the target's, which linear damping must not
        # touch (at high damping it would otherwise be scaled to zero while
        # the position stays exact, and friction reads that velocity).
        fps = bpy.context.scene.render.fps
        for damping in (5.0, 500.0):
            with self.subTest(damping=damping):
                clear_scene()
                obj = build_pinned_cloth(moving=True, damping=damping)
                rest = [tuple(vert.co) for vert in obj.data.vertices]
                pins = [i for i, coord in enumerate(rest) if coord[0] < -0.99]
                run_frames(48)
                offset = 48 / fps
                coords, values = evaluated_points(obj, ("velocity",))
                velocities = values["velocity"]
                self.assertIsNotNone(velocities)
                for i in pins:
                    target = (rest[i][0] + offset, rest[i][1], rest[i][2])
                    self.assertLess(math.dist(coords[i], target), 1e-4)
                    velocity = velocities[i]
                    self.assertLess(
                        abs(velocity[0] - 1.0) + abs(velocity[1]) + abs(velocity[2]), 1e-3)

    def test_hair_root_pins_chunked_solver_path(self):
        # The hair asset hard pins the strand roots, and all of its
        # constraints (rods, pins, damping) stay within one strand, which
        # keeps the simulation on the chunked solver path, unlike the cloth
        # cases above. Horizontal strands are dragged sideways at 1 m/s:
        # the roots have to follow their targets exactly and keep the
        # target velocity despite strong damping, while the free ends sag
        # and lag behind, proof that the strands actually simulated.
        hair = load_group("Hair Dynamics")
        curve = bpy.data.curves.new("strands", 'CURVE')
        obj = bpy.data.objects.new("strands", curve)
        bpy.context.collection.objects.link(obj)
        points_per_strand = 8
        rest = []
        for root_x in (-0.5, 0.0, 0.5):
            spline = curve.splines.new('POLY')
            spline.points.add(points_per_strand - 1)
            for i, point in enumerate(spline.points):
                coord = (root_x, i * 0.1, 0.0)
                point.co = (*coord, 1.0)
                rest.append(coord)

        tree = bpy.data.node_groups.new("Test Hair", 'GeometryNodeTree')
        tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
        tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
        group_input = tree.nodes.new('NodeGroupInput')
        group_output = tree.nodes.new('NodeGroupOutput')
        link = tree.links.new

        # Drag the strands at 1 m/s in +x; the animated input positions are
        # the pin targets for the roots.
        time_node = tree.nodes.new('GeometryNodeInputSceneTime')
        combine = tree.nodes.new('ShaderNodeCombineXYZ')
        transform = tree.nodes.new('GeometryNodeTransform')
        link(time_node.outputs["Seconds"], combine.inputs["X"])
        link(combine.outputs["Vector"], find_socket(transform, "Translation"))
        link(group_input.outputs[0], find_socket(transform, "Geometry"))

        # Track every point back to its input index instead of assuming the
        # solver preserves point order.
        store = tree.nodes.new('GeometryNodeStoreNamedAttribute')
        store.data_type = 'FLOAT'
        store.domain = 'POINT'
        store.inputs["Name"].default_value = "orig_index"
        index_node = tree.nodes.new('GeometryNodeInputIndex')
        link(index_node.outputs["Index"], find_socket(store, "Value", socket_type='VALUE'))
        link(find_socket(transform, "Geometry", 'OUTPUT', 'GEOMETRY'),
             find_socket(store, "Geometry"))

        hair_node = tree.nodes.new('GeometryNodeGroup')
        hair_node.node_tree = hair
        link(find_socket(store, "Geometry", 'OUTPUT', 'GEOMETRY'),
             find_socket(hair_node, "Hair", socket_type='GEOMETRY'))
        link(find_socket(hair_node, "Hair", 'OUTPUT', 'GEOMETRY'), group_output.inputs[0])
        find_socket(hair_node, "Mode").default_value = 'Physics (Experimental)'
        find_socket(hair_node, "Gravity", socket_type='BOOLEAN').default_value = True
        find_socket(hair_node, "Linear", socket_type='VALUE').default_value = 500.0
        modifier = obj.modifiers.new("Hair", 'NODES')
        modifier.node_group = tree

        fps = bpy.context.scene.render.fps
        run_frames(48)
        offset = 48 / fps
        depsgraph = bpy.context.evaluated_depsgraph_get()
        mesh = obj.evaluated_get(depsgraph).to_mesh()
        coords = [tuple(vert.co) for vert in mesh.vertices]
        indices = [int(round(item.value)) for item in mesh.attributes["orig_index"].data]
        velocities = [tuple(item.vector) for item in mesh.attributes["velocity"].data]
        obj.evaluated_get(depsgraph).to_mesh_clear()
        self.assertEqual(sorted(indices), list(range(len(rest))))

        roots = {strand * points_per_strand for strand in range(3)}
        tips = {strand * points_per_strand + points_per_strand - 1 for strand in range(3)}
        tip_lag = []
        for out_i, orig_i in enumerate(indices):
            target = (rest[orig_i][0] + offset, rest[orig_i][1], rest[orig_i][2])
            if orig_i in roots:
                self.assertLess(math.dist(coords[out_i], target), 1e-4)
                velocity = velocities[out_i]
                self.assertLess(
                    abs(velocity[0] - 1.0) + abs(velocity[1]) + abs(velocity[2]), 1e-3)
            elif orig_i in tips:
                tip_lag.append(math.dist(coords[out_i], target))
        self.assertEqual(len(tip_lag), 3)
        for lag in tip_lag:
            self.assertGreater(lag, 0.2)


if __name__ == "__main__":
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
