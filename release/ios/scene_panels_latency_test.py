# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Measure native property action-to-snapshot latency on a physical device."""

import ctypes
import json
from pathlib import Path
import statistics
import time
import traceback

import bpy

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/ResponsiveDeviceProbe.dylib'))
query = probe.blender_device_probe_scene_panels
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
output = Path.home() / 'Documents/scene-panels-latency.json'
report = {'samples_ms': []}


def ui(**request):
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def location():
    return next(prop for section in ui()['properties']['sections'] for prop in section['properties']
                if prop.get('property') == 'location' and prop.get('index') == 0)


def tests():
    ui(action='inspector')
    yield 1.0
    for _ in range(12):
        prop = location()
        target = not prop['lock']['value']
        start = time.perf_counter()
        ui(action='property_lock', key=prop['key'])
        while location()['lock']['value'] != target:
            assert time.perf_counter() - start < 5, 'Native property snapshot timed out'
            yield 0.005
        assert bpy.data.objects['Cube'].lock_location[0] == target
        report['samples_ms'].append((time.perf_counter() - start) * 1000)
        yield 0.071
    report['median_ms'] = statistics.median(report['samples_ms'])
    report['max_ms'] = max(report['samples_ms'])
    report['passed'] = True


steps = tests()


def tick():
    try:
        delay = next(steps)
    except StopIteration:
        delay = None
    except Exception:
        report['error'] = traceback.format_exc()
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=5.0)
