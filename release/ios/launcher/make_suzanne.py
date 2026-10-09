# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Write the Suzanne shown on the iPad's document launcher, as USDZ for RealityKit.

    blender --background --factory-startup --python release/ios/launcher/make_suzanne.py

Suzanne is smoothed and coloured Blender orange, and written Y-up, as RealityKit expects.
"""

from pathlib import Path
import bpy


def linear(channel):
    channel /= 255
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_monkey_add(size=1.6)
suzanne = bpy.context.active_object
subdivision = suzanne.modifiers.new("Subdivision", 'SUBSURF')
subdivision.levels = subdivision.render_levels = 2
bpy.ops.object.shade_smooth()

material = bpy.data.materials.new("Suzanne")
material.use_nodes = True
shader = material.node_tree.nodes["Principled BSDF"]
# Blender's brand orange, #E87D0D.
shader.inputs["Base Color"].default_value = (linear(0xE8), linear(0x7D), linear(0x0D), 1.0)
shader.inputs["Roughness"].default_value = 0.32
suzanne.data.materials.append(material)

bpy.ops.wm.usd_export(
    filepath=str(Path(__file__).with_name("Suzanne.usdz")),
    export_materials=True,
    export_subdivision='TESSELLATE',
    convert_orientation=True,
    export_global_forward_selection='NEGATIVE_Z',
    export_global_up_selection='Y',
)
