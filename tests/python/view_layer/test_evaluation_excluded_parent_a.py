# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Regression test for missing HIERARCHY relations when a non-excluded layer
# collection lives under an excluded ancestor.
#
# Because LAYER_COLLECTION_EXCLUDE is not inherited, the node builder keeps
# recursing through an excluded ancestor and builds the non-excluded
# descendant collection. Before the fix the relation builder stopped at the
# excluded ancestor, so that descendant collection got a node but no incoming
# HIERARCHY relation. The resulting node/relation asymmetry is what
# do_sanity_checks() reports, and the missing ordering edge let parallel
# object evaluation race in Mesh::bounds_min_max().
#
# do_sanity_checks() only runs in debug builds and the crash itself is a
# non-deterministic race, so neither is usable as a CI assertion. Instead this
# test inspects the built relation graph directly (deterministic, release-safe)
# and asserts the re-activated collection has a HIERARCHY edge coming from an
# ancestor outside its own datablock (the nearest non-excluded ancestor, or
# the scene). Without the fix that edge is absent.

import re
import unittest

from view_layer_common import (
    ViewLayerTesting,
    setup_extra_arguments,
)


def _find_layer_collection(layer_collection, target_collection):
    if layer_collection.collection == target_collection:
        return layer_collection
    for child in layer_collection.children:
        found = _find_layer_collection(child, target_collection)
        if found is not None:
            return found
    return None


def _parse_relations_graphviz(graphviz):
    """Parse depsgraph.debug_relations_graphviz() output.

    Returns (node_owner, edges) where node_owner maps a node id to
    (id_ref, operation_label) and edges is a list of (src_id, dst_id).
    Nodes are numeric ids; the enclosing ID datablock is the nearest
    `ID_REF : <name>` cluster label on the brace stack.
    """
    node_owner = {}
    edges = []
    idref_stack = []  # (brace_depth, id_ref)
    depth = 0
    for line in graphviz.splitlines():
        stripped = line.strip()

        idref = re.search(r'label="ID_REF : (\S+)', stripped)
        if idref is not None:
            idref_stack.append((depth, idref.group(1)))

        node = re.match(r'"(\d+)"\s*\[.*label="([^"]*)"', stripped)
        if node is not None:
            owner = idref_stack[-1][1] if idref_stack else None
            node_owner[node.group(1)] = (owner, node.group(2))

        edge = re.search(r'"(\d+)"\s*->\s*"(\d+)"', stripped)
        if edge is not None:
            edges.append((edge.group(1), edge.group(2)))

        depth += stripped.count("{")
        for _ in range(stripped.count("}")):
            depth -= 1
            while idref_stack and idref_stack[-1][0] > depth:
                idref_stack.pop()

    return node_owner, edges


def _external_hierarchy_in_edges(depsgraph, collection):
    """Count HIERARCHY edges entering `collection` from another datablock."""
    node_owner, edges = _parse_relations_graphviz(
        depsgraph.debug_relations_graphviz())
    owner_name = "GR" + collection.name

    hierarchy_nodes = {
        node for node, (owner, label) in node_owner.items()
        if owner == owner_name and label == "HIERARCHY()"
    }

    count = 0
    for src, dst in edges:
        if dst not in hierarchy_nodes:
            continue
        src_owner, src_label = node_owner.get(src, (None, None))
        if src_label == "HIERARCHY()" and src_owner != owner_name:
            count += 1
    return count


class UnitTesting(ViewLayerTesting):
    def test_excluded_parent_re_activated_child(self):
        import bpy

        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        for col in list(bpy.data.collections):
            bpy.data.collections.remove(col, do_unlink=True)
        for mesh in list(bpy.data.meshes):
            bpy.data.meshes.remove(mesh, do_unlink=True)

        scene = bpy.context.scene
        view_layer = bpy.context.view_layer

        parent_coll = bpy.data.collections.new("RegressionParent")
        child_coll = bpy.data.collections.new("RegressionChild")
        scene.collection.children.link(parent_coll)
        parent_coll.children.link(child_coll)

        parent_coll.objects.link(
            bpy.data.objects.new('parent_obj', bpy.data.meshes.new('parent_mesh')))
        child_coll.objects.link(
            bpy.data.objects.new('child_obj_a', bpy.data.meshes.new('child_mesh_a')))
        child_coll.objects.link(
            bpy.data.objects.new('child_obj_b', bpy.data.meshes.new('child_mesh_b')))
        view_layer.update()

        lc_parent = _find_layer_collection(view_layer.layer_collection, parent_coll)
        lc_child = _find_layer_collection(view_layer.layer_collection, child_coll)
        self.assertIsNotNone(lc_parent)
        self.assertIsNotNone(lc_child)

        # Exclude both, then re-activate only the child: it is now a
        # non-excluded collection under an excluded ancestor.
        lc_parent.exclude = True
        lc_child.exclude = True
        view_layer.update()

        lc_child.exclude = False
        view_layer.update()

        depsgraph = bpy.context.evaluated_depsgraph_get()

        # The fix restores the hierarchy edge from the nearest non-excluded
        # ancestor (here the scene, since the parent stays excluded) to the
        # re-activated child collection. Without the fix the child collection
        # node has no incoming HIERARCHY relation.
        self.assertGreaterEqual(
            _external_hierarchy_in_edges(depsgraph, child_coll), 1,
            "Re-activated collection under an excluded ancestor is missing its "
            "incoming HIERARCHY relation")

        # The excluded ancestor itself must not be built as a relation source.
        self.assertEqual(
            _external_hierarchy_in_edges(depsgraph, parent_coll), 0)

        # Sanity: object membership still follows exclude non-inheritance.
        evaluated_names = {ob.name for ob in depsgraph.objects}
        self.assertIn('child_obj_a', evaluated_names)
        self.assertIn('child_obj_b', evaluated_names)
        self.assertNotIn('parent_obj', evaluated_names)


if __name__ == '__main__':
    UnitTesting._extra_arguments = setup_extra_arguments(__file__)
    unittest.main()
