#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generates a blend file containing every compositor node, each wrapped in a node group.

This is intended to be run by Blender in background mode at build time::

   blender --background --factory-startup \
       --python release/datafiles/nodes_as_node_groups.py -- \
       --output /path/to/nodes_as_node_groups.blend

For now the exact contents are a placeholder; the file is produced so it can be
shipped with Blender and populated with real node-group assets later.
"""
__all__ = (
    "main",
)

import os
import sys

import bpy

COMPOSITOR_NODE_TYPES = (
    node_type for node_type in dir(bpy.types)
    if node_type.startswith("CompositorNode")
    or node_type.startswith("ShaderNode")
    or node_type.startswith("FunctionNode")
    or node_type.startswith("GeometryNode")
    or node_type.startswith("Node")
    and hasattr(bpy.types, node_type)
)

def create_compositor_node_groups():
    compositor_node_group = bpy.data.node_groups.new("Compositor Nodes", "CompositorNodeTree")
    x = 0
    y = 0

    nb_nodes = 0
    error_nodes = []
    for node_type_name in COMPOSITOR_NODE_TYPES:
        try:
            new_node = compositor_node_group.nodes.new(type=node_type_name)
            new_node.location = (x, y)
            x += new_node.width + 20
            nb_nodes += 1
        except Exception as e:
            error_nodes.append(node_type_name)

    # todo(habib): remove, no need for reporting
    print("Couldn't create", len(error_nodes), "nodes")
    print("Created", nb_nodes, "nodes")

    nb_groups = 0
    for node in compositor_node_group.nodes:
        # debug
        print("treating node", node.name, "...")

        if node.bl_idname == "CompositorNodeGroup":
            continue
        if node.bl_idname == "NodeFrame":
            # todo(habib): fix crash
            continue
        group = node.group_make()
        # debug
        print("Group:\t",group.name, "\tnode", node.bl_idname, "\tAssigning group name...", end="")

        group.name = node.bl_idname

        print("done")
        # end debug

        group.use_fake_user = True
        allow_usage_in_scene_compositor_effect = True
        nb_groups += 1

    bpy.data.node_groups.remove(compositor_node_group)

    print("Created", nb_groups, "nb_groups")



def create_argparse():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        dest="output",
        type=str,
        required=True,
        metavar="FILE",
        help="The blend file to write.",
    )
    return parser


def main() -> int:
    parser = create_argparse()
    if "--" in sys.argv:
        argv = sys.argv[sys.argv.index("--") + 1:]
    else:
        argv = []
    args = parser.parse_args(argv)

    output = os.path.abspath(args.output)
    output_dir = os.path.dirname(output)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    create_compositor_node_groups()

    # todo(habib):  cleanup blend file before saving
    bpy.ops.wm.save_as_mainfile(filepath=output, check_existing=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
