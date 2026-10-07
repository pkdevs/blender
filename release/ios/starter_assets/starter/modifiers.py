# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Modifiers: Geometry Nodes groups to drop onto any object, in the Generate and Deform catalogs.

They merge with Blender's own Essentials catalogs of the same name in the Add Modifier menu.
Each works on any mesh: sizes that would depend on the object are relative to its bounding box,
and generated amounts are capped so a large object never produces millions of elements.
"""

import math

from .common import (AXES, RADIANS, angle_gizmo, count_gizmo, density_gizmo, modifier_group,
                     palette, resolution_gizmo, simple_material)
from .registry import asset

GENERATE = "Generate"
DEFORM = "Deform"

# Upper bound on generated points (instances, hairs), whatever the object's size.
MAX_POINTS = 5000
# Hairs are 8 triangles each.
MAX_HAIRS = 15000


# -----------------------------------------------------------------------------
# Helpers.

def position(tree):
    return tree.n("GeometryNodeInputPosition").outputs[0]


def normal(tree):
    return tree.n("GeometryNodeInputNormal").outputs[0]


def largest_dimension(tree, geometry):
    """The largest side of the geometry's bounding box, never zero."""
    bounds = tree.n("GeometryNodeBoundBox", Geometry=geometry)
    size = tree.vmath("SUBTRACT", tree.out(bounds, "Max"), tree.out(bounds, "Min"))
    x, y, z = tree.separate(size)
    return tree.math("MAXIMUM", tree.math("MAXIMUM", tree.math("MAXIMUM", x, y), z), 1e-4)


class Bounds:
    """The geometry's bounding box, for placing gizmos on the object whatever its shape."""

    def __init__(self, tree, geometry):
        self.tree = tree
        node = tree.n("GeometryNodeBoundBox", Geometry=geometry)
        self.low = tree.out(node, "Min")
        self.high = tree.out(node, "Max")
        self.center = tree.vmath("SCALE", tree.vmath("ADD", self.low, self.high), scale=0.5)

    def at(self, x=0.0, y=0.0, z=0.0):
        """A point on the box: -1, 0 and 1 are its low side, middle and high side on each axis."""
        half = self.tree.vmath("SCALE", self.tree.vmath("SUBTRACT", self.high, self.low),
                               scale=0.5)
        return self.tree.vmath("ADD", self.center,
                               self.tree.vmath("MULTIPLY", half, (x, y, z)))


def along(tree, origin, axis, distance):
    """origin + axis * distance; `axis` is a name from AXES or a direction socket."""
    direction = AXES[axis] if isinstance(axis, str) else axis
    return tree.vmath("ADD", origin, tree.vmath("SCALE", direction, scale=distance))


def arrow(tree, value, origin, axis, color="PRIMARY"):
    """An arrow `value` away from `origin` along `axis`, dragging adds the distance to `value`."""
    direction = AXES[axis] if isinstance(axis, str) else axis
    return tree.linear_gizmo(value, along(tree, origin, axis, value), direction, color=color)


def within(tree, value, largest, fraction):
    """Limit a size to a fraction of the object's size, so small objects aren't swamped."""
    limit = tree.math("MULTIPLY", largest, fraction)
    return tree.math("MINIMUM", tree.math("MAXIMUM", value, tree.math("MULTIPLY", limit, -1.0)),
                     limit)


def subdivide_to(tree, mesh, faces, extra=0):
    """Subdivide a coarse mesh until it has about `faces` faces (plus `extra` levels)."""
    count = tree.out(tree.n("GeometryNodeAttributeDomainSize", component="MESH", Geometry=mesh),
                     "Face Count")
    needed = tree.math("LOGARITHM", tree.math("DIVIDE", faces, tree.math("MAXIMUM", count, 1)),
                       4.0)
    level = tree.math("MAXIMUM", tree.math("FLOOR", needed), 0.0)
    level = tree.math("MINIMUM", tree.math("ADD", level, extra), 6.0)
    return tree.n("GeometryNodeSubdivideMesh", Mesh=mesh, Level=level).outputs[0]


def surface_area(tree, geometry):
    area = tree.n("GeometryNodeInputMeshFaceArea").outputs[0]
    statistic = tree.n("GeometryNodeAttributeStatistic", Geometry=geometry, Attribute=area,
                       data_type="FLOAT", domain="FACE")
    return tree.math("MAXIMUM", tree.out(statistic, "Sum"), 1e-6)


def capped_density(tree, geometry, density, limit=MAX_POINTS):
    """Points per square meter, lowered so the whole surface never gets more than `limit`."""
    return tree.math("MINIMUM", density, tree.math("DIVIDE", limit, surface_area(tree, geometry)))


def constant_random_vector(tree, seed, low=0.0, high=1.0):
    """One random vector per seed (the same for every element)."""
    zero = tree.n("FunctionNodeInputInt", integer=0).outputs[0]
    node = tree.n("FunctionNodeRandomValue", data_type="FLOAT_VECTOR", Min=(low,) * 3,
                  Max=(high,) * 3, ID=zero, Seed=seed)
    return tree.out(node, "Value")


def random_float(tree, low, high, seed, offset=0):
    node = tree.n("FunctionNodeRandomValue", data_type="FLOAT", Min=low, Max=high,
                  Seed=tree.math("ADD", seed, offset))
    return tree.out(node, "Value")


def store(tree, geometry, name, value, data_type="FLOAT", domain="POINT"):
    return tree.n("GeometryNodeStoreNamedAttribute", Geometry=geometry, Name=name, Value=value,
                  data_type=data_type, domain=domain).outputs[0]


def named(tree, name, data_type="FLOAT"):
    return tree.n("GeometryNodeInputNamedAttribute", Name=name, data_type=data_type).outputs[0]


def mix(tree, a, b, factor, data_type="FLOAT"):
    return tree.out(tree.n("ShaderNodeMix", data_type=data_type, Factor=factor, A=a, B=b),
                    "Result")


def add_to(tree, original, *added):
    """Join new geometry to the original, keeping the original's material order first.

    Join lists its last input first, so the original goes in last.
    """
    return tree.join(*added, original)


def keep_materials(tree, original, mesh):
    """Give a newly built mesh the original's materials, each face the one of the nearest face.

    A mesh made by nodes has no materials of its own. One vertex of the original carries its
    material list through the join and is removed again afterwards.
    """
    index = tree.n("GeometryNodeInputIndex").outputs[0]
    anchor = tree.n("GeometryNodeDeleteGeometry", Geometry=original, domain="POINT",
                    Selection=tree.compare("GREATER_THAN", index, 0, data_type="INT")).outputs[0]
    material_index = tree.n("GeometryNodeInputMaterialIndex").outputs[0]
    nearest = tree.n("GeometryNodeSampleNearestSurface", Mesh=original, Value=material_index,
                     data_type="INT")
    joined = add_to(tree, anchor, mesh)
    joined = tree.n("GeometryNodeSetMaterialIndex", Geometry=joined,
                    Material_Index=tree.out(nearest, "Value")).outputs[0]
    loose = tree.compare("EQUAL", tree.out(tree.n("GeometryNodeInputMeshVertexNeighbors"),
                                           "Vertex Count"), 0, data_type="INT")
    return tree.n("GeometryNodeDeleteGeometry", Geometry=joined, domain="POINT",
                  Selection=loose).outputs[0]


# -----------------------------------------------------------------------------
# Generate.

def _pebbles(tree, material):
    """Three small rounded stones as instances, shown when no collection is chosen."""
    stones = []
    for variant, squash in enumerate(((1.0, 0.8, 0.55), (0.8, 1.0, 0.6), (1.0, 0.9, 0.75))):
        stone = tree.n("GeometryNodeMeshIcoSphere", Radius=0.04, Subdivisions=2).outputs["Mesh"]
        noise = tree.n("ShaderNodeTexNoise", noise_dimensions="3D", Scale=1.6, Detail=1.0,
                       Vector=tree.vmath("ADD", tree.vmath("SCALE", position(tree), scale=25.0),
                                         (variant * 7.3, variant * 3.1, 0.0)))
        bump = tree.math("MULTIPLY", tree.math("SUBTRACT", tree.out(noise, "Factor"), 0.5), 0.05)
        stone = tree.n("GeometryNodeSetPosition", Geometry=stone,
                       Offset=tree.vmath("SCALE", normal(tree), scale=bump)).outputs[0]
        # Squashed and sunk a little into the surface so they rest on it.
        stone = tree.transform(stone, translation=(0.0, 0.0, 0.04 * squash[2] * 0.6),
                               scale=squash)
        # Materials must be set before the geometry becomes an instance.
        stone = tree.material(tree.smooth(stone), material)
        stones.append(tree.n("GeometryNodeGeometryToInstance", Geometry=stone).outputs[0])
    return tree.join(*stones)


@asset("Scatter on Surface", "modifier", GENERATE,
       "Scatters a collection's objects over the surface, or small pebbles when none is chosen.",
       ("scatter", "instance", "distribute", "collection", "rocks", "pebbles"),
       preview_base="plane")
def scatter_on_surface():
    tree = modifier_group("Scatter on Surface")
    geometry = tree.group_in.outputs["Geometry"]
    collection = tree.input("Collection", "COLLECTION",
                            description="Objects to scatter, picked at random. Pebbles if empty")
    density = tree.input("Density", "FLOAT", 40.0, min=0.0, max=10000.0,
                         description="Instances per square meter of surface")
    scale_min = tree.input("Scale Min", "FLOAT", 0.6, min=0.0, max=100.0)
    scale_max = tree.input("Scale Max", "FLOAT", 1.4, min=0.0, max=100.0)
    align = tree.input("Align to Normal", "BOOL", True,
                       description="Tilt instances to follow the surface instead of standing up")
    material = tree.input("Pebble Material", "MATERIAL", palette("Starter Stone"),
                          description="Material of the built-in pebbles")
    seed = tree.input("Seed", "INT", 0, min=0)

    distribute = tree.n("GeometryNodeDistributePointsOnFaces", Mesh=geometry,
                        distribute_method="RANDOM", Seed=seed,
                        Density=capped_density(tree, geometry, density))
    points = tree.out(distribute, "Points")
    spin = tree.n("FunctionNodeAxisAngleToRotation", Axis=(0.0, 0.0, 1.0),
                  Angle=random_float(tree, 0.0, 2.0 * math.pi, seed, 1)).outputs[0]
    aligned = tree.n("FunctionNodeRotateRotation", rotation_space="LOCAL",
                     Rotation=tree.out(distribute, "Rotation"), Rotate_By=spin).outputs[0]
    rotation = tree.n("GeometryNodeSwitch", input_type="ROTATION", Switch=align,
                      **{"False": spin, "True": aligned})
    scale = random_float(tree, scale_min, scale_max, seed, 2)

    objects = tree.n("GeometryNodeCollectionInfo", transform_space="ORIGINAL",
                     Collection=collection, Separate_Children=True, Reset_Children=True)
    count = tree.n("GeometryNodeAttributeDomainSize", component="INSTANCES",
                   Geometry=objects.outputs[0])
    has_objects = tree.compare("GREATER_THAN", tree.out(count, "Instance Count"), 0,
                               data_type="INT")
    instance = tree.n("GeometryNodeSwitch", input_type="GEOMETRY", Switch=has_objects,
                      **{"False": _pebbles(tree, material), "True": objects.outputs[0]})
    pick = tree.n("FunctionNodeRandomValue", data_type="INT", Min=0, Max=1000,
                  Seed=tree.math("ADD", seed, 3))
    instances = tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=instance,
                       Pick_Instance=True, Instance_Index=tree.out(pick, "Value"),
                       Rotation=rotation.outputs[0], Scale=scale).outputs[0]
    # Gizmos: density is pulled out along X past the side, about one meter of drag per 200
    # instances per square meter on a one meter object; the scales stand above the top.
    bounds = Bounds(tree, geometry)
    largest = largest_dimension(tree, geometry)
    tree.linear_gizmo(tree.math("MULTIPLY", density, tree.math("DIVIDE", largest, 200.0)),
                      along(tree, bounds.at(1.0, 0.0, 1.0), "X",
                            tree.math("MULTIPLY", density, tree.math("DIVIDE", largest, 200.0))),
                      AXES["X"], color="SECONDARY", style="BOX")
    arrow(tree, tree.math("MULTIPLY", scale_min, tree.math("MULTIPLY", largest, 0.1)),
          bounds.at(-0.5, 0.0, 1.0), "Z")
    arrow(tree, tree.math("MULTIPLY", scale_max, tree.math("MULTIPLY", largest, 0.1)),
          bounds.at(0.5, 0.0, 1.0), "Z")
    tree.output(add_to(tree, geometry, instances))
    tree.layout()
    return tree.nt


def _growth(tree, surface_normal, seed):
    """Hairs grow out of the surface but lean downhill, so the fur lies down like a pelt.

    A random lean on top keeps hairs on flat tops from standing straight up.
    """
    down = (0.0, 0.0, -1.0)
    along = tree.vmath("DOT_PRODUCT", surface_normal, down)
    downhill = tree.vmath("SUBTRACT", down, tree.vmath("SCALE", surface_normal, scale=along))
    jitter = tree.out(tree.n("FunctionNodeRandomValue", data_type="FLOAT_VECTOR",
                             Min=(-1.0, -1.0, -1.0), Max=(1.0, 1.0, 1.0), Seed=seed), "Value")
    lean = tree.vmath("ADD", tree.vmath("SCALE", downhill, scale=1.5), jitter)
    return tree.vmath("NORMALIZE", tree.vmath("ADD", surface_normal, lean))


@asset("Fur", "modifier", GENERATE,
       "Grows soft, clumpy fur over the surface, with its own fur material.",
       ("fur", "hair", "fuzz", "grass", "strands"), preview_base="sphere")
def fur():
    tree = modifier_group("Fur")
    geometry = tree.group_in.outputs["Geometry"]
    density = tree.input("Density", "FLOAT", 4500.0, min=0.0, max=100000.0,
                         description="Hairs per square meter of surface")
    length = tree.input("Length", "FLOAT", 0.08, min=0.0, max=10.0, subtype="DISTANCE",
                        description="Hair length, at most half the object's size")
    thickness = tree.input("Thickness", "FLOAT", 0.016, min=0.0, max=1.0, subtype="DISTANCE",
                           description="Width of each hair at its root")
    clump = tree.input("Clump", "FLOAT", 0.25, min=0.0, max=1.0, subtype="FACTOR",
                       description="How much neighboring hairs gather into tufts")
    curl = tree.input("Curl", "FLOAT", 0.25, min=0.0, max=1.0, subtype="FACTOR")
    material = tree.input("Fur Material", "MATERIAL",
                          simple_material("Starter Fur", (0.32, 0.18, 0.08), roughness=0.6))
    seed = tree.input("Seed", "INT", 0, min=0)

    hair_density = capped_density(tree, geometry, density, MAX_HAIRS)
    distribute = tree.n("GeometryNodeDistributePointsOnFaces", Mesh=geometry, Seed=seed,
                        distribute_method="RANDOM", Density=hair_density)
    roots = store(tree, tree.out(distribute, "Points"), "fur_normal",
                  _growth(tree, tree.out(distribute, "Normal"), seed), "FLOAT_VECTOR")
    # Fewer guide hairs that the others gather around.
    guides = tree.n("GeometryNodeDistributePointsOnFaces", Mesh=geometry,
                    Seed=tree.math("ADD", seed, 7), distribute_method="RANDOM",
                    Density=tree.math("DIVIDE", hair_density, 25.0))
    guide_points = tree.out(guides, "Points")
    guide_normals = store(tree, guide_points, "fur_normal",
                          _growth(tree, tree.out(guides, "Normal"), seed), "FLOAT_VECTOR")
    nearest = tree.n("GeometryNodeSampleNearest", Geometry=guide_points,
                     Sample_Position=position(tree)).outputs[0]
    guide_root = tree.n("GeometryNodeSampleIndex", Geometry=guide_points, data_type="FLOAT_VECTOR",
                        domain="POINT", Value=position(tree), Index=nearest).outputs[0]
    guide_normal = tree.n("GeometryNodeSampleIndex", Geometry=guide_normals,
                          data_type="FLOAT_VECTOR", domain="POINT",
                          Value=named(tree, "fur_normal", "FLOAT_VECTOR"),
                          Index=nearest).outputs[0]
    guide_count = tree.out(tree.n("GeometryNodeAttributeDomainSize", component="POINTCLOUD",
                                  Geometry=guide_points), "Point Count")
    has_guides = tree.compare("GREATER_THAN", guide_count, 0, data_type="INT")
    roots = store(tree, roots, "fur_surface", tree.out(distribute, "Normal"), "FLOAT_VECTOR")
    roots = store(tree, roots, "fur_root", position(tree), "FLOAT_VECTOR")
    roots = store(tree, roots, "fur_guide", guide_root, "FLOAT_VECTOR")
    roots = store(tree, roots, "fur_guide_normal", guide_normal, "FLOAT_VECTOR")
    roots = store(tree, roots, "fur_length", random_float(tree, 0.7, 1.0, seed, 11))
    roots = store(tree, roots, "fur_phase", random_float(tree, 0.0, 2.0 * math.pi, seed, 12))

    line = tree.n("GeometryNodeCurvePrimitiveLine", Start=(0.0, 0.0, 0.0),
                  End=(0.0, 0.0, 1.0)).outputs[0]
    line = tree.n("GeometryNodeResampleCurve", Curve=line, Mode="Count", Count=5).outputs[0]
    hairs = tree.realize(tree.n("GeometryNodeInstanceOnPoints", Points=roots,
                                Instance=line).outputs[0])

    t = tree.out(tree.n("GeometryNodeSplineParameter"), "Factor")
    hair_normal = named(tree, "fur_normal", "FLOAT_VECTOR")
    largest = largest_dimension(tree, geometry)
    reach_limit = within(tree, length, largest, 0.5)
    hair_length = tree.math("MULTIPLY", reach_limit, named(tree, "fur_length"))
    reach = tree.math("MULTIPLY", hair_length, t)
    straight = tree.vmath("ADD", named(tree, "fur_root", "FLOAT_VECTOR"),
                          tree.vmath("SCALE", hair_normal, scale=reach))
    tuft = tree.vmath("ADD", named(tree, "fur_guide", "FLOAT_VECTOR"),
                      tree.vmath("SCALE", named(tree, "fur_guide_normal", "FLOAT_VECTOR"),
                                 scale=reach))
    gather = tree.math("MULTIPLY", tree.math("MULTIPLY", clump, t), has_guides)
    shaped = mix(tree, straight, tuft, gather, "VECTOR")
    # Curl: circle around the hair's direction, growing toward the tip.
    side = tree.vmath("NORMALIZE", tree.vmath("CROSS_PRODUCT", hair_normal, (0.37, 0.48, 0.79)))
    other = tree.vmath("CROSS_PRODUCT", hair_normal, side)
    angle = tree.math("ADD", tree.math("MULTIPLY", t, 9.0), named(tree, "fur_phase"))
    swirl = tree.vmath("ADD", tree.vmath("SCALE", side, scale=tree.math("COSINE", angle)),
                       tree.vmath("SCALE", other, scale=tree.math("SINE", angle)))
    swirl = tree.vmath("SCALE", swirl, scale=tree.math("MULTIPLY", tree.math(
        "MULTIPLY", curl, 0.35), reach))
    # A little gravity so the fur looks soft.
    droop = tree.xyz(z=tree.math("MULTIPLY", tree.math("MULTIPLY", reach, t), -0.25))
    shaped = tree.vmath("ADD", tree.vmath("ADD", shaped, swirl), droop)
    hairs = tree.n("GeometryNodeSetPosition", Geometry=hairs, Position=shaped).outputs[0]
    # Turn each ribbon to face out of the surface, so it covers it like a pelt.
    across = tree.vmath("CROSS_PRODUCT", hair_normal, named(tree, "fur_surface", "FLOAT_VECTOR"))
    hairs = tree.n("GeometryNodeSetCurveNormal", Curve=hairs, Mode="Free",
                   Normal=tree.vmath("NORMALIZE", across)).outputs[0]

    # Flat ribbons: a third of the triangles of round strands, so the fur can be denser.
    profile = tree.n("GeometryNodeCurvePrimitiveLine", Start=(-0.5, 0.0, 0.0),
                     End=(0.5, 0.0, 0.0)).outputs[0]
    taper = tree.math("MULTIPLY", thickness, tree.math("SUBTRACT", 1.0, tree.math(
        "MULTIPLY", t, 0.9)))
    strands = tree.n("GeometryNodeCurveToMesh", Curve=hairs, Profile_Curve=profile,
                     Scale=taper).outputs[0]
    strands = tree.material(tree.smooth(strands), material)

    # Gizmos at the tips of the hairs on top and on the sides, where they show.
    bounds = Bounds(tree, geometry)
    top = bounds.at(z=1.0)
    tree.linear_gizmo(tree.math("MULTIPLY", length, 1.0), along(tree, top, "Z", reach_limit),
                      AXES["Z"])
    side_tip = along(tree, bounds.at(x=1.0), "X", reach_limit)
    tree.linear_gizmo(tree.math("MULTIPLY", thickness, 0.5),
                      along(tree, side_tip, "Y", tree.math("MULTIPLY", thickness, 0.5)),
                      AXES["Y"], color="Y")
    # One turn goes from straight to fully curled; at the tips on the -Y side.
    tree.dial_gizmo(tree.math("MULTIPLY", curl, math.tau),
                    along(tree, bounds.at(y=-1.0), "-Y", reach_limit), AXES["Y"], radius=0.6)
    # Density pulls out of the -X side: across the whole object for 10000 hairs per square meter.
    spread = tree.math("MULTIPLY", density, tree.math("DIVIDE", largest, 10000.0))
    tree.linear_gizmo(spread, along(tree, bounds.at(x=-1.0), "-X", spread), AXES["-X"],
                      color="SECONDARY", style="BOX")
    tree.output(add_to(tree, geometry, strands))
    tree.layout()
    return tree.nt


@asset("Radial Array", "modifier", GENERATE,
       "Repeats the object in a ring around its origin, like a wreath or the petals of a flower.",
       ("array", "radial", "circular", "repeat", "ring"), preview_base="monkey")
def radial_array():
    tree = modifier_group("Radial Array")
    geometry = tree.group_in.outputs["Geometry"]
    count = tree.input("Count", "INT", 6, min=1, max=256, description="Number of copies")
    radius = tree.input("Radius", "FLOAT", 1.0, min=0.0, max=1000.0, subtype="DISTANCE",
                        description="Distance of each copy from the center")
    axis_menu = tree.menu("Axis", ["X", "Y", "Z"], default="Z",
                          description="Axis the copies are arranged around")
    axis = tree.switch(axis_menu, "VECTOR", {"X": (1.0, 0.0, 0.0), "Y": (0.0, 1.0, 0.0),
                                             "Z": (0.0, 0.0, 1.0)})
    # The direction the first copy moves in, perpendicular to the axis: -Y, or X around Y.
    # (Derived from the one switch: menus driving several switches lose their default.)
    _, _, axis_z = tree.separate(axis)
    away = tree.vmath("ADD", tree.vmath("CROSS_PRODUCT", axis, (0.0, 0.0, 1.0)),
                      tree.xyz(y=tree.math("MULTIPLY", axis_z, -1.0)))

    index = tree.n("GeometryNodeInputIndex").outputs[0]
    angle = tree.math("MULTIPLY", tree.math("DIVIDE", index, count), 2.0 * math.pi)
    rotation = tree.n("FunctionNodeAxisAngleToRotation", Axis=axis, Angle=angle).outputs[0]
    offset = tree.n("FunctionNodeRotateVector", Rotation=rotation,
                    Vector=tree.vmath("SCALE", away, scale=radius)).outputs[0]
    points = tree.n("GeometryNodeMeshLine", count_mode="TOTAL", Count=count).outputs[0]
    points = tree.n("GeometryNodeSetPosition", Geometry=points, Position=offset).outputs[0]
    copies = tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=geometry,
                    Rotation=rotation).outputs[0]
    # The radius arrow sits at the first copy's origin; turning the dial around the axis by one
    # turn adds twelve copies.
    tree.linear_gizmo(tree.math("MULTIPLY", radius, 1.0),
                      tree.vmath("SCALE", away, scale=radius), away)
    tree.dial_gizmo(tree.math("MULTIPLY", count, math.tau / 12.0), (0.0, 0.0, 0.0), axis,
                    radius=0.6, color="SECONDARY")
    tree.output(tree.realize(copies))
    tree.layout()
    return tree.nt


@asset("Tube Frame", "modifier", GENERATE,
       "Turns every edge into a tube and every vertex into a ball, for lattices and wireframes.",
       ("wireframe", "lattice", "tube", "frame", "edges", "structure"), preview_base="torus")
def tube_frame():
    tree = modifier_group("Tube Frame")
    geometry = tree.group_in.outputs["Geometry"]
    radius = tree.input("Tube Radius", "FLOAT", 0.008, min=0.0, max=10.0, subtype="DISTANCE",
                        description="Radius of the tubes, at most 2% of the object's size")
    joint = tree.input("Joint Size", "FLOAT", 0.016, min=0.0, max=10.0, subtype="DISTANCE",
                       description="Radius of the balls at the vertices; 0 for none")
    resolution = tree.input("Resolution", "INT", 6, min=3, max=32,
                            description="Sides of each tube and ball")
    material = tree.input("Material", "MATERIAL", palette("Starter Brass"))

    largest = largest_dimension(tree, geometry)
    tube_radius = within(tree, radius, largest, 0.02)
    joint_radius = within(tree, joint, largest, 0.04)
    profile = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=resolution,
                     Radius=tube_radius).outputs[0]
    curves = tree.n("GeometryNodeMeshToCurve", Mesh=geometry).outputs[0]
    tubes = tree.n("GeometryNodeCurveToMesh", Curve=curves, Profile_Curve=profile).outputs[0]
    ball = tree.n("GeometryNodeMeshUVSphere", Segments=resolution, Radius=joint_radius,
                  Rings=tree.math("MAXIMUM", tree.math("FLOOR", tree.math(
                      "DIVIDE", resolution, 2.0)), 3.0)).outputs["Mesh"]
    vertices = tree.n("GeometryNodeMeshToPoints", Mesh=geometry, mode="VERTICES").outputs[0]
    balls = tree.n("GeometryNodeInstanceOnPoints", Points=vertices, Instance=ball,
                   Selection=tree.compare("GREATER_THAN", joint_radius, 0.0)).outputs[0]
    frame = tree.join(tubes, tree.realize(balls))

    # Gizmos on the frame itself: the joint size on the first vertex's ball, the tube radius
    # on the middle of the last edge, both pointing out of the surface.
    def sample(domain, index):
        value = {}
        for name, field in (("position", position(tree)), ("normal", normal(tree))):
            value[name] = tree.n("GeometryNodeSampleIndex", Geometry=geometry,
                                 data_type="FLOAT_VECTOR", domain=domain, Value=field,
                                 Index=index).outputs[0]
        return value["position"], value["normal"]

    vertex, vertex_normal = sample("POINT", 0)
    tree.linear_gizmo(tree.math("MULTIPLY", joint, 1.0),
                      along(tree, vertex, vertex_normal, joint_radius), vertex_normal)
    edges = tree.out(tree.n("GeometryNodeAttributeDomainSize", component="MESH",
                            Geometry=geometry), "Edge Count")
    middle, edge_normal = sample("EDGE", tree.math("SUBTRACT", edges, 1))
    tree.linear_gizmo(tree.math("MULTIPLY", radius, 1.0),
                      along(tree, middle, edge_normal, tube_radius), edge_normal)
    resolution_gizmo(tree, resolution, Bounds(tree, geometry).at(z=1.0), per_turn=16.0)
    tree.output(tree.material(tree.smooth(frame), material))
    tree.layout()
    return tree.nt


@asset("Voxelize", "modifier", GENERATE,
       "Rebuilds the object's surface from little cubes, for a blocky, voxel-art look.",
       ("voxel", "blocks", "cubes", "pixel", "minecraft", "remesh"), preview_base="monkey")
def voxelize():
    tree = modifier_group("Voxelize")
    geometry = tree.group_in.outputs["Geometry"]
    mode = tree.menu("Size Mode", ["Relative", "Absolute"], default="Relative",
                     description="Size voxels by the object's size or in meters")
    resolution = tree.input("Resolution", "INT", 20, min=2, max=200,
                            description="Voxels across the object's largest side (Relative)")
    voxel_size = tree.input("Voxel Size", "FLOAT", 0.05, min=0.001, max=100.0,
                            subtype="DISTANCE", description="Size of each voxel (Absolute)")
    gap = tree.input("Gap", "FLOAT", 0.06, min=0.0, max=0.9, subtype="FACTOR",
                     description="Space between neighboring cubes")

    largest = largest_dimension(tree, geometry)
    finest = tree.math("DIVIDE", largest, 200.0)
    size = tree.switch(mode, "FLOAT", {
        "Relative": tree.math("DIVIDE", largest, resolution),
        "Absolute": tree.math("MAXIMUM", voxel_size, finest),
    })
    # Sample the surface densely, snap the samples to voxel centers and keep one per voxel.
    density = tree.math("DIVIDE", 10.0, tree.math("MULTIPLY", size, size))
    samples = tree.n("GeometryNodeDistributePointsOnFaces", Mesh=geometry,
                     distribute_method="RANDOM",
                     Density=capped_density(tree, geometry, density, 200000)).outputs["Points"]
    # The grid is centered on the bounding box, so voxels sit symmetrically on flat sides.
    bounds = tree.n("GeometryNodeBoundBox", Geometry=geometry)
    middle = tree.vmath("SCALE", tree.vmath("ADD", tree.out(bounds, "Min"),
                                            tree.out(bounds, "Max")), scale=0.5)
    half = tree.vmath("SCALE", (1.0, 1.0, 1.0), scale=tree.math("MULTIPLY", size, 0.5))
    local = tree.vmath("ADD", tree.vmath("SUBTRACT", position(tree), middle), half)
    center = tree.vmath("ADD", tree.vmath("SNAP", local, tree.xyz(size, size, size)), middle)
    samples = tree.n("GeometryNodeSetPosition", Geometry=samples, Position=center).outputs[0]
    centers = tree.n("GeometryNodeMergeByDistance", Geometry=samples, Mode="All",
                     Distance=tree.math("MULTIPLY", size, 0.1)).outputs[0]
    edge = tree.math("MULTIPLY", size, tree.math("SUBTRACT", 1.0, gap))
    cube = tree.n("GeometryNodeMeshCube", Size=tree.xyz(edge, edge, edge)).outputs["Mesh"]
    cubes = tree.realize(tree.n("GeometryNodeInstanceOnPoints", Points=centers,
                                Instance=cube).outputs[0])
    cubes = tree.smooth(cubes, False)

    # Gizmos: pulling the end of the top edge by one voxel adds one across; the voxel size
    # arrow stands one voxel out of the +Y side.
    bounds = Bounds(tree, geometry)
    count_gizmo(tree, resolution, tree.math("DIVIDE", largest, resolution), "X",
                bounds.at(-1.0, 0.0, 1.0))
    arrow(tree, tree.math("MULTIPLY", voxel_size, 1.0), bounds.at(y=1.0), "Y", color="Y")
    tree.output(keep_materials(tree, geometry, cubes))
    tree.layout()
    return tree.nt


@asset("Low Poly", "modifier", GENERATE,
       "Simplifies the object into large flat facets, for a stylized low-poly look.",
       ("low poly", "faceted", "simplify", "decimate", "stylized", "flat"),
       preview_base="monkey")
def low_poly():
    tree = modifier_group("Low Poly")
    geometry = tree.group_in.outputs["Geometry"]
    detail = tree.input("Detail", "INT", 8, min=2, max=100,
                        description="Facets across the object's largest side")
    seed = tree.input("Seed", "INT", 0, min=0)

    largest = largest_dimension(tree, geometry)
    cell = tree.math("DIVIDE", largest, detail)
    # Cluster vertices on a grid shifted by the seed and move each to its cluster's average.
    shift = tree.vmath("SCALE", constant_random_vector(tree, seed), scale=cell)
    shifted = tree.vmath("ADD", position(tree), shift)
    cells = tree.vmath("FLOOR", tree.vmath("SCALE", shifted,
                                           scale=tree.math("DIVIDE", 1.0, cell)))
    group = tree.n("FunctionNodeHashValue", data_type="VECTOR", Value=cells).outputs[0]
    total = tree.n("GeometryNodeAccumulateField", data_type="FLOAT_VECTOR", domain="POINT",
                   Value=position(tree), Group_ID=group)
    count = tree.n("GeometryNodeAccumulateField", data_type="FLOAT", domain="POINT", Value=1.0,
                   Group_ID=group)
    average = tree.vmath("SCALE", tree.out(total, "Total"),
                         scale=tree.math("DIVIDE", 1.0, tree.out(count, "Total")))
    mesh = tree.n("GeometryNodeSetPosition", Geometry=geometry, Position=average).outputs[0]
    mesh = tree.n("GeometryNodeMergeByDistance", Geometry=mesh, Mode="All",
                  Distance=tree.math("MULTIPLY", cell, 0.001)).outputs[0]
    mesh = tree.n("GeometryNodeTriangulate", Mesh=mesh).outputs[0]
    # Pulling the end of the top edge by one facet adds one facet across.
    count_gizmo(tree, detail, tree.math("DIVIDE", largest, detail), "X",
                Bounds(tree, geometry).at(-1.0, 0.0, 1.0))
    tree.output(tree.smooth(mesh, False))
    tree.layout()
    return tree.nt


# -----------------------------------------------------------------------------
# Deform.

@asset("Noise Displace", "modifier", DEFORM,
       "Pushes the surface in and out with layered noise, for rocks, terrain and organic shapes.",
       ("noise", "displace", "displacement", "organic", "rock", "terrain"))
def noise_displace():
    tree = modifier_group("Noise Displace")
    geometry = tree.group_in.outputs["Geometry"]
    strength = tree.input("Strength", "FLOAT", 0.15, min=-100.0, max=100.0, subtype="DISTANCE",
                          description="How far the surface moves, at most a quarter of the "
                                      "object's size")
    scale = tree.input("Scale", "FLOAT", 3.0, min=0.0, max=1000.0,
                       description="Noise features across the object's largest side")
    detail = tree.input("Detail", "FLOAT", 4.0, min=0.0, max=15.0,
                        description="Layers of finer noise on top of the main shape")
    subdivisions = tree.input("Subdivisions", "INT", 0, min=0, max=4,
                              description="Extra subdivision levels, on top of the automatic "
                                          "ones low-poly objects get")
    seed = tree.input("Seed", "INT", 0, min=0)

    mesh = subdivide_to(tree, geometry, 5000, subdivisions)
    largest = largest_dimension(tree, geometry)
    reach = within(tree, strength, largest, 0.25)
    coordinate = tree.vmath("ADD", tree.vmath("SCALE", position(tree),
                                              scale=tree.math("DIVIDE", scale, largest)),
                            constant_random_vector(tree, seed, -100.0, 100.0))
    noise = tree.n("ShaderNodeTexNoise", noise_dimensions="3D", Vector=coordinate, Scale=1.0,
                   Detail=detail, Roughness=0.55)
    amount = tree.math("MULTIPLY", tree.math("SUBTRACT", tree.out(noise, "Factor"), 0.5),
                       tree.math("MULTIPLY", reach, 2.0))
    mesh = tree.n("GeometryNodeSetPosition", Geometry=mesh,
                  Offset=tree.vmath("SCALE", normal(tree), scale=amount)).outputs[0]

    # Gizmos: strength stands on top, scale is pulled out of the +X side like a density, and
    # dials on the sides add detail (eight per turn) and subdivisions (four per turn).
    bounds = Bounds(tree, geometry)
    tree.linear_gizmo(tree.math("MULTIPLY", strength, 1.0),
                      along(tree, bounds.at(z=1.0), "Z", reach), AXES["Z"])
    density_gizmo(tree, scale, largest, "X", bounds.center)
    resolution_gizmo(tree, detail, bounds.at(y=-1.0), per_turn=8.0, color="PRIMARY")
    resolution_gizmo(tree, subdivisions, bounds.at(x=-1.0), per_turn=4.0)
    tree.output(mesh)
    tree.layout()
    return tree.nt


@asset("Snow Cover", "modifier", DEFORM,
       "Adds a soft layer of snow on the upward-facing parts of the object.",
       ("snow", "winter", "cover", "weather", "christmas"), preview_base="monkey")
def snow_cover():
    tree = modifier_group("Snow Cover")
    geometry = tree.group_in.outputs["Geometry"]
    thickness = tree.input("Thickness", "FLOAT", 0.12, min=0.0, max=100.0, subtype="DISTANCE",
                           description="Depth of the snow, at most a fifth of the object's size")
    limit = tree.input("Angle Limit", "FLOAT", 45 * RADIANS, min=0.0, max=89 * RADIANS,
                       subtype="ANGLE", description="Steepest slope snow settles on")
    smoothness = tree.input("Smoothness", "INT", 3, min=0, max=20,
                            description="How much the snow's edges are softened")
    material = tree.input("Snow Material", "MATERIAL",
                          simple_material("Starter Fresh Snow", (0.72, 0.84, 1.0), roughness=0.35,
                                          Subsurface_Weight=0.3))
    seed = tree.input("Seed", "INT", 0, min=0)

    # How much snow each face gets: full on flat ground, fading out toward the angle limit.
    _, _, up = tree.separate(normal(tree))
    steepest = tree.math("COSINE", limit)
    flat_enough = tree.n("ShaderNodeMapRange", interpolation_type="SMOOTHSTEP", Value=up,
                         From_Min=steepest, From_Max=tree.math("ADD", steepest, 0.2))
    cover = tree.n("GeometryNodeBlurAttribute", data_type="FLOAT", Iterations=smoothness,
                   Value=tree.out(flat_enough, "Result")).outputs[0]
    # Blurring thins the cover out; stretch it back so the middle of each patch is full depth.
    cover = tree.out(tree.n("ShaderNodeMapRange", interpolation_type="SMOOTHSTEP", Value=cover,
                            From_Min=0.02, From_Max=0.2), "Result")
    # Never on faces steeper than the limit, however far the blur spreads.
    allowed = tree.out(tree.n("ShaderNodeMapRange", Value=tree.out(flat_enough, "Result"),
                              From_Min=0.0, From_Max=0.05), "Result")
    cover = tree.math("MULTIPLY", cover, allowed)
    # Work on a finer copy of coarse meshes, so the snow's edges follow the shape closely.
    shell = store(tree, subdivide_to(tree, geometry, 2000), "snow", cover, domain="FACE")
    # Depth per vertex averages the faces around it (squared), so the snow thins out at its edges.
    edge = named(tree, "snow")
    shell = store(tree, shell, "snow_depth", tree.math("MULTIPLY", edge, edge))
    shell = tree.n("GeometryNodeDeleteGeometry", Geometry=shell, domain="FACE",
                   Selection=tree.compare("LESS_THAN", named(tree, "snow"), 0.001)).outputs[0]
    largest = largest_dimension(tree, geometry)
    depth_limit = within(tree, thickness, largest, 0.2)
    coordinate = tree.vmath("ADD", tree.vmath("SCALE", position(tree),
                                              scale=tree.math("DIVIDE", 4.0, largest)),
                            constant_random_vector(tree, seed, -100.0, 100.0))
    drift = tree.out(tree.n("ShaderNodeTexNoise", noise_dimensions="3D", Vector=coordinate,
                            Detail=2.0), "Factor")
    depth = tree.math("MULTIPLY", tree.math("MULTIPLY", depth_limit, named(tree, "snow_depth")),
                      tree.math("ADD", 0.5, drift))
    # Lift the shell off the surface a little, then extrude it up into a slab with sides,
    # which the subdivision rounds into soft drifts.
    shell = tree.n("GeometryNodeSetPosition", Geometry=shell,
                   Offset=tree.vmath("SCALE", normal(tree),
                                     scale=tree.math("MULTIPLY", depth_limit, 0.03))).outputs[0]
    shell = tree.n("GeometryNodeExtrudeMesh", Mesh=shell, mode="FACES", Individual=False,
                   Offset=tree.xyz(z=1.0), Offset_Scale=depth).outputs["Mesh"]
    shell = tree.n("GeometryNodeSubdivisionSurface", Mesh=shell, Level=1).outputs[0]
    shell = tree.material(tree.smooth(shell), material)

    # Gizmos: the depth on top of the snow, the slope limit as a dial on the +X side turning
    # around Y like a slope does, and the smoothness dial (ten per turn) on the -X side.
    bounds = Bounds(tree, geometry)
    tree.linear_gizmo(tree.math("MULTIPLY", thickness, 1.0),
                      along(tree, bounds.at(z=1.0), "Z", depth_limit), AXES["Z"])
    angle_gizmo(tree, limit, bounds.at(x=1.0, z=0.5), up="Y")
    resolution_gizmo(tree, smoothness, bounds.at(x=-1.0, z=0.5), per_turn=10.0)
    tree.output(add_to(tree, geometry, shell))
    tree.layout()
    return tree.nt
