# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import pathlib
import sys
import unittest

import bpy


class VertexGroupRenameTest(unittest.TestCase):
    """Regression test for vertex group renames updating modifier references."""

    def setUp(self) -> None:
        bpy.ops.wm.read_homefile(use_factory_startup=True)

    def test_all_modifier_references_update_on_vertex_group_rename(self):
        # Modifiers that store vertex group names, mapping the modifier type to
        # the RNA properties (on the modifier) holding those names.
        mesh_modifiers = {
            'ARMATURE': ('vertex_group',),
            'BEVEL': ('vertex_group',),
            'CAST': ('vertex_group',),
            'CORRECTIVE_SMOOTH': ('vertex_group',),
            'CURVE': ('vertex_group',),
            'DATA_TRANSFER': ('vertex_group',),
            'DECIMATE': ('vertex_group',),
            'DISPLACE': ('vertex_group',),
            'HOOK': ('vertex_group',),
            'LAPLACIANDEFORM': ('vertex_group',),
            'LAPLACIANSMOOTH': ('vertex_group',),
            'LATTICE': ('vertex_group',),
            'MASK': ('vertex_group',),
            'MESH_CACHE': ('vertex_group',),
            'MESH_DEFORM': ('vertex_group',),
            'NORMAL_EDIT': ('vertex_group',),
            'SHRINKWRAP': ('vertex_group',),
            'SIMPLE_DEFORM': ('vertex_group',),
            'SMOOTH': ('vertex_group',),
            'SOLIDIFY': ('vertex_group', 'shell_vertex_group', 'rim_vertex_group'),
            'SURFACE_DEFORM': ('vertex_group',),
            'UV_WARP': ('vertex_group',),
            'WARP': ('vertex_group',),
            'WAVE': ('vertex_group',),
            'VERTEX_WEIGHT_EDIT': ('vertex_group', 'mask_vertex_group'),
            'VERTEX_WEIGHT_MIX': ('vertex_group_a', 'vertex_group_b', 'mask_vertex_group'),
            'VERTEX_WEIGHT_PROXIMITY': ('vertex_group', 'mask_vertex_group'),
            'WEIGHTED_NORMAL': ('vertex_group',),
            'WELD': ('vertex_group',),
            'WIREFRAME': ('vertex_group',),
        }

        grease_pencil_modifiers = {
            'GREASE_PENCIL_VERTEX_WEIGHT_ANGLE': ('target_vertex_group', 'vertex_group_name'),
            'GREASE_PENCIL_VERTEX_WEIGHT_PROXIMITY': ('target_vertex_group', 'vertex_group_name'),
            'GREASE_PENCIL_BUILD': ('target_vertex_group',),
            'GREASE_PENCIL_OFFSET': ('vertex_group_name',),
            'GREASE_PENCIL_OPACITY': ('vertex_group_name',),
            'GREASE_PENCIL_TINT': ('vertex_group_name',),
            'GREASE_PENCIL_SMOOTH': ('vertex_group_name',),
            'GREASE_PENCIL_NOISE': ('vertex_group_name',),
            'GREASE_PENCIL_THICKNESS': ('vertex_group_name',),
            'GREASE_PENCIL_LATTICE': ('vertex_group_name',),
            'GREASE_PENCIL_HOOK': ('vertex_group_name',),
            'GREASE_PENCIL_ARMATURE': ('vertex_group_name',),
            'GREASE_PENCIL_SIMPLIFY': ('vertex_group_name',),
            'GREASE_PENCIL_ENVELOPE': ('vertex_group_name',),
            'GREASE_PENCIL_SHRINKWRAP': ('vertex_group_name',),
            'LINEART': ('vertex_group',),
        }

        for modifiers, object_data_factory in (
            (mesh_modifiers, self._new_mesh_object),
            (grease_pencil_modifiers, self._new_grease_pencil_object),
        ):
            for modifier_type, prop_names in modifiers.items():
                with self.subTest(modifier_type=modifier_type):
                    obj = object_data_factory()

                    vg = obj.vertex_groups.new(name="Group")
                    modifier = obj.modifiers.new(name="Modifier", type=modifier_type)
                    for prop_name in prop_names:
                        setattr(modifier, prop_name, vg.name)

                    vg.name = "RenamedGroup"

                    for prop_name in prop_names:
                        self.assertEqual(
                            getattr(modifier, prop_name),
                            "RenamedGroup",
                            f"Property '{prop_name}' was not updated after rename",
                        )

    @staticmethod
    def _new_mesh_object():
        mesh = bpy.data.meshes.new(name="Mesh")
        obj = bpy.data.objects.new(name="Object", object_data=mesh)
        bpy.context.collection.objects.link(obj)
        return obj

    @staticmethod
    def _new_grease_pencil_object():
        grease_pencil = bpy.data.grease_pencils.new(name="GreasePencil")
        obj = bpy.data.objects.new(name="Object", object_data=grease_pencil)
        bpy.context.collection.objects.link(obj)
        return obj


def main():
    global args

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', type=pathlib.Path, default=None)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
