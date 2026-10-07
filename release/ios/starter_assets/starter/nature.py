# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Nature: rocks, plants, landscapes, water, clouds and planets."""

import math

import bpy

from .common import RADIANS, generator_object, object_group, palette, rest_on_ground
from .nodes import Tree
from .registry import asset

CATALOG = "Objects/Nature"
GOLDEN_ANGLE = 2.39996


# -----------------------------------------------------------------------------
# Node helpers.

def _position(tree):
    return tree.n("GeometryNodeInputPosition").outputs[0]


def _normal(tree):
    return tree.n("GeometryNodeInputNormal").outputs[0]


def _index(tree):
    return tree.n("GeometryNodeInputIndex").outputs[0]


def _id(tree, id):
    """A constant ID must be linked: an unlinked ID socket means the index."""
    if isinstance(id, int):
        return tree.n("FunctionNodeInputInt", integer=id).outputs[0]
    return id


def _random(tree, low, high, seed, id=None):
    """A random float; pass `id` to make it a function of something other than the index."""
    id = _id(tree, id)
    node = tree.n("FunctionNodeRandomValue", data_type="FLOAT", Min=low, Max=high, Seed=seed,
                  ID=id)
    return tree.out(node, "Value")


def _random_vector(tree, low, high, seed, id=None):
    id = _id(tree, id)
    node = tree.n("FunctionNodeRandomValue", data_type="FLOAT_VECTOR", Min=low, Max=high,
                  Seed=seed, ID=id)
    return tree.out(node, "Value")


def _seed(tree, seed, offset):
    return tree.math("ADD", seed, offset)


def _noise(tree, vector, scale, seed=None, detail=2.0, roughness=0.5, output="Factor"):
    """Noise texture; a seed picks a different 4D slice of it."""
    node = tree.n("ShaderNodeTexNoise", noise_dimensions="3D" if seed is None else "4D",
                  Vector=vector, W=None if seed is None else tree.math("MULTIPLY", seed, 1.731),
                  Scale=scale, Detail=detail, Roughness=roughness)
    return tree.out(node, output)


def _smoothstep(tree, value, low, high):
    node = tree.n("ShaderNodeMapRange", interpolation_type="SMOOTHSTEP", Value=value,
                  From_Min=low, From_Max=high)
    return tree.out(node, "Result")


def _remap(tree, value, from_low, from_high, to_low, to_high, clamp=True):
    node = tree.n("ShaderNodeMapRange", clamp=clamp, Value=value, From_Min=from_low,
                  From_Max=from_high, To_Min=to_low, To_Max=to_high)
    return tree.out(node, "Result")


def _mix_vector(tree, factor, a, b):
    return tree.out(tree.n("ShaderNodeMix", data_type="VECTOR", Factor=factor, A=a, B=b), "Result")


def _switch(tree, input_type, condition, false, true):
    node = tree.n("GeometryNodeSwitch", input_type=input_type, Switch=condition)
    tree.set_input(tree._input(node, "False"), false)
    tree.set_input(tree._input(node, "True"), true)
    return node.outputs[0]


def _euler(tree, x=0.0, y=0.0, z=0.0):
    return tree.n("FunctionNodeEulerToRotation", Euler=tree.xyz(x, y, z)).outputs[0]


def _store(tree, geometry, name, value, domain="POINT"):
    return tree.n("GeometryNodeStoreNamedAttribute", data_type="FLOAT", domain=domain,
                  Geometry=geometry, Name=name, Value=value).outputs[0]


def _instance(tree, points, instance, rotation=None, scale=None):
    return tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=instance,
                  Rotation=rotation, Scale=scale).outputs[0]


def _offset(tree, geometry, offset, selection=None):
    return tree.n("GeometryNodeSetPosition", Geometry=geometry, Selection=selection,
                  Offset=offset).outputs[0]


def _move(tree, geometry, position, selection=None):
    return tree.n("GeometryNodeSetPosition", Geometry=geometry, Selection=selection,
                  Position=position).outputs[0]


def _ico(tree, radius, subdivisions):
    return tree.n("GeometryNodeMeshIcoSphere", Radius=radius,
                  Subdivisions=subdivisions).outputs["Mesh"]


def _uv_sphere(tree, segments, rings, radius=1.0):
    return tree.n("GeometryNodeMeshUVSphere", Segments=segments, Rings=rings,
                  Radius=radius).outputs["Mesh"]


def _group(tree, sub_tree, **inputs):
    return tree.n("GeometryNodeGroup", node_tree=sub_tree.nt, **inputs).outputs[0]


# -----------------------------------------------------------------------------
# Default materials. Procedural, using object coordinates or attributes the generators store.

def _material(name, color, roughness=0.5, build=None, **inputs):
    """A Principled BSDF material; `build(tree, bsdf)` adds procedural detail in front of it."""
    material = bpy.data.materials.get(name)
    if material:
        return material
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = Tree(material.node_tree, group_io=False)
    bsdf = tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    for key, value in inputs.items():
        socket = bsdf.inputs[key.replace("_", " ")]
        if socket.type == "RGBA":
            value = (*value, 1.0)
        socket.default_value = value
    if build:
        build(tree, bsdf)
        tree.layout()
    material.diffuse_color = (*color, 1.0)
    material.roughness = roughness
    return material


def _mix_color(tree, factor, a, b):
    if isinstance(a, tuple):
        a = (*a, 1.0)
    if isinstance(b, tuple):
        b = (*b, 1.0)
    return tree.out(tree.n("ShaderNodeMix", data_type="RGBA", Factor=factor, A=a, B=b), "Result")


def _attribute(tree, name):
    return tree.out(tree.n("ShaderNodeAttribute", attribute_name=name), "Factor")


def _object_coordinates(tree):
    return tree.out(tree.n("ShaderNodeTexCoord"), "Object")


def _bump(tree, bsdf, height, strength, distance=0.02):
    bump = tree.n("ShaderNodeBump", Strength=strength, Distance=distance, Height=height)
    tree.links.new(bump.outputs[0], bsdf.inputs["Normal"])


def _rock_material():
    def build(tree, bsdf):
        coordinates = _object_coordinates(tree)
        broad = _noise(tree, coordinates, 2.5, detail=4.0)
        fine = _noise(tree, coordinates, 14.0, detail=6.0, roughness=0.6)
        color = _mix_color(tree, _smoothstep(tree, broad, 0.35, 0.7),
                           (0.17, 0.16, 0.15), (0.42, 0.4, 0.37))
        color = _mix_color(tree, tree.math("MULTIPLY", _smoothstep(tree, fine, 0.55, 0.75), 0.4),
                           color, (0.55, 0.53, 0.5))
        tree.links.new(color, bsdf.inputs["Base Color"])
        _bump(tree, bsdf, fine, 0.35)
    return _material("Starter Rock", (0.32, 0.31, 0.29), 0.85, build)


def _foliage_material():
    def build(tree, bsdf):
        shade = _attribute(tree, "shade")
        color = _mix_color(tree, shade, (0.05, 0.16, 0.025), (0.2, 0.4, 0.05))
        tree.links.new(color, bsdf.inputs["Base Color"])
    return _material("Starter Foliage", (0.1, 0.28, 0.05), 0.6, build, Subsurface_Weight=0.05)


def _grass_material():
    def build(tree, bsdf):
        tip = _attribute(tree, "tip")
        shade = _attribute(tree, "shade")
        top = _mix_color(tree, shade, (0.16, 0.42, 0.05), (0.42, 0.5, 0.12))
        color = _mix_color(tree, _smoothstep(tree, tip, 0.0, 0.8), (0.03, 0.1, 0.015), top)
        tree.links.new(color, bsdf.inputs["Base Color"])
    return _material("Starter Grass Blades", (0.16, 0.4, 0.06), 0.55, build)


# -----------------------------------------------------------------------------
# Rocks.

def _rock_shape():
    """A shared group making one rock of unit-ish size, resting on the ground."""
    name = "Starter Rock Shape"
    existing = bpy.data.node_groups.get(name)
    if existing:
        return Tree(existing, group_io=False)
    tree = Tree.geometry(name, modifier=False)
    size = tree.input("Size", "FLOAT", 1.0, min=0.01, subtype="DISTANCE")
    roughness = tree.input("Roughness", "FLOAT", 0.5, min=0.0, max=1.0, subtype="FACTOR")
    cracks = tree.input("Cracks", "FLOAT", 0.4, min=0.0, max=1.0, subtype="FACTOR")
    seed = tree.input("Seed", "INT", 0)
    detail = tree.input("Detail", "INT", 4, min=1, max=6)

    # Proportions vary per seed: long, a little narrower, and flatter still.
    proportions = _random_vector(tree, (0.5, 0.36, 0.22), (0.5, 0.47, 0.36), seed, id=0)
    proportions = tree.vmath("SCALE", proportions, scale=size)
    direction = tree.vmath("NORMALIZE", _random_vector(tree, (-1, -1, -1), (1, 1, 1), seed))
    points = tree.n("GeometryNodePoints", Count=24,
                    Position=tree.vmath("MULTIPLY", direction, proportions)).outputs[0]
    hull = tree.n("GeometryNodeConvexHull", Geometry=points).outputs[0]
    # Creases keep the hull's facets readable once it's subdivided.
    mesh = tree.n("GeometryNodeSubdivisionSurface", Mesh=hull, Level=detail,
                  Edge_Crease=0.75).outputs[0]

    position = _position(tree)
    lumps = tree.math("MULTIPLY", tree.math("SUBTRACT", _noise(
        tree, position, tree.math("DIVIDE", 1.4, size), seed, detail=2.0), 0.5),
        tree.math("MULTIPLY", size, 0.3))
    fine = tree.math("MULTIPLY", tree.math("SUBTRACT", _noise(
        tree, position, tree.math("DIVIDE", 7.0, size), seed, detail=6.0, roughness=0.6), 0.5),
        tree.math("MULTIPLY", tree.math("MULTIPLY", size, roughness), 0.12))
    voronoi = tree.n("ShaderNodeTexVoronoi", voronoi_dimensions="4D", feature="DISTANCE_TO_EDGE",
                     Vector=position, W=tree.math("MULTIPLY", seed, 2.13),
                     Scale=tree.math("DIVIDE", 2.2, size))
    edge = tree.out(voronoi, "Distance")
    groove = tree.math("SUBTRACT", 1.0, _smoothstep(tree, edge, 0.0, 0.06))
    groove = tree.math("MULTIPLY", groove, tree.math("MULTIPLY", tree.math(
        "MULTIPLY", size, cracks), 0.035))
    height = tree.math("SUBTRACT", tree.math("ADD", lumps, fine), groove)
    mesh = _offset(tree, mesh, tree.vmath("SCALE", _normal(tree), scale=height))

    # Flatten the underside so it sits on the ground.
    _, _, depth = tree.separate(proportions)
    x, y, z = tree.separate(_position(tree))
    floor = tree.math("MULTIPLY", depth, -0.55)
    mesh = _move(tree, mesh, tree.xyz(x, y, tree.math("MAXIMUM", z, floor)))
    mesh = tree.smooth(mesh, angle=50 * RADIANS)
    tree.output(rest_on_ground(tree, mesh))
    tree.layout()
    return tree


@asset("Rock", "object", CATALOG,
       "A lumpy, faceted boulder with cracks and a flat underside that sits on the ground.",
       ("rock", "stone", "boulder", "nature", "terrain"))
def rock():
    tree = object_group("Rock")
    size = tree.input("Size", "FLOAT", 1.0, min=0.01, subtype="DISTANCE",
                      description="Length of the rock along its longest side")
    roughness = tree.input("Roughness", "FLOAT", 0.5, min=0.0, max=1.0, subtype="FACTOR",
                           description="Amount of small surface bumps")
    cracks = tree.input("Cracks", "FLOAT", 0.4, min=0.0, max=1.0, subtype="FACTOR",
                        description="Depth of the cracks running across the surface")
    seed = tree.input("Seed", "INT", 0)
    detail = tree.input("Detail", "INT", 4, min=1, max=6,
                        description="Subdivision level; each step has four times the faces")
    material = tree.input("Material", "MATERIAL", _rock_material())
    mesh = _group(tree, _rock_shape(), Size=size, Roughness=roughness, Cracks=cracks, Seed=seed,
                  Detail=detail)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Rock", tree)


@asset("Rock Cluster", "object", CATALOG,
       "A group of differently sized rocks huddled together on the ground.",
       ("rocks", "stones", "boulders", "cluster", "nature"))
def rock_cluster():
    tree = object_group("Rock Cluster")
    count = tree.input("Count", "INT", 6, min=3, max=12)
    size = tree.input("Size", "FLOAT", 0.7, min=0.01, subtype="DISTANCE",
                      description="Length of the largest rock")
    spread = tree.input("Spread", "FLOAT", 1.1, min=0.0, subtype="DISTANCE",
                        description="Width of the area the rocks are scattered over")
    variation = tree.input("Size Variation", "FLOAT", 0.55, min=0.0, max=0.95, subtype="FACTOR",
                           description="How much smaller the other rocks get")
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL", _rock_material())

    shape = _rock_shape()
    variants = tree.n("GeometryNodeGeometryToInstance")
    for offset in range(3):
        rock_mesh = _group(tree, shape, Size=1.0, Roughness=0.5, Cracks=0.3, Detail=3,
                           Seed=_seed(tree, tree.math("MULTIPLY", seed, 3.0), offset))
        tree.links.new(rock_mesh, variants.inputs[0])

    # A sunflower spiral from the center outward; rocks shrink toward the edge.
    index = _index(tree)
    fraction = tree.math("DIVIDE", index, count)
    radius = tree.math("MULTIPLY", tree.math("SQRT", fraction), tree.math("MULTIPLY", spread, 0.5))
    angle = tree.math("ADD", tree.math("MULTIPLY", index, GOLDEN_ANGLE),
                      _random(tree, 0.0, 6.283, seed, id=0))
    position = tree.xyz(tree.math("MULTIPLY", radius, tree.math("COSINE", angle)),
                        tree.math("MULTIPLY", radius, tree.math("SINE", angle)), 0.0)
    points = tree.n("GeometryNodePoints", Count=count, Position=position).outputs[0]
    shrink = tree.math("MULTIPLY", variation, tree.math(
        "MULTIPLY", tree.math("SQRT", fraction), _random(tree, 0.7, 1.15, _seed(tree, seed, 1))))
    scale = tree.math("MULTIPLY", size, tree.math("SUBTRACT", 1.0, shrink, clamp=True))
    stretch = _random_vector(tree, (0.85, 0.85, 0.8), (1.15, 1.15, 1.15), _seed(tree, seed, 2))
    pick = tree.n("FunctionNodeRandomValue", data_type="INT", Min=0, Max=2,
                  Seed=_seed(tree, seed, 3))
    rocks = tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=variants.outputs[0],
                   Pick_Instance=True, Instance_Index=tree.out(pick, "Value"),
                   Rotation=_euler(tree, z=_random(tree, 0.0, 6.283, _seed(tree, seed, 4))),
                   Scale=tree.vmath("SCALE", stretch, scale=scale)).outputs[0]
    mesh = tree.material(tree.realize(rocks), material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Rock Cluster", tree)


# -----------------------------------------------------------------------------
# Plants.

def _branch(tree, branch, count, height, seed):
    """Height on the trunk, direction and length of one branch, a function of its number."""
    t = tree.math("DIVIDE", branch, tree.math("MAXIMUM", tree.math("SUBTRACT", count, 1.0), 1.0))
    base = tree.math("MULTIPLY", height, tree.math("ADD", tree.math("MULTIPLY", t, 0.3), tree.math(
        "ADD", 0.42, _random(tree, -0.04, 0.04, seed, id=branch))))
    yaw = tree.math("ADD", tree.math("MULTIPLY", branch, GOLDEN_ANGLE), tree.math(
        "ADD", _random(tree, -0.4, 0.4, _seed(tree, seed, 1), id=branch),
        _random(tree, 0.0, 6.283, _seed(tree, seed, 2), id=0)))
    tilt = _random(tree, 0.65, 0.95, _seed(tree, seed, 3), id=branch)
    length = tree.math("MULTIPLY", tree.math("MULTIPLY", height, _random(
        tree, 0.26, 0.34, _seed(tree, seed, 4), id=branch)),
        tree.math("SUBTRACT", 1.0, tree.math("MULTIPLY", t, 0.35)))
    sin_tilt = tree.math("SINE", tilt)
    direction = tree.xyz(tree.math("MULTIPLY", sin_tilt, tree.math("SINE", yaw)),
                         tree.math("MULTIPLY", tree.math("MULTIPLY", sin_tilt, -1.0),
                                   tree.math("COSINE", yaw)),
                         tree.math("COSINE", tilt))
    tip = tree.vmath("ADD", tree.xyz(z=base), tree.vmath("SCALE", direction, scale=length))
    return base, yaw, tilt, length, tip


@asset("Low-Poly Tree", "object", CATALOG,
       "A stylized low-poly tree with a tapered trunk, a few branches and chunky leaf clumps.",
       ("tree", "low poly", "stylized", "plant", "nature", "forest"))
def low_poly_tree():
    tree = object_group("Low-Poly Tree")
    height = tree.input("Height", "FLOAT", 4.0, min=0.2, subtype="DISTANCE")
    count = tree.input("Branching", "INT", 5, min=0, max=12, description="Number of branches")
    density = tree.input("Leaf Density", "INT", 4, min=1, max=8,
                         description="Leaf clumps at the end of each branch and on top")
    seed = tree.input("Seed", "INT", 0)
    bark = tree.input("Trunk Material", "MATERIAL", palette("Starter Bark"))
    leaves = tree.input("Leaf Material", "MATERIAL", _foliage_material())

    trunk_height = tree.math("MULTIPLY", height, 0.8)
    trunk = tree.n("GeometryNodeMeshCone", Vertices=7, Side_Segments=3,
                   Radius_Bottom=tree.math("MULTIPLY", height, 0.055),
                   Radius_Top=tree.math("MULTIPLY", height, 0.025), Depth=trunk_height,
                   fill_type="NGON").outputs["Mesh"]

    # Branches: unit-length cones pointing up, scaled and tilted per branch.
    cone = tree.n("GeometryNodeMeshCone", Vertices=5, Radius_Bottom=tree.math(
        "MULTIPLY", height, 0.025), Radius_Top=tree.math("MULTIPLY", height, 0.008), Depth=1.0,
        fill_type="NGON").outputs["Mesh"]
    branch_points = tree.n("GeometryNodePoints", Count=count).outputs[0]
    base, yaw, tilt, length, _ = _branch(tree, _index(tree), count, height, seed)
    branch_points = _move(tree, branch_points, tree.xyz(z=base))
    branches = _instance(tree, branch_points, cone, _euler(tree, tilt, 0.0, yaw),
                         tree.xyz(1.0, 1.0, length))
    wood = tree.material(tree.join(trunk, tree.realize(branches)), bark)
    wood = tree.smooth(wood, angle=50 * RADIANS)

    # Leaf clumps around each branch tip, plus a bigger crown on top of the trunk.
    clumps = tree.math("MULTIPLY", tree.math("ADD", count, 1.0), density)
    index = _index(tree)
    branch = tree.math("FLOOR", tree.math("DIVIDE", index, density))
    on_top = tree.math("GREATER_THAN", branch, tree.math("SUBTRACT", count, 0.5))
    *_, tip = _branch(tree, branch, count, height, seed)
    crown = tree.xyz(z=tree.math("MULTIPLY", height, 0.8))
    center = _mix_vector(tree, on_top, tip, crown)
    radius = tree.math("MULTIPLY", tree.math("MULTIPLY", height, _random(
        tree, 0.14, 0.19, _seed(tree, seed, 5))), tree.math("ADD", 1.0, tree.math(
            "MULTIPLY", on_top, 0.35)))
    jitter = _random_vector(tree, (-1.0, -1.0, -0.4), (1.0, 1.0, 0.8), _seed(tree, seed, 6))
    jitter = tree.vmath("SCALE", jitter, scale=tree.math("MULTIPLY", radius, 0.75))
    leaf_points = tree.n("GeometryNodePoints", Count=clumps,
                         Position=tree.vmath("ADD", center, jitter)).outputs[0]
    squash = _random_vector(tree, (0.85, 0.85, 0.7), (1.15, 1.15, 0.95), _seed(tree, seed, 7))
    blobs = _instance(tree, leaf_points, _ico(tree, 1.0, 2),
                      _euler(tree, z=_random(tree, 0.0, 6.283, _seed(tree, seed, 8))),
                      tree.vmath("SCALE", squash, scale=radius))
    blobs = _store(tree, blobs, "shade", _random(tree, 0.0, 1.0, _seed(tree, seed, 9)),
                   domain="INSTANCE")
    foliage = tree.realize(blobs)
    wobble = tree.vmath("SUBTRACT", _noise(tree, _position(tree), tree.math(
        "DIVIDE", 2.5, height), seed, output="Color"), (0.5, 0.5, 0.5))
    foliage = _offset(tree, foliage, tree.vmath("SCALE", wobble, scale=tree.math(
        "MULTIPLY", height, 0.09)))
    foliage = tree.smooth(tree.material(foliage, leaves), False)

    tree.output(rest_on_ground(tree, tree.join(wood, foliage)))
    return generator_object("Low-Poly Tree", tree)


def _leaf_card(tree, length, width):
    """A flat, pointed leaf lying along +Y from the origin, facing +Z."""
    leaf = tree.n("GeometryNodeMeshCircle", Vertices=6, Radius=0.5, fill_type="NGON").outputs[0]
    leaf = tree.transform(leaf, translation=(0.0, 0.5, 0.0), rotation=(0.0, 0.0, 90 * RADIANS))
    return tree.transform(leaf, scale=tree.xyz(width, length, 1.0))


@asset("Bush", "object", CATALOG,
       "A round, leafy shrub made of hundreds of small leaves around a dense core.",
       ("bush", "shrub", "plant", "hedge", "nature", "garden"))
def bush():
    tree = object_group("Bush")
    size = tree.input("Size", "FLOAT", 0.9, min=0.05, subtype="DISTANCE",
                      description="Width of the bush")
    density = tree.input("Density", "INT", 1600, min=0, max=8000, description="Number of leaves")
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL", _foliage_material())

    radius = tree.math("MULTIPLY", size, 0.5)
    shape = tree.xyz(radius, radius, tree.math("MULTIPLY", radius, 0.82))

    def lumpy(position):
        """Push a point on the unit sphere out to the bush's lumpy surface."""
        bumps = _noise(tree, position, 1.8, seed, detail=1.0)
        scale = _remap(tree, bumps, 0.3, 0.7, 0.8, 1.05)
        return tree.vmath("MULTIPLY", tree.vmath("SCALE", position, scale=scale), shape)

    core = _ico(tree, 1.0, 3)
    core = _move(tree, core, tree.vmath("SCALE", lumpy(_position(tree)), scale=0.86))
    core = _store(tree, core, "shade", 0.0)
    core = tree.smooth(core)

    direction = tree.vmath("NORMALIZE", _random_vector(tree, (-1, -1, -0.7), (1, 1, 1), seed))
    leaf_points = tree.n("GeometryNodePoints", Count=density, Position=lumpy(direction)).outputs[0]
    # Leaves face outward, with random spin and droop.
    outward = tree.n("FunctionNodeAlignRotationToVector", axis="Z", Vector=direction).outputs[0]
    spin = _euler(tree, _random(tree, -0.5, 0.5, _seed(tree, seed, 1)),
                  _random(tree, -0.5, 0.5, _seed(tree, seed, 2)),
                  _random(tree, 0.0, 6.283, _seed(tree, seed, 3)))
    rotation = tree.n("FunctionNodeRotateRotation", rotation_space="LOCAL", Rotation=outward,
                      Rotate_By=spin).outputs[0]
    leaf_size = tree.math("MULTIPLY", size, _random(tree, 0.07, 0.11, _seed(tree, seed, 4)))
    leaf = _leaf_card(tree, 1.0, 0.45)
    leaves = _instance(tree, leaf_points, leaf, rotation, tree.xyz(leaf_size, leaf_size,
                                                                   leaf_size))
    leaves = _store(tree, leaves, "shade", _random(tree, 0.2, 1.0, _seed(tree, seed, 5)),
                    domain="INSTANCE")
    mesh = tree.join(core, tree.realize(leaves))
    x, y, z = tree.separate(_position(tree))
    floor = tree.math("MULTIPLY", radius, -0.62)
    mesh = _move(tree, mesh, tree.xyz(x, y, tree.math("MAXIMUM", z, floor)))
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Bush", tree)


@asset("Grass Patch", "object", CATALOG,
       "A round patch of soil covered in grass blades that bend in the wind.",
       ("grass", "lawn", "meadow", "plant", "nature", "ground"))
def grass_patch():
    tree = object_group("Grass Patch")
    size = tree.input("Size", "FLOAT", 1.0, min=0.05, subtype="DISTANCE",
                      description="Diameter of the patch")
    density = tree.input("Density", "FLOAT", 2200.0, min=0.0, max=20000.0,
                         description="Blades of grass per square meter")
    length = tree.input("Length", "FLOAT", 0.22, min=0.01, subtype="DISTANCE",
                        description="Average length of a blade")
    wind = tree.input("Wind", "FLOAT", 0.35, min=0.0, max=1.0, subtype="FACTOR",
                      description="How far the blades bend over in one direction")
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL", _grass_material())
    soil_material = tree.input("Soil Material", "MATERIAL", palette("Starter Soil"))

    ground = tree.n("GeometryNodeMeshCircle", Vertices=48,
                    Radius=tree.math("MULTIPLY", size, 0.5), fill_type="NGON").outputs[0]
    ground_points = tree.n("GeometryNodeDistributePointsOnFaces", Mesh=ground, Density=density,
                           Seed=seed).outputs["Points"]
    soil = tree.material(ground, soil_material)

    # One blade, a unit tall: tapered to a point and curving slightly along its length.
    blade = tree.n("GeometryNodeMeshGrid", Size_X=1.0, Size_Y=1.0, Vertices_X=2,
                   Vertices_Y=5).outputs["Mesh"]
    bx, by, _ = tree.separate(_position(tree))
    along = tree.math("ADD", by, 0.5)
    taper = tree.math("SUBTRACT", 1.0, tree.math("POWER", along, 1.6))
    blade = _move(tree, blade, tree.xyz(tree.math("MULTIPLY", tree.math("MULTIPLY", bx, taper),
                                                  0.04),
                                        tree.math("MULTIPLY", tree.math("POWER", along, 2.0),
                                                  0.22),
                                        along))
    blade = _store(tree, blade, "tip", along)

    lean = _euler(tree, _random(tree, -0.35, 0.35, _seed(tree, seed, 1)),
                  _random(tree, -0.35, 0.35, _seed(tree, seed, 2)),
                  _random(tree, 0.0, 6.283, _seed(tree, seed, 3)))
    blade_length = tree.math("MULTIPLY", length, _random(tree, 0.55, 1.2, _seed(tree, seed, 4)))
    blades = _instance(tree, ground_points, blade, lean, tree.xyz(blade_length, blade_length,
                                                                    blade_length))
    blades = _store(tree, blades, "shade", _random(tree, 0.0, 1.0, _seed(tree, seed, 5)),
                    domain="INSTANCE")
    blades = tree.realize(blades)
    # Wind bends every blade the same way, more toward the tip.
    _, _, z = tree.separate(_position(tree))
    bend = tree.math("MULTIPLY", tree.math("POWER", tree.math("DIVIDE", z, length), 2.0),
                     tree.math("MULTIPLY", length, wind))
    blades = _offset(tree, blades, tree.xyz(tree.math("MULTIPLY", bend, 0.8), 0.0,
                                            tree.math("MULTIPLY", bend, -0.35)))
    blades = tree.material(blades, material)
    tree.output(rest_on_ground(tree, tree.join(soil, blades)))
    return generator_object("Grass Patch", tree)


@asset("Flower", "object", CATALOG,
       "A single flower on a gently curved stem with two leaves and a ring of petals.",
       ("flower", "daisy", "plant", "garden", "nature", "petal"))
def flower():
    tree = object_group("Flower")
    petals = tree.input("Petals", "INT", 10, min=3, max=32)
    stem_height = tree.input("Stem Height", "FLOAT", 0.3, min=0.02, subtype="DISTANCE")
    petal_length = tree.input("Petal Length", "FLOAT", 0.04, min=0.005, subtype="DISTANCE")
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL", palette("Starter Pink"),
                          description="Material of the petals")
    center_material = tree.input("Center Material", "MATERIAL", palette("Starter Yellow"))
    stem_material = tree.input("Stem Material", "MATERIAL", palette("Starter Grass"))

    # The stem leans a little to one side and its tip turns the head the same way.
    lean = tree.vmath("SCALE", tree.vmath("ADD", (0.35, -0.55, 0.0), _random_vector(
        tree, (-0.3, -0.3, 0.0), (0.3, 0.3, 0.0), seed, id=0)), scale=stem_height)
    lx, ly, _ = tree.separate(lean)
    top = tree.xyz(tree.math("MULTIPLY", lx, 0.22), tree.math("MULTIPLY", ly, 0.22), stem_height)
    middle = tree.xyz(0.0, 0.0, tree.math("MULTIPLY", stem_height, 0.55))
    stem_curve = tree.n("GeometryNodeCurveQuadraticBezier", Resolution=10, Start=(0.0, 0.0, 0.0),
                        Middle=middle, End=top).outputs[0]
    profile = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=6,
                     Radius=tree.math("MULTIPLY", stem_height, 0.012)).outputs[0]
    stem = tree.n("GeometryNodeCurveToMesh", Curve=stem_curve, Profile_Curve=profile,
                  Fill_Caps=True).outputs[0]

    leaf_shape = _uv_sphere(tree, 12, 6)
    leaf_shape = tree.transform(leaf_shape, translation=(0.0, 1.0, 0.0),
                                scale=(0.28, 1.0, 0.05))
    lx_, ly_, lz_ = tree.separate(_position(tree))
    # Arch the leaf: up from the stem, then drooping toward its tip.
    arch = tree.math("MULTIPLY", tree.math("SUBTRACT", ly_, tree.math("MULTIPLY", tree.math(
        "MULTIPLY", ly_, ly_), 0.5)), 0.35)
    leaf_shape = _move(tree, leaf_shape, tree.xyz(lx_, ly_, tree.math("ADD", lz_, arch)))
    leaf_length = tree.math("MULTIPLY", stem_height, 0.28)
    leaf_points = tree.n("GeometryNodePoints", Count=2).outputs[0]
    index = _index(tree)
    sampled = tree.n("GeometryNodeSampleCurve", Curves=stem_curve, Factor=tree.math(
        "ADD", 0.12, tree.math("MULTIPLY", index, 0.18)))
    leaf_points = _move(tree, leaf_points, tree.out(sampled, "Position"))
    leaves = _instance(tree, leaf_points, leaf_shape, _euler(
        tree, 0.45, 0.0, tree.math("ADD", tree.math("MULTIPLY", index, math.pi),
                                   _random(tree, -1.25, -0.65, _seed(tree, seed, 1)))),
        tree.xyz(leaf_length, leaf_length, leaf_length))
    green = tree.smooth(tree.join(stem, tree.realize(leaves)))
    green = tree.material(green, stem_material)

    # The head is built facing up at the origin, then turned and moved onto the stem.
    center = _uv_sphere(tree, 16, 8, 1.0)
    center = tree.transform(center, scale=tree.vmath("SCALE", (1.0, 1.0, 0.45),
                                                     scale=tree.math("MULTIPLY", petal_length,
                                                                     0.38)))
    center = tree.material(tree.smooth(center), center_material)
    petal = _uv_sphere(tree, 10, 6)
    petal = tree.transform(petal, translation=(0.0, 0.95, 0.0), scale=(0.32, 1.0, 0.06))
    px, py, pz = tree.separate(_position(tree))
    petal = _move(tree, petal, tree.xyz(px, py, tree.math("ADD", pz, tree.math(
        "MULTIPLY", tree.math("MULTIPLY", py, py), 0.12))))
    ring = tree.n("GeometryNodePoints", Count=petals).outputs[0]
    turn = tree.math("MULTIPLY", _index(tree), tree.math("DIVIDE", 2.0 * math.pi, petals))
    petal_rotation = _euler(tree, _random(tree, 0.05, 0.25, _seed(tree, seed, 2)), 0.0,
                            tree.math("ADD", turn, _random(tree, -0.1, 0.1, _seed(tree, seed, 3))))
    ring = _instance(tree, ring, petal, petal_rotation, tree.xyz(
        petal_length, petal_length, petal_length))
    head_petals = tree.material(tree.smooth(tree.realize(ring)), material)
    head = tree.join(center, head_petals)
    facing = tree.vmath("NORMALIZE", tree.vmath("ADD", tree.vmath("SCALE", lean, scale=1.4),
                                                tree.xyz(z=stem_height)))
    turn_head = tree.n("FunctionNodeAlignRotationToVector", axis="Z", Vector=facing).outputs[0]
    head = tree.transform(head, translation=top, rotation=turn_head)

    tree.output(rest_on_ground(tree, tree.join(green, head)))
    return generator_object("Flower", tree)


@asset("Mushroom", "object", CATALOG,
       "A fairy-tale toadstool with a red, white-spotted cap on a stout white stem.",
       ("mushroom", "toadstool", "fungus", "fly agaric", "nature", "forest"))
def mushroom():
    tree = object_group("Mushroom")
    cap_size = tree.input("Cap Size", "FLOAT", 0.06, min=0.005, subtype="DISTANCE",
                          description="Radius of the cap")
    stem_height = tree.input("Stem Height", "FLOAT", 0.08, min=0.005, subtype="DISTANCE")
    spots = tree.input("Spots", "BOOL", True)
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL", palette("Starter Red"),
                          description="Material of the cap")
    stem_material = tree.input("Stem Material", "MATERIAL", _material(
        "Starter Mushroom Stem", (0.82, 0.78, 0.68), 0.6, Subsurface_Weight=0.2))
    spot_material = tree.input("Spot Material", "MATERIAL", palette("Starter White"))

    # Stem: a cylinder that bulges toward its base.
    stem_radius = tree.math("MULTIPLY", cap_size, 0.3)
    top = tree.math("ADD", stem_height, tree.math("MULTIPLY", cap_size, 0.1))
    stem = tree.n("GeometryNodeMeshCylinder", Vertices=20, Side_Segments=8, Radius=stem_radius,
                  Depth=top, fill_type="NGON").outputs["Mesh"]
    stem = tree.transform(stem, translation=tree.xyz(z=tree.math("MULTIPLY", top, 0.5)))
    sx, sy, sz = tree.separate(_position(tree))
    t = tree.math("DIVIDE", sz, top)
    bulge = tree.math("ADD", 1.0, tree.math("MULTIPLY", tree.math("POWER", tree.math(
        "SUBTRACT", 1.0, t, clamp=True), 3.0), 0.45))
    stem = _move(tree, stem, tree.xyz(tree.math("MULTIPLY", sx, bulge),
                                      tree.math("MULTIPLY", sy, bulge), sz))
    stem = tree.material(tree.smooth(stem), stem_material)

    # Cap: a sphere with a domed top and a nearly flat, cream underside.
    cap = _uv_sphere(tree, 32, 16, cap_size)
    cx, cy, cz = tree.separate(_position(tree))
    upper = tree.math("GREATER_THAN", cz, 0.0)
    cz = tree.math("MULTIPLY", cz, tree.math("ADD", 0.18, tree.math("MULTIPLY", upper, 0.52)))
    cap = _move(tree, cap, tree.xyz(cx, cy, cz))
    cap = tree.smooth(cap)
    _, _, nz = tree.separate(_normal(tree))
    cap = tree.material(cap, material)
    cap = tree.material(cap, stem_material, selection=tree.math("LESS_THAN", nz, -0.2))

    spot_points = tree.n("GeometryNodeDistributePointsOnFaces", distribute_method="POISSON",
                         Mesh=cap, Selection=tree.math("GREATER_THAN", nz, 0.25),
                         Distance_Min=tree.math("MULTIPLY", cap_size, 0.36),
                         Density_Max=tree.math("DIVIDE", 40.0, tree.math("MULTIPLY", cap_size,
                                                                          cap_size)),
                         Seed=seed)
    spot_size = tree.math("MULTIPLY", cap_size, _random(tree, 0.08, 0.14, _seed(tree, seed, 1)))
    spot_shapes = _instance(tree, tree.out(spot_points, "Points"), _ico(tree, 1.0, 2),
                            tree.out(spot_points, "Rotation"),
                            tree.xyz(spot_size, spot_size, tree.math("MULTIPLY", spot_size, 0.3)))
    spot_shapes = tree.material(tree.smooth(tree.realize(spot_shapes)), spot_material)
    cap = tree.join(cap, _switch(tree, "GEOMETRY", spots, None, spot_shapes))
    cap = tree.transform(cap, translation=tree.xyz(z=stem_height))

    tree.output(rest_on_ground(tree, tree.join(stem, cap)))
    return generator_object("Mushroom", tree)


# -----------------------------------------------------------------------------
# Landscapes, water and sky.

def _terrain_material():
    def build(tree, bsdf):
        coordinates = _object_coordinates(tree)
        patches = _noise(tree, coordinates, 0.4, detail=3.0)
        grass = _mix_color(tree, _smoothstep(tree, patches, 0.4, 0.65), (0.06, 0.16, 0.03),
                           (0.2, 0.26, 0.07))
        rock_color = _mix_color(tree, _smoothstep(tree, _noise(tree, coordinates, 1.2), 0.4, 0.6),
                                (0.17, 0.15, 0.13), (0.3, 0.28, 0.25))
        color = _mix_color(tree, _attribute(tree, "rock"), grass, rock_color)
        color = _mix_color(tree, _attribute(tree, "snow"), color, (0.85, 0.88, 0.92))
        tree.links.new(color, bsdf.inputs["Base Color"])
        roughness = _remap(tree, _attribute(tree, "snow"), 0.0, 1.0, 0.9, 0.45)
        tree.links.new(roughness, bsdf.inputs["Roughness"])
    return _material("Starter Terrain", (0.15, 0.24, 0.06), 0.9, build)


@asset("Terrain", "object", CATALOG,
       "A mountainous landscape tile with grassy valleys, rocky slopes and snowy peaks.",
       ("terrain", "landscape", "mountain", "hills", "snow", "nature"), preview_view="top")
def terrain():
    tree = object_group("Terrain")
    size = tree.input("Size", "FLOAT", 40.0, min=0.1, subtype="DISTANCE")
    height = tree.input("Height", "FLOAT", 9.0, min=0.0, subtype="DISTANCE",
                        description="Height of the highest peaks")
    ridges = tree.input("Ridges", "FLOAT", 0.6, min=0.0, max=1.0, subtype="FACTOR",
                        description="Blend from rolling hills to sharp mountain ridges")
    snow_line = tree.input("Snow Line", "FLOAT", 0.55, min=0.0, max=1.0, subtype="FACTOR",
                           description="Height, as a fraction of the peaks, above which "
                           "flat ground is covered in snow")
    seed = tree.input("Seed", "INT", 0)
    resolution = tree.input("Resolution", "INT", 128, min=2, max=1000,
                            description="Vertices along each side")
    material = tree.input("Material", "MATERIAL", _terrain_material())

    grid = tree.n("GeometryNodeMeshGrid", Size_X=size, Size_Y=size, Vertices_X=resolution,
                  Vertices_Y=resolution).outputs["Mesh"]
    uv = tree.vmath("DIVIDE", _position(tree), tree.xyz(size, size, 1.0))
    hills = _noise(tree, uv, 2.2, seed, detail=6.0, roughness=0.5)
    ridged = tree.math("SUBTRACT", 1.0, tree.math("ABSOLUTE", tree.math("SUBTRACT", tree.math(
        "MULTIPLY", _noise(tree, uv, 2.6, _seed(tree, seed, 7), detail=5.0, roughness=0.55), 2.0),
        1.0)))
    ridged = tree.math("MULTIPLY", tree.math("POWER", ridged, 2.0), 0.85)
    shape = tree.math("ADD", tree.math("MULTIPLY", hills, tree.math("SUBTRACT", 1.0, ridges)),
                      tree.math("MULTIPLY", ridged, ridges))
    # Raise the middle a little so the tile reads as a mountain range, not a flat plain.
    middle = tree.math("SUBTRACT", 1.0, tree.math("MULTIPLY", tree.vmath(
        "LENGTH", tree.vmath("MULTIPLY", uv, (1.0, 1.0, 0.0))), 1.6), clamp=True)
    shape = tree.math("ADD", shape, tree.math("MULTIPLY", middle, 0.25))
    shape = tree.math("POWER", _remap(tree, shape, 0.35, 1.05, 0.0, 1.0), 1.6)
    ux, uy, _ = tree.separate(_position(tree))
    mesh = _move(tree, grid, tree.xyz(ux, uy, tree.math("MULTIPLY", shape, height)))

    _, _, z = tree.separate(_position(tree))
    _, _, nz = tree.separate(_normal(tree))
    level = tree.math("DIVIDE", z, tree.math("MAXIMUM", height, 0.001))
    flat = _smoothstep(tree, nz, 0.55, 0.8)
    snow = tree.math("MULTIPLY", flat, _smoothstep(tree, level, tree.math(
        "SUBTRACT", snow_line, 0.05), tree.math("ADD", snow_line, 0.05)))
    mesh = _store(tree, mesh, "snow", snow)
    mesh = _store(tree, mesh, "rock", tree.math("SUBTRACT", 1.0, _smoothstep(tree, nz, 0.7, 0.88)))
    mesh = tree.material(tree.smooth(mesh), material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Terrain", tree)


def _ocean_material():
    def build(tree, bsdf):
        foam = _attribute(tree, "foam")
        speckle = _noise(tree, _object_coordinates(tree), 3.0, detail=4.0)
        foam = tree.math("MULTIPLY", foam, _smoothstep(tree, speckle, 0.35, 0.6))
        water = _mix_color(tree, _attribute(tree, "crest"), (0.004, 0.025, 0.055),
                           (0.015, 0.11, 0.14))
        color = _mix_color(tree, foam, water, (0.85, 0.9, 0.92))
        tree.links.new(color, bsdf.inputs["Base Color"])
        tree.links.new(_remap(tree, foam, 0.0, 1.0, 0.3, 0.6), bsdf.inputs["Roughness"])
    return _material("Starter Ocean", (0.01, 0.06, 0.09), 0.3, build, IOR=1.33)


@asset("Ocean", "object", CATALOG,
       "A square of open sea whose rolling waves move as the animation plays.",
       ("ocean", "sea", "water", "waves", "animated", "nature"), preview_view="top")
def ocean():
    tree = object_group("Ocean")
    size = tree.input("Size", "FLOAT", 20.0, min=0.1, subtype="DISTANCE")
    wave_height = tree.input("Wave Height", "FLOAT", 0.7, min=0.0, subtype="DISTANCE")
    choppiness = tree.input("Choppiness", "FLOAT", 0.6, min=0.0, max=1.0, subtype="FACTOR",
                            description="How sharp the wave crests are")
    speed = tree.input("Speed", "FLOAT", 1.0, min=0.0, description="Speed of the waves")
    resolution = tree.input("Resolution", "INT", 120, min=2, max=1000,
                            description="Vertices along each side")
    material = tree.input("Material", "MATERIAL", _ocean_material())

    grid = tree.n("GeometryNodeMeshGrid", Size_X=size, Size_Y=size, Vertices_X=resolution,
                  Vertices_Y=resolution).outputs["Mesh"]
    seconds = tree.math("MULTIPLY", tree.out(tree.n("GeometryNodeInputSceneTime"), "Seconds"),
                        speed)
    position = _position(tree)
    height = 0.0
    sideways = (0.0, 0.0, 0.0)
    # Sum of a few crossing Gerstner waves: each lifts and pushes points toward its crests.
    for angle, wavelength, amplitude in ((0.0, 0.42, 0.5), (35.0, 0.23, 0.25),
                                         (-50.0, 0.13, 0.14), (80.0, 0.07, 0.07)):
        direction = (math.cos(angle * RADIANS), math.sin(angle * RADIANS), 0.0)
        length = tree.math("MULTIPLY", size, wavelength)
        number = tree.math("DIVIDE", 2.0 * math.pi, length)
        frequency = tree.math("SQRT", tree.math("MULTIPLY", number, 9.81))
        phase = tree.math("SUBTRACT", tree.math("MULTIPLY", tree.vmath(
            "DOT_PRODUCT", position, direction), number), tree.math(
            "MULTIPLY", frequency, seconds))
        lift = tree.math("MULTIPLY", wave_height, amplitude)
        height = tree.math("ADD", height, tree.math("MULTIPLY", tree.math("SINE", phase), lift))
        push = tree.math("MULTIPLY", tree.math("MULTIPLY", tree.math("COSINE", phase), lift),
                         tree.math("MULTIPLY", choppiness, -1.2))
        sideways = tree.vmath("ADD", sideways, tree.vmath("SCALE", direction, scale=push))
    ripples = tree.math("SUBTRACT", _noise(tree, tree.vmath("DIVIDE", position, tree.xyz(
        size, size, 1.0)), 18.0, tree.math("MULTIPLY", seconds, 0.4), detail=3.0), 0.5)
    height = tree.math("ADD", height, tree.math("MULTIPLY", ripples, tree.math(
        "MULTIPLY", wave_height, 0.12)))
    mesh = _offset(tree, grid, tree.vmath("ADD", sideways, tree.xyz(z=height)))

    crest = tree.math("DIVIDE", height, tree.math("MAXIMUM", wave_height, 0.001))
    mesh = _store(tree, mesh, "crest", _smoothstep(tree, crest, -0.2, 0.8))
    mesh = _store(tree, mesh, "foam", _smoothstep(tree, crest, tree.math(
        "SUBTRACT", 1.1, tree.math("MULTIPLY", choppiness, 0.25)), 1.6))
    mesh = tree.material(tree.smooth(mesh), material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Ocean", tree)


@asset("Cloud", "object", CATALOG,
       "A puffy cartoon cloud with a flat underside, made of merged round billows.",
       ("cloud", "sky", "weather", "cartoon", "stylized", "nature"))
def cloud():
    tree = object_group("Cloud")
    size = tree.input("Size", "FLOAT", 3.0, min=0.05, subtype="DISTANCE",
                      description="Width of the cloud")
    puffiness = tree.input("Puffiness", "FLOAT", 0.5, min=0.0, max=1.0, subtype="FACTOR",
                           description="Number of smaller billows heaped on top")
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL", _material(
        "Starter Cloud", (0.9, 0.92, 0.95), 0.9, Subsurface_Weight=0.3, Sheen_Weight=0.4,
        Subsurface_Radius=(0.5, 0.5, 0.6)))

    # A row of big billows along X, centered on the cloud's spine.
    rows = 5
    index = _index(tree)
    along = tree.math("SUBTRACT", tree.math("DIVIDE", index, rows - 1), 0.5)
    sides = tree.math("SUBTRACT", 1.0, tree.math("MULTIPLY", tree.math("ABSOLUTE", along), 1.1))
    base_radius = tree.math("MULTIPLY", size, tree.math("MULTIPLY", sides, _random(
        tree, 0.14, 0.2, seed)))
    base = tree.xyz(tree.math("MULTIPLY", along, tree.math("MULTIPLY", size, 0.7)),
                    tree.math("MULTIPLY", size, _random(tree, -0.04, 0.04, _seed(tree, seed, 1))),
                    tree.math("MULTIPLY", size, _random(tree, -0.02, 0.06, _seed(tree, seed, 2))))
    big = tree.n("GeometryNodePoints", Count=rows, Position=base, Radius=base_radius).outputs[0]
    # Smaller billows heaped on top.
    heaped = tree.math("ROUND", tree.math("MULTIPLY", puffiness, 12.0))
    top_x = _random(tree, -0.26, 0.26, _seed(tree, seed, 3))
    top_radius = tree.math("MULTIPLY", size, tree.math("MULTIPLY", _random(
        tree, 0.11, 0.17, _seed(tree, seed, 4)), tree.math("SUBTRACT", 1.0, tree.math(
            "ABSOLUTE", top_x))))
    top = tree.xyz(tree.math("MULTIPLY", top_x, size),
                   tree.math("MULTIPLY", size, _random(tree, -0.1, 0.1, _seed(tree, seed, 5))),
                   tree.math("MULTIPLY", size, _random(tree, 0.1, 0.2, _seed(tree, seed, 6))))
    small = tree.n("GeometryNodePoints", Count=heaped, Position=top, Radius=top_radius).outputs[0]
    billows = tree.join(big, small)
    radius = tree.n("GeometryNodeInputRadius").outputs[0]
    spheres = tree.realize(_instance(tree, billows, _ico(tree, 1.0, 3),
                                     scale=tree.xyz(radius, radius, radius)))

    # Shrink-wrap an ellipsoid onto the billows, aiming every ray at the cloud's spine, so they
    # merge into one smooth surface like metaballs.
    shell = _ico(tree, 1.0, 6)
    shell = tree.transform(shell, translation=tree.xyz(z=tree.math("MULTIPLY", size, 0.1)),
                           scale=tree.vmath("SCALE", (0.75, 0.5, 0.55), scale=size))
    sx, _, _ = tree.separate(_position(tree))
    limit = tree.math("MULTIPLY", size, 0.3)
    spine = tree.xyz(tree.math("MINIMUM", tree.math("MAXIMUM", tree.math(
        "MULTIPLY", sx, 0.8), tree.math("MULTIPLY", limit, -1.0)), limit), 0.0,
        tree.math("MULTIPLY", size, 0.02))
    aim = tree.vmath("SUBTRACT", spine, _position(tree))
    ray = tree.n("GeometryNodeRaycast", Target_Geometry=spheres, Source_Position=_position(tree),
                 Ray_Direction=tree.vmath("NORMALIZE", aim), Ray_Length=tree.math(
                     "MULTIPLY", size, 2.0))
    hit = tree.out(ray, "Is Hit")
    landing = _switch(tree, "VECTOR", hit, spine, tree.out(ray, "Hit Position"))
    mesh = _move(tree, shell, landing)
    # Soften the creases between billows a little.
    blur = tree.n("GeometryNodeBlurAttribute", data_type="FLOAT_VECTOR", Value=_position(tree),
                  Iterations=1)
    mesh = _move(tree, mesh, tree.out(blur, "Value"))
    x, y, z = tree.separate(_position(tree))
    floor = tree.math("MULTIPLY", size, -0.1)
    mesh = _move(tree, mesh, tree.xyz(x, y, tree.math("MAXIMUM", z, floor)))
    mesh = tree.material(tree.smooth(mesh), material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Cloud", tree)


def _planet_land_material():
    def build(tree, bsdf):
        elevation = _attribute(tree, "elevation")
        ramp = tree.n("ShaderNodeValToRGB", Factor=elevation)
        elements = ramp.color_ramp.elements
        elements[0].position = 0.0
        elements[0].color = (0.55, 0.48, 0.3, 1.0)
        elements[1].position = 1.0
        elements[1].color = (0.9, 0.92, 0.95, 1.0)
        for position, color in ((0.04, (0.08, 0.25, 0.05)), (0.5, (0.05, 0.15, 0.03)),
                                (0.72, (0.2, 0.14, 0.08)), (0.88, (0.3, 0.28, 0.26))):
            element = elements.new(position)
            element.color = (*color, 1.0)
        tree.links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    return _material("Starter Planet Land", (0.15, 0.3, 0.08), 0.85, build)


def _planet_ocean_material():
    def build(tree, bsdf):
        color = _mix_color(tree, _smoothstep(tree, _attribute(tree, "depth"), 0.0, 0.08),
                           (0.03, 0.3, 0.42), (0.01, 0.05, 0.2))
        tree.links.new(color, bsdf.inputs["Base Color"])
    return _material("Starter Planet Ocean", (0.01, 0.06, 0.22), 0.15, build)


@asset("Planet", "object", CATALOG,
       "A small world with continents, mountains, craters and oceans, shown as a globe.",
       ("planet", "globe", "world", "earth", "moon", "space"))
def planet():
    tree = object_group("Planet")
    radius = tree.input("Radius", "FLOAT", 0.5, min=0.01, subtype="DISTANCE")
    sea_level = tree.input("Sea Level", "FLOAT", 0.5, min=0.0, max=1.0, subtype="FACTOR",
                           description="How much of the surface is under water")
    continents = tree.input("Continents", "FLOAT", 1.4, min=0.1, max=10.0,
                            description="Scale of the land masses: more gives smaller continents")
    craters = tree.input("Craters", "FLOAT", 0.3, min=0.0, max=1.0, subtype="FACTOR",
                         description="Depth of the impact craters on land")
    seed = tree.input("Seed", "INT", 0)
    land_material = tree.input("Material", "MATERIAL", _planet_land_material())
    ocean_material = tree.input("Ocean Material", "MATERIAL", _planet_ocean_material())

    sphere = _ico(tree, 1.0, 6)
    direction = tree.vmath("NORMALIZE", _position(tree))
    terrain = _noise(tree, direction, continents, seed, detail=7.0, roughness=0.55)
    threshold = tree.math("ADD", 0.38, tree.math("MULTIPLY", sea_level, 0.24))
    above = tree.math("SUBTRACT", terrain, threshold)
    elevation = _remap(tree, above, 0.0, 0.25, 0.0, 1.0)
    land = tree.math("GREATER_THAN", above, 0.0)

    cells = tree.n("ShaderNodeTexVoronoi", voronoi_dimensions="4D", Vector=direction,
                   W=tree.math("MULTIPLY", seed, 1.37), Scale=4.0)
    distance = tree.out(cells, "Distance")
    size = tree.math("ADD", 0.12, tree.math("MULTIPLY", tree.out(tree.n(
        "FunctionNodeSeparateColor", Color=tree.out(cells, "Color")), "Red"), 0.18))
    bowl = tree.math("SUBTRACT", tree.math("POWER", tree.math("DIVIDE", distance, size), 2.0), 1.0)
    rim = tree.math("MULTIPLY", tree.math("EXPONENT", tree.math("MULTIPLY", tree.math(
        "POWER", tree.math("DIVIDE", tree.math("SUBTRACT", distance, size), 0.05), 2.0), -1.0)),
        0.35)
    inside = tree.math("LESS_THAN", distance, size)
    crater = _switch(tree, "FLOAT", inside, rim, tree.math("MINIMUM", bowl, 0.0))
    crater = tree.math("MULTIPLY", crater, tree.math("MULTIPLY", craters, 0.012))

    lift = tree.math("ADD", tree.math("ADD", 0.004, tree.math("MULTIPLY", elevation, 0.045)),
                     crater)
    lift = tree.math("MULTIPLY", lift, land)
    surface = tree.vmath("SCALE", direction, scale=tree.math("MULTIPLY", radius, tree.math(
        "ADD", 1.0, lift)))
    mesh = _move(tree, sphere, surface)
    mesh = _store(tree, mesh, "elevation", tree.math("ADD", elevation, tree.math(
        "MULTIPLY", crater, -8.0)))
    mesh = _store(tree, mesh, "depth", tree.math("MULTIPLY", above, -1.0))
    mesh = _store(tree, mesh, "land", land)
    land_attribute = tree.out(tree.n("GeometryNodeInputNamedAttribute", data_type="FLOAT",
                                     Name="land"), "Attribute")
    mesh = tree.material(mesh, land_material)
    mesh = tree.material(mesh, ocean_material,
                         selection=tree.math("LESS_THAN", land_attribute, 0.5))
    mesh = tree.smooth(mesh)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Planet", tree)
