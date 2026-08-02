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

    def test_modifier_reference_updates_on_vertex_group_rename(self):
        bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
        obj = bpy.context.active_object

        vg = obj.vertex_groups.new(name="Group")
        bpy.ops.object.modifier_add(type='ARMATURE')
        modifier = obj.modifiers[-1]
        modifier.vertex_group = vg.name

        vg.name = "RenamedGroup"

        self.assertEqual(modifier.vertex_group, "RenamedGroup")
        self.assertIn("RenamedGroup", obj.vertex_groups)


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
