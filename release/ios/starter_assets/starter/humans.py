# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Human Bases and a sculpt head: plain meshes from Blender Studio's Human Base Meshes.

The meshes come from `sources/human_base_meshes.blend`, extracted from the CC0 bundle by
`sources/extract_human_base_meshes.py`. They're plain meshes, made to sculpt or model from.
"""

import os

import bpy
from mathutils import Matrix, Vector

from .common import palette
from .registry import asset

CATALOG = "Objects/Human Bases"
SOURCE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "sources", "human_base_meshes.blend")


def human(name, mesh_name):
    """An object with a mesh appended from the source file, resting on the ground in clay."""
    with bpy.data.libraries.load(SOURCE, link=False) as (source, target):
        if mesh_name not in source.meshes:
            raise KeyError(f"{mesh_name!r} is not in {SOURCE}")
        target.meshes = [mesh_name]
    mesh = target.meshes[0]
    mesh.name = name
    # Centered on X/Y with the lowest point at Z = 0, whatever the source file holds.
    points = [vertex.co for vertex in mesh.vertices]
    low = Vector(min(point[i] for point in points) for i in range(3))
    high = Vector(max(point[i] for point in points) for i in range(3))
    mesh.transform(Matrix.Translation((-(low.x + high.x) * 0.5, -(low.y + high.y) * 0.5, -low.z)))
    mesh.shade_smooth()
    mesh.materials.clear()
    mesh.materials.append(palette("Starter Clay"))
    return bpy.data.objects.new(name, mesh)


@asset("Human Male", "object", CATALOG,
       "A realistic adult male body in an A-pose with clean quad topology, ready to sculpt.",
       ("human", "body", "male", "man", "realistic", "base mesh", "character", "sculpt"))
def human_male():
    return human("Human Male", "Human Male Realistic")


@asset("Human Female", "object", CATALOG,
       "A realistic adult female body in an A-pose with clean quad topology, ready to sculpt.",
       ("human", "body", "female", "woman", "realistic", "base mesh", "character", "sculpt"))
def human_female():
    return human("Human Female", "Human Female Realistic")


@asset("Stylized Male", "object", CATALOG,
       "A stylized male body with simplified forms, a base for cartoon and game characters.",
       ("human", "body", "male", "stylized", "cartoon", "base mesh", "character"))
def stylized_male():
    return human("Stylized Male", "Human Male Stylized")


@asset("Stylized Female", "object", CATALOG,
       "A stylized female body with simplified forms, a base for cartoon and game characters.",
       ("human", "body", "female", "stylized", "cartoon", "base mesh", "character"))
def stylized_female():
    return human("Stylized Female", "Human Female Stylized")


@asset("Human Hand", "object", CATALOG,
       "A realistic human hand with relaxed fingers, for sculpting hands and anatomy studies.",
       ("hand", "human", "fingers", "anatomy", "realistic", "base mesh", "sculpt"))
def human_hand():
    return human("Human Hand", "Human Hand Realistic")


@asset("Sculpt Head", "object", "Objects/Sculpt Bases",
       "A neutral realistic human head and neck with eyes, the starting point for a bust.",
       ("head", "bust", "face", "human", "portrait", "sculpt", "base mesh"))
def sculpt_head():
    return human("Sculpt Head", "Human Head Realistic")
