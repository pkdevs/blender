# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Open, edit, save and reopen every seed with a real Blender executable."""
import json
import importlib.util
from pathlib import Path
import tempfile

import bpy

LIBRARY = Path(__file__).resolve().parent / 'library'
spec = importlib.util.spec_from_file_location('native_templates',
    Path(__file__).resolve().parents[3] / 'scripts/startup/bl_ui/native_templates.py')
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)
manifest = json.loads((LIBRARY / 'manifest.json').read_text())
assert manifest['version'] == 1
assert len({entry['id'] for entry in manifest['templates']}) == 5
preferences = (bpy.context.preferences.inputs.use_mouse_emulate_3_button,
               bpy.context.preferences.view.ui_scale)


def validate(entry):
    identifier = entry['id']
    obj = bpy.context.object
    assert not bpy.data.texts
    assert bpy.context.scene['blender_ipad_template'] == identifier
    if identifier == 'sculpt':
        assert bpy.context.mode == 'SCULPT'
        assert 20000 < len(obj.data.vertices) < 50000
        assert obj.data.use_mirror_x and not obj.use_dynamic_topology_sculpting
        assert bpy.context.tool_settings.sculpt.brush is not None
    elif identifier in {'drawing', 'animation-2d'}:
        assert bpy.context.mode == 'PAINT_GREASE_PENCIL'
        assert obj.type == 'GREASEPENCIL' and obj.data.layers.active
        assert obj.data.layers.active.frames and len(obj.data.materials) > 0
        assert bpy.context.tool_settings.gpencil_paint.brush is not None
        assert all(len(frame.drawing.strokes) == 0 for layer in obj.data.layers for frame in layer.frames)
        assert bpy.context.scene.tool_settings.use_keyframe_insert_auto == entry['presentation']['timeline']
    else:
        assert bpy.context.mode == 'OBJECT' and obj.type == 'MESH'
        assert bpy.context.scene.camera is not None
    assert (bpy.context.preferences.inputs.use_mouse_emulate_3_button,
            bpy.context.preferences.view.ui_scale) == preferences


with tempfile.TemporaryDirectory() as directory:
    for entry in manifest['templates']:
        assert (LIBRARY / entry['preview']).stat().st_size > 1000
        bpy.ops.wm.open_mainfile(filepath=str(LIBRARY / entry['file']), use_scripts=False)
        validate(entry)
        assert not preflight.export_error(), preflight.export_error()
        bpy.context.object['template_roundtrip'] = entry['id']
        target = str(Path(directory) / entry['file'])
        bpy.ops.wm.save_as_mainfile(filepath=target, compress=True)
        bpy.ops.wm.open_mainfile(filepath=target, use_scripts=False)
        validate(entry)
        assert bpy.context.object['template_roundtrip'] == entry['id']
        print('PASS:', entry['id'], 'mode, brush, data, preferences, edit and roundtrip')
    text = bpy.data.texts.new('Script')
    assert 'scripts' in preflight.export_error()
    bpy.data.texts.remove(text)
    image = bpy.data.images.new('External', width=8, height=8)
    image.source = 'FILE'
    image.filepath = '/missing/template-texture.png'
    assert 'images' in preflight.export_error()
    bpy.data.images.remove(image)
    image = bpy.data.images.new('Packed', width=8, height=8)
    image.pack()
    assert not preflight.export_error()
    bpy.data.images.remove(image)
    assert not preflight.export_error()
    print('PASS: template export preflight')
print('PASS: iPad template seeds')
