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


def file_cleanup():
    """Cleans up the file for publish by removing all unnecessary data."""

    # Remove all workspaces except the currently in use one.
    current_ws = bpy.context.window.workspace
    workspaces = [ws for ws in bpy.data.workspaces if ws != current_ws]
    bpy.data.batch_remove(ids=workspaces)

    # Create an empty scene to keep, and delete all the other ones.
    empty_scene = bpy.data.scenes.new("Library")
    bpy.context.window.scene = empty_scene

    scenes = [sc for sc in bpy.data.scenes if sc is not empty_scene]
    bpy.data.batch_remove(ids=scenes)

    # Purge all now-unused data.
    bpy.data.orphans_purge(do_recursive=True)


def create_compositor_node_groups():
    compositor_node_group = bpy.data.node_groups.new("Compositor Nodes", "CompositorNodeTree")

    # Create nodes with an offset for easier inspection of the blend file.
    x = 0
    y = 0

    for node_type_name in COMPOSITOR_NODE_TYPES:
        try:
            new_node = compositor_node_group.nodes.new(type=node_type_name)
            new_node.location = (x, y)
            x += new_node.width + 20
        except Exception:
            # Failing to add a new node means the compositor doesn't support it. This is not an error.
            pass

    for node in compositor_node_group.nodes:
        group = node.group_make()

        # Using node names is more prone to conflicts, for example when a
        # deprecated node has the same name as its replacement, so use #bl_idname
        # as a unique identifier for the node group.
        group.name = node.bl_idname

        group.use_fake_user = True
        group.allow_usage_in_scene_compositor_effect = True

    bpy.data.node_groups.remove(compositor_node_group)


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
    file_cleanup()

    bpy.ops.wm.save_as_mainfile(filepath=output, check_existing=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
