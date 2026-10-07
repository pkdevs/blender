# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
import pathlib
import sys
import tempfile
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
    def create_fixture(self, directory, *, shared_mask, linked, long_name):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.context.scene.frame_end = 2
        # Adding a suffix truncates UTF-8 and collides with an unweighted bone.
        bone_name = "\u00e9" * 30 + "Jaw" if long_name else "Jaw"
        suffix_base = "\u00e9" * 29 if long_name else "Jaw"
        unweighted_bone = suffix_base + ".001"
        other_bone = suffix_base + ".002"
        mesh = bpy.data.meshes.new("Mesh")
        mesh.from_pydata([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)], [], [(0, 1, 2, 3)])
        obj = bpy.data.objects.new("Mesh", mesh)
        bpy.context.collection.objects.link(obj)
        arm = bpy.data.armatures.new("Rig")
        rig = bpy.data.objects.new("Rig", arm)
        bpy.context.collection.objects.link(rig)
        bpy.context.view_layer.objects.active = rig
        rig.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        # The .001 bone has no weights, so import creates no group for it.
        for name in (bone_name, unweighted_bone, other_bone):
            bone = arm.edit_bones.new(name)
            bone.head = (0, 0, 0)
            bone.tail = (0, 0, 1)
        bpy.ops.object.mode_set(mode='OBJECT')
        modifier = obj.modifiers.new("Rig", 'ARMATURE')
        modifier.object = rig
        skin_weights = (1.0, 0.75, 0.25, 0.5)
        group = obj.vertex_groups.new(name=bone_name)
        other = obj.vertex_groups.new(name=other_bone)
        # Confirm that normal group naming would collide with the unweighted bone.
        candidate = obj.vertex_groups.new(name=bone_name)
        self.assertEqual(candidate.name, unweighted_bone)
        obj.vertex_groups.remove(candidate)
        for i, weight in enumerate(skin_weights):
            group.add([i], weight, 'REPLACE')
            other.add([i], 1.0 - weight, 'REPLACE')
        mask_weights = skin_weights if shared_mask else (0.0, 0.25, 0.75, 1.0)
        if not shared_mask:
            group = obj.vertex_groups.new(name="ShapeMask")
            for i, weight in enumerate(mask_weights):
                group.add([i], weight, 'REPLACE')
        obj.shape_key_add(name="Basis")
        key = obj.shape_key_add(name=bone_name)
        key.vertex_group = group.name
        for point in key.data:
            point.co.z += 1
        key.value = 0.0
        key.keyframe_insert("value", frame=1)
        key.value = 0.5
        key.keyframe_insert("value", frame=2)
        bpy.context.scene.frame_set(1)
        if linked:
            instance = obj.copy()
            instance.name = "Linked"
            bpy.context.collection.objects.link(instance)
        shape_path = directory / "shape.fbx"
        skin_path = directory / "skin.fbx"
        export_args = dict(use_mesh_modifiers=False, add_leaf_bones=False,
                           bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False)
        bpy.ops.export_scene.fbx(filepath=str(shape_path), **export_args)
        obj.shape_key_clear()
        bpy.ops.export_scene.fbx(filepath=str(skin_path), **export_args)
        return shape_path, skin_path, bone_name, unweighted_bone, mask_weights

    @staticmethod
    def import_file(path, importer):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        importer(filepath=str(path), anim_offset=0.0)
        return sorted((obj for obj in bpy.context.scene.objects if obj.type == 'MESH'),
                      key=lambda obj: obj.name)

    @staticmethod
    def weights(obj, group):
        return [next((entry.weight for entry in vertex.groups if entry.group == group.index), None)
                for vertex in obj.data.vertices]

    @staticmethod
    def positions(objects):
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        return {obj.name: [tuple(vertex.co) for vertex in obj.evaluated_get(depsgraph).data.vertices]
                for obj in objects}

    @staticmethod
    def move_unweighted_bone(name):
        for obj in bpy.context.scene.objects:
            if obj.type == 'ARMATURE':
                obj.pose.bones[name].location.x = 2

    def test_shape_masks_preserve_skinning(self):
        args.outdir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.outdir) as directory:
            directory = pathlib.Path(directory)
            for shared_mask, linked, long_name in ((False, True, False), (True, False, False),
                                                   (False, False, True)):
                shape_path, skin_path, bone_name, unweighted_bone, mask_weights = self.create_fixture(
                    directory, shared_mask=shared_mask, linked=linked, long_name=long_name)
                for importer in (bpy.ops.wm.fbx_import, bpy.ops.import_scene.fbx):
                    with self.subTest(importer=importer.idname(), shared_mask=shared_mask,
                                      linked=linked, long_name=long_name):
                        objects = self.import_file(skin_path, importer)
                        skin = {obj.name: {group.name: self.weights(obj, group)
                                           for group in obj.vertex_groups} for obj in objects}
                        self.move_unweighted_bone(unweighted_bone)
                        expected_positions = self.positions(objects)
                        objects = self.import_file(shape_path, importer)
                        if linked:
                            self.assertEqual(len(objects), 2)
                            self.assertEqual(objects[0].data, objects[1].data)
                        key = objects[0].data.shape_keys.key_blocks[bone_name]
                        for obj in objects:
                            for name, weights in skin[obj.name].items():
                                self.assertEqual(self.weights(obj, obj.vertex_groups[name]), weights)
                            self.assertEqual(len(obj.vertex_groups), len(skin[obj.name]) + 1)
                            for modifier in obj.modifiers:
                                if modifier.type == 'ARMATURE':
                                    self.assertNotIn(key.vertex_group, modifier.object.data.bones)
                            mask = obj.vertex_groups[key.vertex_group]
                            self.assertEqual([w or 0.0 for w in self.weights(obj, mask)], list(mask_weights))
                        bpy.context.scene.frame_set(2)
                        self.assertAlmostEqual(key.value, 0.5)
                        bpy.context.scene.frame_set(1)
                        key.value = 0.0
                        self.move_unweighted_bone(unweighted_bone)
                        actual_positions = self.positions(objects)
                        for name, vertices in expected_positions.items():
                            for expected, actual in zip(vertices, actual_positions[name], strict=True):
                                for a, b in zip(expected, actual):
                                    self.assertAlmostEqual(a, b, places=6)


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
