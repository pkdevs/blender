# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# blender -b --factory-startup --python tests/python/bl_pyapi_gpu.py

import unittest

import gpu


class TestGpuInit(unittest.TestCase):
    def test_gpu_module_is_usable_after_init(self):
        gpu.init()

        # GPU module functions will raise SystemError if the GPU has not been initialized.
        gpu.platform.vendor_get()
        gpu.platform.renderer_get()
        gpu.platform.version_get()
        gpu.platform.device_type_get()
        gpu.types.GPUTexture(size=(1024, 1024), format="RGBA8")


class TestGpuFrameBuffer(unittest.TestCase):
    def test_read_color_bounds_check(self):
        gpu.init()

        # Setup a framebuffer.
        tex = gpu.types.GPUTexture(size=(16, 16), format="RGBA8")
        fb = gpu.types.GPUFrameBuffer(color_slots=[tex])

        with fb.bind():
            # Reading within bounds should succeed.
            buf = fb.read_color(0, 0, 1, 1, 4, 0, "UBYTE")

            # Reading outside bounds should raise ValueError.
            with self.assertRaises(ValueError):
                fb.read_color(-1, 0, 1, 1, 4, 0, "UBYTE")


class TestGpuTextureUpdate(unittest.TestCase):
    @staticmethod
    def _rgba8_payload(width, height):
        return [(index * 17 + 31) & 0xFF for index in range(width * height * 4)]

    def test_update_rgba8_ubyte_buffer(self):
        gpu.init()

        payload = self._rgba8_payload(2, 2)
        tex = gpu.types.GPUTexture(size=(2, 2), format="RGBA8")

        tex.update(gpu.types.Buffer("UBYTE", len(payload), payload), format="UBYTE")

        self.assertEqual(memoryview(tex.read()).tobytes(), bytes(payload))

    def test_update_uses_ubyte_default_format(self):
        gpu.init()

        payload = self._rgba8_payload(2, 2)
        tex = gpu.types.GPUTexture(size=(2, 2), format="RGBA8")

        tex.update(gpu.types.Buffer("UBYTE", len(payload), payload))

        self.assertEqual(memoryview(tex.read()).tobytes(), bytes(payload))

    def test_update_rejects_float_buffer(self):
        gpu.init()

        tex = gpu.types.GPUTexture(size=(2, 2), format="RGBA8")
        data = gpu.types.Buffer("FLOAT", 16, [0.0] * 16)

        with self.assertRaises(ValueError):
            tex.update(data, format="UBYTE")

    def test_update_rejects_non_ubyte_format(self):
        gpu.init()

        tex = gpu.types.GPUTexture(size=(2, 2), format="RGBA8")
        data = gpu.types.Buffer("UBYTE", 16, [0] * 16)

        with self.assertRaises(ValueError):
            tex.update(data, format="FLOAT")

    def test_update_rejects_non_rgba8_texture(self):
        gpu.init()

        tex = gpu.types.GPUTexture(size=(2, 2), format="RGBA32F")
        data = gpu.types.Buffer("UBYTE", 16, [0] * 16)

        with self.assertRaises(ValueError):
            tex.update(data, format="UBYTE")

    def test_update_rejects_wrong_byte_count(self):
        gpu.init()

        tex = gpu.types.GPUTexture(size=(2, 2), format="RGBA8")
        data = gpu.types.Buffer("UBYTE", 15, [0] * 15)

        with self.assertRaises(ValueError):
            tex.update(data, format="UBYTE")

    def test_update_rejects_non_2d_texture(self):
        gpu.init()

        data = gpu.types.Buffer("UBYTE", 16, [0] * 16)
        for tex in (
            gpu.types.GPUTexture(size=4, format="RGBA8"),
            gpu.types.GPUTexture(size=(2, 2, 2), format="RGBA8"),
        ):
            with self.subTest(texture=repr(tex)):
                with self.assertRaises(ValueError):
                    tex.update(data, format="UBYTE")

    def test_update_rejects_array_and_cube_texture(self):
        gpu.init()

        data = gpu.types.Buffer("UBYTE", 16, [0] * 16)
        for tex in (
            gpu.types.GPUTexture(size=(2, 2), layers=2, format="RGBA8"),
            gpu.types.GPUTexture(size=2, is_cubemap=True, format="RGBA8"),
        ):
            with self.subTest(texture=repr(tex)):
                with self.assertRaises(ValueError):
                    tex.update(data, format="UBYTE")


if __name__ == "__main__":
    import sys

    sys.argv = [__file__] + (
        sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    )

    unittest.main()
