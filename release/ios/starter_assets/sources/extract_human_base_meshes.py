# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Extract the human meshes the Starter library uses from Blender Studio's Human Base Meshes.

    blender -b --factory-startup --python extract_human_base_meshes.py -- \\
        path/to/human_base_meshes_bundle.blend [--output human_base_meshes.blend]

The bundle (v1.4.1, CC0) is about 50 MB; this keeps only the chosen meshes, as plain meshes in
one small file: multires applied at a level that stays under the triangle budget, eyes joined in,
transforms applied, resting on the ground at the origin, and editor-only attributes removed.
"""

import argparse
import os
import sys

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

# Output mesh name: (bundle object, multires level, bundle objects joined in,
# turned upside down, for the hand that hangs fingers down in the bundle).
MESHES = {
    "Human Male Realistic": ("GEO-body_male_realistic", 0,
                             ("GEO-body_male_realistic.eye.L", "GEO-body_male_realistic.eye.R"),
                             False),
    "Human Female Realistic": ("GEO-body_female_realistic", 0,
                               ("GEO-body_female_realistic.eye.L",
                                "GEO-body_female_realistic.eye.R"), False),
    "Human Male Stylized": ("GEO-body_male_stylized", 0,
                            ("GEO-body_male_stylized.eye.L", "GEO-body_male_stylized.eye.R"),
                            False),
    "Human Female Stylized": ("GEO-body_female_stylized", 0,
                              ("GEO-body_female_stylized.eye.L",
                               "GEO-body_female_stylized.eye.R"), False),
    "Human Hand Realistic": ("Hand  - Realistic", 1, (), True),
    "Human Head Realistic": ("GEO-head_sculpting_realistic", 1,
                             ("GEO-head_sculpting_realistic.sclera.L",
                              "GEO-head_sculpting_realistic.sclera.R"), False),
}


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", help="human_base_meshes_bundle.blend from the v1.4.1 zip")
    parser.add_argument("--output", default=os.path.join(HERE, "human_base_meshes.blend"))
    return parser.parse_args(argv)


def evaluated_part(obj, level):
    """Positions (world space), face corners and UVs of the object, multires at `level`."""
    for modifier in obj.modifiers:
        if modifier.type == "MULTIRES":
            modifier.levels = level
        else:
            modifier.show_viewport = False
    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    positions = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    positions = positions.reshape(-1, 3)
    matrix = np.array(evaluated.matrix_world, dtype=np.float32)
    positions = positions @ matrix[:3, :3].T + matrix[:3, 3]
    corners = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corners)
    sizes = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", sizes)
    uvs = np.zeros(len(mesh.loops) * 2, dtype=np.float32)
    if mesh.uv_layers.active:
        mesh.uv_layers.active.data.foreach_get("uv", uvs)
    evaluated.to_mesh_clear()
    return positions, corners, sizes, uvs


def extract(name, source, level, extras, upside_down):
    """A plain mesh (positions, faces, UVs only), so no multires or editor data comes along."""
    parts = [evaluated_part(bpy.data.objects[source], level)]
    parts += [evaluated_part(bpy.data.objects[extra], 0) for extra in extras]
    offsets = np.cumsum([0] + [len(part[0]) for part in parts[:-1]])
    positions = np.concatenate([part[0] for part in parts])
    corners = np.concatenate([part[1] + offset for part, offset in zip(parts, offsets)])
    sizes = np.concatenate([part[2] for part in parts])
    uvs = np.concatenate([part[3] for part in parts])

    if upside_down:
        positions *= np.array([-1.0, 1.0, -1.0], dtype=np.float32)
    low, high = positions.min(axis=0), positions.max(axis=0)
    positions -= np.array([(low[0] + high[0]) * 0.5, (low[1] + high[1]) * 0.5, low[2]],
                          dtype=np.float32)

    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(positions))
    mesh.vertices.foreach_set("co", positions.ravel())
    mesh.loops.add(len(corners))
    mesh.loops.foreach_set("vertex_index", corners)
    mesh.polygons.add(len(sizes))
    mesh.polygons.foreach_set("loop_start", np.concatenate(([0], np.cumsum(sizes)[:-1])))
    mesh.polygons.foreach_set("use_smooth", np.ones(len(sizes), dtype=bool))
    mesh.update(calc_edges=True)
    mesh.uv_layers.new(name="UVMap").data.foreach_set("uv", uvs)
    mesh.validate()
    mesh.calc_loop_triangles()
    print(f"{name:26} {len(mesh.loop_triangles):7d} tris  "
          f"{' x '.join(f'{d:.3f}' for d in high - low)} m")
    return mesh


def main():
    args = parse_args()
    bpy.ops.wm.open_mainfile(filepath=os.path.abspath(args.bundle), load_ui=False)
    meshes = {extract(name, *spec) for name, spec in MESHES.items()}
    output = os.path.abspath(args.output)
    bpy.data.libraries.write(output, meshes, fake_user=True, compress=True, path_remap="NONE")
    print(f"Wrote {len(meshes)} meshes to {output} ({os.path.getsize(output) / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
