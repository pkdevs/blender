# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Character Animation through the real native document lifecycle on a test iPad app."""
import ctypes
import json
from pathlib import Path
import time
import traceback

import bpy
from bl_ui import native_templates

probe = ctypes.CDLL(str(Path(bpy.app.binary_path).parent / 'Frameworks/TemplatesDeviceProbe.dylib'))
query = probe.blender_device_probe_templates
query.argtypes = [ctypes.c_char_p]
query.restype = ctypes.c_void_p
free = ctypes.CDLL('/usr/lib/libSystem.B.dylib').free
free.argtypes = [ctypes.c_void_p]
report = {'checks': []}
output = Path.home() / 'Documents/character-template-test.json'


def ui(**request):
    ptr = query(json.dumps(request).encode())
    try:
        return json.loads(ctypes.string_at(ptr))
    finally:
        free(ptr)


def check(condition, title):
    assert condition, title
    report['checks'].append(title)
    output.write_text(json.dumps(report, indent=2))


def opened(identifier):
    for _ in range(150):
        yield 0.2
        state = ui()
        if not state['opening'] and not state['creating'] and bpy.context.scene.get('blender_ipad_template') == identifier:
            return
    raise AssertionError('Timed out opening ' + identifier + ': ' + json.dumps(ui()))


def tests():
    yield 4
    check(any(e['id'] == 'character-animation' for e in ui()['entries']), 'Character Animation is bundled')
    yield from opened('general')
    previous = ui()['document']
    ui(action='new')
    yield 0.8
    check(ui()['choosing'], 'The native chooser opens')
    ui(action='choose', id='character-animation')
    yield from opened('character-animation')
    yield 2
    check(ui()['document'] != previous, 'The chooser creates an independent character document')
    check(bpy.context.mode == 'POSE' and ui()['toolbar'].get('objectMode') == 'POSE',
          'Blender and native controls start in Pose Mode')
    check(ui()['timeline'], 'The animation timeline is visible')
    check(not bpy.data.texts and not bpy.data.libraries, 'No embedded scripts or linked dependencies')
    credit = json.loads(native_templates.attribution_json())
    check(credit['license'] == 'CC-BY-4.0' and credit['author'] and credit['changes'],
          'The document retains author, license, source and modification metadata')
    rig = bpy.context.object
    check(bpy.ops.pose.native_character_pose(action='face_angry') == {'FINISHED'}, 'Native facial pose operator works')
    check(bpy.ops.pose.native_character_pose(action='face_default') == {'FINISHED'}, 'Neutral face restores')
    for value in (0, 1):
        check(bpy.ops.pose.native_character_switch(bone='PRP-Thigh.L', prop='ik_left_leg', value=value) == {'FINISHED'},
              'Native leg IK/FK switch works: ' + str(value))
    hand = rig.pose.bones['IK-MSTR-Wrist.L']
    for bone in rig.pose.bones:
        bone.select = False
    hand.select = True
    rig.data.bones.active = hand.bone
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for frame in (1, 24):
        bpy.context.scene.frame_set(frame)
        if frame == 24:
            hand.location.x += 0.1
        with bpy.context.temp_override(area=area, region=region):
            check(bpy.ops.anim.keyframe_insert() == {'FINISHED'}, 'Pose keyframe inserted at frame ' + str(frame))
    start = time.perf_counter()
    for frame in range(1, 49):
        bpy.context.scene.frame_set(frame)
    report['evaluation_ms_per_frame'] = (time.perf_counter() - start) / 48 * 1000
    bpy.context.scene.frame_set(1)
    path = bpy.data.filepath
    ui(action='save')
    for _ in range(150):
        yield 0.2
        if ui()['result'] != 'pending':
            break
    check(ui()['result'] == 'ok', 'Save as Template completes: ' + ui()['result'])
    check(bpy.data.filepath == path, 'Template export leaves the live document open')
    entry = next(e for e in ui()['entries'] if e['personal'] and e['title'] == 'Device Test Template')
    manifest = json.loads((Path(entry['path']).parent / 'manifest.json').read_text())
    saved = next(e for e in manifest['templates'] if e['id'] == entry['id'])
    check(saved['attribution'] == credit, 'Save as Template preserves the full CC-BY attribution in the chooser')
    bpy.ops.wm.open_mainfile(filepath=entry['path'], use_scripts=False)
    check(bpy.context.mode == 'POSE' and bpy.context.object.animation_data.action,
          'Saved template restores Pose Mode and animation')
    check(json.loads(native_templates.attribution_json()) == credit, 'Saved template retains document credits')
    ui(action='new')
    yield 0.8
    check(ui()['choosing'], 'The chooser reopens with the exported template')
    report['passed'] = True


steps = tests()


def tick():
    try:
        return next(steps)
    except StopIteration:
        output.write_text(json.dumps(report, indent=2))
    except Exception:
        report['error'] = traceback.format_exc()
        output.write_text(json.dumps(report, indent=2))
    return None


output.write_text(json.dumps({'stage': 'bootstrapping'}))
ui(action='bootstrap')
bpy.app.timers.register(tick, first_interval=1, persistent=True)
