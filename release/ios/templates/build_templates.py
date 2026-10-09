# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Generate portable iPad document seeds using Blender 5.1, never user preferences.

blender --background --factory-startup --python build_templates.py
"""
import argparse
import json
from pathlib import Path
import sys

import bpy
from mathutils import Quaternion

OUTPUT = Path(__file__).resolve().parent / 'library'
TEMPLATES = (
    ('general', 'General', '', False),
    ('sculpt', 'Sculpt', 'Sculpting', False),
    ('drawing', 'Grease Pencil Drawing', '2D_Animation', False),
    ('animation-2d', '2D Animation', '2D_Animation', True),
    ('animation-3d', '3D Animation', '', True),
)


def local_brushes():
    # A seed must not depend on the build machine's Essentials library paths.
    linked = [item for item in bpy.data.user_map() if item.library]
    for item in linked:
        local = item.copy()
        local.name = item.name
        item.user_remap(local)
    bpy.data.batch_remove(ids=linked)
    bpy.data.orphans_purge(do_recursive=True)
    assert not any(item.library for item in bpy.data.user_map())
    for library in list(bpy.data.libraries):
        bpy.data.libraries.remove(library)


def build(identifier, title, base, timeline):
    bpy.ops.wm.read_factory_settings(app_template=base)
    scene = bpy.context.scene
    if identifier == 'sculpt':
        # Keep Blender's sculpt-ready quad sphere, symmetry and remesh defaults.
        obj = bpy.context.object
        obj.name = 'Sculpt'
        bpy.ops.brush.asset_activate(asset_library_type='ESSENTIALS',
            relative_asset_identifier='brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw')
        sculpt_space = next(area.spaces.active for area in bpy.context.screen.areas
                            if area.type == 'VIEW_3D')
        sculpt_matcap = sculpt_space.shading.studio_light
        assert sculpt_space.shading.light == 'MATCAP'
    if base == '2D_Animation':
        bpy.context.object.name = 'Drawing'
        scene.tool_settings.use_keyframe_insert_auto = timeline
        scene.frame_end = 120 if timeline else 250
        scene.render.fps = 24
        for screen in bpy.data.screens:
            for area in screen.areas:
                if area.type == 'VIEW_3D':
                    space = area.spaces.active
                    if not timeline:
                        space.region_3d.view_perspective = 'ORTHO'
                        space.region_3d.view_rotation = Quaternion((1, 0, 0), 1.57079632679)
                        space.region_3d.view_location = (0, 0, 0)
                        space.region_3d.view_distance = 10
    if identifier == 'animation-3d':
        scene.frame_end = 120
        scene.render.fps = 24
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D' and identifier == 'sculpt':
                space = area.spaces.active
                space.shading.type = 'SOLID'
                space.shading.light = 'MATCAP'
                space.shading.studio_light = sculpt_matcap
                space.shading.show_shadows = False
                space.shading.show_cavity = False
                space.overlay.show_floor = False
                space.overlay.show_axis_x = False
                space.overlay.show_axis_y = False
    local_brushes()
    assert not bpy.data.libraries, 'Template has external library dependencies'
    assert not bpy.data.texts, 'Template must not contain embedded scripts'
    scene['blender_ipad_template'] = identifier
    bpy.ops.file.pack_all()
    # Save before temporary preview camera and render settings are introduced.
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT / f'{identifier}.blend'), compress=True)
    original_mode = bpy.context.object.mode if bpy.context.object else 'OBJECT'
    if original_mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    if not scene.camera:
        data = bpy.data.cameras.new('Preview')
        camera = bpy.data.objects.new('Preview', data)
        scene.collection.objects.link(camera)
        camera.location = (4, -6, 4)
        camera.rotation_euler = (-camera.location).to_track_quat('-Z', 'Y').to_euler()
        camera.data.type = 'ORTHO'
        camera.data.ortho_scale = max(bpy.context.object.dimensions) * 1.85 if identifier == 'sculpt' else 3.2
        scene.camera = camera
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'STUDIO'
    if identifier == 'sculpt':
        scene.display.shading.light = 'MATCAP'
        scene.display.shading.studio_light = sculpt_matcap
        scene.display.shading.show_shadows = False
        scene.display.shading.show_cavity = False
    scene.display.shading.color_type = 'MATERIAL'
    scene.display.shading.background_type = 'WORLD'
    scene.world.color = (1, 1, 1) if base == '2D_Animation' else (0.16, 0.16, 0.16)
    scene.render.resolution_x = 640
    scene.render.resolution_y = 420
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.filepath = str(OUTPUT / f'{identifier}.png')
    bpy.ops.render.render(write_still=True)
    return dict(id=identifier, title=title, file=f'{identifier}.blend',
                preview=f'{identifier}.png', minimumBlender=[5, 1, 0],
                presentation=dict(timeline=timeline))


OUTPUT.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--template', choices=[template[0] for template in TEMPLATES])
arguments = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
if arguments.template:
    build(*next(template for template in TEMPLATES if template[0] == arguments.template))
else:
    entries = [build(*template) for template in TEMPLATES]
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from build_character import build as build_character
    entries.append(build_character())
    (OUTPUT / 'manifest.json').write_text(json.dumps(dict(version=1, templates=entries), indent=2) + '\n')
