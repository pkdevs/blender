# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
import pathlib
import sys
import unittest

import bpy

sys.path.append(str(pathlib.Path(__file__).parent.absolute()))

args = None


def do_fbx_import(filepath, params):
    # old Python based importer
    # bpy.ops.import_scene.fbx(filepath=filepath, use_subsurf=True, use_custom_props=True, **params)
    # new C++/ufbx based importer
    bpy.ops.wm.fbx_import(filepath=filepath, import_subdivision=True, use_custom_props=True, **params)


class FBXImportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.testdir = args.testdir
        cls.output_dir = args.outdir

    def test_import_fbx(self):
        input_files = sorted(pathlib.Path(self.testdir).glob("*.fbx"))
        self.passed_tests = []
        self.failed_tests = []
        self.updated_tests = []

        from modules import io_report
        report = io_report.Report("FBX Import", self.output_dir, self.testdir, self.testdir.joinpath("reference"))

        for input_file in input_files:
            with self.subTest(pathlib.Path(input_file).stem):
                bpy.ops.wm.open_mainfile(filepath=str(self.testdir / "../empty.blend"))
                ok = report.import_and_check(input_file, lambda filepath, params: do_fbx_import(filepath, params))
                if not ok:
                    self.fail(f"{input_file.stem} import result does not match expectations")

        report.finish("io_fbx")


class FBXShapeMaskTest(unittest.TestCase):
    @staticmethod
    def positions(objects):
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        return {obj.name: [tuple(vertex.co) for vertex in obj.evaluated_get(depsgraph).data.vertices]
                for obj in objects}

    def test_shape_masks_preserve_skinning(self):
        # Linked quads have one distinct mask and one sharing a skin group in the source.
        # Suffixing the 63-byte name must truncate a whole é and avoid an unweighted bone.
        suffix_base = "Mâchoire_très_expressive_paddingpaddingpaddingpadding_l_"
        bone_name = suffix_base + "été"
        for importer in (bpy.ops.wm.fbx_import, bpy.ops.import_scene.fbx):
            with self.subTest(importer=importer.idname()):
                bpy.ops.wm.read_factory_settings(use_empty=True)
                importer(filepath=str(args.testdir / "issue151861_shape_mask_collision.fbx"), anim_offset=0.0)
                objects = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']
                self.assertEqual(len(objects), 2)
                self.assertEqual(objects[0].data, objects[1].data)
                obj = objects[0]
                bones = bpy.data.objects["Rig"].data.bones
                keys = obj.data.shape_keys.key_blocks
                expected = {
                    bone_name: [1.0, 0.75, 0.25, 0.5],
                    suffix_base + ".002": [None, 0.25, 0.75, 0.5],
                    keys[bone_name].vertex_group: [0.0, 0.25, 0.75, 1.0],
                    keys[suffix_base + ".002"].vertex_group: [0.0, 0.25, 0.75, 0.5],
                }
                for key in list(keys)[1:]:
                    self.assertNotIn(key.vertex_group, bones)
                self.assertEqual(len(obj.vertex_groups), 4)
                actual = {group.name: [next((entry.weight for entry in vertex.groups
                                            if entry.group == group.index), None)
                                      for vertex in obj.data.vertices] for group in obj.vertex_groups}
                self.assertEqual(actual, expected)
                bpy.context.scene.frame_set(2)
                for key in list(keys)[1:]:
                    self.assertAlmostEqual(key.value, 0.5)
                bpy.context.scene.frame_set(1)
                before = self.positions(objects)
                bpy.data.objects["Rig"].pose.bones[suffix_base + ".001"].location.x = 2
                after = self.positions(objects)
                for name, vertices in before.items():
                    for a, b in zip(vertices, after[name], strict=True):
                        for x, y in zip(a, b, strict=True):
                            self.assertAlmostEqual(x, y, places=6)


def main():
    global args
    import argparse

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    parser.add_argument('--outdir', required=True, type=pathlib.Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
