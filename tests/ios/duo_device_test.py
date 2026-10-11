# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Full-app Duo layout probe. Run in a disposable simulator QA application.

Requires Frameworks/ResponsiveDeviceProbe.dylib. For 90 seconds, record each
window geometry as the simulator rotates or switches displays. Screenshot
verification is still required: geometry alone cannot prove Metal drew a frame.
"""

import ctypes
import json
import time
import traceback
from pathlib import Path

import bpy


probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
probe.blender_device_probe_geometry.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
output = Path.home() / 'Documents/duo-layout-test.json'
report = {'checks': [], 'geometries': []}
started = time.monotonic()


def check(condition, name):
    assert condition, name
    if name not in report['checks']:
        report['checks'].append(name)


def sample():
    try:
        pointer = probe.blender_device_probe_geometry()
        try:
            geometry = json.loads(ctypes.string_at(pointer))
        finally:
            free(pointer)
        if not geometry:
            assert time.monotonic() - started < 20, 'Native controller did not appear'
            return 0.5
        check(geometry['sdk'] == 'iphonesimulator27.1', 'Built with the iOS 27.1 Simulator SDK')
        check(geometry['sceneManifest'], 'Scene-based app sizing')
        check(geometry['idiom'] == 0, 'Native iPhone idiom, not iPad compatibility mode')
        check(all(abs(a - b) < 1 for a, b in zip(geometry['window'], geometry['viewport'])),
              'Viewport fills the window, including behind native bars')
        check(all(abs(a - b) < 1 for a, b in zip(sorted(geometry['screen'][2:]),
                                                sorted(geometry['window'][2:]))),
              'Window fills the active display without compatibility letterboxing')
        check(geometry['leadingItems'][:3] == ['blender.mode', 'blender.actions', 'blender.undo'],
              'Mode, Blender menus and Undo grouped on the leading side')
        check(geometry['pinnedTrailingItem'] == 'blender.shading', 'Shading pinned on trailing side')
        safe = geometry['safeArea']
        lane = geometry['_action_rail']['frame']
        check(abs((safe[1] + safe[3]) - (lane[1] + lane[3]) - 6) < 1,
              'Actions stay at the bottom when folded, unfolded or rotated')
        check(lane[2] >= 44, 'Actions retain a usable scrolling lane')
        for neighbor in ('_tool_rail', '_view_gizmo_hotspot'):
            rect = geometry[neighbor]['frame']
            check(lane[0] + lane[2] <= rect[0] or rect[0] + rect[2] <= lane[0] or
                  lane[1] + lane[3] <= rect[1] or rect[1] + rect[3] <= lane[1],
                  f'Actions do not overlap {neighbor}')
        if geometry not in report['geometries']:
            report['geometries'].append(geometry)
        if 'numpy' not in report:
            import numpy as np
            matrix = np.array([[2.0, 0.0], [0.0, 3.0]])
            check(np.allclose(matrix @ np.linalg.inv(matrix), np.eye(2)),
                  'Target NumPy extensions import and execute linear algebra')
            report['numpy'] = np.__version__
        report['status'] = 'passed' if time.monotonic() - started >= 90 else 'sampling'
        output.write_text(json.dumps(report, indent=2))
        return 1.0 if report['status'] == 'sampling' else None
    except Exception:
        report['status'] = 'failed'
        report['error'] = traceback.format_exc()
        output.write_text(json.dumps(report, indent=2))
        traceback.print_exc()
        return None


bpy.app.timers.register(sample, first_interval=3.0)
