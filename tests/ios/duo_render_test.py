# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run in a disposable, factory-startup simulator app, never against a user's scene."""

import bpy
import json
import traceback
from pathlib import Path


output = Path.home() / 'Documents/duo-render-test.json'
report = {'status': 'running', 'completed': [], 'stage': 'startup', 'steps': 0}
modes = iter(['SOLID', 'MATERIAL', 'RENDERED', 'WIREFRAME', 'SOLID'])
mode = None
step = 0


def save():
    output.write_text(json.dumps(report, indent=2))


def tick():
    global mode, step
    try:
        window = bpy.context.window_manager.windows[0]
        area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
        region = next(r for r in area.regions if r.type == 'WINDOW')
        if mode is None or step == 30:
            if mode is not None:
                report['completed'].append(mode)
            mode = next(modes, None)
            if mode is None:
                # Leave EEVEE visible for screenshot and Metal-log verification.
                area.spaces.active.shading.type = 'RENDERED'
                area.tag_redraw()
                report['status'] = 'passed'
                save()
                return None
            report['stage'] = mode
            step = 0
            area.spaces.active.shading.type = mode
            area.tag_redraw()
            save()
            return 3.0
        with bpy.context.temp_override(window=window, area=area, region=region):
            assert bpy.ops.view3d.view_orbit(
                type=('ORBITLEFT', 'ORBITUP', 'ORBITRIGHT', 'ORBITDOWN')[step % 4],
            ) == {'FINISHED'}
            assert bpy.ops.view3d.view_pan(
                'INVOKE_DEFAULT', type='PANLEFT' if step % 2 else 'PANRIGHT',
            ) == {'FINISHED'}
            assert bpy.ops.view3d.zoom(delta=1 if step % 2 else -1) == {'FINISHED'}
        area.tag_redraw()
        step += 1
        report['steps'] += 1
        save()
        return 0.15
    except Exception:
        report['status'] = 'failed'
        report['error'] = traceback.format_exc()
        save()
        traceback.print_exc()
        return None


bpy.context.preferences.view.smooth_view = 0
save()
bpy.app.timers.register(tick, first_interval=3.0)
