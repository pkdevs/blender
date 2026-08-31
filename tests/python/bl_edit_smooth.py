# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_edit_smooth.py -- --verbose

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
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj


def _make_grid(size=1.0, x_segments=8, y_segments=8):
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
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj, boundary


def _positions(obj):
    return [Vector(v.co) for v in obj.data.vertices]


def _volume(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    volume = bm.calc_volume(signed=False)
    bm.free()
    return volume


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
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj


def _max_distance(a, b):
    return max((p - q).length for p, q in zip(a, b))


def _frequency_amplitude(positions, mode):
    """Project Z coordinates onto one sine mode of the ring."""
    verts_num = len(positions)
    return 2.0 / verts_num * sum(
        p.z * sin(2.0 * pi * mode * i / verts_num) for i, p in enumerate(positions)
    )


def _frequency_equation_relative_residual(obj, original, filtered, cutoff, axis=2):
    """Return ||b - (I + L^T L / cutoff^2)x|| / ||b|| for uniform edge weights."""
    neighbors = [[] for _ in obj.data.vertices]
    for edge in obj.data.edges:
        a, b = edge.vertices
        neighbors[a].append(b)
        neighbors[b].append(a)

    def laplacian(values):
        result = []
        for i, adjacent in enumerate(neighbors):
            if adjacent:
                result.append(values[i] - sum(values[j] for j in adjacent) / len(adjacent))
            else:
                result.append(0.0)
        return result

    filtered_axis = [position[axis] for position in filtered]
    laplacian_values = laplacian(filtered_axis)
    transposed_laplacian = []
    for i, adjacent in enumerate(neighbors):
        value = laplacian_values[i]
        value -= sum(laplacian_values[j] / len(neighbors[j]) for j in adjacent)
        transposed_laplacian.append(value)

    residual = [
        original[i][axis] -
        (filtered_axis[i] + transposed_laplacian[i] / (cutoff * cutoff))
        for i in range(len(original))
    ]
    right_hand_side_squared = sum(position[axis] ** 2 for position in original)
    return (sum(value * value for value in residual) / right_hand_side_squared) ** 0.5


def _smooth_edit(obj, select='ALL', **params):
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.context.tool_settings.mesh_select_mode = (True, False, False)
    bm = bmesh.from_edit_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    if select == 'ALL':
        for v in bm.verts:
            v.select = True
    elif select == 'INTERIOR':
        for v in bm.verts:
            v.select = not any(e.is_boundary for e in v.link_edges)
    elif isinstance(select, set):
        for v in bm.verts:
            v.select = v.index in select
    bm.select_flush_mode()
    bmesh.update_edit_mesh(obj.data)
    bpy.ops.mesh.vertices_smooth(**params)
    bpy.ops.object.mode_set(mode='OBJECT')


class TestEditSmoothDisabled(unittest.TestCase):
    """The operator must be a true no-op when its parameters cancel out."""

    def _assert_unchanged(self, params, places=6):
        obj = _make_uv_sphere()
        original = _positions(obj)
        _smooth_edit(obj, **params)
        self.assertAlmostEqual(_max_distance(original, _positions(obj)), 0.0, places=places)

    def test_simple_zero_factor(self):
        self._assert_unchanged({'method': 'SIMPLE', 'factor': 0.0, 'repeat': 5})

    def test_taubin_zero_factor_zero_mu(self):
        self._assert_unchanged(
            {'method': 'TAUBIN', 'factor': 0.0, 'taubin_mu': 0.0, 'repeat': 5}
        )

    def test_hc_zero_factor(self):
        self._assert_unchanged(
            {'method': 'HC', 'factor': 0.0, 'hc_alpha': 0.5, 'hc_beta': 0.5, 'repeat': 5}
        )

    def test_frequency_zero_factor(self):
        self._assert_unchanged(
            {'method': 'FREQUENCY', 'factor': 0.0, 'frequency_cutoff': 0.25, 'repeat': 5}
        )

    def test_all_axes_off(self):
        self._assert_unchanged({
            'method': 'SIMPLE', 'factor': 0.5, 'repeat': 5,
            'xaxis': False, 'yaxis': False, 'zaxis': False,
        })

    def test_taubin_zero_factor_nonzero_mu_runs(self):
        obj = _make_uv_sphere()
        original = _positions(obj)
        _smooth_edit(obj, method='TAUBIN', factor=0.0, taubin_mu=-0.5, repeat=5)
        self.assertGreater(_max_distance(original, _positions(obj)), 1e-4)


class TestEditSmoothVolumePreservation(unittest.TestCase):
    """Taubin and HC should retain more sphere volume than Simple smoothing."""

    @staticmethod
    def _smoothed_volume_ratio(method, **params):
        obj = _make_uv_sphere(radius=1.0)
        v0 = _volume(obj)
        _smooth_edit(obj, method=method, repeat=10, **params)
        return _volume(obj) / v0

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


class TestEditSmoothAxisMask(unittest.TestCase):

    def _assert_only_z_keeps_xy_unchanged(self, method, **params):
        obj = _make_uv_sphere()
        original = _positions(obj)
        _smooth_edit(
            obj, method=method, repeat=3,
            xaxis=False, yaxis=False, zaxis=True,
            **params,
        )
        deformed = _positions(obj)
        for a, b in zip(original, deformed):
            self.assertAlmostEqual(a.x, b.x, places=6)
            self.assertAlmostEqual(a.y, b.y, places=6)
        max_dz = max(abs(a.z - b.z) for a, b in zip(original, deformed))
        self.assertGreater(max_dz, 1e-4)

    def test_simple(self):
        self._assert_only_z_keeps_xy_unchanged('SIMPLE', factor=0.5)

    def test_taubin(self):
        self._assert_only_z_keeps_xy_unchanged('TAUBIN', factor=0.5, taubin_mu=-0.53)

    def test_hc(self):
        self._assert_only_z_keeps_xy_unchanged(
            'HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5,
        )

    def test_frequency(self):
        self._assert_only_z_keeps_xy_unchanged(
            'FREQUENCY', factor=1.0, frequency_cutoff=0.25,
        )


class TestEditSmoothFrequencyResponse(unittest.TestCase):
    """The implicit biharmonic filter must discriminate Laplacian frequencies."""

    @staticmethod
    def _retained_amplitude(
            mode, cutoff, repeat=1, factor=1.0, verts_num=32, scale=1.0):
        obj = _make_frequency_ring(mode, verts_num)
        if scale != 1.0:
            for vertex in obj.data.vertices:
                vertex.co *= scale
        original = _frequency_amplitude(_positions(obj), mode)
        _smooth_edit(
            obj,
            method='FREQUENCY',
            factor=factor,
            frequency_cutoff=cutoff,
            repeat=repeat,
            xaxis=False,
            yaxis=False,
            zaxis=True,
        )
        filtered = _frequency_amplitude(_positions(obj), mode)
        return abs(filtered / original)

    def test_cutoff_is_half_amplitude_frequency(self):
        """On a 32-ring, mode 8 has normalized Laplacian frequency exactly 1."""
        retained = self._retained_amplitude(mode=8, cutoff=1.0)
        self.assertAlmostEqual(retained, 0.5, places=4)

    def test_factor_is_applied_after_filter(self):
        retained = self._retained_amplitude(mode=8, cutoff=1.0, factor=0.5)
        self.assertAlmostEqual(retained, 0.75, places=4)

    def test_frequency_is_normalized_to_connectivity(self):
        """Equal cycles per edge have equal response even at different vertex counts."""
        ring_32 = self._retained_amplitude(mode=8, cutoff=1.0, verts_num=32)
        ring_64 = self._retained_amplitude(mode=16, cutoff=1.0, verts_num=64)
        self.assertAlmostEqual(ring_32, ring_64, places=4)

    def test_response_is_coordinate_scale_invariant(self):
        regular = self._retained_amplitude(mode=8, cutoff=1.0)
        microscopic = self._retained_amplitude(mode=8, cutoff=1.0, scale=1e-12)
        self.assertAlmostEqual(regular, microscopic, places=4)

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

    def test_low_cutoff_mixed_spectrum_satisfies_filter_equation(self):
        """A mixed spectrum catches false convergence that a single Fourier mode cannot."""
        cutoff = 0.01
        obj, _boundary = _make_grid(x_segments=24, y_segments=25)
        for i, vertex in enumerate(obj.data.vertices):
            vertex.co.z = sin(i * 0.731) + 0.35 * cos(i * 1.913)
        original = _positions(obj)
        _smooth_edit(
            obj,
            method='FREQUENCY',
            factor=1.0,
            frequency_cutoff=cutoff,
            repeat=1,
            xaxis=False,
            yaxis=False,
            zaxis=True,
        )
        filtered = _positions(obj)
        residual = _frequency_equation_relative_residual(
            obj, original, filtered, cutoff, axis=2,
        )
        self.assertLess(residual, 5e-3)


class TestEditSmoothSelection(unittest.TestCase):
    """Unselected vertices must stay fixed while selected vertices still move."""

    def _assert_unselected_fixed(self, method, **params):
        obj, boundary = _make_grid()
        for i, v in enumerate(obj.data.vertices):
            if i not in boundary:
                v.co.z += 0.3 if (i % 2 == 0) else -0.3
        original = _positions(obj)
        _smooth_edit(obj, select='INTERIOR', method=method, repeat=5, **params)
        deformed = _positions(obj)

        max_boundary_drift = max((original[i] - deformed[i]).length for i in boundary)
        self.assertLess(max_boundary_drift, 1e-5)

        interior = [i for i in range(len(original)) if i not in boundary]
        max_interior_move = max((original[i] - deformed[i]).length for i in interior)
        self.assertGreater(max_interior_move, 1e-3)

    def test_simple(self):
        self._assert_unselected_fixed('SIMPLE', factor=0.5)

    def test_taubin(self):
        self._assert_unselected_fixed('TAUBIN', factor=0.5, taubin_mu=-0.53)

    def test_hc(self):
        self._assert_unselected_fixed('HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5)

    def test_frequency(self):
        self._assert_unselected_fixed(
            'FREQUENCY', factor=1.0, frequency_cutoff=0.25,
        )


class TestEditSmoothCotangentWeights(unittest.TestCase):

    def _assert_cotangent_changes_result_on_irregular_mesh(self, method, **params):
        obj_uniform = _make_uv_sphere()
        _smooth_edit(
            obj_uniform, method=method, use_cotangent_weights=False, repeat=5, **params,
        )
        uniform = _positions(obj_uniform)

        obj_cotan = _make_uv_sphere()
        _smooth_edit(
            obj_cotan, method=method, use_cotangent_weights=True, repeat=5, **params,
        )
        cotan = _positions(obj_cotan)

        self.assertGreater(_max_distance(uniform, cotan), 1e-4)

    def test_taubin(self):
        self._assert_cotangent_changes_result_on_irregular_mesh(
            'TAUBIN', factor=0.5, taubin_mu=-0.53,
        )

    def test_hc(self):
        self._assert_cotangent_changes_result_on_irregular_mesh(
            'HC', factor=1.0, hc_alpha=0.0, hc_beta=0.5,
        )

    def test_frequency(self):
        self._assert_cotangent_changes_result_on_irregular_mesh(
            'FREQUENCY', factor=1.0, frequency_cutoff=0.25,
        )


def main():
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()


if __name__ == "__main__":
    main()
