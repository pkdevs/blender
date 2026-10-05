# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sample real view transitions from native command queues in a factory QA scene."""

import bpy
import ctypes
import json
import time
import traceback
from pathlib import Path
from mathutils import Quaternion

engine = ctypes.CDLL(None)
command = engine.WM_apple_ios_ui_command_enqueue
command.argtypes = [ctypes.c_int]
quick_action = engine.WM_apple_ios_ui_operator_enum_enqueue
quick_action.argtypes = [ctypes.c_char_p] * 3
operator = engine.WM_apple_ios_ui_operator_enqueue
operator.argtypes = [ctypes.c_char_p]
output = Path.home() / 'Documents/smooth-view-test.json'
report = {'checks': [], 'transitions': {}}


def check(ok, name):
    assert ok, name
    report['checks'].append(name)


def tests():
    window = bpy.context.window_manager.windows[0]
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    view = area.spaces.active.region_3d
    preferences = bpy.context.preferences
    original_duration = preferences.view.smooth_view
    original_reduce_motion = preferences.view.use_reduce_motion

    def state():
        return tuple(view.view_location) + tuple(view.view_rotation) + (view.view_distance,)

    def different(a, b):
        return max(abs(x - y) for x, y in zip(a, b)) > 0.0001

    try:
        preferences.view.smooth_view = 600
        preferences.view.use_reduce_motion = False
        cases = [('Frame Selected', lambda: command(14)),
                 ('Frame All', lambda: command(24)),
                 ('Local View Enter', lambda: command(25)),
                 ('Local View Exit', lambda: command(25)),
                 ('Camera Enter', lambda: command(26)),
                 ('Camera Exit', lambda: command(26))]
        cases += [(name, lambda value=value: command(value)) for name, value in (
            ('Reset View', 15), ('Front', 28), ('Back', 29), ('Right', 30),
            ('Left', 31), ('Top', 32), ('Bottom', 33), ('Orbit Left', 16),
            ('Orbit Right', 17), ('Orbit Up', 18), ('Orbit Down', 19))]
        cases += [('Quick Action Frame', lambda: quick_action(b'VIEW3D_OT_view_selected', b'', b'')),
                  ('Enum Axis', lambda: quick_action(b'VIEW3D_OT_view_axis', b'type', b'TOP')),
                  ('Operator Frame All', lambda: operator(b'VIEW3D_OT_view_all'))]
        for name, invoke in cases:
            if name not in ('Local View Exit', 'Camera Exit'):
                view.view_perspective = 'PERSP'
                view.view_location = (5, 7, 2)
                view.view_distance = 30
                view.view_rotation = Quaternion((0.8, 0.3, 0.4, 0.2)).normalized()
                area.tag_redraw()
                yield 0.2
            samples = [state()]
            invoke()
            deadline = time.monotonic() + 1.3
            while time.monotonic() < deadline:
                yield 0.005
                samples.append(state())
            start, end = samples[0], samples[-1]
            intermediates = [s for s in samples if different(s, start) and different(s, end)]
            report['transitions'][name] = {'samples': len(samples), 'intermediate': len(intermediates)}
            check(different(start, end), f'{name}: reaches a different view')
            # Blender scales rotation duration by angle; a 15-degree step lasts only 50 ms.
            check(bool(intermediates), f'{name}: animates through intermediate views')
            yield 0.1
            check(not different(state(), end), f'{name}: settles at its destination')
        preferences.view.use_reduce_motion = True
        view.view_location = (5, 7, 2)
        view.view_distance = 30
        start = state()
        command(14)
        yield 0.15
        end = state()
        check(different(start, end), 'Reduce Motion still changes the view')
        yield 0.25
        check(not different(state(), end), 'Reduce Motion disables animation')
        report['passed'] = True
    finally:
        preferences.view.smooth_view = original_duration
        preferences.view.use_reduce_motion = original_reduce_motion


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


bpy.app.timers.register(tick, first_interval=4)
