# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Shapes: parametric primitives beyond Blender's built-in ones."""

import math

from .common import (RADIANS, generator_object, object_group, palette, radius_gizmo,
                     resolution_gizmo, rest_on_ground, simple_material, size_gizmo)
from .registry import asset

CATALOG = "Objects/Shapes"


def _lerp(tree, factor, a, b):
    """`a` where factor is 0, `b` where it's 1."""
    return tree.math("MULTIPLY_ADD", tree.math("SUBTRACT", b, a), factor, a)


def _revolve_rounded(tree, height, radius, bevel, segments, bevel_segments):
    """A closed solid of revolution: a cylinder whose rims are rounded by `bevel`.

    A grid is wrapped around Z: its columns go around, its rows follow the profile from the
    bottom center, over the bottom rim, up the side, over the top rim to the top center.
    """
    rows = tree.math("ADD", tree.math("MULTIPLY", bevel_segments, 2.0), 4.0)
    grid = tree.n("GeometryNodeMeshGrid", Size_X=1.0, Size_Y=1.0,
                  Vertices_X=tree.math("ADD", segments, 1.0), Vertices_Y=rows).outputs["Mesh"]
    u, v, _ = tree.separate(tree.n("GeometryNodeInputPosition").outputs[0])
    theta = tree.math("MULTIPLY", tree.math("ADD", u, 0.5), math.tau)
    row = tree.math("ROUND", tree.math("MULTIPLY", tree.math("ADD", v, 0.5),
                                       tree.math("SUBTRACT", rows, 1.0)))
    # Index along the two rim arcs, then which arc and how far along it.
    j = tree.math("MINIMUM", tree.math("MAXIMUM", tree.math("SUBTRACT", row, 1.0), 0.0),
                  tree.math("ADD", tree.math("MULTIPLY", bevel_segments, 2.0), 1.0))
    top = tree.math("GREATER_THAN", j, tree.math("ADD", bevel_segments, 0.5))
    t = tree.math("DIVIDE", tree.math("SUBTRACT", j, tree.math(
        "MULTIPLY", top, tree.math("ADD", bevel_segments, 1.0))), bevel_segments)
    angle = tree.math("MULTIPLY", tree.math("SUBTRACT", tree.math("ADD", t, top), 1.0),
                      math.pi / 2)
    is_center = tree.math("MAXIMUM", tree.math("LESS_THAN", row, 0.5),
                          tree.math("GREATER_THAN", row, tree.math("SUBTRACT", rows, 1.5)))
    r = tree.math("MULTIPLY_ADD", bevel, tree.math("COSINE", angle),
                  tree.math("SUBTRACT", radius, bevel))
    r = tree.math("MULTIPLY", r, tree.math("SUBTRACT", 1.0, is_center))
    z = tree.math("MULTIPLY_ADD", bevel, tree.math("SINE", angle), bevel)
    z = tree.math("MULTIPLY_ADD", top, tree.math("SUBTRACT", height,
                                                 tree.math("MULTIPLY", bevel, 2.0)), z)
    position = tree.xyz(tree.math("MULTIPLY", r, tree.math("COSINE", theta)),
                        tree.math("MULTIPLY", r, tree.math("SINE", theta)), z)
    mesh = tree.n("GeometryNodeSetPosition", Geometry=grid, Position=position).outputs[0]
    return tree.n("GeometryNodeMergeByDistance", Geometry=mesh, Distance=1e-5).outputs[0]


def _revolved_gizmos(tree, height, radius, segments):
    """Height at the top, radius at the side and segments around the middle of a shape made
    by `_revolve_rounded()`, which already starts at the ground."""
    size_gizmo(tree, height, "Z", centered=False)
    radius_gizmo(tree, radius, origin=tree.xyz(0.0, 0.0, tree.math("MULTIPLY", height, 0.5)))
    resolution_gizmo(tree, segments, tree.xyz(0.0, 0.0, tree.math("MULTIPLY", height, 0.5)),
                     per_turn=48.0)


def _tube(tree, curve, thickness, resolution=16, caps=False):
    """Sweep a round profile of diameter `thickness` along a curve."""
    profile = tree.n("GeometryNodeCurvePrimitiveCircle", mode="RADIUS", Resolution=resolution,
                     Radius=tree.math("MULTIPLY", thickness, 0.5)).outputs["Curve"]
    mesh = tree.n("GeometryNodeCurveToMesh", Curve=curve, Profile_Curve=profile,
                  Fill_Caps=caps).outputs["Mesh"]
    return tree.smooth(mesh, angle=60 * RADIANS)


def _index_switch(tree, index, values):
    """Pick one of `values` (vectors) by index."""
    node = tree.nodes.new("GeometryNodeIndexSwitch")
    node.data_type = "VECTOR"
    while len([s for s in node.inputs if s.name.isdigit()]) < len(values):
        node.index_switch_items.new()
    tree.set_input(node.inputs["Index"], index)
    for i, value in enumerate(values):
        tree.set_input(node.inputs[str(i)], value)
    return node.outputs[0]


@asset("Star", "object", CATALOG,
       "A faceted solid star with any number of points, standing upright on two of them.",
       ("star", "shape", "primitive", "decoration", "christmas"))
def star():
    tree = object_group("Star")
    points = tree.input("Points", "INT", 5, min=3, max=64)
    outer = tree.input("Radius", "FLOAT", 0.25, min=0.01, subtype="DISTANCE")
    inner = tree.input("Inner Radius", "FLOAT", 0.45, min=0.05, max=1.0,
                       subtype="FACTOR", description="Inner radius as a fraction of the radius")
    depth = tree.input("Depth", "FLOAT", 0.12, min=0.0, subtype="DISTANCE",
                       description="Thickness at the center of the star")
    rim = tree.input("Rim", "FLOAT", 0.02, min=0.0, subtype="DISTANCE",
                     description="Thickness at the edges and tips")
    material = tree.input("Material", "MATERIAL",
                          simple_material("Starter Shape Gold", (1.0, 0.62, 0.2), metallic=1.0,
                                          roughness=0.32))

    corners = tree.math("MULTIPLY", points, 2.0)
    mesh = tree.n("GeometryNodeMeshCylinder", fill_type="TRIANGLE_FAN", Vertices=corners,
                  Radius=1.0, Depth=rim).outputs["Mesh"]
    x, y, z = tree.separate(tree.n("GeometryNodeInputPosition").outputs[0])
    is_center = tree.math("LESS_THAN", tree.vmath("LENGTH", tree.xyz(x, y)), 0.001)
    # Alternate outer and inner corners, starting with a tip along +X.
    angle = tree.math("ARCTAN2", y, x)
    corner = tree.math("ROUND", tree.math("DIVIDE", tree.math("MULTIPLY", angle, points), math.pi))
    is_inner = tree.math("FLOORED_MODULO", corner, 2.0)
    radius = _lerp(tree, is_inner, outer, tree.math("MULTIPLY", outer, inner))
    center_z = tree.math("MULTIPLY", tree.math("SIGN", z), tree.math("MULTIPLY", depth, 0.5))
    position = tree.xyz(tree.math("MULTIPLY", x, radius), tree.math("MULTIPLY", y, radius),
                        _lerp(tree, is_center, z, center_z))
    mesh = tree.n("GeometryNodeSetPosition", Geometry=mesh, Position=position).outputs[0]
    mesh = tree.smooth(mesh, False)
    # Stand it up with a tip pointing up.
    mesh = tree.transform(mesh, rotation=(0.0, 0.0, 90 * RADIANS))
    mesh = tree.transform(mesh, rotation=(90 * RADIANS, 0.0, 0.0))
    _star_gizmos(tree, points, outer, inner, depth, rim)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Star", tree)


def _star_gizmos(tree, points, outer, inner, depth, rim):
    """Gizmos on the upright star: X across, Z up, its faces toward -Y."""
    def corner(index):
        """Direction to a corner, counting tips and inner corners from the top tip."""
        angle = tree.math("ADD", tree.math("DIVIDE", index * math.pi, points), math.pi / 2)
        return tree.math("COSINE", angle), tree.math("SINE", angle)

    # With an odd number of points the star stands on two tips, with an even one on a tip, so
    # its bottom is this far below the center, in radii. The top tip is pulled up by the radius
    # plus that, so it follows the finger once the bottom rests on the ground.
    odd = tree.math("FLOORED_MODULO", points, 2.0)
    low_tips = _lerp(tree, odd, 1.0, tree.math("COSINE", tree.math("DIVIDE", math.pi, points)))
    low = tree.math("MAXIMUM", low_tips, tree.math("MULTIPLY", inner, odd))
    tree.linear_gizmo(tree.math("MULTIPLY", outer, tree.math("ADD", low, 1.0)),
                      tree.xyz(0.0, 0.0, outer), (0.0, 0.0, 1.0))
    # The inner corner beside the top tip, dragged out along its own direction.
    cx, cz = corner(1.0)
    inner_radius = tree.math("MULTIPLY", outer, inner)
    tree.linear_gizmo(tree.math("MULTIPLY", inner, outer),
                      tree.xyz(tree.math("MULTIPLY", cx, inner_radius), 0.0,
                               tree.math("MULTIPLY", cz, inner_radius)), tree.xyz(cx, 0.0, cz))
    # Thickness at the center, and at the next tip.
    size_gizmo(tree, depth, "-Y", color="Y")
    tx, tz = corner(2.0)
    size_gizmo(tree, rim, "-Y", origin=tree.xyz(tree.math("MULTIPLY", tx, outer), 0.0,
                                                 tree.math("MULTIPLY", tz, outer)), color="Y")
    # Below the center, between the bottom tips.
    resolution_gizmo(tree, points, tree.xyz(0.0, 0.0, tree.math("MULTIPLY", inner_radius, -0.5)),
                     up="-Y", per_turn=8.0)


def _rounded_axis(tree, coordinate, half, radius, segments):
    """Remap a uniform cube grid coordinate so the outer rows fall in the rounded band."""
    vertices = tree.math("ADD", tree.math("MULTIPLY", segments, 2.0), 2.0)
    step = tree.math("DIVIDE", 2.0, tree.math("SUBTRACT", vertices, 1.0))
    middle = tree.math("MULTIPLY", step, 0.5)
    s = tree.math("DIVIDE", coordinate, half)
    a = tree.math("ABSOLUTE", s)
    inner = tree.math("SUBTRACT", half, radius)
    in_middle = tree.math("MULTIPLY", tree.math("DIVIDE", a, middle), inner)
    in_band = tree.math("ADD", inner, tree.math("MULTIPLY",
                        tree.math("DIVIDE", tree.math("SUBTRACT", a, middle),
                                  tree.math("SUBTRACT", 1.0, middle)), radius))
    use_band = tree.math("GREATER_THAN", a, middle)
    mix = tree.n("ShaderNodeMix", data_type="FLOAT", Factor=use_band, A=in_middle, B=in_band)
    mapped = tree.out(mix, "Result")
    mapped = tree.math("MULTIPLY", mapped, tree.math("SIGN", s))
    # Project onto the rounded corner: clamp to the inner box, then push out by the radius.
    clamped = tree.math("MAXIMUM", mapped, tree.math("MULTIPLY", inner, -1.0))
    clamped = tree.math("MINIMUM", clamped, inner)
    return mapped, clamped


@asset("Rounded Box", "object", CATALOG,
       "A box with true rounded edges, the best starting point for blockouts and kitbashing.",
       ("box", "cube", "rounded", "bevel", "blockout", "primitive"), preview_view="front")
def rounded_box():
    tree = object_group("Rounded Box")
    size = tree.input("Size", "VECTOR", (1.0, 1.0, 1.0), min=0.001, subtype="XYZ")
    bevel = tree.input("Bevel", "FLOAT", 0.1, min=0.0, subtype="DISTANCE",
                       description="Radius of the rounded edges")
    segments = tree.input("Segments", "INT", 4, min=1, max=16)
    material = tree.input("Material", "MATERIAL",
                          simple_material("Starter Shape Clay", (0.4, 0.33, 0.28),
                                          roughness=0.55))

    sx, sy, sz = tree.separate(size)
    half_x, half_y, half_z = (tree.math("MULTIPLY", s, 0.5) for s in (sx, sy, sz))
    # Keep the bevel inside the smallest half size.
    smallest = tree.math("MINIMUM", tree.math("MINIMUM", half_x, half_y), half_z)
    radius = tree.math("MINIMUM", bevel, tree.math("MULTIPLY", smallest, 0.999))
    count = tree.math("ADD", tree.math("MULTIPLY", segments, 2.0), 2.0)
    cube = tree.n("GeometryNodeMeshCube", Size=size, Vertices_X=count, Vertices_Y=count,
                  Vertices_Z=count).outputs["Mesh"]
    px, py, pz = tree.separate(tree.n("GeometryNodeInputPosition").outputs[0])
    mx, cx = _rounded_axis(tree, px, half_x, radius, segments)
    my, cy = _rounded_axis(tree, py, half_y, radius, segments)
    mz, cz = _rounded_axis(tree, pz, half_z, radius, segments)
    mapped = tree.xyz(mx, my, mz)
    core = tree.xyz(cx, cy, cz)
    direction = tree.vmath("NORMALIZE", tree.vmath("SUBTRACT", mapped, core))
    distance = tree.vmath("LENGTH", tree.vmath("SUBTRACT", mapped, core))
    has_offset = tree.math("GREATER_THAN", distance, 0.0)
    rounded = tree.vmath("ADD", core, tree.vmath("SCALE", direction,
                                                 scale=tree.math("MULTIPLY", radius, has_offset)))
    rounded = tree.out(tree.n("ShaderNodeMix", data_type="VECTOR", Factor=has_offset, A=mapped,
                              B=rounded), "Result")
    mesh = tree.n("GeometryNodeSetPosition", Geometry=cube, Position=rounded).outputs[0]
    size_gizmo(tree, sx, "X")
    size_gizmo(tree, sy, "Y")
    size_gizmo(tree, sz, "Z", origin=tree.xyz(0.0, 0.0, tree.math("MULTIPLY", sz, -0.5)),
               centered=False)
    # Where the top face starts to round over its +X edge: pulling it in widens the round.
    tree.linear_gizmo(tree.math("MULTIPLY", bevel, 1.0),
                      tree.xyz(tree.math("SUBTRACT", half_x, radius), 0.0, half_z),
                      (-1.0, 0.0, 0.0))
    resolution_gizmo(tree, segments, tree.xyz(half_x, tree.math("MULTIPLY", half_y, -1.0),
                                              half_z), per_turn=8.0)
    mesh = tree.smooth(mesh, angle=60 * RADIANS)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Rounded Box", tree)


@asset("Capsule", "object", CATALOG,
       "A pill-shaped capsule: a cylinder with hemispheres on both ends.",
       ("capsule", "pill", "primitive", "rounded", "collider"))
def capsule():
    tree = object_group("Capsule")
    length = tree.input("Length", "FLOAT", 0.6, min=0.002, subtype="DISTANCE",
                        description="Total length from end to end")
    radius = tree.input("Radius", "FLOAT", 0.15, min=0.001, subtype="DISTANCE")
    segments = tree.input("Segments", "INT", 48, min=3, max=256,
                          description="Number of vertices around the capsule")
    rings = tree.input("Rings", "INT", 12, min=1, max=64,
                       description="Number of rings in each rounded end")
    material = tree.input("Material", "MATERIAL", palette("Starter Blue"))

    _revolved_gizmos(tree, length, radius, segments)
    # The rings of the top end, around its front.
    end = tree.math("MINIMUM", radius, tree.math("MULTIPLY", length, 0.5))
    resolution_gizmo(tree, rings, tree.xyz(0.0, tree.math("MULTIPLY", end, -1.0),
                                           tree.math("SUBTRACT", length, end)),
                     up="X", per_turn=12.0)
    radius = end
    mesh = _revolve_rounded(tree, length, radius, radius, segments, rings)
    mesh = tree.smooth(mesh)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Capsule", tree)


@asset("Rounded Cylinder", "object", CATALOG,
       "A cylinder with smoothly rounded rims, like a puck, a can or a pedestal.",
       ("cylinder", "rounded", "bevel", "primitive", "blockout"))
def rounded_cylinder():
    tree = object_group("Rounded Cylinder")
    height = tree.input("Height", "FLOAT", 0.5, min=0.001, subtype="DISTANCE")
    radius = tree.input("Radius", "FLOAT", 0.25, min=0.001, subtype="DISTANCE")
    bevel = tree.input("Bevel", "FLOAT", 0.05, min=0.0, subtype="DISTANCE",
                       description="Radius of the rounded rims")
    segments = tree.input("Segments", "INT", 48, min=3, max=256,
                          description="Number of vertices around the cylinder")
    bevel_segments = tree.input("Bevel Segments", "INT", 6, min=1, max=32)
    material = tree.input("Material", "MATERIAL",
                          simple_material("Starter Shape Orange", (0.75, 0.16, 0.02),
                                          roughness=0.4))

    # Keep the bevel inside the cylinder.
    _revolved_gizmos(tree, height, radius, segments)
    limit = tree.math("MINIMUM", radius, tree.math("MULTIPLY", height, 0.5))
    round_ = tree.math("MINIMUM", bevel, tree.math("MULTIPLY", limit, 0.999))
    # Where the top starts to round over the rim: pulling it in widens the round.
    tree.linear_gizmo(tree.math("MULTIPLY", bevel, 1.0),
                      tree.xyz(tree.math("SUBTRACT", radius, round_), 0.0, height),
                      (-1.0, 0.0, 0.0))
    # Around the front of the rim, at the center of its rounding.
    inset = tree.math("SUBTRACT", round_, radius)
    resolution_gizmo(tree, bevel_segments, tree.xyz(0.0, inset,
                                                    tree.math("SUBTRACT", height, round_)),
                     up="X", per_turn=8.0)
    mesh = _revolve_rounded(tree, height, radius, round_, segments, bevel_segments)
    mesh = tree.smooth(mesh, angle=30 * RADIANS)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Rounded Cylinder", tree)


@asset("Torus Knot", "object", CATALOG,
       "A smooth tube winding around a torus, forming a (p, q) knot like the trefoil.",
       ("knot", "torus", "trefoil", "tube", "math", "primitive"))
def torus_knot():
    tree = object_group("Torus Knot")
    p = tree.input("P", "INT", 2, min=1, max=16,
                   description="Times the tube winds around the torus axis")
    q = tree.input("Q", "INT", 3, min=1, max=16,
                   description="Times the tube winds through the hole; "
                               "use a value with no common factor with P for a single knot")
    size = tree.input("Size", "FLOAT", 0.25, min=0.001, subtype="DISTANCE",
                      description="Outer radius of the knot")
    thickness = tree.input("Thickness", "FLOAT", 0.07, min=0.0001, subtype="DISTANCE",
                           description="Diameter of the tube")
    material = tree.input("Material", "MATERIAL", palette("Starter Copper"))

    count = tree.math("MULTIPLY", tree.math("ADD", p, q), 48.0)
    curve = tree.n("GeometryNodeCurvePrimitiveCircle", mode="RADIUS", Resolution=count,
                   Radius=1.0).outputs["Curve"]
    index = tree.n("GeometryNodeInputIndex").outputs[0]
    phi = tree.math("MULTIPLY", tree.math("DIVIDE", index, count), math.tau)
    p_phi = tree.math("MULTIPLY", p, phi)
    q_phi = tree.math("MULTIPLY", q, phi)
    major = tree.math("MULTIPLY", size, 2.0 / 3.0)
    minor = tree.math("MULTIPLY", size, 1.0 / 3.0)
    ring = tree.math("MULTIPLY_ADD", minor, tree.math("COSINE", q_phi), major)
    position = tree.xyz(tree.math("MULTIPLY", ring, tree.math("COSINE", p_phi)),
                        tree.math("MULTIPLY", ring, tree.math("SINE", p_phi)),
                        tree.math("MULTIPLY", minor, tree.math("SINE", q_phi)))
    curve = tree.n("GeometryNodeSetPosition", Geometry=curve, Position=position).outputs[0]
    mesh = _tube(tree, curve, thickness, resolution=20)
    mesh = tree.transform(mesh, rotation=(90 * RADIANS, 0.0, 0.0))
    # It stands in the XZ plane once turned up, with the torus axis along Y.
    # Most knots reach further along +X than -X and get re-centered on the ground, so the arrow
    # on the +X side moves by half the width: scale the drag by that to follow the finger.
    x = tree.separate(tree.n("GeometryNodeInputPosition").outputs[0])[0]
    span = tree.n("GeometryNodeAttributeStatistic", data_type="FLOAT", domain="POINT",
                  Geometry=curve, Attribute=x)
    width = tree.math("SUBTRACT", tree.out(span, "Max"), tree.out(span, "Min"))
    tree.linear_gizmo(tree.math("MULTIPLY", size, tree.math("DIVIDE", width,
                                                            tree.math("MULTIPLY", size, 2.0))),
                      tree.xyz(tree.math("MULTIPLY_ADD", thickness, 0.5, tree.out(span, "Max"))),
                      (1.0, 0.0, 0.0))
    # The front of the tube, where the knot comes closest to -Y.
    front = tree.math("DIVIDE", math.pi / 2, q)
    front_ring = tree.math("MULTIPLY_ADD", minor, tree.math("COSINE", tree.math(
        "MULTIPLY", q, front)), major)
    front_angle = tree.math("MULTIPLY", p, front)
    origin = tree.xyz(tree.math("MULTIPLY", front_ring, tree.math("COSINE", front_angle)),
                      tree.math("MULTIPLY", minor, -1.0),
                      tree.math("MULTIPLY", front_ring, tree.math("SINE", front_angle)))
    size_gizmo(tree, thickness, "-Y", origin=origin, color="Y")
    # P turns around the torus axis, Q around the tube.
    resolution_gizmo(tree, p, tree.xyz(), up="-Y", per_turn=4.0)
    resolution_gizmo(tree, q, tree.xyz(major), up="Z", per_turn=4.0)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Torus Knot", tree)


PLATONIC_TYPES = ("Tetrahedron", "Cube", "Octahedron", "Dodecahedron", "Icosahedron")


@asset("Platonic Solid", "object", CATALOG,
       "One of the five regular polyhedra, from the tetrahedron to the icosahedron, "
       "resting on a face.",
       ("platonic", "polyhedron", "dice", "geometry", "math", "primitive"))
def platonic_solid():
    tree = object_group("Platonic Solid")
    kind = tree.menu("Type", PLATONIC_TYPES, default="Dodecahedron")
    size = tree.input("Size", "FLOAT", 0.4, min=0.001, subtype="DISTANCE",
                      description="Diameter of the sphere through the corners")
    smooth = tree.input("Shade Smooth", "BOOL", False,
                        description="Shade smoothly instead of showing the flat faces")
    material = tree.input("Material", "MATERIAL",
                          simple_material("Starter Shape Teal", (0.04, 0.42, 0.4),
                                          roughness=0.3))

    # Each solid fits a unit sphere and rests on one of its faces.
    edge = 4.0 / math.sqrt(6.0)
    tetrahedron = tree.n("GeometryNodeMeshCone", fill_type="NGON", Vertices=3,
                         Radius_Bottom=edge / math.sqrt(3.0), Depth=4.0 / 3.0).outputs["Mesh"]
    cube = tree.n("GeometryNodeMeshCube", Size=(2.0 / math.sqrt(3.0),) * 3).outputs["Mesh"]
    octahedron = tree.n("GeometryNodeMeshUVSphere", Segments=4, Rings=2,
                        Radius=1.0).outputs["Mesh"]
    octahedron = tree.transform(octahedron, rotation=(-2.356194, -0.615480, 1.308997))
    icosahedron = tree.n("GeometryNodeMeshIcoSphere", Radius=1.0, Subdivisions=1)
    dodecahedron = tree.n("GeometryNodeDualMesh", Mesh=icosahedron.outputs["Mesh"])
    dodecahedron = tree.transform(dodecahedron.outputs[0], scale=(1.0 / 0.794652,) * 3)
    icosahedron = tree.transform(icosahedron.outputs["Mesh"],
                                 rotation=(0.628323, 0.188715, 0.061481))
    mesh = tree.switch(kind, "GEOMETRY", {
        "Tetrahedron": tetrahedron,
        "Cube": cube,
        "Octahedron": octahedron,
        "Dodecahedron": dodecahedron,
        "Icosahedron": icosahedron,
    })
    half = tree.math("MULTIPLY", size, 0.5)
    mesh = tree.transform(mesh, scale=tree.xyz(half, half, half))
    # Each solid is centered on Z, its height a fraction of the size, so the top arrow drags the
    # height and follows the finger once the solid rests on the ground.
    height = tree.math("MULTIPLY", size, tree.switch(kind, "FLOAT", {
        "Tetrahedron": 2.0 / 3.0,
        "Cube": 1.0 / math.sqrt(3.0),
        "Octahedron": 1.0 / math.sqrt(3.0),
        "Dodecahedron": 0.794654,
        "Icosahedron": 0.794654,
    }))
    tree.linear_gizmo(height, tree.xyz(0.0, 0.0, tree.math("MULTIPLY", height, 0.5)),
                      (0.0, 0.0, 1.0), color="Z")
    mesh = tree.n("GeometryNodeSetShadeSmooth", Mesh=mesh, domain="FACE",
                  Shade_Smooth=smooth).outputs[0]
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Platonic Solid", tree)


@asset("Helix", "object", CATALOG,
       "A round tube coiled into a helix, like a spring or a spiral ornament.",
       ("helix", "spiral", "spring", "coil", "tube"))
def helix():
    tree = object_group("Helix")
    turns = tree.input("Turns", "FLOAT", 5.0, min=0.1, max=100.0)
    radius = tree.input("Radius", "FLOAT", 0.12, min=0.001, subtype="DISTANCE",
                        description="Radius of the coil, to the center of the tube")
    height = tree.input("Height", "FLOAT", 0.4, min=0.0, subtype="DISTANCE")
    thickness = tree.input("Thickness", "FLOAT", 0.03, min=0.0001, subtype="DISTANCE",
                           description="Diameter of the tube")
    material = tree.input("Material", "MATERIAL", palette("Starter Steel"))

    curve = tree.n("GeometryNodeCurveSpiral", Resolution=32, Rotations=turns,
                   Start_Radius=radius, End_Radius=radius, Height=height).outputs[0]
    mesh = _tube(tree, curve, thickness, resolution=12, caps=True)
    size_gizmo(tree, height, "Z", centered=False)
    radius_gizmo(tree, radius, origin=tree.xyz(0.0, 0.0, tree.math("MULTIPLY", height, 0.5)))
    # Inside the tube where the coil starts.
    tree.linear_gizmo(tree.math("MULTIPLY", thickness, 0.5),
                      tree.xyz(tree.math("MULTIPLY_ADD", thickness, -0.5, radius)),
                      (-1.0, 0.0, 0.0), color="SECONDARY")
    # At the end of the coil: a full turn of the dial adds a full turn of the coil.
    end = tree.math("MULTIPLY", turns, math.tau)
    tree.dial_gizmo(tree.math("MULTIPLY", turns, math.tau),
                    tree.xyz(tree.math("MULTIPLY", radius, tree.math("COSINE", end)),
                             tree.math("MULTIPLY", radius, tree.math("SINE", end)), height),
                    (0.0, 0.0, 1.0), radius=0.6, color="SECONDARY")
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Helix", tree)


@asset("Heart", "object", CATALOG,
       "A puffy, rounded heart standing upright on its point.",
       ("heart", "love", "valentine", "shape", "decoration"), preview_view="side")
def heart():
    tree = object_group("Heart")
    size = tree.input("Size", "FLOAT", 0.3, min=0.001, subtype="DISTANCE",
                      description="Width of the heart")
    depth = tree.input("Depth", "FLOAT", 0.14, min=0.0, subtype="DISTANCE",
                       description="Thickness at the middle of the heart")
    roundness = tree.input("Roundness", "FLOAT", 0.5, min=0.0, max=1.0, subtype="FACTOR",
                           description="From flat sides with rounded edges to a full dome")
    material = tree.input("Material", "MATERIAL", palette("Starter Red"))

    # A sphere with its poles on the depth axis: each ring becomes a heart-shaped outline.
    sphere = tree.n("GeometryNodeMeshUVSphere", Segments=64, Rings=32, Radius=1.0)
    x, y, z = tree.separate(tree.n("GeometryNodeInputPosition").outputs[0])
    spread = tree.vmath("LENGTH", tree.xyz(x, y))
    t = tree.math("SUBTRACT", math.pi / 2, tree.math("ARCTAN2", y, x))

    def cos(factor):
        return tree.math("COSINE", tree.math("MULTIPLY", t, factor))

    heart_x = tree.math("MULTIPLY", tree.math("POWER", tree.math("SINE", t), 3.0), 16.0)
    heart_y = tree.math("SUBTRACT", tree.math("MULTIPLY", cos(1.0), 13.0),
                        tree.math("MULTIPLY", cos(2.0), 5.0))
    heart_y = tree.math("SUBTRACT", heart_y, tree.math("MULTIPLY", cos(3.0), 2.0))
    heart_y = tree.math("SUBTRACT", heart_y, cos(4.0))
    unit = tree.math("DIVIDE", size, 32.0)
    scale = tree.math("MULTIPLY", unit, spread)
    # Thickness grows with the distance to the outline, rounding off the rim like a pillow.
    outline = tree.math("MULTIPLY", tree.vmath("LENGTH", tree.xyz(heart_x, heart_y)), unit)
    inset = tree.math("MULTIPLY", tree.math("SUBTRACT", 1.0, spread), outline)
    rim = tree.math("MULTIPLY", size, tree.math("MULTIPLY_ADD", roundness, 0.3, 0.04))
    c = tree.math("MINIMUM", tree.math("DIVIDE", inset, rim), 1.0)
    puff = tree.math("SQRT", tree.math("MULTIPLY", c, tree.math("SUBTRACT", 2.0, c)))
    puff = tree.math("MULTIPLY", puff, tree.math("MULTIPLY_ADD", tree.math(
        "MULTIPLY", spread, spread), -0.25, 1.0))
    bulge = tree.math("MULTIPLY", tree.math("SIGN", z), puff)
    # Facing +X, so the side view in the preview shows it straight on.
    position = tree.xyz(tree.math("MULTIPLY", bulge, tree.math("MULTIPLY", depth, 0.5)),
                        tree.math("MULTIPLY", heart_x, scale),
                        tree.math("MULTIPLY", heart_y, scale))
    mesh = tree.n("GeometryNodeSetPosition", Geometry=sphere.outputs["Mesh"],
                  Position=position).outputs[0]
    # The heart is widest across its lobes, 4/32 of the size above its center.
    lobes = tree.math("MULTIPLY", size, 4.0 / 32.0)
    tree.linear_gizmo(tree.math("MULTIPLY", size, 0.5),
                      tree.xyz(0.0, tree.math("MULTIPLY", size, 0.5), lobes), (0.0, 1.0, 0.0),
                      color="Y")
    size_gizmo(tree, depth, "X")
    # On the left lobe where its rim stops rounding: pulling it in widens the rounding.
    edge = tree.math("MULTIPLY_ADD", size, -0.5, rim)
    tree.linear_gizmo(tree.math("MULTIPLY", roundness, tree.math("MULTIPLY", size, 0.3)),
                      tree.xyz(tree.math("MULTIPLY", depth, 0.35), edge, lobes),
                      (0.0, 1.0, 0.0))
    mesh = tree.smooth(mesh)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Heart", tree)


@asset("Arrow", "object", CATALOG,
       "A 3D arrow lying on the ground, flat like a floor marker or round like a gizmo.",
       ("arrow", "direction", "pointer", "sign", "shape"), preview_view="top")
def arrow():
    tree = object_group("Arrow")
    style = tree.menu("Style", ("Flat", "Round"))
    length = tree.input("Length", "FLOAT", 0.6, min=0.002, subtype="DISTANCE")
    head_size = tree.input("Head Size", "FLOAT", 0.24, min=0.001, subtype="DISTANCE",
                           description="Length of the arrowhead; it's a little wider than long")
    thickness = tree.input("Thickness", "FLOAT", 0.1, min=0.0001, subtype="DISTANCE",
                           description="Width of the shaft")
    depth = tree.input("Depth", "FLOAT", 0.04, min=0.0001, subtype="DISTANCE",
                       description="Height of the flat style above the ground")
    material = tree.input("Material", "MATERIAL",
                          simple_material("Starter Shape Amber", (0.9, 0.38, 0.0),
                                          roughness=0.35))

    head = tree.math("MINIMUM", head_size, tree.math("MULTIPLY", length, 0.999))
    neck = tree.math("SUBTRACT", length, head)
    shaft = tree.math("MULTIPLY", thickness, 0.5)
    wing = tree.math("MAXIMUM", tree.math("MULTIPLY", head, 0.6), shaft)

    # Flat: an arrow outline extruded upward.
    outline = tree.n("GeometryNodeMeshCircle", fill_type="NGON", Vertices=7).outputs[0]
    corners = _index_switch(tree, tree.n("GeometryNodeInputIndex").outputs[0], (
        tree.xyz(0.0, tree.math("MULTIPLY", shaft, -1.0)),
        tree.xyz(neck, tree.math("MULTIPLY", shaft, -1.0)),
        tree.xyz(neck, tree.math("MULTIPLY", wing, -1.0)),
        tree.xyz(length, 0.0),
        tree.xyz(neck, wing),
        tree.xyz(neck, shaft),
        tree.xyz(0.0, shaft),
    ))
    outline = tree.n("GeometryNodeSetPosition", Geometry=outline, Position=corners).outputs[0]
    flat = tree.n("GeometryNodeExtrudeMesh", Mesh=outline, Offset_Scale=depth,
                  Individual=False).outputs["Mesh"]
    bottom = tree.n("GeometryNodeFlipFaces", Mesh=outline).outputs[0]
    flat = tree.join(flat, bottom)
    flat = tree.n("GeometryNodeMergeByDistance", Geometry=flat, Distance=1e-6).outputs[0]
    flat = tree.smooth(flat, False)

    # Round: a cylinder shaft and a cone head along +X.
    along_x = (0.0, 90 * RADIANS, 0.0)
    rod = tree.n("GeometryNodeMeshCylinder", Vertices=32, Radius=shaft, Depth=neck)
    rod = tree.transform(rod.outputs["Mesh"], rotation=along_x,
                         translation=tree.xyz(tree.math("MULTIPLY", neck, 0.5)))
    cone = tree.n("GeometryNodeMeshCone", Vertices=32, Radius_Bottom=wing, Depth=head)
    cone = tree.transform(cone.outputs["Mesh"], rotation=along_x, translation=tree.xyz(
        tree.math("MULTIPLY_ADD", head, 0.5, neck)))
    round_ = tree.smooth(tree.join(rod, cone), angle=30 * RADIANS)

    # The flat style sits on Z = 0, the round one is centered on it.
    middle = tree.switch(style, "FLOAT", {"Flat": tree.math("MULTIPLY", depth, 0.5),
                                          "Round": 0.0})
    top = tree.switch(style, "FLOAT", {"Flat": depth, "Round": shaft})
    tree.linear_gizmo(tree.math("MULTIPLY", length, 0.5), tree.xyz(length, 0.0, middle),
                      (1.0, 0.0, 0.0), color="X")
    # The back of the arrowhead: pulling it back makes the head longer.
    tree.linear_gizmo(tree.math("MULTIPLY", head_size, 1.0), tree.xyz(neck, wing, middle),
                      (-1.0, 0.0, 0.0))
    tree.linear_gizmo(tree.math("MULTIPLY", thickness, 0.5),
                      tree.xyz(tree.math("MULTIPLY", neck, 0.5), shaft, middle),
                      (0.0, 1.0, 0.0), color="Y")
    tree.linear_gizmo(tree.math("MULTIPLY", depth, 1.0),
                      tree.xyz(tree.math("MULTIPLY", neck, 0.25), 0.0, top), (0.0, 0.0, 1.0),
                      color="Z")

    mesh = tree.switch(style, "GEOMETRY", {"Flat": flat, "Round": round_})
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Arrow", tree)
