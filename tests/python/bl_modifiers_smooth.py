# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_modifiers_smooth.py -- --verbose

__all__ = (
    "main",
)

import unittest

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
