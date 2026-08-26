# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# blender -b --factory-startup --python tests/python/bevel_preserve_edge_data.py -- --verbose
import bpy
import sys
import unittest
from mathutils import Vector

EPS = 1e-5


def set_bool_edge_attr(mesh, name, indices=None):
    """Set a boolean edge attribute (creating it) on the given edge indices (default all)."""
    attr = mesh.attributes.get(name) or mesh.attributes.new(name=name, type='BOOLEAN', domain='EDGE')
    for i in (range(len(mesh.edges)) if indices is None else indices):
        attr.data[i].value = True


def make_seamed_cube():
    """Fresh default cube with all 12 edges marked as UV seams; returns the object."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.mesh.primitive_cube_add(size=2.0)
    ob = bpy.context.active_object
    set_bool_edge_attr(ob.data, "uv_seam")
    return ob


def bevel(ob, **kwargs):
    """Run MESH_OT_bevel on all edges of ob with given operator kwargs."""
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.bevel(affect='EDGES', **kwargs)
    bpy.ops.object.mode_set(mode='OBJECT')
    return ob.data


def bool_edges(mesh, name):
    """Indices of edges whose bool attribute `name` is set. Missing layer = empty."""
    attr = mesh.attributes.get(name)
    if attr is None or attr.domain != 'EDGE':
        return set()
    return {i for i, d in enumerate(attr.data) if d.value}


def float_edges(mesh, name, value, tol=1e-6):
    attr = mesh.attributes.get(name)
    if attr is None or attr.domain != 'EDGE':
        return set()
    return {i for i, d in enumerate(attr.data) if abs(d.value - value) <= tol}


# The 12 edge lines of the default 2x2x2 cube (pairs of corner coordinates).
_C = (-1.0, 1.0)
CUBE_EDGE_LINES = (
    [(Vector((x, y, -1.0)), Vector((x, y, 1.0))) for x in _C for y in _C]
    + [(Vector((x, -1.0, z)), Vector((x, 1.0, z))) for x in _C for z in _C]
    + [(Vector((-1.0, y, z)), Vector((1.0, y, z))) for y in _C for z in _C]
)


def point_on_segment(p, a, b, eps=EPS):
    ab = b - a
    t = (p - a).dot(ab) / ab.length_squared
    if t < -eps or t > 1.0 + eps:
        return None
    closest = a + ab * t
    if (p - closest).length > eps:
        return None
    return max(0.0, min(1.0, t))


def seam_coverage_gapless(mesh, seam_indices):
    """Check that every original cube edge line is fully covered by merged
    [0,1] intervals of seam edges lying on it (only valid at profile 1.0,
    where the preserved center loop lies exactly on the original edge line)."""
    for a, b in CUBE_EDGE_LINES:
        intervals = []
        for i in seam_indices:
            e = mesh.edges[i]
            p1 = mesh.vertices[e.vertices[0]].co
            p2 = mesh.vertices[e.vertices[1]].co
            t1 = point_on_segment(p1, a, b)
            t2 = point_on_segment(p2, a, b)
            if t1 is not None and t2 is not None:
                intervals.append(tuple(sorted((t1, t2))))
        if not intervals:
            return False
        intervals.sort()
        end = 0.0
        for lo, hi in intervals:
            if lo > end + EPS:
                return False
            end = max(end, hi)
        if end < 1.0 - EPS:
            return False
    return True


def island_count(mesh, seam_indices):
    """Count UV islands: union-find over faces linked by non-seam manifold edges."""
    parent = list(range(len(mesh.polygons)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    edge_faces = {}
    for poly in mesh.polygons:
        for ek in poly.edge_keys:
            edge_faces.setdefault(ek, []).append(poly.index)
    key_to_index = {tuple(sorted(e.vertices)): e.index for e in mesh.edges}
    for ek, faces in edge_faces.items():
        if len(faces) != 2:
            continue
        if key_to_index[ek] in seam_indices:
            continue
        ra, rb = find(faces[0]), find(faces[1])
        if ra != rb:
            parent[ra] = rb
    return len({find(i) for i in range(len(mesh.polygons))})


class TestBevelPreserveEdgeDataOption(unittest.TestCase):

    def test_operator_accepts_option(self):
        # Reference: master behavior copies edge data onto the two rail
        # (boundary) edges of every band, so 12 * 2 = 24 seam edges.
        ob = make_seamed_cube()
        reference = bool_edges(bevel(ob, offset=0.2, segments=2, profile=1.0),
                               "uv_seam")
        self.assertEqual(len(reference), 24)
        # Must not raise TypeError (unknown keyword) and must be identical
        # to the reference with the option off.
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=False)
        self.assertEqual(bool_edges(mesh, "uv_seam"), reference)

    def test_modifier_has_property(self):
        ob = make_seamed_cube()
        mod = ob.modifiers.new(name="Bevel", type='BEVEL')
        self.assertFalse(mod.preserve_edge_data)
        mod.preserve_edge_data = True
        self.assertTrue(mod.preserve_edge_data)


class TestBevelBandCenter(unittest.TestCase):

    def test_center_edges_seamed_segments_2(self):
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=True)
        seams = bool_edges(mesh, "uv_seam")
        # One center edge plus one spoke per corner for each original edge,
        # rails suppressed: 12 * (1 + 2) = 36.
        self.assertEqual(len(seams), 36)
        # At profile 1.0 every seam edge must lie on an original cube edge line.
        for i in seams:
            e = mesh.edges[i]
            on_line = any(
                point_on_segment(mesh.vertices[e.vertices[0]].co, a, b) is not None
                and point_on_segment(mesh.vertices[e.vertices[1]].co, a, b) is not None
                for a, b in CUBE_EDGE_LINES)
            self.assertTrue(on_line, "seam edge %d not on an original edge line" % i)

    def test_odd_segments_fall_back(self):
        # No center row exists: current (rail copy) behavior, byte-identical.
        ob = make_seamed_cube()
        reference = bool_edges(bevel(ob, offset=0.2, segments=3, profile=1.0),
                               "uv_seam")
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=3, profile=1.0,
                     preserve_edge_data=True)
        self.assertEqual(bool_edges(mesh, "uv_seam"), reference)
        self.assertEqual(len(reference), 24)

    def test_option_off_unchanged(self):
        ob = make_seamed_cube()
        reference = bool_edges(bevel(ob, offset=0.2, segments=2, profile=1.0),
                               "uv_seam")
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=False)
        self.assertEqual(bool_edges(mesh, "uv_seam"), reference)


class TestBevelPreserveEdgeDataAcceptance(unittest.TestCase):

    def test_seg2_counts_coverage_islands(self):
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=True)
        seams = bool_edges(mesh, "uv_seam")
        self.assertEqual(len(mesh.vertices), 56)
        self.assertEqual(len(mesh.edges), 108)
        self.assertEqual(len(seams), 36)
        self.assertTrue(seam_coverage_gapless(mesh, seams))
        self.assertEqual(island_count(mesh, seams), 6)

    def test_seg4_counts_islands(self):
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=4, profile=1.0,
                     preserve_edge_data=True)
        seams = bool_edges(mesh, "uv_seam")
        self.assertEqual(len(seams), 60)
        self.assertTrue(seam_coverage_gapless(mesh, seams))
        self.assertEqual(island_count(mesh, seams), 6)

    def test_profile_05_same_topology(self):
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=2, profile=0.5,
                     preserve_edge_data=True)
        seams = bool_edges(mesh, "uv_seam")
        # Geometry differs (no on-line check) but topology must match.
        self.assertEqual(len(seams), 36)
        self.assertEqual(island_count(mesh, seams), 6)

    def test_union_with_mark_seam(self):
        # mark_seam only extends seams of unbeveled edges crossing a bevel;
        # with every edge beveled it adds nothing (the 24 seams with the
        # option off come from the boundary copy, not from mark_seam --
        # see test_operator_accepts_option).
        ob = make_seamed_cube()
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=True, mark_seam=True)
        seams = bool_edges(mesh, "uv_seam")
        self.assertEqual(len(seams), 36)

    def test_sharp_edges(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = bpy.context.active_object
        set_bool_edge_attr(ob.data, "sharp_edge")
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=True)
        self.assertEqual(len(bool_edges(mesh, "sharp_edge")), 36)

    def test_crease_partial(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = bpy.context.active_object
        mesh_in = ob.data
        attr = mesh_in.attributes.new(name="crease_edge", type='FLOAT', domain='EDGE')
        # The four edges parallel to Z.
        z_parallel = [e.index for e in mesh_in.edges
                      if abs(mesh_in.vertices[e.vertices[0]].co.z
                             - mesh_in.vertices[e.vertices[1]].co.z) > 1.5]
        self.assertEqual(len(z_parallel), 4)
        for i in z_parallel:
            attr.data[i].value = 0.7
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=True)
        # 4 bands x (1 center + 2 corners x 1 spoke) = 12 edges with crease 0.7.
        self.assertEqual(len(float_edges(mesh, "crease_edge", 0.7)), 12)

    def test_bevel_weight_single_edge(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = bpy.context.active_object
        attr = ob.data.attributes.new(name="bevel_weight_edge", type='FLOAT', domain='EDGE')
        attr.data[0].value = 1.0
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=True)
        # 1 band x (1 center + 2 spokes) = 3 edges with weight 1.0.
        self.assertEqual(len(float_edges(mesh, "bevel_weight_edge", 1.0)), 3)

    def test_custom_attribute_and_mixed_corner(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = bpy.context.active_object
        attr = ob.data.attributes.new(name="my_attr", type='FLOAT', domain='EDGE')
        # Two edges sharing a corner vertex get different values.
        e0 = ob.data.edges[0]
        shared = set(e0.vertices)
        e1 = next(e for e in ob.data.edges
                  if e.index != e0.index and shared & set(e.vertices))
        attr.data[e0.index].value = 0.3
        attr.data[e1.index].value = 0.9
        mesh = bevel(ob, offset=0.2, segments=2, profile=1.0,
                     preserve_edge_data=True)
        # Each band: 1 center + 2 spokes = 3 edges carrying its own value.
        # No blending where the spokes meet at the shared corner.
        self.assertEqual(len(float_edges(mesh, "my_attr", 0.3)), 3)
        self.assertEqual(len(float_edges(mesh, "my_attr", 0.9)), 3)


class TestBevelModifierPath(unittest.TestCase):

    def test_modifier_preserves_seams(self):
        ob = make_seamed_cube()
        mod = ob.modifiers.new(name="Bevel", type='BEVEL')
        mod.width = 0.2
        mod.segments = 2
        mod.profile = 1.0
        mod.limit_method = 'NONE'
        mod.preserve_edge_data = True
        depsgraph = bpy.context.evaluated_depsgraph_get()
        mesh = ob.evaluated_get(depsgraph).to_mesh()
        seams = bool_edges(mesh, "uv_seam")
        self.assertEqual(len(seams), 36)


if __name__ == '__main__':
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:]
                             if "--" in sys.argv else [])
    unittest.main()
