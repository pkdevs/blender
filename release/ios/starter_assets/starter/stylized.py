# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Stylized: playful props with bold shapes and colors."""

import math

import bpy

from .common import RADIANS, generator_object, object_group, palette, rest_on_ground, \
    simple_material
from .registry import asset

CATALOG = "Objects/Stylized"


# -----------------------------------------------------------------------------
# Small node helpers.

def _position(tree):
    return tree.n("GeometryNodeInputPosition").outputs[0]


def _normal(tree):
    return tree.n("GeometryNodeInputNormal").outputs[0]


def _random(tree, low, high, seed, offset=0):
    """A random float per element, decorrelated from other calls by `offset`."""
    if offset:
        seed = tree.math("ADD", seed, offset)
    node = tree.n("FunctionNodeRandomValue", data_type="FLOAT", Min=low, Max=high, Seed=seed)
    return tree.out(node, "Value")


def _noise(tree, vector, scale, w=0.0, detail=2.0):
    node = tree.n("ShaderNodeTexNoise", noise_dimensions="4D", Vector=vector, W=w, Scale=scale,
                  Detail=detail)
    return tree.out(node, "Factor")


def _switch(tree, condition, off, on, input_type="GEOMETRY"):
    node = tree.n("GeometryNodeSwitch", input_type=input_type, Switch=condition)
    tree.set_input(node.inputs[1], off)
    tree.set_input(node.inputs[2], on)
    return node.outputs[0]


def _euler(tree, x=0.0, y=0.0, z=0.0):
    return tree.n("FunctionNodeEulerToRotation", Euler=tree.xyz(x, y, z)).outputs[0]


def _smoothstep(tree, value, low, high):
    node = tree.n("ShaderNodeMapRange", interpolation_type="SMOOTHSTEP", Value=value,
                  From_Min=low, From_Max=high, To_Min=0.0, To_Max=1.0)
    return tree.out(node, "Result")


def _named(tree, name):
    node = tree.n("GeometryNodeInputNamedAttribute", data_type="FLOAT", Name=name)
    return tree.out(node, "Attribute")


def _store(tree, geometry, name, value, domain="POINT"):
    return tree.n("GeometryNodeStoreNamedAttribute", data_type="FLOAT", domain=domain,
                  Geometry=geometry, Name=name, Value=value).outputs[0]


# -----------------------------------------------------------------------------
# Crystal Cluster.

def _crystal_material():
    return simple_material("Crystal Amethyst", (0.5, 0.16, 0.95), roughness=0.06,
                           Transmission_Weight=0.4, IOR=1.55,
                           Emission_Color=(0.45, 0.1, 0.9), Emission_Strength=0.25)


def _unit_crystal(tree):
    """A hexagonal prism with a pointed tip, one unit wide and tall, base at Z = 0."""
    cone = tree.n("GeometryNodeMeshCone", Vertices=6, Side_Segments=4, Radius_Top=0.0,
                  Radius_Bottom=1.0, Depth=1.0).outputs["Mesh"]
    # The cone's rings shrink linearly; widen all but the tip ring back to full width.
    x, y, z = tree.separate(_position(tree))
    widen = tree.math("MINIMUM", tree.math("DIVIDE", 1.0, tree.math(
        "MAXIMUM", tree.math("SUBTRACT", 1.0, z), 0.001)), 4.0)
    prism = tree.xyz(tree.math("MULTIPLY", x, widen), tree.math("MULTIPLY", y, widen), z)
    return tree.n("GeometryNodeSetPosition", Geometry=cone, Position=prism).outputs[0]


@asset("Crystal Cluster", "object", CATALOG,
       "A cluster of faceted glowing crystals growing out of a rocky base.",
       ("crystal", "gem", "mineral", "fantasy", "rock", "stylized"))
def crystal_cluster():
    tree = object_group("Crystal Cluster")
    count = tree.input("Count", "INT", 11, min=1, max=60, description="Number of crystals")
    height = tree.input("Height", "FLOAT", 0.3, min=0.01, subtype="DISTANCE",
                        description="Height of the tallest, central crystal")
    spread = tree.input("Spread", "FLOAT", 0.12, min=0.0, subtype="DISTANCE",
                        description="Radius of the area the crystals grow from")
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL", _crystal_material())
    rock_material = tree.input("Rock Material", "MATERIAL", palette("Starter Stone"))

    # Rocky base: a lumpy low-poly dome, flattened underneath.
    rock_radius = tree.math("ADD", tree.math("MULTIPLY", spread, 0.9),
                            tree.math("MULTIPLY", height, 0.1))
    rock_height = tree.math("MULTIPLY", rock_radius, 0.45)
    rock = tree.n("GeometryNodeMeshIcoSphere", Radius=1.0, Subdivisions=2).outputs["Mesh"]
    position = _position(tree)
    lumps = tree.math("ADD", 0.75, tree.math("MULTIPLY", _noise(tree, position, 1.2, seed), 0.55))
    rx, ry, rz = tree.separate(tree.vmath("SCALE", position, scale=lumps))
    rz = tree.math("MAXIMUM", rz, -0.35)
    rock_shape = tree.xyz(tree.math("MULTIPLY", rx, rock_radius),
                          tree.math("MULTIPLY", ry, rock_radius),
                          tree.math("MULTIPLY", rz, rock_height))
    rock = tree.n("GeometryNodeSetPosition", Geometry=rock, Position=rock_shape).outputs[0]
    rock = tree.smooth(rock, smooth=False)
    rock = tree.material(rock, rock_material)

    # Crystal roots: the first in the middle, the rest scattered on the dome.
    index = tree.n("GeometryNodeInputIndex").outputs[0]
    outer = tree.math("GREATER_THAN", index, 0.5)
    angle = _random(tree, 0.0, 2.0 * math.pi, seed, 1)
    distance = tree.math("SQRT", _random(tree, 0.08, 1.0, seed, 2))
    distance = tree.math("MULTIPLY", distance, outer)
    radial = tree.math("MULTIPLY", distance, tree.math("MULTIPLY", spread, 0.85))
    px = tree.math("MULTIPLY", radial, tree.math("COSINE", angle))
    py = tree.math("MULTIPLY", radial, tree.math("SINE", angle))
    pz = tree.math("MULTIPLY", rock_height,
                   tree.math("SUBTRACT", 0.55, tree.math("MULTIPLY", distance, 0.6)))
    points = tree.n("GeometryNodePoints", Count=count, Position=tree.xyz(px, py, pz)).outputs[0]

    # Lean outward with distance from the middle; outer crystals are shorter.
    lean = tree.math("MULTIPLY", distance, 0.6)
    jx = tree.math("MULTIPLY", _random(tree, -0.25, 0.25, seed, 3), outer)
    jy = tree.math("MULTIPLY", _random(tree, -0.25, 0.25, seed, 4), outer)
    direction = tree.xyz(tree.math("ADD", tree.math("MULTIPLY", tree.math("COSINE", angle), lean),
                                   jx),
                         tree.math("ADD", tree.math("MULTIPLY", tree.math("SINE", angle), lean),
                                   jy), 1.0)
    rotation = tree.n("FunctionNodeAlignRotationToVector", axis="Z", Vector=direction).outputs[0]
    spin = _euler(tree, z=_random(tree, 0.0, 2.0 * math.pi, seed, 5))
    rotation = tree.n("FunctionNodeRotateRotation", rotation_space="LOCAL", Rotation=rotation,
                      Rotate_By=spin).outputs[0]
    size = tree.math("SUBTRACT", 1.0, tree.math("MULTIPLY", distance, 0.35))
    size = tree.math("MULTIPLY", size, _random(tree, 0.6, 0.95, seed, 6))
    size = tree.math("MAXIMUM", size, tree.math("MULTIPLY", tree.math("SUBTRACT", 1.0, outer),
                                                 1.0))
    crystal_height = tree.math("MULTIPLY", height, size)
    width = tree.math("MULTIPLY", crystal_height, _random(tree, 0.12, 0.16, seed, 7))
    crystals = tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=_unit_crystal(tree),
                      Rotation=rotation,
                      Scale=tree.xyz(width, width, crystal_height)).outputs[0]
    crystals = tree.realize(crystals)
    crystals = tree.smooth(crystals, smooth=False)
    crystals = tree.material(crystals, material)

    tree.output(rest_on_ground(tree, tree.join(rock, crystals)))
    return generator_object("Crystal Cluster", tree)


# -----------------------------------------------------------------------------
# Gem.

def _gem_material():
    return simple_material("Gem Ruby", (0.7, 0.0, 0.04), roughness=0.04, IOR=2.4,
                           Specular_Tint=(1.0, 0.12, 0.2), Transmission_Weight=0.15,
                           Emission_Color=(0.6, 0.0, 0.03), Emission_Strength=0.2)


def _ring(tree, curve, scale, z, angle=0.0):
    if not isinstance(scale, tuple):
        scale = (scale, scale, 1.0)
    rotation = _euler(tree, z=angle) if not isinstance(angle, float) else (0.0, 0.0, angle)
    return tree.transform(curve, translation=(0.0, 0.0, z), rotation=rotation, scale=scale)


def _point(tree, z):
    return tree.n("GeometryNodePoints", Count=1, Position=(0.0, 0.0, z)).outputs[0]


def _hull(tree, *parts):
    return tree.n("GeometryNodeConvexHull", Geometry=tree.join(*parts)).outputs[0]


def _brilliant(tree, facets):
    """Round brilliant: a table, star and girdle rings and a deep pavilion, radius 1."""
    half = tree.math("MAXIMUM", tree.math("FLOOR", tree.math("MULTIPLY", facets, 0.5)), 3.0)
    girdle = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=tree.math(
        "MULTIPLY", half, 2.0), Radius=1.0).outputs["Curve"]
    star = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=half,
                  Radius=1.0).outputs["Curve"]
    offset = tree.math("DIVIDE", math.pi, half)
    return _hull(tree,
                 _ring(tree, star, 0.56, 0.38),
                 _ring(tree, star, 0.8, 0.24, offset),
                 _ring(tree, girdle, 1.0, 0.025),
                 _ring(tree, girdle, 1.0, -0.025),
                 _ring(tree, star, 0.56, -0.46, offset),
                 _point(tree, -0.86))


def _emerald(tree):
    """Step cut: nested octagons, rectangular with clipped corners, 2 units long."""
    rectangle = tree.n("GeometryNodeCurvePrimitiveQuadrilateral", Width=2.0,
                       Height=1.45).outputs[0]
    octagon = tree.n("GeometryNodeFilletCurve", Curve=rectangle, Radius=0.28, Count=1,
                     Limit_Radius=True, mode="POLY").outputs[0]
    return _hull(tree,
                 _ring(tree, octagon, 0.62, 0.3),
                 _ring(tree, octagon, 0.82, 0.21),
                 _ring(tree, octagon, 1.0, 0.025),
                 _ring(tree, octagon, 1.0, -0.025),
                 _ring(tree, octagon, 0.78, -0.24),
                 _ring(tree, octagon, 0.5, -0.46),
                 _ring(tree, octagon, (0.42, 0.05, 1.0), -0.62))


def _princess(tree):
    """Square princess cut with a crisp crown and a chevron pavilion, 1.8 units wide."""
    square = tree.n("GeometryNodeCurvePrimitiveQuadrilateral", Width=1.8,
                    Height=1.8).outputs[0]
    return _hull(tree,
                 _ring(tree, square, 0.64, 0.3),
                 _ring(tree, square, 0.84, 0.19),
                 _ring(tree, square, 1.0, 0.025),
                 _ring(tree, square, 1.0, -0.025),
                 _ring(tree, square, 0.72, -0.4),
                 _ring(tree, square, 0.5, -0.5, 45 * RADIANS),
                 _point(tree, -0.92))


@asset("Gem", "object", CATALOG,
       "A large sparkling gemstone in brilliant, emerald or princess cut, standing on its point.",
       ("gem", "jewel", "diamond", "ruby", "treasure", "stylized"))
def gem():
    tree = object_group("Gem")
    cut = tree.menu("Cut", ["Brilliant", "Emerald", "Princess"],
                    description="Shape the stone is cut into")
    facets = tree.input("Facets", "INT", 16, min=6, max=64,
                        description="Facets around the girdle of the round brilliant cut")
    size = tree.input("Size", "FLOAT", 0.08, min=0.001, subtype="DISTANCE",
                      description="Width of the stone at its girdle")
    material = tree.input("Material", "MATERIAL", _gem_material())

    stone = tree.switch(cut, "GEOMETRY", {"Brilliant": _brilliant(tree, facets),
                                          "Emerald": _emerald(tree),
                                          "Princess": _princess(tree)})
    half = tree.math("MULTIPLY", size, 0.5)
    stone = tree.transform(stone, scale=tree.xyz(half, half, half))
    stone = tree.smooth(stone, smooth=False)
    stone = tree.material(stone, material)
    tree.output(rest_on_ground(tree, stone))
    return generator_object("Gem", tree)


# -----------------------------------------------------------------------------
# Donut.

def _sprinkles_material():
    """Sprinkle colors picked per sprinkle from a random value stored on the geometry."""
    material = bpy.data.materials.get("Donut Sprinkles")
    if material:
        return material
    material = simple_material("Donut Sprinkles", (0.9, 0.9, 0.9), roughness=0.35)
    nodes = material.node_tree.nodes
    attribute = nodes.new("ShaderNodeAttribute")
    attribute.attribute_type = "GEOMETRY"
    attribute.attribute_name = "sprinkle"
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.interpolation = "CONSTANT"
    colors = ((0.95, 0.93, 0.88), (0.9, 0.08, 0.1), (0.05, 0.35, 0.9), (0.98, 0.7, 0.05),
              (0.15, 0.7, 0.2), (0.6, 0.15, 0.85))
    elements = ramp.color_ramp.elements
    while len(elements) < len(colors):
        elements.new(0.5)
    for i, (element, color) in enumerate(zip(elements, colors)):
        element.position = i / len(colors)
        element.color = (*color, 1.0)
    links = material.node_tree.links
    links.new(attribute.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], nodes["Principled BSDF"].inputs["Base Color"])
    attribute.location = (-600, 300)
    ramp.location = (-350, 300)
    return material


# Bite: overlapping spheres on the rim, as (angle around the donut, offset along the rim).
BITE_RADIUS = 0.0125
BITES = ((-20.0, -0.011), (-20.0, 0.0), (-20.0, 0.011))


def _bite_centers(major, minor):
    centers = []
    for angle, along in BITES:
        a = angle * RADIANS
        reach = major + minor + 0.0035
        if along:
            reach -= 0.004
        centers.append((reach * math.cos(a) - along * math.sin(a),
                        reach * math.sin(a) + along * math.cos(a), 0.003))
    return centers


@asset("Donut", "object", CATALOG,
       "The classic donut with wavy icing, colorful sprinkles and an optional bite.",
       ("donut", "doughnut", "food", "sweet", "dessert", "stylized"), preview_view="top")
def donut():
    major, minor = 0.032, 0.0165
    tree = object_group("Donut")
    icing_on = tree.input("Icing", "BOOL", True)
    sprinkles_on = tree.input("Sprinkles", "BOOL", True, description="Sprinkles on the icing")
    count = tree.input("Sprinkle Count", "INT", 140, min=0, max=600)
    bite_on = tree.input("Bite", "BOOL", True, description="Take a bite out of the donut")
    seed = tree.input("Seed", "INT", 0)
    dough_material = tree.input("Dough Material", "MATERIAL", simple_material(
        "Donut Dough", (0.55, 0.27, 0.09), roughness=0.55, Subsurface_Weight=0.1))
    icing_material = tree.input("Icing Material", "MATERIAL", simple_material(
        "Donut Icing", (0.92, 0.2, 0.45), roughness=0.22, Subsurface_Weight=0.1))
    sprinkles_material = tree.input("Sprinkles Material", "MATERIAL", _sprinkles_material())

    ring = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=64,
                  Radius=major).outputs["Curve"]
    profile = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=32,
                     Radius=minor).outputs["Curve"]
    dough = tree.n("GeometryNodeCurveToMesh", Curve=ring, Profile_Curve=profile).outputs[0]
    dough = tree.transform(dough, scale=(1.0, 1.0, 0.82))
    lumps = tree.math("MULTIPLY", tree.math("SUBTRACT", _noise(tree, _position(tree), 60.0,
                                                                seed), 0.5), 0.003)
    dough = tree.n("GeometryNodeSetPosition", Geometry=dough,
                   Offset=tree.vmath("SCALE", _normal(tree), scale=lumps)).outputs[0]

    # Icing: the dough's upper half pushed out along the normals, with a wavy dripping edge.
    _, _, nz = tree.separate(_normal(tree))
    wave = tree.math("SUBTRACT", _noise(tree, _position(tree), 38.0, seed, 1.0), 0.5)
    coverage = tree.math("ADD", tree.math("ADD", nz, 0.12), tree.math("MULTIPLY", wave, 1.6))
    coverage = _smoothstep(tree, coverage, 0.0, 0.35)
    icing = _store(tree, dough, "icing", coverage)
    thickness = tree.math("SUBTRACT", tree.math("MULTIPLY", _named(tree, "icing"), 0.0034),
                          0.0008)
    icing = tree.n("GeometryNodeSetPosition", Geometry=icing,
                   Offset=tree.vmath("SCALE", _normal(tree), scale=thickness)).outputs[0]
    bare = tree.math("LESS_THAN", _named(tree, "icing"), 0.002)
    icing = tree.n("GeometryNodeDeleteGeometry", domain="FACE", Geometry=icing,
                   Selection=bare).outputs[0]

    # Sprinkles: lying flat on the fully iced faces, spun randomly.
    covered = tree.math("GREATER_THAN", _named(tree, "icing"), 0.98)
    area = tree.n("GeometryNodeAttributeStatistic", data_type="FLOAT", domain="FACE",
                  Geometry=icing, Selection=covered,
                  Attribute=tree.n("GeometryNodeInputMeshFaceArea").outputs[0])
    density = tree.math("DIVIDE", count, tree.math("MAXIMUM", tree.out(area, "Sum"), 1e-6))
    scatter = tree.n("GeometryNodeDistributePointsOnFaces", Mesh=icing, Selection=covered,
                     Density=density, Seed=seed)
    spin = _euler(tree, z=_random(tree, 0.0, 2.0 * math.pi, seed, 11))
    rotation = tree.n("FunctionNodeRotateRotation", rotation_space="LOCAL",
                      Rotation=tree.out(scatter, "Rotation"), Rotate_By=spin).outputs[0]
    sprinkle = tree.n("GeometryNodeMeshUVSphere", Segments=8, Rings=4, Radius=1.0).outputs[0]
    sprinkles = tree.n("GeometryNodeInstanceOnPoints", Points=tree.out(scatter, "Points"),
                       Instance=sprinkle, Rotation=rotation,
                       Scale=(0.0029, 0.0008, 0.0008)).outputs[0]
    sprinkles = _store(tree, sprinkles, "sprinkle", _random(tree, 0.0, 1.0, seed, 12),
                       domain="INSTANCE")

    # Bite: cut overlapping spheres out of the dough; trim the icing and sprinkles to match.
    centers = _bite_centers(major, minor)
    position = _position(tree)
    inside = None
    bitten_position = position
    spheres = []
    for center in centers:
        sphere = tree.n("GeometryNodeMeshUVSphere", Segments=24, Rings=12,
                        Radius=BITE_RADIUS).outputs[0]
        spheres.append(tree.transform(sphere, translation=center))
        offset = tree.vmath("SUBTRACT", bitten_position, center)
        distance = tree.vmath("LENGTH", offset)
        hit = tree.math("LESS_THAN", distance, BITE_RADIUS)
        inside = hit if inside is None else tree.math("MAXIMUM", inside, hit)
        pushed = tree.vmath("ADD", center, tree.vmath("SCALE", tree.vmath("NORMALIZE", offset),
                                                      scale=BITE_RADIUS))
        bitten_position = tree.out(tree.n("ShaderNodeMix", data_type="VECTOR", Factor=hit,
                                          A=bitten_position, B=pushed), "Result")
    bitten = tree.n("GeometryNodeMeshBoolean", operation="DIFFERENCE", solver="MANIFOLD",
                    Mesh_1=dough)
    for sphere in spheres:
        tree.set_input(bitten.inputs["Mesh 2"], sphere)
    dough = _switch(tree, bite_on, dough, tree.out(bitten, "Mesh"))
    bitten_icing = _store(tree, icing, "bitten", inside)
    eaten = tree.math("GREATER_THAN", _named(tree, "bitten"), 0.999)
    bitten_icing = tree.n("GeometryNodeDeleteGeometry", domain="FACE", Geometry=bitten_icing,
                          Selection=eaten).outputs[0]
    bitten_icing = tree.n("GeometryNodeSetPosition", Geometry=bitten_icing,
                          Position=bitten_position).outputs[0]
    icing = _switch(tree, bite_on, icing, bitten_icing)
    bitten_sprinkles = tree.n("GeometryNodeDeleteGeometry", domain="INSTANCE",
                              Geometry=sprinkles, Selection=inside).outputs[0]
    sprinkles = _switch(tree, bite_on, sprinkles, bitten_sprinkles)
    sprinkles = tree.material(tree.realize(sprinkles), sprinkles_material)

    dough = tree.material(tree.smooth(dough), dough_material)
    icing = tree.material(tree.smooth(icing), icing_material)
    sprinkles = tree.smooth(sprinkles)
    sprinkles = _switch(tree, tree.math("MULTIPLY", sprinkles_on, icing_on), None, sprinkles)
    icing = _switch(tree, icing_on, None, icing)
    tree.output(rest_on_ground(tree, tree.join(dough, icing, sprinkles)))
    return generator_object("Donut", tree)


# -----------------------------------------------------------------------------
# Balloon.

@asset("Balloon", "object", CATALOG,
       "A shiny party balloon with a tied knot and a curly string that rests on the ground.",
       ("balloon", "party", "birthday", "toy", "celebration", "stylized"))
def balloon():
    tree = object_group("Balloon")
    size = tree.input("Size", "FLOAT", 0.3, min=0.01, subtype="DISTANCE",
                      description="Width of the inflated balloon")
    length = tree.input("String Length", "FLOAT", 0.55, min=0.0, subtype="DISTANCE")
    material = tree.input("Material", "MATERIAL", simple_material(
        "Balloon Red", (0.8, 0.02, 0.025), roughness=0.12, Coat_Weight=0.6,
        Subsurface_Weight=0.2))
    string_material = tree.input("String Material", "MATERIAL", palette("Starter White"))

    # Body: a sphere stretched up and tapered into a teardrop toward the knot. Radius 1.
    body = tree.n("GeometryNodeMeshUVSphere", Segments=48, Rings=24, Radius=1.0).outputs[0]
    x, y, z = tree.separate(_position(tree))
    below = tree.math("MAXIMUM", tree.math("MULTIPLY", z, -1.0), 0.0)
    taper = tree.math("SUBTRACT", 1.0, tree.math("MULTIPLY", tree.math("POWER", below, 1.6),
                                                 0.42))
    bulge = tree.math("ADD", 1.0, tree.math("MULTIPLY", tree.math("MAXIMUM", z, 0.0), 0.04))
    taper = tree.math("MULTIPLY", taper, bulge)
    shape = tree.xyz(tree.math("MULTIPLY", x, taper), tree.math("MULTIPLY", y, taper),
                     tree.math("MULTIPLY", z, 1.15))
    body = tree.n("GeometryNodeSetPosition", Geometry=body, Position=shape).outputs[0]
    # Knot: a pinched neck and a little rolled lip under the body.
    neck = tree.n("GeometryNodeMeshCone", Vertices=16, Side_Segments=1, Radius_Top=0.05,
                  Radius_Bottom=0.035, Depth=0.15).outputs["Mesh"]
    neck = tree.transform(neck, translation=(0.0, 0.0, -1.25))
    lip = tree.n("GeometryNodeCurveToMesh", Curve=tree.n(
        "GeometryNodeCurvePrimitiveCircle", Resolution=16, Radius=0.055).outputs["Curve"],
        Profile_Curve=tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=8,
                             Radius=0.03).outputs["Curve"]).outputs[0]
    lip = tree.transform(lip, translation=(0.0, 0.0, -1.25))
    radius = tree.math("MULTIPLY", size, 0.5)
    body = tree.join(body, neck, lip)
    body = tree.transform(body, translation=tree.xyz(z=tree.math("ADD", length, tree.math(
        "MULTIPLY", radius, 1.28))), scale=tree.xyz(radius, radius, radius))
    body = tree.material(tree.smooth(body), material)

    # String: hangs from the knot to the ground in a gentle S curve.
    line = tree.n("GeometryNodeCurvePrimitiveLine", Start=(0.0, 0.0, 0.0),
                  End=tree.xyz(z=tree.math("MAXIMUM", length, 0.001))).outputs[0]
    line = tree.n("GeometryNodeResampleCurve", Curve=line, mode="COUNT", Count=40).outputs[0]
    t = tree.n("GeometryNodeSplineParameter").outputs["Factor"]
    sway = tree.math("SINE", tree.math("MULTIPLY", t, 2.0 * math.pi))
    sway = tree.math("MULTIPLY", sway, tree.math("MULTIPLY", length, 0.06))
    swing = tree.math("SINE", tree.math("MULTIPLY", t, math.pi))
    swing = tree.math("MULTIPLY", swing, tree.math("MULTIPLY", length, 0.03))
    line = tree.n("GeometryNodeSetPosition", Geometry=line,
                  Offset=tree.xyz(sway, swing, 0.0)).outputs[0]
    profile = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=6,
                     Radius=0.0015).outputs["Curve"]
    string = tree.n("GeometryNodeCurveToMesh", Curve=line, Profile_Curve=profile).outputs[0]
    string = tree.material(tree.smooth(string), string_material)

    tree.output(rest_on_ground(tree, tree.join(body, string)))
    return generator_object("Balloon", tree)
