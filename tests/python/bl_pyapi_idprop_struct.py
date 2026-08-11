# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_pyapi_idprop_struct.py -- --verbose

import os
import tempfile
import unittest

import bpy
from bpy.props import PointerProperty
from bpy.types import PropertyGroup


class IDPStructTestProps(PropertyGroup):
    curve: PointerProperty(type=bpy.types.CurveMapping)
    ramp: PointerProperty(type=bpy.types.ColorRamp)
    profile: PointerProperty(type=bpy.types.CurveProfile)


class TestIdPropertyStruct(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        bpy.utils.register_class(IDPStructTestProps)
        bpy.types.Scene.idp_struct_test = PointerProperty(type=IDPStructTestProps)

    @classmethod
    def tearDownClass(cls):
        del bpy.types.Scene.idp_struct_test
        bpy.utils.unregister_class(IDPStructTestProps)

    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_create_and_access(self):
        scene = bpy.context.scene
        props = scene.idp_struct_test

        self.assertIsInstance(props.curve, bpy.types.CurveMapping)
        self.assertIsInstance(props.ramp, bpy.types.ColorRamp)
        self.assertIsInstance(props.profile, bpy.types.CurveProfile)

        # Mutate curve mapping.
        curve = props.curve.curves[0]
        self.assertGreaterEqual(len(curve.points), 2)
        curve.points[0].location = (0.0, 0.25)
        curve.points[-1].location = (1.0, 0.75)
        self.assertAlmostEqual(curve.points[0].location[1], 0.25)
        self.assertAlmostEqual(curve.points[-1].location[1], 0.75)

        # Mutate color ramp.
        elements = props.ramp.elements
        self.assertGreaterEqual(len(elements), 2)
        elements[0].color = (1.0, 0.0, 0.0, 1.0)
        elements[-1].color = (0.0, 0.0, 1.0, 1.0)
        self.assertAlmostEqual(elements[0].color[0], 1.0)
        self.assertAlmostEqual(elements[-1].color[2], 1.0)

        # Mutate curve profile.
        points = props.profile.points
        self.assertGreaterEqual(len(points), 2)
        points[0].location = (0.0, 0.2)
        points[-1].location = (1.0, 0.8)
        self.assertAlmostEqual(points[0].location[1], 0.2)
        self.assertAlmostEqual(points[-1].location[1], 0.8)

    def test_struct_not_assignable(self):
        scene = bpy.context.scene
        props = scene.idp_struct_test
        # Access once so the owned data exists.
        _ = props.curve
        _ = props.profile
        with self.assertRaises((TypeError, RuntimeError, AttributeError)):
            props.curve = None
        with self.assertRaises((TypeError, RuntimeError, AttributeError)):
            props.profile = None

    def test_copy_independence(self):
        """Deep-copy must duplicate owned DNA structs (not share pointers)."""
        scene = bpy.context.scene
        props = scene.idp_struct_test

        props.curve.curves[0].points[0].location = (0.0, 0.25)
        props.curve.curves[0].points[-1].location = (1.0, 0.75)
        props.ramp.elements[0].color = (1.0, 0.0, 0.0, 1.0)
        props.ramp.elements[-1].position = 0.7
        props.profile.points[0].location = (0.0, 0.3)
        props.profile.points[-1].location = (1.0, 0.6)

        scene_copy = scene.copy()
        props_copy = scene_copy.idp_struct_test

        self.assertAlmostEqual(props_copy.curve.curves[0].points[0].location[1], 0.25)
        self.assertAlmostEqual(props_copy.curve.curves[0].points[-1].location[1], 0.75)
        self.assertAlmostEqual(props_copy.ramp.elements[0].color[0], 1.0)
        self.assertAlmostEqual(props_copy.ramp.elements[-1].position, 0.7)
        self.assertAlmostEqual(props_copy.profile.points[0].location[1], 0.3)
        self.assertAlmostEqual(props_copy.profile.points[-1].location[1], 0.6)

        # Mutating the original must not affect the copy (and vice versa).
        props.curve.curves[0].points[0].location = (0.0, 0.9)
        props.ramp.elements[0].color = (0.0, 1.0, 0.0, 1.0)
        props.profile.points[0].location = (0.0, 0.05)
        self.assertAlmostEqual(props_copy.curve.curves[0].points[0].location[1], 0.25)
        self.assertAlmostEqual(props_copy.ramp.elements[0].color[1], 0.0)
        self.assertAlmostEqual(props_copy.profile.points[0].location[1], 0.3)

        props_copy.curve.curves[0].points[-1].location = (1.0, 0.1)
        self.assertAlmostEqual(props.curve.curves[0].points[-1].location[1], 0.75)

    def test_pop_rejection(self):
        """Owned DNA structs cannot be popped into a detached Python value."""
        scene = bpy.context.scene
        props = scene.idp_struct_test
        _ = props.curve
        _ = props.ramp
        _ = props.profile

        with self.assertRaisesRegex(TypeError, "cannot pop IDProperty 'curve'"):
            props.pop("curve")
        with self.assertRaisesRegex(TypeError, "cannot pop IDProperty 'ramp'"):
            props.pop("ramp")
        with self.assertRaisesRegex(TypeError, "cannot pop IDProperty 'profile'"):
            scene.bl_system_properties_get()["idp_struct_test"].pop("profile")

        # Properties must still be accessible after failed pops.
        self.assertIsInstance(props.curve, bpy.types.CurveMapping)
        self.assertIsInstance(props.ramp, bpy.types.ColorRamp)
        self.assertIsInstance(props.profile, bpy.types.CurveProfile)

    def test_unsupported_pointer_property_type(self):
        """PointerProperty only allows ID, PropertyGroup, or registered struct types."""
        import io
        from contextlib import redirect_stderr

        class UnsupportedPointerProps(PropertyGroup):
            bad: PointerProperty(type=bpy.types.Event)

        # Registration prints the TypeError with PyErr_Print, then raises ValueError.
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            with self.assertRaises(ValueError):
                bpy.utils.register_class(UnsupportedPointerProps)

    def test_blend_roundtrip(self):
        scene = bpy.context.scene
        props = scene.idp_struct_test

        props.curve.curves[0].points[0].location = (0.0, 0.1)
        props.curve.curves[0].points[-1].location = (1.0, 0.9)
        props.ramp.elements[0].color = (0.2, 0.4, 0.6, 1.0)
        props.ramp.elements[-1].position = 0.8
        props.profile.points[0].location = (0.0, 0.15)
        props.profile.points[-1].location = (1.0, 0.85)

        with tempfile.TemporaryDirectory() as temp_dir:
            path = os.path.join(temp_dir, "idp_struct.blend")
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)

            props = bpy.context.scene.idp_struct_test
            self.assertAlmostEqual(props.curve.curves[0].points[0].location[1], 0.1)
            self.assertAlmostEqual(props.curve.curves[0].points[-1].location[1], 0.9)
            self.assertAlmostEqual(props.ramp.elements[0].color[0], 0.2)
            self.assertAlmostEqual(props.ramp.elements[0].color[1], 0.4)
            self.assertAlmostEqual(props.ramp.elements[0].color[2], 0.6)
            self.assertAlmostEqual(props.ramp.elements[-1].position, 0.8)
            self.assertAlmostEqual(props.profile.points[0].location[1], 0.15)
            self.assertAlmostEqual(props.profile.points[-1].location[1], 0.85)


if __name__ == "__main__":
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
