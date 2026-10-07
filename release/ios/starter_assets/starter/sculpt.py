# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Sculpt Bases: plain meshes with clean, even topology to start sculpting from."""

import math

import bmesh
import bpy
from mathutils import Vector

from .common import palette
from .registry import asset

CATALOG = "Objects/Sculpt Bases"


def _mesh_object(name, bm):
    """A plain smooth-shaded mesh object from `bm`, centered on X/Y and resting on Z = 0."""
    low = Vector([min(v.co[i] for v in bm.verts) for i in range(3)])
    high = Vector([max(v.co[i] for v in bm.verts) for i in range(3)])
    offset = Vector((-(low.x + high.x) * 0.5, -(low.y + high.y) * 0.5, -low.z))
    bmesh.ops.translate(bm, verts=bm.verts, vec=offset)
    for face in bm.faces:
        face.smooth = True
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(palette("Starter Clay"))
    return bpy.data.objects.new(name, mesh)


@asset("Sculpt Sphere", "object", CATALOG,
       "A quad sphere with evenly sized faces and no poles, ideal for sculpting and remeshing.",
       ("sphere", "ball", "sculpt", "quad sphere", "base mesh", "clay"))
def sculpt_sphere():
    radius = 0.25
    cuts = 24
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=2.0)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=cuts - 1, use_grid_fill=True)
    # Equal-angle cube to sphere projection keeps the faces close to the same size.
    for vert in bm.verts:
        warped = Vector(math.tan(c * math.pi / 4.0) for c in vert.co)
        vert.co = warped.normalized() * radius
    bm.normal_update()
    return _mesh_object("Sculpt Sphere", bm)


@asset("Suzanne", "object", CATALOG,
       "Blender's monkey head, subdivided and smoothed into a dense mesh ready to sculpt.",
       ("suzanne", "monkey", "head", "sculpt", "base mesh", "blender"))
def suzanne():
    bm = bmesh.new()
    bmesh.ops.create_monkey(bm)
    temporary = bpy.data.meshes.new("Suzanne Base")
    bm.to_mesh(temporary)
    bm.free()
    # Subdivide with the Subdivision Surface modifier, then keep its result as a plain mesh.
    obj = bpy.data.objects.new("Suzanne Base", temporary)
    bpy.context.scene.collection.objects.link(obj)
    modifier = obj.modifiers.new("Subdivision", "SUBSURF")
    modifier.levels = modifier.render_levels = 2
    depsgraph = bpy.context.evaluated_depsgraph_get()
    subdivided = obj.evaluated_get(depsgraph).to_mesh()
    bm = bmesh.new()
    bm.from_mesh(subdivided)
    obj.evaluated_get(depsgraph).to_mesh_clear()
    bpy.data.objects.remove(obj)
    bpy.data.meshes.remove(temporary)
    # The monkey is about 2.7 units wide; make it a head-sized 0.3 m.
    width = max(v.co.x for v in bm.verts) - min(v.co.x for v in bm.verts)
    bmesh.ops.scale(bm, vec=Vector((0.3 / width,) * 3), verts=bm.verts)
    return _mesh_object("Suzanne", bm)
