# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import unittest

import bpy


class VertexGroupRenameTest(unittest.TestCase):
    """Regression test for vertex group renames updating modifier references."""

    def setUp(self) -> None:
        bpy.ops.wm.read_homefile(use_factory_startup=True)

    def test_all_modifier_references_update_on_vertex_group_rename(self):
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

        for modifiers, new_object_and_data_fn in (
            (mesh_modifiers, self._new_mesh_object_and_data),
            (grease_pencil_modifiers, self._new_grease_pencil_object_and_data),
        ):
            for modifier_type, prop_names in modifiers.items():
                with self.subTest(modifier_type=modifier_type):
                    obj = new_object_and_data_fn()
                    try:
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
                    finally:
                        self._remove_object_and_data(obj)

    def test_vertex_group_rename_does_not_rename_bones(self):
        armature_obj = self._new_armature_object_and_data()
        mesh_obj = self._new_mesh_object_and_data()
        grease_pencil_obj = self._new_grease_pencil_object_and_data()
        try:
            bone_name = armature_obj.data.bones[0].name
            armature_object_name = armature_obj.name
            armature_data_name = armature_obj.data.name

            for obj, modifier_type, prop_name in (
                (mesh_obj, 'ARMATURE', 'vertex_group'),
                (grease_pencil_obj, 'GREASE_PENCIL_ARMATURE', 'vertex_group_name'),
            ):
                with self.subTest(modifier_type=modifier_type):
                    vg = obj.vertex_groups.new(name=bone_name)
                    modifier = obj.modifiers.new(name="Modifier", type=modifier_type)
                    modifier.object = armature_obj
                    modifier.use_bone_envelopes = True
                    setattr(modifier, prop_name, vg.name)

                    vg.name = "RenamedGroup"

                    self.assertEqual(getattr(modifier, prop_name), "RenamedGroup")

            self.assertEqual([bone.name for bone in armature_obj.data.bones], [bone_name])
            self.assertEqual([bone.name for bone in armature_obj.pose.bones], [bone_name])
            self.assertEqual(armature_obj.name, armature_object_name)
            self.assertEqual(armature_obj.data.name, armature_data_name)
        finally:
            self._remove_object_and_data(mesh_obj)
            self._remove_object_and_data(grease_pencil_obj)
            self._remove_object_and_data(armature_obj)

    def test_shape_key_mask_updates_on_vertex_group_rename(self):
        obj = self._new_mesh_object_and_data()
        try:
            vg = obj.vertex_groups.new(name="Group")
            obj.shape_key_add(name="Basis")
            key = obj.shape_key_add(name="Key")
            key.vertex_group = vg.name

            vg.name = "RenamedGroup"

            self.assertEqual(
                key.vertex_group,
                "RenamedGroup",
                "Shape key mask was not updated after rename",
            )
        finally:
            self._remove_object_and_data(obj)

    def test_shared_mesh_references_update_on_vertex_group_rename(self):
        # The mesh owns the vertex groups, so references on both objects must
        # update for deform and non-deform modifiers.
        mesh = bpy.data.meshes.new(name="Mesh")
        obj1 = bpy.data.objects.new(name="Object1", object_data=mesh)
        obj2 = bpy.data.objects.new(name="Object2", object_data=mesh)
        bpy.context.collection.objects.link(obj1)
        bpy.context.collection.objects.link(obj2)
        try:
            for modifier_type in ('ARMATURE', 'HOOK', 'MASK', 'SMOOTH', 'WEIGHTED_NORMAL'):
                with self.subTest(modifier_type=modifier_type):
                    vg = obj1.vertex_groups.new(name=f"Group_{modifier_type}")
                    modifier1 = obj1.modifiers.new(name="Modifier", type=modifier_type)
                    modifier1.vertex_group = vg.name
                    modifier2 = obj2.modifiers.new(name="Modifier", type=modifier_type)
                    modifier2.vertex_group = vg.name

                    vg.name = f"Renamed_{modifier_type}"

                    self.assertEqual(
                        modifier1.vertex_group,
                        f"Renamed_{modifier_type}",
                        "First object's reference was not updated after rename",
                    )
                    self.assertEqual(
                        modifier2.vertex_group,
                        f"Renamed_{modifier_type}",
                        "Second object's reference was not updated after rename",
                    )
        finally:
            self._remove_object_and_data(obj1)
            self._remove_object_and_data(obj2)

    def test_shared_geometry_references_update_on_vertex_group_rename(self):
        mesh = bpy.data.meshes.new(name="Mesh")
        mesh_obj1 = bpy.data.objects.new(name="MeshObject1", object_data=mesh)
        mesh_obj2 = bpy.data.objects.new(name="MeshObject2", object_data=mesh)
        lattice = bpy.data.lattices.new(name="Lattice")
        lattice_obj1 = bpy.data.objects.new(name="LatticeObject1", object_data=lattice)
        lattice_obj2 = bpy.data.objects.new(name="LatticeObject2", object_data=lattice)
        for obj in (mesh_obj1, mesh_obj2, lattice_obj1, lattice_obj2):
            bpy.context.collection.objects.link(obj)
        try:
            mesh_group = mesh_obj1.vertex_groups.new(name="MeshGroup")
            mesh_obj1.shape_key_add(name="Basis")
            mesh_key = mesh_obj1.shape_key_add(name="Key")
            mesh_key.vertex_group = mesh_group.name

            lattice_group = lattice_obj1.vertex_groups.new(name="LatticeGroup")
            lattice.vertex_group = lattice_group.name

            mesh_group.name = "RenamedMeshGroup"
            lattice_group.name = "RenamedLatticeGroup"

            self.assertEqual(
                mesh_obj2.data.shape_keys.key_blocks["Key"].vertex_group,
                "RenamedMeshGroup",
                "Shared shape-key mask was not updated after rename",
            )
            self.assertEqual(
                lattice_obj2.data.vertex_group,
                "RenamedLatticeGroup",
                "Shared lattice reference was not updated after rename",
            )
        finally:
            self._remove_object_and_data(mesh_obj1)
            self._remove_object_and_data(mesh_obj2)
            self._remove_object_and_data(lattice_obj1)
            self._remove_object_and_data(lattice_obj2)

    def test_lattice_influence_group_updates_on_vertex_group_rename(self):
        lattice = bpy.data.lattices.new(name="Lattice")
        obj = bpy.data.objects.new(name="Object", object_data=lattice)
        bpy.context.collection.objects.link(obj)
        try:
            vg = obj.vertex_groups.new(name="Group")
            lattice.vertex_group = vg.name

            vg.name = "RenamedGroup"

            self.assertEqual(
                lattice.vertex_group,
                "RenamedGroup",
                "Lattice influence group was not updated after rename",
            )
        finally:
            self._remove_object_and_data(obj)

    def test_dynamic_paint_weight_output_updates_on_vertex_group_rename(self):
        obj = self._new_mesh_object_and_data()
        try:
            obj.select_set(True)
            bpy.context.view_layer.objects.active = obj
            modifier = obj.modifiers.new(name="Modifier", type='DYNAMIC_PAINT')
            self.assertEqual(bpy.ops.dpaint.type_toggle(type='CANVAS'), {'FINISHED'})

            surface = modifier.canvas_settings.canvas_surfaces.active
            self.assertEqual(surface.surface_format, 'VERTEX')
            surface.surface_type = 'WEIGHT'
            group = obj.vertex_groups.new(name=surface.output_name_a)
            self.assertEqual(surface.output_name_a, group.name)
            self.assertTrue(surface.output_exists(object=obj, index=0))

            group.name = "RenamedGroup"

            self.assertEqual(
                surface.output_name_a,
                "RenamedGroup",
                "Dynamic Paint weight output was not updated after vertex group rename",
            )
            self.assertTrue(surface.output_exists(object=obj, index=0))
        finally:
            self._remove_object_and_data(obj)

    def test_rename_to_existing_name_uniquifies_and_updates_references(self):
        # The name is made unique before references are updated, so the resulting
        # name may differ from the requested one and references must follow the result.
        obj = self._new_mesh_object_and_data()
        try:
            existing = obj.vertex_groups.new(name="Group")
            renamed = obj.vertex_groups.new(name="Target")
            modifier = obj.modifiers.new(name="Modifier", type='MASK')
            modifier.vertex_group = renamed.name

            renamed.name = "Group"

            self.assertNotEqual(renamed.name, "Group", "name should have been made unique")
            self.assertEqual(existing.name, "Group", "the other group should keep its name")
            self.assertEqual(
                modifier.vertex_group,
                renamed.name,
                "reference should follow the resulting name, not the requested one",
            )
        finally:
            self._remove_object_and_data(obj)

    def test_rename_to_other_entity_kind_name_leaves_that_entity_untouched(self):
        # Vertex group names are only unique within their own namespace: colliding with
        # another kind of named entity is allowed and must not affect that entity.
        mesh_obj = self._new_mesh_object_and_data()
        armature_obj = self._new_armature_object_and_data()
        try:
            # A UV map sharing the group's name must not be treated as a reference.
            same_named_uv_layer = mesh_obj.data.uv_layers.new(name="Group")
            # Taking over a name used by another kind of entity is allowed.
            other_uv_layer = mesh_obj.data.uv_layers.new(name="Shared")
            armature_modifier = mesh_obj.modifiers.new(name="ArmatureModifier", type='ARMATURE')
            armature_modifier.object = armature_obj
            armature_modifier.use_bone_envelopes = True
            vg = mesh_obj.vertex_groups.new(name="Group")
            armature_modifier.vertex_group = vg.name

            vg.name = "RenamedGroup"

            self.assertEqual(vg.name, "RenamedGroup")
            self.assertEqual(
                same_named_uv_layer.name,
                "Group",
                "a UV map sharing the old group name should not have been renamed",
            )
            self.assertEqual(armature_modifier.vertex_group, "RenamedGroup")
            self.assertEqual([bone.name for bone in armature_obj.data.bones], ["Group"])

            vg.name = "Shared"

            self.assertEqual(vg.name, "Shared", "colliding with a UV map name is allowed")
            self.assertEqual(other_uv_layer.name, "Shared", "the UV map should keep its name")
            self.assertEqual(armature_modifier.vertex_group, "Shared")
            self.assertEqual([bone.name for bone in armature_obj.data.bones], ["Group"])
            self.assertEqual(armature_obj.name, "Armature")
            self.assertEqual(armature_obj.data.name, "Armature")
        finally:
            self._remove_object_and_data(mesh_obj)
            self._remove_object_and_data(armature_obj)

    @staticmethod
    def _new_armature_object_and_data():
        armature = bpy.data.armatures.new(name="Armature")
        obj = bpy.data.objects.new(name="Armature", object_data=armature)
        bpy.context.collection.objects.link(obj)
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        armature.edit_bones.new(name="Group")
        bpy.ops.object.mode_set(mode='OBJECT')
        return obj

    @staticmethod
    def _new_mesh_object_and_data():
        mesh = bpy.data.meshes.new(name="Mesh")
        obj = bpy.data.objects.new(name="Object", object_data=mesh)
        bpy.context.collection.objects.link(obj)
        return obj

    @staticmethod
    def _new_grease_pencil_object_and_data():
        grease_pencil = bpy.data.grease_pencils.new(name="GreasePencil")
        obj = bpy.data.objects.new(name="Object", object_data=grease_pencil)
        bpy.context.collection.objects.link(obj)
        return obj

    @staticmethod
    def _remove_object_and_data(obj):
        data = obj.data
        bpy.data.objects.remove(obj, do_unlink=True)
        # Remove the data-block as well, to avoid stale data being reused.
        if data is not None and data.users == 0:
            if isinstance(data, bpy.types.Mesh):
                bpy.data.meshes.remove(data)
            elif isinstance(data, bpy.types.Armature):
                bpy.data.armatures.remove(data)
            elif isinstance(data, bpy.types.GreasePencil):
                bpy.data.grease_pencils.remove(data)
            elif isinstance(data, bpy.types.Lattice):
                bpy.data.lattices.remove(data)


def main():
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()


if __name__ == "__main__":
    main()
