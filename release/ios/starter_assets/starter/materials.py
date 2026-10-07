# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Materials: procedural showpiece materials that need no UVs.

Every material wraps its network in a shader node group, so its tweakable inputs show in
Properties. Patterns use Object coordinates, so they scale with the object they're on.
"""

import math

import bpy

from .nodes import Tree
from .registry import asset

HERO = "Materials/Hero"
SCULPT = "Materials/Sculpt"
METALS = "Materials/Metals"
GLASS = "Materials/Glass and Liquid"
SURFACES = "Materials/Surfaces"
LIGHT = "Materials/Light"
NATURE = "Materials/Nature"


# -----------------------------------------------------------------------------
# Helpers.

def _rgba(value):
    return (*value, 1.0) if isinstance(value, tuple) and len(value) == 3 else value


def _node(tree, idname, settings=None, **inputs):
    """Add a node: `settings` sets properties, keywords set inputs by name (spaces as _)."""
    node = tree.nodes.new(idname)
    for key, value in (settings or {}).items():
        setattr(node, key, value)
    for key, value in inputs.items():
        socket = tree._input(node, key)
        tree.set_input(socket, _rgba(value) if socket.type == "RGBA" else value)
    return node


def _color_input(tree, name, color, description=""):
    return tree.input(name, "COLOR", _rgba(color), description=description)


def _factor_input(tree, name, default, description="", min=0.0, max=1.0):
    return tree.input(name, "FLOAT", default, min=min, max=max, subtype="FACTOR",
                      description=description)


def _coords(tree, scale=None):
    """Object coordinates, optionally multiplied by `scale`."""
    vector = tree.n("ShaderNodeTexCoord").outputs["Object"]
    return vector if scale is None else tree.vmath("SCALE", vector, scale=scale)


def _stretch(tree, vector, x, y, z):
    return tree.vmath("MULTIPLY", vector, tree.xyz(x, y, z))


def _noise(tree, vector, scale=1.0, detail=4.0, roughness=0.5, distortion=0.0, **settings):
    node = _node(tree, "ShaderNodeTexNoise", settings, Vector=vector, Scale=scale,
                 Detail=detail, Roughness=roughness, Distortion=distortion)
    return node.outputs["Factor"], node.outputs["Color"]


def _voronoi(tree, vector, scale=1.0, feature="F1", randomness=1.0, **inputs):
    return _node(tree, "ShaderNodeTexVoronoi", {"feature": feature}, Vector=vector,
                 Scale=scale, Randomness=randomness, **inputs)


def _remap(tree, value, from_min, from_max, to_min=0.0, to_max=1.0, smooth=False):
    settings = {"interpolation_type": "SMOOTHSTEP" if smooth else "LINEAR", "clamp": True}
    node = _node(tree, "ShaderNodeMapRange", settings, Value=value, From_Min=from_min,
                 From_Max=from_max, To_Min=to_min, To_Max=to_max)
    return tree.out(node, "Result")


def _mix(tree, factor, a, b):
    node = _node(tree, "ShaderNodeMix", {"data_type": "RGBA", "clamp_factor": True},
                 Factor=factor, A=a, B=b)
    return tree.out(node, "Result")


def _lerp(tree, factor, a, b):
    node = _node(tree, "ShaderNodeMix", {"data_type": "FLOAT", "clamp_factor": True},
                 Factor=factor, A=a, B=b)
    return tree.out(node, "Result")


def _shade(tree, color, value=1.0, saturation=1.0, hue=0.5):
    """Hue/Saturation/Value adjustment of a color."""
    node = _node(tree, "ShaderNodeHueSaturation", Hue=hue, Saturation=saturation,
                 Value=value, Color=color)
    return node.outputs[0]


def _ramp(tree, factor, stops, interpolation="LINEAR"):
    """Color Ramp with `stops` as (position, color) pairs."""
    node = _node(tree, "ShaderNodeValToRGB", Factor=factor)
    ramp = node.color_ramp
    ramp.interpolation = interpolation
    while len(ramp.elements) < len(stops):
        ramp.elements.new(0.5)
    for element, (position, color) in zip(ramp.elements, stops):
        element.position = position
        element.color = _rgba(color)
    return node.outputs["Color"]


def _bump(tree, height, strength=1.0, distance=0.01):
    node = _node(tree, "ShaderNodeBump", Strength=strength, Distance=distance, Height=height)
    return node.outputs["Normal"]


def _facing(tree, blend=0.5):
    """0 where the surface faces the camera, 1 at grazing angles."""
    return _node(tree, "ShaderNodeLayerWeight", Blend=blend).outputs["Facing"]


def _principled(tree, **inputs):
    node = tree.nodes.new("ShaderNodeBsdfPrincipled")
    for key, value in inputs.items():
        socket = node.inputs[key.replace("_", " ")]
        tree.set_input(socket, _rgba(value) if socket.type == "RGBA" else value)
    return node.outputs[0]


def _material(name, tree, color, transmissive=False):
    """A material whose only shader is the group `tree`, so its inputs show in Properties."""
    tree.layout()
    material = bpy.data.materials.new(name)
    wrapper = Tree.for_material(material)
    group = _node(wrapper, "ShaderNodeGroup", {"node_tree": tree.nt})
    _node(wrapper, "ShaderNodeOutputMaterial", Surface=group.outputs[0])
    wrapper.layout()
    material.diffuse_color = _rgba(color)
    if transmissive:
        material.use_raytrace_refraction = True
        material.thickness_mode = "SPHERE"
    return material


def _group(name):
    return Tree.shader_group(name + " Shader")


# -----------------------------------------------------------------------------
# Hero.

@asset("Car Paint", "material", HERO,
       "Glossy metallic car paint with sparkling flakes under a deep clear coat.",
       ("car paint", "metallic", "flakes", "clear coat", "automotive", "glossy"))
def car_paint():
    tree = _group("Car Paint")
    color = _color_input(tree, "Color", (0.4, 0.005, 0.01))
    flake_size = tree.input("Flake Size", "FLOAT", 0.004, min=0.0005, max=0.1,
                            subtype="DISTANCE", description="Size of the metal flakes")
    amount = _factor_input(tree, "Flake Amount", 0.6, "How strongly the flakes sparkle")
    roughness = _factor_input(tree, "Roughness", 0.35, "Roughness of the paint under the coat")

    cells = _voronoi(tree, _coords(tree), tree.math("DIVIDE", 1.0, flake_size))
    tilt = tree.vmath("SUBTRACT", cells.outputs["Color"], (0.5, 0.5, 0.5))
    tilt = tree.vmath("SCALE", tilt, scale=tree.math("MULTIPLY", amount, 1.2))
    normal = tree.n("ShaderNodeNewGeometry").outputs["Normal"]
    normal = tree.vmath("NORMALIZE", tree.vmath("ADD", normal, tilt))

    # Two-tone flop: deeper and richer toward grazing angles.
    deep = _shade(tree, color, value=0.35, saturation=1.2)
    base = _mix(tree, _facing(tree, 0.35), color, deep)
    tree.output(_principled(tree, Base_Color=base, Metallic=0.75, Roughness=roughness,
                            Normal=normal, Coat_Weight=1.0, Coat_Roughness=0.02,
                            Coat_IOR=1.55))
    return _material("Car Paint", tree, (0.4, 0.005, 0.01))


@asset("Holographic", "material", HERO,
       "Iridescent holographic foil, rainbow thin film swirling over dark polished metal.",
       ("holographic", "iridescent", "rainbow", "thin film", "foil", "sci-fi"))
def holographic():
    tree = _group("Holographic")
    thickness = tree.input("Thickness", "FLOAT", 520.0, min=0.0, max=2000.0,
                           description="Thin film thickness in nanometers, sets the colors")
    hue_shift = _factor_input(tree, "Hue Shift", 0.0, "Rotates the rainbow colors")
    scale = tree.input("Scale", "FLOAT", 2.5, min=0.0, description="Size of the swirls")
    roughness = _factor_input(tree, "Roughness", 0.12)

    swirl, _ = _noise(tree, _coords(tree, scale), 1.0, detail=3.0, roughness=0.55,
                      distortion=1.2)
    film = tree.math("MULTIPLY", thickness, tree.math("ADD", 0.55, tree.math("MULTIPLY",
                                                                             swirl, 0.9)))
    film = tree.math("ADD", film, tree.math("MULTIPLY", hue_shift, 400.0))
    hue = tree.math("FRACT", tree.math("ADD", tree.math("ADD", swirl, hue_shift),
                                       tree.math("MULTIPLY", _facing(tree, 0.4), 0.7)))
    tint = _node(tree, "ShaderNodeCombineColor", {"mode": "HSV"}, Red=hue, Green=0.9,
                 Blue=0.35).outputs[0]
    tree.output(_principled(tree, Base_Color=tint, Metallic=1.0, Roughness=roughness,
                            Thin_Film_Thickness=film, Thin_Film_IOR=1.8))
    return _material("Holographic", tree, (0.3, 0.25, 0.4))


@asset("Marble", "material", HERO,
       "Polished white marble with layered dark veins and a hint of translucency.",
       ("marble", "stone", "veins", "polished", "luxury", "countertop"))
def marble():
    tree = _group("Marble")
    color = _color_input(tree, "Color", (0.8, 0.79, 0.76))
    vein_color = _color_input(tree, "Vein Color", (0.05, 0.05, 0.055))
    scale = tree.input("Scale", "FLOAT", 1.5, min=0.0, description="Size of the vein pattern")
    roughness = _factor_input(tree, "Roughness", 0.08)

    coords = _coords(tree, scale)
    warp, _ = _noise(tree, coords, 1.2, detail=8.0, roughness=0.62, distortion=0.3)
    along = tree.vmath("DOT_PRODUCT", coords, (2.0, 0.7, 1.2))
    wave = tree.math("SINE", tree.math("MULTIPLY", tree.math("ADD", along,
                                                             tree.math("MULTIPLY", warp, 4.0)),
                                       math.pi))
    veins = _remap(tree, tree.math("ABSOLUTE", wave), 0.0, 0.12, 1.0, 0.0, smooth=True)
    halo = _remap(tree, tree.math("ABSOLUTE", wave), 0.0, 0.5, 0.45, 0.0, smooth=True)
    fine_warp, _ = _noise(tree, coords, 3.0, detail=8.0, roughness=0.6, distortion=0.6)
    fine = tree.math("SINE", tree.math("MULTIPLY", tree.math("ADD", tree.math(
        "MULTIPLY", along, -1.7), tree.math("MULTIPLY", fine_warp, 6.0)), math.pi))
    fine = _remap(tree, tree.math("ABSOLUTE", fine), 0.0, 0.05, 0.6, 0.0, smooth=True)
    cloud, _ = _noise(tree, coords, 2.0, detail=6.0, roughness=0.6)
    base = _mix(tree, _remap(tree, cloud, 0.35, 0.75, 0.0, 0.25), color,
                _shade(tree, color, value=0.8))
    base = _mix(tree, tree.math("MAXIMUM", tree.math("MAXIMUM", veins, halo), fine), base,
                vein_color)
    tree.output(_principled(tree, Base_Color=base, Roughness=roughness,
                            Subsurface_Weight=0.25, Subsurface_Radius=(1.0, 0.9, 0.8),
                            Subsurface_Scale=0.02, Coat_Weight=0.4, Coat_Roughness=0.03))
    return _material("Marble", tree, (0.8, 0.79, 0.76))


@asset("Toon", "material", HERO,
       "Cel shaded cartoon look with crisp light bands, a highlight and a rim light.",
       ("toon", "cel", "cartoon", "anime", "npr", "stylized"), preview_base="monkey")
def toon():
    tree = _group("Toon")
    color = _color_input(tree, "Color", (0.03, 0.22, 0.85))
    bands = tree.input("Bands", "INT", 3, min=2, max=8, description="Number of light bands")
    rim = _factor_input(tree, "Rim", 0.6, "Strength of the light along the silhouette")
    highlight = _factor_input(tree, "Highlight", 0.8, "Strength of the specular highlight")

    lit = _node(tree, "ShaderNodeBsdfDiffuse", Color=(1.0, 1.0, 1.0))
    light = _node(tree, "ShaderNodeShaderToRGB", Shader=lit.outputs[0]).outputs["Color"]
    light = _node(tree, "ShaderNodeRGBToBW", Color=light).outputs[0]
    # Compress any light intensity into 0..1, then shape it with a key from the upper left of
    # the view, so the bands read well even under flat lighting.
    light = tree.math("DIVIDE", light, tree.math("ADD", light, 0.3))
    normal = _node(tree, "ShaderNodeVectorTransform", {"vector_type": "NORMAL",
                                                       "convert_from": "WORLD",
                                                       "convert_to": "CAMERA"},
                   Vector=tree.n("ShaderNodeNewGeometry").outputs["Normal"]).outputs[0]
    key = tree.vmath("DOT_PRODUCT", tree.vmath("NORMALIZE", normal), (-0.55, 0.6, -0.58))
    key = tree.math("ADD", 0.1, tree.math("MULTIPLY", tree.math("MAXIMUM", key, 0.0), 1.1))
    light = tree.math("MINIMUM", tree.math("MULTIPLY", light, key), 0.999)
    step = tree.math("DIVIDE", tree.math("FLOOR", tree.math("MULTIPLY", light, bands)),
                     tree.math("SUBTRACT", bands, 1.0))
    shadow = _shade(tree, color, value=0.35, saturation=1.2, hue=0.52)
    shaded = _mix(tree, step, shadow, color)

    gloss = _node(tree, "ShaderNodeBsdfGlossy", Color=(1.0, 1.0, 1.0), Roughness=0.15)
    spot = _node(tree, "ShaderNodeShaderToRGB", Shader=gloss.outputs[0]).outputs["Color"]
    spot = _node(tree, "ShaderNodeRGBToBW", Color=spot).outputs[0]
    spot = tree.math("MULTIPLY", tree.math("GREATER_THAN", spot, 7.0), highlight)
    shaded = _mix(tree, spot, shaded, (1.0, 1.0, 1.0))

    edge = tree.math("MULTIPLY", tree.math("GREATER_THAN", _facing(tree, 0.5), 0.8), rim)
    rim_color = _shade(tree, color, value=2.5, saturation=0.5, hue=0.46)
    shaded = _mix(tree, edge, shaded, rim_color)
    tree.output(_node(tree, "ShaderNodeEmission", Color=shaded, Strength=1.0).outputs[0])
    return _material("Toon", tree, (0.03, 0.22, 0.85))


# -----------------------------------------------------------------------------
# Sculpt.

def _clay(name, color, sheen):
    tree = _group(name)
    color_socket = _color_input(tree, "Color", color)
    roughness = _factor_input(tree, "Roughness", 0.6)
    cavity = _factor_input(tree, "Cavity", 0.7, "Darkening in creases and cavities")
    sheen_socket = _factor_input(tree, "Sheen", sheen, "Soft velvety light at grazing angles")

    occlusion = _node(tree, "ShaderNodeAmbientOcclusion", {"samples": 16, "only_local": True},
                      Distance=0.15).outputs["AO"]
    occlusion = _lerp(tree, cavity, 1.0, occlusion)
    crease = _shade(tree, color_socket, value=0.35, saturation=1.15)
    grain, _ = _noise(tree, _coords(tree), 40.0, detail=4.0, roughness=0.6)
    base = _mix(tree, _remap(tree, grain, 0.3, 0.7, 0.0, 0.08), color_socket,
                _shade(tree, color_socket, value=0.85))
    base = _mix(tree, occlusion, crease, base)
    tree.output(_principled(tree, Base_Color=base, Roughness=roughness,
                            Specular_IOR_Level=0.35, Sheen_Weight=sheen_socket,
                            Sheen_Roughness=0.4, Sheen_Tint=(1.0, 0.95, 0.9),
                            Normal=_bump(tree, grain, 0.08, 0.002)))
    return _material(name, tree, color)


@asset("Sculpting Clay Warm", "material", SCULPT,
       "Warm matte modeling clay that shows every form, with darkened creases.",
       ("clay", "sculpt", "matcap", "matte", "warm"), preview_base="monkey")
def sculpting_clay_warm():
    return _clay("Sculpting Clay Warm", (0.46, 0.18, 0.09), 0.3)


@asset("Sculpting Clay Grey", "material", SCULPT,
       "Neutral grey matte clay for judging forms while sculpting, with darkened creases.",
       ("clay", "sculpt", "grey", "matte", "neutral"), preview_base="monkey")
def sculpting_clay_grey():
    return _clay("Sculpting Clay Grey", (0.3, 0.3, 0.31), 0.25)


@asset("Skin", "material", SCULPT,
       "Soft translucent skin with natural tone variation, subtle redness and fine pores.",
       ("skin", "subsurface", "character", "human", "face"), preview_base="monkey")
def skin():
    tree = _group("Skin")
    tone = _factor_input(tree, "Tone", 0.3, "From fair skin to deep brown skin")
    redness = _factor_input(tree, "Redness", 0.5, "Blotches of redness, as on cheeks and lips")
    roughness = _factor_input(tree, "Roughness", 0.45)
    scale = tree.input("Scale", "FLOAT", 1.0, min=0.0,
                       description="Size of the tone variation and pores")

    base = _ramp(tree, tone, ((0.0, (0.75, 0.48, 0.36)), (0.4, (0.5, 0.27, 0.17)),
                              (0.7, (0.24, 0.11, 0.06)), (1.0, (0.08, 0.035, 0.02))))
    coords = _coords(tree, scale)
    blotch, _ = _noise(tree, coords, 3.0, detail=4.0, roughness=0.55)
    blotch = tree.math("MULTIPLY", _remap(tree, blotch, 0.4, 0.75), redness)
    base = _mix(tree, tree.math("MULTIPLY", blotch, 0.6), base,
                _shade(tree, base, value=0.85, saturation=1.6, hue=0.48))
    freckles, _ = _noise(tree, coords, 22.0, detail=2.0, roughness=0.5)
    base = _mix(tree, _remap(tree, freckles, 0.55, 0.75, 0.0, 0.15), base,
                _shade(tree, base, value=0.7, saturation=1.2))
    pores = _voronoi(tree, coords, 180.0).outputs["Distance"]
    fine, _ = _noise(tree, coords, 60.0, detail=6.0, roughness=0.6)
    height = tree.math("ADD", tree.math("MULTIPLY", pores, 0.6), fine)
    shine = tree.math("ADD", roughness, tree.math("MULTIPLY", tree.math("SUBTRACT", fine,
                                                                        0.5), 0.3))
    tree.output(_principled(tree, Base_Color=base, Roughness=shine, Subsurface_Weight=1.0,
                            Subsurface_Radius=(1.0, 0.35, 0.18), Subsurface_Scale=0.04,
                            Specular_IOR_Level=0.45, Coat_Weight=0.08, Coat_Roughness=0.3,
                            Normal=_bump(tree, height, 0.12, 0.002)))
    return _material("Skin", tree, (0.75, 0.5, 0.4))


# -----------------------------------------------------------------------------
# Metals.

def _metal(name, color, roughness, variation=0.5):
    tree = _group(name)
    color_socket = _color_input(tree, "Color", color)
    roughness_socket = _factor_input(tree, "Roughness", roughness)
    variation_socket = _factor_input(tree, "Variation", variation,
                                     "Smudges and uneven polish across the surface")
    scale = tree.input("Scale", "FLOAT", 3.0, min=0.0, description="Size of the smudges")

    coords = _coords(tree, scale)
    smudge, _ = _noise(tree, coords, 1.0, detail=8.0, roughness=0.6, distortion=0.3)
    smudge = _remap(tree, smudge, 0.35, 0.7)
    spread = tree.math("MULTIPLY", variation_socket, 0.18)
    rough = tree.math("ADD", roughness_socket, tree.math("MULTIPLY", tree.math(
        "SUBTRACT", smudge, 0.4), spread), clamp=True)
    tint = _mix(tree, tree.math("MULTIPLY", smudge, tree.math("MULTIPLY", variation_socket,
                                                              0.25)),
                color_socket, _shade(tree, color_socket, value=0.75))
    scratches, _ = _noise(tree, _stretch(tree, coords, 60.0, 4.0, 60.0), 1.0, detail=2.0)
    scratches = tree.math("MULTIPLY", _remap(tree, scratches, 0.62, 0.7), variation_socket)
    tree.output(_principled(tree, Base_Color=tint, Metallic=1.0, Roughness=rough,
                            Normal=_bump(tree, scratches, 0.05, 0.001)))
    return _material(name, tree, color)


@asset("Gold", "material", METALS,
       "Polished yellow gold with subtle smudges and uneven polish.",
       ("gold", "metal", "precious", "jewelry", "shiny"))
def gold():
    return _metal("Gold", (1.0, 0.68, 0.26), 0.22)


@asset("Copper", "material", METALS,
       "Warm polished copper with soft smudges and uneven polish.",
       ("copper", "metal", "warm", "pipes", "shiny"))
def copper():
    return _metal("Copper", (0.95, 0.44, 0.24), 0.25)


@asset("Chrome", "material", METALS,
       "Mirror-like chrome plating with faint smudges.",
       ("chrome", "mirror", "metal", "reflective", "shiny"))
def chrome():
    return _metal("Chrome", (0.86, 0.87, 0.88), 0.03, variation=0.25)


@asset("Brushed Steel", "material", METALS,
       "Brushed stainless steel with anisotropic highlights and fine streaks.",
       ("steel", "brushed", "anisotropic", "metal", "stainless", "appliance"))
def brushed_steel():
    tree = _group("Brushed Steel")
    color = _color_input(tree, "Color", (0.62, 0.63, 0.65))
    roughness = _factor_input(tree, "Roughness", 0.25)
    anisotropy = _factor_input(tree, "Anisotropy", 0.85, "How stretched the highlights are")
    streaks = _factor_input(tree, "Streaks", 0.6, "Strength of the brushing streaks")

    # Brushed around the object's Z axis, so streaks run horizontally.
    tangent = _node(tree, "ShaderNodeTangent", {"direction_type": "RADIAL", "axis": "Z"})
    coords = _coords(tree)
    lines, _ = _noise(tree, _stretch(tree, coords, 8.0, 8.0, 260.0), 1.0, detail=4.0,
                      roughness=0.7)
    bands, _ = _noise(tree, _stretch(tree, coords, 1.5, 1.5, 25.0), 1.0, detail=2.0)
    grain = tree.math("ADD", tree.math("MULTIPLY", lines, 0.7), tree.math("MULTIPLY", bands,
                                                                          0.3))
    grain = tree.math("MULTIPLY", tree.math("SUBTRACT", grain, 0.5), tree.math(
        "MULTIPLY", streaks, 2.0))
    rough = tree.math("ADD", roughness, tree.math("MULTIPLY", grain, 0.3), clamp=True)
    tint = _shade(tree, color, value=tree.math("ADD", 1.0, tree.math("MULTIPLY", grain, 0.9)))
    tree.output(_principled(tree, Base_Color=tint, Metallic=1.0, Roughness=rough,
                            Anisotropic=anisotropy, Tangent=tangent.outputs[0],
                            Normal=_bump(tree, grain, 0.35, 0.001)))
    return _material("Brushed Steel", tree, (0.62, 0.63, 0.65))


@asset("Rust", "material", METALS,
       "Painted metal eaten by rust, with chipped paint edges and bare metal showing through.",
       ("rust", "corrosion", "weathered", "painted metal", "grunge", "worn"))
def rust():
    tree = _group("Rust")
    paint = _color_input(tree, "Paint Color", (0.08, 0.13, 0.09))
    amount = _factor_input(tree, "Rust Amount", 0.7, "How much of the surface is rusted")
    scale = tree.input("Scale", "FLOAT", 2.5, min=0.0, description="Size of the rust patches")

    coords = _coords(tree, scale)
    mask, _ = _noise(tree, coords, 1.0, detail=12.0, roughness=0.68, distortion=0.15)
    threshold = _lerp(tree, amount, 0.78, 0.3)
    rusted = _remap(tree, mask, threshold, tree.math("ADD", threshold, 0.03))
    edge = tree.math("SUBTRACT", threshold, 0.05)
    bare = _remap(tree, mask, edge, tree.math("ADD", edge, 0.01))

    flakes, _ = _noise(tree, coords, 14.0, detail=10.0, roughness=0.7)
    rust_color = _ramp(tree, flakes, ((0.3, (0.08, 0.025, 0.01)), (0.5, (0.32, 0.09, 0.02)),
                                      (0.65, (0.55, 0.22, 0.06)), (0.8, (0.2, 0.07, 0.02))))
    grime, _ = _noise(tree, coords, 5.0, detail=6.0)
    paint = _mix(tree, _remap(tree, grime, 0.4, 0.7, 0.0, 0.35), paint,
                 _shade(tree, paint, value=0.6, saturation=0.8))
    base = _mix(tree, bare, paint, (0.5, 0.49, 0.48))
    base = _mix(tree, rusted, base, rust_color)
    metallic = tree.math("MULTIPLY", bare, tree.math("SUBTRACT", 1.0, rusted))
    rough = _lerp(tree, rusted, _lerp(tree, bare, 0.38, 0.3), 0.9)
    height = tree.math("ADD", tree.math("MULTIPLY", tree.math("SUBTRACT", 1.0, bare), 0.4),
                       tree.math("MULTIPLY", rusted, tree.math("MULTIPLY", flakes, 0.8)))
    tree.output(_principled(tree, Base_Color=base, Metallic=metallic, Roughness=rough,
                            Normal=_bump(tree, height, 0.6, 0.004)))
    return _material("Rust", tree, (0.35, 0.12, 0.04))


# -----------------------------------------------------------------------------
# Glass and liquid.

@asset("Clear Glass", "material", GLASS,
       "Perfectly clear glass with crisp reflections and refraction.",
       ("glass", "clear", "transparent", "refraction", "window"))
def clear_glass():
    tree = _group("Clear Glass")
    color = _color_input(tree, "Color", (0.96, 0.98, 1.0), "Tint of the glass")
    roughness = _factor_input(tree, "Roughness", 0.0)
    ior = tree.input("IOR", "FLOAT", 1.5, min=1.0, max=3.0,
                     description="Index of refraction: 1.5 for glass, 2.4 for diamond")
    tree.output(_principled(tree, Base_Color=color, Roughness=roughness, IOR=ior,
                            Transmission_Weight=1.0))
    return _material("Clear Glass", tree, (0.96, 0.98, 1.0), transmissive=True)


@asset("Frosted Glass", "material", GLASS,
       "Sandblasted frosted glass that blurs what's behind it into a soft glow.",
       ("glass", "frosted", "translucent", "matte", "sandblasted"))
def frosted_glass():
    tree = _group("Frosted Glass")
    color = _color_input(tree, "Color", (0.92, 0.96, 1.0), "Tint of the glass")
    frost = _factor_input(tree, "Frost", 0.35, "How blurry the glass is")
    coords = _coords(tree)
    speckle, _ = _noise(tree, coords, 300.0, detail=2.0)
    patches, _ = _noise(tree, coords, 4.0, detail=4.0)
    rough = tree.math("ADD", frost, tree.math("MULTIPLY", tree.math("SUBTRACT", patches, 0.5),
                                              0.15), clamp=True)
    normal = _bump(tree, speckle, 0.05, 0.001)
    glass = _principled(tree, Base_Color=color, Roughness=rough, IOR=1.5,
                        Transmission_Weight=1.0, Normal=normal)
    # A little translucency gives the soft inner glow of light scattering through the frost.
    glow = _node(tree, "ShaderNodeBsdfTranslucent", Color=color, Normal=normal).outputs[0]
    tree.output(tree.n("ShaderNodeMixShader", tree.math("MULTIPLY", frost, 0.6), glass, glow))
    return _material("Frosted Glass", tree, (0.92, 0.96, 1.0), transmissive=True)


@asset("Ice", "material", GLASS,
       "Cold blue ice with frosted cracks and trapped air bubbles inside.",
       ("ice", "frozen", "cracks", "bubbles", "winter", "cold"))
def ice():
    tree = _group("Ice")
    color = _color_input(tree, "Color", (0.4, 0.72, 1.0))
    cracks = _factor_input(tree, "Cracks", 0.7, "Amount of white cracks")
    bubbles = _factor_input(tree, "Bubbles", 0.5, "Amount of trapped air bubbles")
    scale = tree.input("Scale", "FLOAT", 1.6, min=0.0, description="Size of the cracks")

    # Cracks are sampled at the surface and again deeper along the view, for depth.
    surface = _coords(tree, scale)
    incoming = tree.n("ShaderNodeNewGeometry").outputs["Incoming"]
    deep = tree.vmath("SUBTRACT", surface, tree.vmath("SCALE", incoming, scale=0.6))

    def crack_lines(vector, width):
        warp, warp_color = _noise(tree, vector, 2.0, detail=3.0)
        vector = tree.vmath("ADD", vector, tree.vmath("SCALE", warp_color, scale=0.4))
        distance = _voronoi(tree, vector, 1.5, "DISTANCE_TO_EDGE").outputs["Distance"]
        return _remap(tree, distance, 0.0, width, 1.0, 0.0, smooth=True)

    lines = tree.math("MAXIMUM", crack_lines(surface, 0.012),
                      tree.math("MULTIPLY", crack_lines(deep, 0.03), 0.35))
    lines = tree.math("MULTIPLY", lines, cracks)
    cells = _voronoi(tree, deep, 14.0)
    size = _remap(tree, _node(tree, "ShaderNodeSeparateColor",
                              Color=cells.outputs["Color"]).outputs[0], 0.6, 1.0, 0.0, 0.25)
    pocket = tree.math("LESS_THAN", cells.outputs["Distance"], size)
    pocket = tree.math("MULTIPLY", pocket, bubbles)
    white = tree.math("MAXIMUM", lines, tree.math("MULTIPLY", pocket, 0.8))
    frost, _ = _noise(tree, surface, 6.0, detail=6.0)

    base = _mix(tree, white, color, (0.95, 0.97, 1.0))
    rough = tree.math("ADD", tree.math("MULTIPLY", white, 0.35),
                      _remap(tree, frost, 0.4, 0.7, 0.02, 0.12))
    tree.output(_principled(tree, Base_Color=base, Roughness=rough, IOR=1.31,
                            Transmission_Weight=tree.math("SUBTRACT", 1.0,
                                                          tree.math("MULTIPLY", white, 0.6)),
                            Normal=_bump(tree, frost, 0.1, 0.003)))
    return _material("Ice", tree, (0.4, 0.72, 1.0), transmissive=True)


@asset("Water", "material", GLASS,
       "Clear water with a slight blue tint and gentle ripples on its surface.",
       ("water", "liquid", "ripples", "pool", "ocean", "transparent"))
def water():
    tree = _group("Water")
    color = _color_input(tree, "Color", (0.55, 0.84, 0.9), "Tint of the water")
    ripples = _factor_input(tree, "Ripples", 0.35, "Height of the ripples")
    scale = tree.input("Scale", "FLOAT", 4.0, min=0.0, description="Size of the ripples")

    coords = _coords(tree, scale)
    swell, _ = _noise(tree, coords, 1.0, detail=2.0, roughness=0.4, distortion=0.4)
    chop, _ = _noise(tree, coords, 4.0, detail=3.0, roughness=0.5)
    height = tree.math("ADD", swell, tree.math("MULTIPLY", chop, 0.3))
    tree.output(_principled(tree, Base_Color=color, Roughness=0.01, IOR=1.33,
                            Transmission_Weight=1.0, Normal=_bump(tree, height, ripples, 0.02)))
    return _material("Water", tree, (0.55, 0.84, 0.9), transmissive=True)


# -----------------------------------------------------------------------------
# Surfaces.

@asset("Wood", "material", SURFACES,
       "Oiled hardwood with growth rings on its end grain and long grain on its sides.",
       ("wood", "grain", "rings", "timber", "oak", "furniture"), preview_base="cube")
def wood():
    tree = _group("Wood")
    color = _color_input(tree, "Color", (0.4, 0.19, 0.07))
    ring_color = _color_input(tree, "Ring Color", (0.15, 0.06, 0.02))
    scale = tree.input("Scale", "FLOAT", 1.0, min=0.0, description="Size of the grain")
    roughness = _factor_input(tree, "Roughness", 0.42)

    # Rings run around the Z axis, from a trunk center just outside the object.
    coords = tree.vmath("ADD", _coords(tree, scale), (0.9, 0.55, 0.0))
    warp, warp_color = _noise(tree, _stretch(tree, coords, 1.0, 1.0, 0.25), 1.5, detail=3.0)
    coords = tree.vmath("ADD", coords, tree.vmath("SCALE", tree.vmath(
        "SUBTRACT", warp_color, (0.5, 0.5, 0.5)), scale=0.12))
    rings = _node(tree, "ShaderNodeTexWave", {"wave_type": "RINGS", "rings_direction": "Z",
                                              "wave_profile": "SAW"},
                  Vector=coords, Scale=6.0, Distortion=4.0, Detail=4.0,
                  Detail_Scale=1.0).outputs["Factor"]
    rings = _remap(tree, rings, 0.72, 1.0, smooth=True)
    fibers, _ = _noise(tree, _stretch(tree, coords, 120.0, 120.0, 3.0), 1.0, detail=4.0,
                       roughness=0.6)
    pores = _remap(tree, fibers, 0.55, 0.8, 0.0, 0.35)
    figure, _ = _noise(tree, coords, 3.0, detail=4.0)

    base = _mix(tree, _remap(tree, figure, 0.3, 0.7, 0.0, 0.3), color,
                _shade(tree, color, value=0.8, saturation=1.1))
    base = _mix(tree, tree.math("MULTIPLY", rings, 0.75), base, ring_color)
    base = _mix(tree, pores, base, _shade(tree, ring_color, value=0.6))
    rough = tree.math("ADD", roughness, tree.math("MULTIPLY", pores, 0.2))
    height = tree.math("SUBTRACT", tree.math("MULTIPLY", rings, 0.3), pores)
    tree.output(_principled(tree, Base_Color=base, Roughness=rough, Coat_Weight=0.25,
                            Coat_Roughness=0.15, Normal=_bump(tree, height, 0.25, 0.002)))
    return _material("Wood", tree, (0.4, 0.19, 0.07))


@asset("Concrete", "material", SURFACES,
       "Cast concrete with air pockets, mottled tone and weathered water stains.",
       ("concrete", "cement", "stone", "architecture", "urban", "rough"), preview_base="cube")
def concrete():
    tree = _group("Concrete")
    color = _color_input(tree, "Color", (0.3, 0.29, 0.27))
    stains = _factor_input(tree, "Stains", 0.85, "Dark weathering stains and streaks")
    scale = tree.input("Scale", "FLOAT", 1.0, min=0.0, description="Size of the texture")
    roughness = _factor_input(tree, "Roughness", 0.88)

    coords = _coords(tree, scale)
    mottle, _ = _noise(tree, coords, 6.0, detail=10.0, roughness=0.65)
    base = _mix(tree, _remap(tree, mottle, 0.3, 0.7), _shade(tree, color, value=0.82), color)
    patch, _ = _noise(tree, coords, 1.5, detail=5.0, roughness=0.6, distortion=0.3)
    streak, _ = _noise(tree, _stretch(tree, coords, 6.0, 6.0, 0.4), 1.0, detail=4.0)
    dirt = tree.math("MAXIMUM", _remap(tree, patch, 0.5, 0.75),
                     tree.math("MULTIPLY", _remap(tree, streak, 0.5, 0.7), 0.7))
    base = _mix(tree, tree.math("MULTIPLY", dirt, tree.math("MULTIPLY", stains, 0.7)), base,
                _shade(tree, color, value=0.45, saturation=1.3, hue=0.52))
    cells = _voronoi(tree, coords, 40.0)
    chance = _node(tree, "ShaderNodeSeparateColor", Color=cells.outputs["Color"]).outputs[0]
    pit = tree.math("MULTIPLY", tree.math("GREATER_THAN", chance, 0.75),
                    _remap(tree, cells.outputs["Distance"], 0.12, 0.2, 1.0, 0.0))
    base = _mix(tree, tree.math("MULTIPLY", pit, 0.7), base, _shade(tree, color, value=0.3))
    fine, _ = _noise(tree, coords, 80.0, detail=4.0)
    height = tree.math("SUBTRACT", tree.math("ADD", tree.math("MULTIPLY", fine, 0.6), mottle),
                       tree.math("MULTIPLY", pit, 1.5))
    tree.output(_principled(tree, Base_Color=base, Roughness=roughness,
                            Normal=_bump(tree, height, 0.35, 0.003)))
    return _material("Concrete", tree, (0.3, 0.29, 0.27))


@asset("Ceramic Glaze", "material", SURFACES,
       "Glossy glazed ceramic whose glaze pools darker in hollows, with fine iron speckles.",
       ("ceramic", "glaze", "pottery", "porcelain", "glossy", "kitchen"), preview_base="torus")
def ceramic_glaze():
    tree = _group("Ceramic Glaze")
    color = _color_input(tree, "Color", (0.02, 0.15, 0.25))
    pooling = _factor_input(tree, "Pooling", 0.8, "How much darker the glaze pools in hollows")
    speckles = _factor_input(tree, "Speckles", 0.5, "Amount of dark iron speckles")

    occlusion = _node(tree, "ShaderNodeAmbientOcclusion", {"samples": 16, "only_local": True},
                      Distance=0.25).outputs["AO"]
    pooled = tree.math("MULTIPLY", tree.math("SUBTRACT", 1.0, occlusion), pooling)
    coords = _coords(tree)
    flow, _ = _noise(tree, _stretch(tree, coords, 3.0, 3.0, 0.8), 3.0, detail=6.0,
                     distortion=0.5)
    thin = _remap(tree, flow, 0.45, 0.7, 0.0, 0.5)
    base = _mix(tree, thin, color, _shade(tree, color, value=1.4, saturation=0.8))
    base = _mix(tree, tree.math("ADD", tree.math("MULTIPLY", pooled, 1.6), tree.math(
        "MULTIPLY", tree.math("SUBTRACT", 0.5, thin), 0.3)), base,
        _shade(tree, color, value=0.3, saturation=1.3))
    dots = _voronoi(tree, coords, 30.0)
    chance = _node(tree, "ShaderNodeSeparateColor", Color=dots.outputs["Color"]).outputs[0]
    speck = tree.math("MULTIPLY", tree.math("GREATER_THAN", chance, 0.7),
                      _remap(tree, dots.outputs["Distance"], 0.04, 0.07, 1.0, 0.0))
    base = _mix(tree, tree.math("MULTIPLY", speck, speckles), base, (0.06, 0.04, 0.03))
    tree.output(_principled(tree, Base_Color=base, Roughness=0.2, Coat_Weight=1.0,
                            Coat_Roughness=0.02, Coat_IOR=1.55, Subsurface_Weight=0.1,
                            Subsurface_Scale=0.01))
    return _material("Ceramic Glaze", tree, (0.02, 0.15, 0.25))


@asset("Velvet", "material", SURFACES,
       "Plush velvet with a dark core and a bright soft sheen along its edges.",
       ("velvet", "fabric", "cloth", "sheen", "textile", "plush"))
def velvet():
    tree = _group("Velvet")
    color = _color_input(tree, "Color", (0.3, 0.005, 0.03))
    sheen = _factor_input(tree, "Sheen", 1.0, "Brightness of the soft edge glow")
    crush = _factor_input(tree, "Crush", 0.5, "Pattern of crushed, darker pile")

    coords = _coords(tree)
    pile, _ = _noise(tree, coords, 4.0, detail=5.0, roughness=0.55, distortion=0.6)
    pile = tree.math("MULTIPLY", _remap(tree, pile, 0.35, 0.7), crush)
    core = _mix(tree, pile, _shade(tree, color, value=0.35), _shade(tree, color, value=0.15))
    tint = _shade(tree, color, value=3.0, saturation=0.85)
    # Fibers catch the light at grazing angles: brighter rims around a dark core.
    rim = tree.math("MULTIPLY", tree.math("POWER", _facing(tree, 0.45), 3.0), sheen)
    core = _mix(tree, tree.math("MULTIPLY", rim, 0.8), core, _shade(tree, color, value=1.4))
    fuzz, _ = _noise(tree, coords, 200.0, detail=2.0)
    tree.output(_principled(tree, Base_Color=core, Roughness=0.9, Specular_IOR_Level=0.2,
                            Sheen_Weight=sheen, Sheen_Roughness=0.45, Sheen_Tint=tint,
                            Normal=_bump(tree, fuzz, 0.1, 0.001)))
    return _material("Velvet", tree, (0.3, 0.005, 0.03))


# -----------------------------------------------------------------------------
# Light.

@asset("Neon", "material", LIGHT,
       "Glowing neon tube with a white-hot core, saturated edges and a glassy sheen.",
       ("neon", "emission", "glow", "light", "sign", "bloom"), preview_base="torus")
def neon():
    tree = _group("Neon")
    color = _color_input(tree, "Color", (1.0, 0.0, 0.3))
    strength = tree.input("Strength", "FLOAT", 1.5, min=0.0, max=1000.0,
                          description="Brightness of the glow")
    core = _factor_input(tree, "Core", 0.35, "How white-hot the middle of the tube is")

    center = tree.math("SUBTRACT", 1.0, _facing(tree, 0.5))
    hot = tree.math("MULTIPLY", tree.math("POWER", center, 6.0), core)
    glow = _mix(tree, hot, color, _shade(tree, color, saturation=0.15))
    brightness = tree.math("MULTIPLY", strength, tree.math("ADD", 0.35, tree.math(
        "MULTIPLY", tree.math("POWER", center, 1.5), 0.9)))
    light = _node(tree, "ShaderNodeEmission", Color=glow, Strength=brightness).outputs[0]
    glass = _principled(tree, Base_Color=(0.0, 0.0, 0.0), Roughness=0.05,
                        Specular_IOR_Level=0.1)
    tree.output(tree.n("ShaderNodeAddShader", light, glass).outputs[0])
    return _material("Neon", tree, (1.0, 0.0, 0.3))


# -----------------------------------------------------------------------------
# Nature.

@asset("Snowy Terrain", "material", NATURE,
       "Mountain terrain with snow on high flat ground, grass below and rock on steep slopes.",
       ("terrain", "snow", "mountain", "landscape", "rock", "grass"), preview_base="monkey")
def snowy_terrain():
    tree = _group("Snowy Terrain")
    snow_line = tree.input("Snow Line", "FLOAT", 0.05, subtype="DISTANCE",
                           description="Height above the object's origin where snow begins")
    snow_amount = _factor_input(tree, "Snow Amount", 0.5, "How steep a slope snow covers")
    scale = tree.input("Scale", "FLOAT", 1.0, min=0.0, description="Size of the texture")

    coords = _coords(tree, scale)
    _, _, height = tree.separate(_coords(tree))
    _, _, up = tree.separate(tree.n("ShaderNodeNewGeometry").outputs["Normal"])
    broken, _ = _noise(tree, coords, 4.0, detail=6.0, roughness=0.6)
    wobble = tree.math("SUBTRACT", broken, 0.5)

    rock_detail, _ = _noise(tree, coords, 12.0, detail=10.0, roughness=0.7, distortion=0.2)
    strata, _ = _noise(tree, _stretch(tree, coords, 1.0, 1.0, 12.0), 2.0, detail=4.0)
    rock = _ramp(tree, tree.math("ADD", tree.math("MULTIPLY", rock_detail, 0.7),
                                 tree.math("MULTIPLY", strata, 0.3)),
                 ((0.3, (0.04, 0.035, 0.03)), (0.55, (0.13, 0.115, 0.095)),
                  (0.75, (0.27, 0.24, 0.2))))
    grass_detail, _ = _noise(tree, coords, 20.0, detail=6.0)
    grass = _ramp(tree, grass_detail, ((0.35, (0.03, 0.08, 0.012)), (0.6, (0.1, 0.18, 0.025)),
                                       (0.8, (0.22, 0.25, 0.05))))

    slope = tree.math("ADD", up, tree.math("MULTIPLY", wobble, 0.3))
    flat = _remap(tree, slope, 0.4, 0.55)
    snow_start = tree.math("SUBTRACT", 1.0, tree.math("MULTIPLY", snow_amount, 0.7))
    snow_slope = _remap(tree, slope, snow_start, tree.math("ADD", snow_start, 0.08))
    above = _remap(tree, tree.math("ADD", tree.math("SUBTRACT", height, snow_line),
                                   tree.math("MULTIPLY", wobble, 0.2)), 0.0, 0.05)
    snow = tree.math("MULTIPLY", snow_slope, above)

    base = _mix(tree, flat, rock, grass)
    base = _mix(tree, snow, base, (0.85, 0.88, 0.93))
    rough = _lerp(tree, snow, _lerp(tree, flat, 0.85, 0.75), 0.55)
    bump_height = tree.math("MULTIPLY", rock_detail, tree.math("SUBTRACT", 1.0, tree.math(
        "MULTIPLY", snow, 0.85)))
    tree.output(_principled(tree, Base_Color=base, Roughness=rough,
                            Subsurface_Weight=tree.math("MULTIPLY", snow, 0.3),
                            Subsurface_Scale=0.02,
                            Normal=_bump(tree, bump_height, 0.6, 0.01)))
    return _material("Snowy Terrain", tree, (0.5, 0.52, 0.5))
