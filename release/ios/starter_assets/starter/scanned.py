# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Materials/Scanned: photo-scanned CC0 surfaces from Poly Haven, with their images packed.

Each material projects its images with Box projection in object space, so it works on meshes
without UVs at real-world size. Relief comes from the height map through a Bump node, which
(unlike a tangent space normal map) is correct with any projection. The tweakable inputs live on
a shader node group in front of the Material Output, so they show in Properties.
"""

import os

import bpy

from .nodes import Tree
from .registry import asset

CATALOG = "Materials/Scanned"
TEXTURES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "sources", "textures")
# How much the box projection blends where faces turn between axes.
PROJECTION_BLEND = 0.25


def _image(name, path, color):
    image = bpy.data.images.load(path, check_existing=False)
    image.name = name
    if not color:
        image.colorspace_settings.name = "Non-Color"
    # Pack the file's bytes under its bare name, so neither the image nor its packed data
    # records where the build found it.
    image.filepath_raw = os.path.basename(path)
    with open(path, "rb") as file:
        data = file.read()
    image.pack(data=data, data_len=len(data))
    return image


def _texture(tree, image, vector):
    return tree.n("ShaderNodeTexImage", image=image, projection="BOX",
                  projection_blend=PROJECTION_BLEND, Vector=vector)


def scanned_material(name, texture_id, size, relief, roughness=1.0, sheen=0.0,
                     color_map="diff", height_map="disp"):
    """A material from the 1K color, roughness and height maps in `sources/textures/<id>/`.

    `size` is the meters one image covers and `relief` the height of its bumps in meters.
    `sheen` is the Principled sheen weight, for cloth. `color_map` and `height_map` are the map
    names in the Poly Haven file names.
    """
    color_file, rough_file, height_file = (
        os.path.join(TEXTURES, texture_id, f"{texture_id}_{map_name}_1k.jpg")
        for map_name in (color_map, "rough", height_map))

    tree = Tree.shader_group(name)
    scale = tree.input("Scale", "FLOAT", 1.0, min=0.01, max=100.0,
                       description="How often the texture repeats; 1 is its real-world size")
    tint = tree.input("Tint", "COLOR", (1.0, 1.0, 1.0, 1.0),
                      description="Multiplied with the scanned color")
    rough = tree.input("Roughness", "FLOAT", roughness, min=0.0, max=2.0,
                       description="Multiplies the scanned roughness")
    bump = tree.input("Bump", "FLOAT", 1.0, min=0.0, max=4.0,
                      description="Strength of the surface relief")

    coordinates = tree.n("ShaderNodeTexCoord").outputs["Object"]
    vector = tree.vmath("SCALE", coordinates, scale=tree.math("DIVIDE", scale, size))
    color = _texture(tree, _image(f"{name} Color", color_file, True), vector)
    color = tree.n("ShaderNodeMix", data_type="RGBA", blend_type="MULTIPLY", Factor=1.0,
                   A=color.outputs["Color"], B=tint)
    roughness_map = _texture(tree, _image(f"{name} Roughness", rough_file, False), vector)
    roughness_value = tree.math("MULTIPLY", roughness_map.outputs["Color"], rough, clamp=True)
    height = _texture(tree, _image(f"{name} Height", height_file, False), vector)
    # Smaller features when the texture repeats more, so the relief keeps its proportions.
    distance = tree.math("DIVIDE", tree.math("MULTIPLY", bump, relief), scale)
    normal = tree.n("ShaderNodeBump", Strength=1.0, Distance=distance,
                    Height=height.outputs["Color"])
    bsdf = tree.n("ShaderNodeBsdfPrincipled", Base_Color=tree.out(color, "Result"),
                  Roughness=roughness_value, Normal=normal.outputs["Normal"],
                  Sheen_Weight=sheen)
    tree.output(bsdf.outputs["BSDF"])
    tree.layout()

    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.node_tree.nodes.clear()
    shader = Tree(material.node_tree, group_io=False)
    group = shader.n("ShaderNodeGroup", node_tree=tree.nt)
    shader.n("ShaderNodeOutputMaterial", Surface=group.outputs[0])
    shader.layout()
    material.diffuse_color = _average_color(bpy.data.images[f"{name} Color"])
    return material


def _average_color(image):
    """The image's mean color, for the material's viewport display color."""
    pixels = image.pixels[:]
    samples = range(0, len(pixels) // 4, 97)
    mean = [sum(pixels[4 * i + c] for i in samples) / len(samples) for c in range(3)]
    # Pixels of an 8-bit image are sRGB; the display color is linear.
    return (*(value ** 2.2 for value in mean), 1.0)


@asset("Red Brick Wall", "material", CATALOG,
       "Weathered red brick in running bond with deep mortar joints, scanned from a real wall.",
       ("brick", "wall", "red", "masonry", "scanned", "exterior"), preview_base="cube")
def red_brick():
    return scanned_material("Red Brick Wall", "red_brick", size=1.4, relief=0.02)


@asset("Cobblestone", "material", CATALOG,
       "Rounded cobblestones with moss in the gaps, for old streets, courtyards and paths.",
       ("cobblestone", "street", "pavement", "stone", "floor", "scanned"), preview_base="cube")
def cobblestone():
    return scanned_material("Cobblestone", "cobblestone_floor_04", size=1.5, relief=0.03)


@asset("Tree Bark", "material", CATALOG,
       "Deeply furrowed brown tree bark with knots, for trunks, branches and logs.",
       ("bark", "tree", "wood", "trunk", "forest", "nature", "scanned"), preview_base="cube")
def tree_bark():
    return scanned_material("Tree Bark", "bark_brown_02", size=1.0, relief=0.03)


@asset("Worn Plaster", "material", CATALOG,
       "Cracked, crumbling plaster that has fallen away to expose old brick underneath.",
       ("plaster", "wall", "worn", "damaged", "brick", "stucco", "weathered", "scanned"),
       preview_base="cube")
def worn_plaster():
    return scanned_material("Worn Plaster", "damaged_plaster", size=1.85, relief=0.02)


@asset("Plaid Fabric", "material", CATALOG,
       "Woven cotton in a soft windowpane plaid, for upholstery, cushions, blankets and clothes.",
       ("fabric", "cloth", "woven", "plaid", "checkered", "cotton", "textile", "scanned"))
def plaid_fabric():
    # No height map for this scan; its ambient occlusion follows the weave just as well.
    return scanned_material("Plaid Fabric", "fabric_pattern_05", size=0.5, relief=0.002,
                            sheen=0.6, color_map="col_01", height_map="ao")


@asset("Rocky Ground", "material", CATALOG,
       "Muddy ground scattered with stones and pebbles, for terrain, trails and riverbanks.",
       ("ground", "rocks", "stones", "dirt", "terrain", "gravel", "nature", "scanned"),
       preview_base="cube")
def rocky_ground():
    return scanned_material("Rocky Ground", "rocks_ground_02", size=2.0, relief=0.04,
                            color_map="col", height_map="height")
