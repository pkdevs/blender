# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run via --factory-startup --python in a separate, signed QA application.

Checks the same framework packaging used by TestFlight. Writes results to the
QA application's Documents directory. Never run against a user's open scene.
"""

import ctypes
import importlib
import json
from pathlib import Path
import sys
import traceback

import bpy


def main():
    report = {'checks': [], 'success': False}
    documents = Path.home() / 'Documents'
    documents.mkdir(exist_ok=True)
    bundle = Path(bpy.app.binary_path).parent
    try:
        assert Path(sys.executable).parent == bundle, sys.executable
        report['checks'].append('Python resolves frameworks relative to the app bundle')
        import numpy as np
        frameworks = sorted((bundle / 'Frameworks').glob('numpy.*.framework'))
        assert frameworks, 'No NumPy frameworks found'
        for framework in frameworks:
            module = importlib.import_module(framework.stem)
            assert str(module.__file__).endswith('.fwork'), module.__file__
        report['checks'].append(f'Imported all {len(frameworks)} NumPy frameworks')
        np.testing.assert_allclose(np.linalg.solve([[2.0, 0.0], [0.0, 4.0]], [6.0, 8.0]), [3.0, 2.0])
        np.testing.assert_allclose(np.fft.ifft(np.fft.fft([1.0, 2.0, 3.0])), [1.0, 2.0, 3.0])
        assert np.random.default_rng(42).integers(0, 10, size=4).shape == (4,)
        report['checks'].append('NumPy linear algebra, FFT and random generation work')
        oidn = ctypes.CDLL(str(bundle / 'Frameworks/OpenImageDenoise.framework/OpenImageDenoise'))
        oidn.oidnNewDevice.argtypes = [ctypes.c_int]
        oidn.oidnNewDevice.restype = ctypes.c_void_p
        oidn.oidnCommitDevice.argtypes = [ctypes.c_void_p]
        oidn.oidnGetDeviceError.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
        oidn.oidnGetDeviceError.restype = ctypes.c_int
        oidn.oidnReleaseDevice.argtypes = [ctypes.c_void_p]
        device = oidn.oidnNewDevice(0)
        assert device, 'Could not create the OpenImageDenoise device'
        try:
            oidn.oidnCommitDevice(device)
            message = ctypes.c_char_p()
            error = oidn.oidnGetDeviceError(device, ctypes.byref(message))
            assert error == 0, (error, message.value)
        finally:
            oidn.oidnReleaseDevice(device)
        report['checks'].append('OpenImageDenoise loads and initializes its Metal framework')
        scene = bpy.context.scene
        scene.render.resolution_x = 64
        scene.render.resolution_y = 64
        scene.render.resolution_percentage = 100
        scene.render.filepath = str(documents / 'testflight-render.png')
        bpy.ops.wm.save_as_mainfile(filepath=str(documents / 'testflight-check.blend'))
        bpy.ops.render.render(write_still=True)
        assert (documents / 'testflight-render.png').stat().st_size > 0
        report['checks'].append('Blender saves a scene and renders an image')
        report['success'] = True
    except Exception:
        report['error'] = traceback.format_exc()
    (documents / 'testflight-device-test.json').write_text(json.dumps(report, indent=2))
    print('TESTFLIGHT_DEVICE_TEST ' + json.dumps(report), flush=True)


main()
