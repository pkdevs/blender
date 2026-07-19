# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_modifiers_smooth.py -- --verbose

__all__ = (
    "main",
)

import unittest
from math import cos, pi, sin

import bmesh
import bpy
from mathutils import Vector


def _make_uv_sphere(radius=1.0, u_segments=24, v_segments=16):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh = bpy.data.meshes.new("sphere")
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(
        bm,
        u_segments=u_segments,
        v_segments=v_segments,
        radius=radius,
    )
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new("sphere", mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _make_grid(size=1.0, x_segments=8, y_segments=8):
    """Flat quad grid — every border vertex is on the mesh boundary."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh = bpy.data.meshes.new("grid")
    bm = bmesh.new()
    bmesh.ops.create_grid(
        bm,
        x_segments=x_segments,
        y_segments=y_segments,
        size=size,
    )
    boundary = {v.index for v in bm.verts if any(e.is_boundary for e in v.link_edges)}
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new("grid", mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj, boundary


def _make_grid_marked(mark, size=1.0, x_segments=8, y_segments=8):
    """Flat quad grid with the interior `y == 0` edge line marked as `seam` or `sharp`.

    Returns (obj, marked, boundary): `marked` is the set of vertex indices touched by a
    marked edge, `boundary` the set of open-boundary vertex indices.
    """
    assert mark in {'seam', 'sharp'}
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh = bpy.data.meshes.new("grid")
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=x_segments, y_segments=y_segments, size=size)
    boundary = {v.index for v in bm.verts if any(e.is_boundary for e in v.link_edges)}
    marked = set()
    for e in bm.edges:
        v0, v1 = e.verts
        if abs(v0.co.y) < 1e-6 and abs(v1.co.y) < 1e-6:
            if mark == 'seam':
                e.seam = True
            else:
                e.smooth = False
            marked.update((v0.index, v1.index))
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new("grid", mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj, marked, boundary


def _make_frequency_ring(mode, verts_num=32):
    """Wire ring with a single discrete Fourier mode stored in Z."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh = bpy.data.meshes.new("frequency_ring")
    verts = []
    for i in range(verts_num):
        angle = 2.0 * pi * i / verts_num
        verts.append((cos(angle), sin(angle), sin(mode * angle)))
    edges = [(i, (i + 1) % verts_num) for i in range(verts_num)]
    mesh.from_pydata(verts, edges, [])
    obj = bpy.data.objects.new("frequency_ring", mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _add_smooth(obj, **params):
    mod = obj.modifiers.new("smooth", 'SMOOTH')
    for key, value in params.items():
        setattr(mod, key, value)
    return mod


def _evaluated_positions(obj):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    eval_mesh = obj.evaluated_get(depsgraph).data
    return [Vector(v.co) for v in eval_mesh.vertices]


def _evaluated_volume(obj):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    eval_mesh = obj.evaluated_get(depsgraph).data
    bm = bmesh.new()
    bm.from_mesh(eval_mesh)
    volume = bm.calc_volume(signed=False)
    bm.free()
    return volume


def _max_distance(a, b):
    return max((p - q).length for p, q in zip(a, b))


def _frequency_amplitude(positions, mode):
    """Project Z coordinates onto one sine mode of the ring."""
    verts_num = len(positions)
    return 2.0 / verts_num * sum(
        p.z * sin(2.0 * pi * mode * i / verts_num) for i, p in enumerate(positions)
    )


class TestSmoothDisabled(unittest.TestCase):
    """The modifier must be a true no-op when its parameters cancel out."""

    def _assert_unchanged(self, params, places=6):
        obj = _make_uv_sphere()
        original = _evaluated_positions(obj)
        _add_smooth(obj, **params)
        deformed = _evaluated_positions(obj)
        self.assertAlmostEqual(_max_distance(original, deformed), 0.0, places=places)

    def test_simple_zero_factor(self):
        self._assert_unchanged({'method': 'SIMPLE', 'factor': 0.0, 'iterations': 5})

    def test_taubin_zero_factor_zero_mu(self):
        self._assert_unchanged(
            {'method': 'TAUBIN', 'factor': 0.0, 'taubin_mu': 0.0, 'iterations': 5}
        )

    def test_hc_zero_factor(self):
        self._assert_unchanged(
            {'method': 'HC', 'factor': 0.0, 'hc_alpha': 0.5, 'hc_beta': 0.5, 'iterations': 5}
        )

    def test_frequency_zero_factor(self):
        self._assert_unchanged(
            {'method': 'FREQUENCY', 'factor': 0.0, 'frequency_cutoff': 0.25, 'iterations': 5}
        )

    def test_all_axes_off(self):
        self._assert_unchanged({
            'method': 'SIMPLE', 'factor': 0.5, 'iterations': 5,
            'use_x': False, 'use_y': False, 'use_z': False,
        })

    def test_taubin_zero_factor_nonzero_mu_runs(self):
        """With Taubin, a non-zero mu must keep the modifier active even at factor=0."""
        obj = _make_uv_sphere()
        original = _evaluated_positions(obj)
        _add_smooth(obj, method='TAUBIN', factor=0.0, taubin_mu=-0.5, iterations=5)
        deformed = _evaluated_positions(obj)
        self.assertGreater(_max_distance(original, deformed), 1e-4)


class TestSmoothZeroIterations(unittest.TestCase):

    def _assert_zero_iter_is_noop(self, params):
        obj = _make_uv_sphere()
        original = _evaluated_positions(obj)
        _add_smooth(obj, iterations=0, **params)
        self.assertAlmostEqual(_max_distance(original, _evaluated_positions(obj)), 0.0, places=6)

    def test_simple(self):
        self._assert_zero_iter_is_noop({'method': 'SIMPLE', 'factor': 0.5})

    def test_taubin(self):
        self._assert_zero_iter_is_noop({'method': 'TAUBIN', 'factor': 0.5, 'taubin_mu': -0.53})

    def test_hc(self):
        self._assert_zero_iter_is_noop(
            {'method': 'HC', 'factor': 1.0, 'hc_alpha': 0.5, 'hc_beta': 0.5}
        )

    def test_frequency(self):
        self._assert_zero_iter_is_noop(
            {'method': 'FREQUENCY', 'factor': 1.0, 'frequency_cutoff': 0.25}
        )


class TestSmoothFrequencyResponse(unittest.TestCase):
    """The implicit biharmonic filter must discriminate Laplacian frequencies."""

    @staticmethod
    def _retained_amplitude(mode, cutoff, iterations=1):
        obj = _make_frequency_ring(mode)
        original = _frequency_amplitude(_evaluated_positions(obj), mode)
        _add_smooth(
            obj,
            method='FREQUENCY',
            factor=1.0,
            frequency_cutoff=cutoff,
            iterations=iterations,
            use_x=False,
            use_y=False,
            use_z=True,
        )
        filtered = _frequency_amplitude(_evaluated_positions(obj), mode)
        return abs(filtered / original)

    def test_cutoff_is_half_power_frequency(self):
        """On a 32-ring, mode 8 has normalized Laplacian frequency exactly 1."""
        retained = self._retained_amplitude(mode=8, cutoff=1.0)
        self.assertAlmostEqual(retained, 0.5, places=4)

    def test_high_frequency_is_attenuated_more(self):
        low = self._retained_amplitude(mode=1, cutoff=0.25)
        high = self._retained_amplitude(mode=8, cutoff=0.25)
        self.assertGreater(low, 0.98)
        self.assertLess(high, 0.1)
        self.assertLess(high, low * 0.1)

    def test_lower_cutoff_removes_more_detail(self):
        lower_cutoff = self._retained_amplitude(mode=8, cutoff=0.1)
        higher_cutoff = self._retained_amplitude(mode=8, cutoff=0.8)
        self.assertLess(lower_cutoff, higher_cutoff)


class TestSmoothVolumePreservation(unittest.TestCase):
    """Taubin and HC should retain more sphere volume than Simple smoothing."""

    @staticmethod
    def _smoothed_volume_ratio(method, **params):
        obj = _make_uv_sphere(radius=1.0)
        v0 = _evaluated_volume(obj)
        _add_smooth(obj, method=method, iterations=10, **params)
        return _evaluated_volume(obj) / v0

    def test_simple_shrinks_sphere(self):
        ratio = self._smoothed_volume_ratio('SIMPLE', factor=0.5)
        self.assertLess(ratio, 0.99)

    def test_taubin_preserves_more_than_simple(self):
        ratio_simple = self._smoothed_volume_ratio('SIMPLE', factor=0.5)
        ratio_taubin = self._smoothed_volume_ratio('TAUBIN', factor=0.5, taubin_mu=-0.53)
        self.assertGreater(ratio_taubin, ratio_simple)

    def test_hc_preserves_more_than_simple(self):
        ratio_simple = self._smoothed_volume_ratio('SIMPLE', factor=0.5)
        ratio_hc = self._smoothed_volume_ratio(
            'HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5,
        )
        self.assertGreater(ratio_hc, ratio_simple)


class TestSmoothAxisMask(unittest.TestCase):

    def test_only_z_keeps_xy_unchanged(self):
        obj = _make_uv_sphere()
        original = _evaluated_positions(obj)
        _add_smooth(
            obj, method='SIMPLE', factor=0.5, iterations=3,
            use_x=False, use_y=False, use_z=True,
        )
        deformed = _evaluated_positions(obj)
        for a, b in zip(original, deformed):
            self.assertAlmostEqual(a.x, b.x, places=6)
            self.assertAlmostEqual(a.y, b.y, places=6)
        # Z should actually move for at least some verts.
        max_dz = max(abs(a.z - b.z) for a, b in zip(original, deformed))
        self.assertGreater(max_dz, 1e-4)


class TestSmoothDeterminism(unittest.TestCase):
    """Repeated evaluations with identical parameters must give identical output.

    A regression here usually signals a race condition in the per-vertex gather.
    """

    @staticmethod
    def _run(method, **params):
        obj = _make_uv_sphere()
        _add_smooth(obj, method=method, iterations=10, **params)
        return _evaluated_positions(obj)

    def _assert_deterministic(self, method, **params):
        reference = self._run(method, **params)
        for _ in range(3):
            again = self._run(method, **params)
            for p, q in zip(reference, again):
                self.assertEqual(p, q)

    def test_simple(self):
        self._assert_deterministic('SIMPLE', factor=0.5)

    def test_taubin(self):
        self._assert_deterministic('TAUBIN', factor=0.5, taubin_mu=-0.53)

    def test_hc(self):
        self._assert_deterministic('HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5)

    def test_frequency(self):
        self._assert_deterministic('FREQUENCY', factor=1.0, frequency_cutoff=0.25)


class TestSmoothPinBoundary(unittest.TestCase):
    """`use_pin_boundary` must freeze boundary verts while interior smoothing still runs."""

    @staticmethod
    def _perturb_interior_z(obj, boundary):
        for i, v in enumerate(obj.data.vertices):
            if i not in boundary:
                v.co.z += 0.3 if (i % 2 == 0) else -0.3

    def _assert_boundary_pinned(self, method, **params):
        obj, boundary = _make_grid()
        self._perturb_interior_z(obj, boundary)
        original = _evaluated_positions(obj)
        _add_smooth(obj, method=method, use_pin_boundary=True, iterations=5, **params)
        deformed = _evaluated_positions(obj)

        max_boundary_drift = max((original[i] - deformed[i]).length for i in boundary)
        self.assertLess(max_boundary_drift, 1e-5)

        interior = [i for i in range(len(original)) if i not in boundary]
        max_interior_move = max((original[i] - deformed[i]).length for i in interior)
        self.assertGreater(max_interior_move, 1e-3)

    def test_simple(self):
        self._assert_boundary_pinned('SIMPLE', factor=0.5)

    def test_taubin(self):
        self._assert_boundary_pinned('TAUBIN', factor=0.5, taubin_mu=-0.53)

    def test_hc(self):
        self._assert_boundary_pinned('HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5)

    def test_frequency(self):
        self._assert_boundary_pinned('FREQUENCY', factor=1.0, frequency_cutoff=0.25)

    def test_off_by_default_boundaries_move(self):
        """Without the toggle, boundary verts must be free to drift (regression guard)."""
        obj, boundary = _make_grid()
        self._perturb_interior_z(obj, boundary)
        original = _evaluated_positions(obj)
        _add_smooth(obj, method='SIMPLE', factor=0.5, iterations=5)
        deformed = _evaluated_positions(obj)
        max_boundary_drift = max((original[i] - deformed[i]).length for i in boundary)
        self.assertGreater(max_boundary_drift, 1e-3)

    def test_closed_mesh_pin_is_noop(self):
        """A closed mesh has no boundary — pinning must not affect the output."""
        params = {'method': 'TAUBIN', 'factor': 0.5, 'taubin_mu': -0.53, 'iterations': 5}
        obj_off = _make_uv_sphere()
        _add_smooth(obj_off, use_pin_boundary=False, **params)
        off = _evaluated_positions(obj_off)

        obj_on = _make_uv_sphere()
        _add_smooth(obj_on, use_pin_boundary=True, **params)
        on = _evaluated_positions(obj_on)

        for a, b in zip(off, on):
            self.assertAlmostEqual((a - b).length, 0.0, places=6)


class TestSmoothPinEdgeMarks(unittest.TestCase):
    """`use_pin_seam` / `use_pin_sharp` must freeze verts on marked edges."""

    @staticmethod
    def _perturb_z(obj):
        for i, v in enumerate(obj.data.vertices):
            v.co.z += 0.3 if (i % 2 == 0) else -0.3

    def _assert_marked_pinned(self, mark, pin_param, method, **params):
        obj, marked, _boundary = _make_grid_marked(mark)
        self._perturb_z(obj)
        original = _evaluated_positions(obj)
        _add_smooth(obj, method=method, iterations=5, **{pin_param: True}, **params)
        deformed = _evaluated_positions(obj)

        max_marked_drift = max((original[i] - deformed[i]).length for i in marked)
        self.assertLess(max_marked_drift, 1e-5)

        free = [i for i in range(len(original)) if i not in marked]
        max_free_move = max((original[i] - deformed[i]).length for i in free)
        self.assertGreater(max_free_move, 1e-3)

    def test_seam_simple(self):
        self._assert_marked_pinned('seam', 'use_pin_seam', 'SIMPLE', factor=0.5)

    def test_seam_taubin(self):
        self._assert_marked_pinned('seam', 'use_pin_seam', 'TAUBIN', factor=0.5, taubin_mu=-0.53)

    def test_seam_hc(self):
        self._assert_marked_pinned('seam', 'use_pin_seam', 'HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5)

    def test_sharp_simple(self):
        self._assert_marked_pinned('sharp', 'use_pin_sharp', 'SIMPLE', factor=0.5)

    def test_sharp_taubin(self):
        self._assert_marked_pinned('sharp', 'use_pin_sharp', 'TAUBIN', factor=0.5, taubin_mu=-0.53)

    def test_sharp_hc(self):
        self._assert_marked_pinned('sharp', 'use_pin_sharp', 'HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5)

    def test_seam_off_by_default_marked_verts_move(self):
        """Interior seam verts must drift when the toggle is off (regression guard)."""
        obj, marked, boundary = _make_grid_marked('seam')
        self._perturb_z(obj)
        original = _evaluated_positions(obj)
        _add_smooth(obj, method='SIMPLE', factor=0.5, iterations=5)
        deformed = _evaluated_positions(obj)
        interior_marked = [i for i in marked if i not in boundary]
        max_drift = max((original[i] - deformed[i]).length for i in interior_marked)
        self.assertGreater(max_drift, 1e-3)

    def test_pins_combine(self):
        """Enabling seam and sharp together pins the union of both edge sets."""
        obj, marked, _boundary = _make_grid_marked('seam')
        for e in obj.data.edges:
            v0, v1 = (obj.data.vertices[i].co for i in e.vertices)
            if abs(v0.x) < 1e-6 and abs(v1.x) < 1e-6:
                e.use_edge_sharp = True
        sharp_verts = {i for e in obj.data.edges if e.use_edge_sharp for i in e.vertices}
        self._perturb_z(obj)
        original = _evaluated_positions(obj)
        _add_smooth(obj, method='SIMPLE', factor=0.5, iterations=5,
                    use_pin_seam=True, use_pin_sharp=True)
        deformed = _evaluated_positions(obj)
        pinned = marked | sharp_verts
        max_pinned_drift = max((original[i] - deformed[i]).length for i in pinned)
        self.assertLess(max_pinned_drift, 1e-5)


class TestSmoothCotangentWeights(unittest.TestCase):

    def test_cotangent_toggle_changes_result_on_irregular_mesh(self):
        """A UV sphere's pole triangulation is non-uniform — cotangent must differ."""
        params = {
            'method': 'TAUBIN', 'factor': 0.5, 'taubin_mu': -0.53, 'iterations': 5,
        }
        obj_uniform = _make_uv_sphere()
        _add_smooth(obj_uniform, use_cotangent_weights=False, **params)
        uniform = _evaluated_positions(obj_uniform)

        obj_cotan = _make_uv_sphere()
        _add_smooth(obj_cotan, use_cotangent_weights=True, **params)
        cotan = _evaluated_positions(obj_cotan)

        self.assertGreater(_max_distance(uniform, cotan), 1e-4)


def main():
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()


if __name__ == "__main__":
    main()
