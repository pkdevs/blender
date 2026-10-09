# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Preflight portable document templates without changing the open scene."""
from pathlib import Path
import json
import bpy


def attribution_json():
    """Carry the bundled character's credit through Save as Template as well as duplication."""
    owners = [bpy.context.scene] + list(bpy.context.scene.objects)
    invalid_credit = False
    for owner in owners:
        if 'asset_attribution' not in owner:
            continue
        text = owner.get('asset_attribution')
        invalid_credit = True
        if not isinstance(text, str):
            continue
        try:
            credit = json.loads(text)
        except (TypeError, ValueError):
            continue
        if isinstance(credit, dict) and all(isinstance(credit.get(key), str) and credit[key]
                for key in ('title', 'author', 'license', 'licenseURL', 'sourceURL', 'changes')):
            return json.dumps(credit, ensure_ascii=True)
    if invalid_credit:
        raise ValueError('Repair the asset attribution before saving this template.')
    return '{}'


def export_error():
    if bpy.data.texts:
        return 'Remove embedded scripts before saving a template.'
    essentials = Path(bpy.utils.system_resource('DATAFILES', path='assets')).resolve()
    for library in bpy.data.libraries:
        if not Path(bpy.path.abspath(library.filepath)).resolve().is_relative_to(essentials):
            return 'Make linked data local before saving a template.'
    for image in bpy.data.images:
        if image.source not in {'GENERATED', 'VIEWER'} and image.filepath and not image.packed_files:
            return 'Pack external images before saving a template.'
    for sound in bpy.data.sounds:
        if sound.filepath and not sound.packed_file:
            return 'Pack external audio before saving a template.'
    for font in bpy.data.fonts:
        if font.filepath not in {'', '<builtin>'} and not font.packed_file:
            return 'Pack external fonts before saving a template.'
    if bpy.data.movieclips or bpy.data.volumes or bpy.data.cache_files:
        return 'Templates cannot yet include external movies, volumes or simulation caches.'
    for scene in bpy.data.scenes:
        if scene.sequence_editor and scene.sequence_editor.strips:
            return 'Templates cannot yet include video sequences.'
    for obj in bpy.data.objects:
        caches = [system.point_cache for system in obj.particle_systems]
        for modifier in obj.modifiers:
            if modifier.type == 'FLUID' and modifier.fluid_type == 'DOMAIN':
                return 'Remove baked fluid domains before saving a template.'
            if hasattr(modifier, 'point_cache'):
                caches.append(modifier.point_cache)
        if any(cache.use_external or cache.use_disk_cache or cache.is_baked for cache in caches):
            return 'Free external simulation caches before saving a template.'
    return ''
