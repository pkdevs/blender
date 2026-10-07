# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Hardware: screws, bolts, nuts, gears, springs, chains, pipes and cables.

Real fasteners are only millimeters long, too small to see when dropped into a default scene.
Small parts are therefore modeled at about ten times their real size (a 0.15 m screw, a 0.08 m
nut), with real proportions, so they read well next to the default cube. Larger parts (pipes,
cables, chains) use their real sizes.
"""

import math

from .common import (AXES, RADIANS, generator_object, object_group, palette, radius_gizmo,
                     resolution_gizmo, rest_on_ground, simple_material, size_gizmo)
from .registry import asset

CATALOG = "Objects/Hardware"


def steel():
    """Brushed steel: rougher than the polished palette metals, so turned and milled sides
    catch the light instead of mirroring the dark floor."""
    return simple_material("Hardware Steel", (0.62, 0.63, 0.65), metallic=1.0, roughness=0.5)


def brass():
    return simple_material("Hardware Brass", (0.84, 0.62, 0.3), metallic=1.0, roughness=0.45)

# -----------------------------------------------------------------------------
# Math on constants or sockets: constants fold in Python, sockets add nodes.

def _is_number(value):
    return isinstance(value, (int, float))


def add(tree, a, b):
    return a + b if _is_number(a) and _is_number(b) else tree.math("ADD", a, b)


def sub(tree, a, b):
    return a - b if _is_number(a) and _is_number(b) else tree.math("SUBTRACT", a, b)


def mul(tree, a, b):
    return a * b if _is_number(a) and _is_number(b) else tree.math("MULTIPLY", a, b)


def div(tree, a, b):
    return a / b if _is_number(a) and _is_number(b) else tree.math("DIVIDE", a, b)


def vector(tree, x, y, z):
    if all(_is_number(value) for value in (x, y, z)):
        return (x, y, z)
    return tree.xyz(x, y, z)


def position(tree):
    return tree.n("GeometryNodeInputPosition").outputs[0]


def index(tree):
    return tree.n("GeometryNodeInputIndex").outputs[0]


def index_switch(tree, data_type, which, values):
    """Index Switch: picks one of `values` (constants or sockets) by the `which` field."""
    node = tree.nodes.new("GeometryNodeIndexSwitch")
    node.data_type = data_type
    while len(node.index_switch_items) < len(values):
        node.index_switch_items.new()
    tree.set_input(node.inputs[0], which)
    for i, value in enumerate(values):
        tree.set_input(node.inputs[i + 1], value)
    return node.outputs[0]


def menu_choice(tree, menu, names):
    """The index of the chosen menu item.

    A menu input can only drive one Menu Switch (more leave it without items), so menus that
    choose several values go through an index and Index Switches.
    """
    return tree.switch(menu, "INT", {name: i for i, name in enumerate(names)})


def smoothstep(tree, value, low, high):
    node = tree.n("ShaderNodeMapRange", data_type="FLOAT", interpolation_type="SMOOTHSTEP",
                  Value=value, From_Min=low, From_Max=high)
    return tree.out(node, "Result")


# -----------------------------------------------------------------------------
# Geometry helpers.

def polyline(tree, points, cyclic=False):
    """A poly curve through `points`, each an (x, y, z) of constants or sockets."""
    values = [vector(tree, *point) for point in points]
    where = index_switch(tree, "VECTOR", index(tree), values)
    cloud = tree.n("GeometryNodePoints", Count=len(points), Position=where).outputs[0]
    curve = tree.n("GeometryNodePointsToCurves", Points=cloud).outputs[0]
    if cyclic:
        curve = tree.n("GeometryNodeSetSplineCyclic", Curve=curve, Cyclic=True).outputs[0]
    return curve


def revolve(tree, profile, segments=48):
    """Lathe a closed (radius, height) profile around Z. Points on the axis weld shut."""
    # Curve to Mesh places profile X along the circle's outward normal and profile Y down,
    # mirroring the profile, so walk it backwards to keep the normals outward.
    points = [(sub(tree, r, 1.0), mul(tree, z, -1.0), 0.0) for r, z in reversed(profile)]
    shape = polyline(tree, points, cyclic=True)
    circle = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=segments,
                    Radius=1.0).outputs["Curve"]
    mesh = tree.n("GeometryNodeCurveToMesh", Curve=circle, Profile_Curve=shape).outputs[0]
    return tree.n("GeometryNodeMergeByDistance", Geometry=mesh, Distance=1e-5).outputs[0]


def polar(tree):
    """Angle around Z and distance from the Z axis of each point."""
    x, y, _ = tree.separate(position(tree))
    angle = tree.math("ARCTAN2", y, x)
    radius = tree.math("SQRT", add(tree, mul(tree, x, x), mul(tree, y, y)))
    return angle, radius


def scale_radially(tree, geometry, factor, selection=None):
    x, y, z = tree.separate(position(tree))
    node = tree.n("GeometryNodeSetPosition", Geometry=geometry,
                  Position=tree.xyz(mul(tree, x, factor), mul(tree, y, factor), z))
    if selection is not None:
        tree.set_input(node.inputs["Selection"], selection)
    return node.outputs[0]


def mill_polygon(tree, mesh, sides, flat_radius, bottom, top):
    """Cut flats into a turned part: intersect it with a regular prism, a corner on +X."""
    corner = div(tree, flat_radius, tree.math("COSINE", div(tree, math.pi, sides)))
    margin = mul(tree, sub(tree, top, bottom), 0.1)
    prism = tree.n("GeometryNodeMeshCylinder", Vertices=sides, Radius=corner,
                   Depth=add(tree, sub(tree, top, bottom), mul(tree, margin, 2.0)),
                   fill_type="NGON").outputs["Mesh"]
    prism = tree.transform(prism, translation=vector(tree, 0.0, 0.0,
                                                     mul(tree, add(tree, top, bottom), 0.5)))
    node = tree.n("GeometryNodeMeshBoolean", operation="INTERSECT", solver="MANIFOLD")
    tree.set_input(node.inputs["Mesh 2"], mesh)
    tree.set_input(node.inputs["Mesh 2"], prism)
    return node.outputs["Mesh"]


def helix_thread(tree, core, depth, width, pitch, bottom, top, scale=None):
    """A helical thread ridge from `bottom` to `top` around a core of radius `core`.

    The ridge is `depth` high and `width` wide at its base, and fades in and out over half a
    turn at both ends. `scale` optionally maps a height to a factor shrinking core and thread,
    for a pointed tip.
    """
    length = sub(tree, top, bottom)
    turns = div(tree, length, pitch)
    spiral = tree.n("GeometryNodeCurveSpiral", Resolution=32, Rotations=turns,
                    Start_Radius=1.0, End_Radius=1.0, Height=length).outputs[0]
    spiral = tree.transform(spiral, translation=vector(tree, 0.0, 0.0, bottom))
    along = mul(tree, tree.n("GeometryNodeSplineParameter").outputs["Factor"], turns)
    fade = smoothstep(tree, tree.math("MINIMUM", along, sub(tree, turns, along)), 0.0, 0.5)
    if scale is not None:
        _, _, z = tree.separate(position(tree))
        shrink = scale(z)
        fade = mul(tree, fade, shrink)
        core = mul(tree, core, shrink)
    spiral = scale_radially(tree, spiral, core)
    spiral = tree.n("GeometryNodeSetCurveRadius", Curve=spiral, Radius=fade).outputs[0]
    x, y, _ = tree.separate(position(tree))
    outward = tree.vmath("NORMALIZE", tree.xyz(x, y, 0.0))
    spiral = tree.n("GeometryNodeSetCurveNormal", Curve=spiral, Mode="Free",
                    Normal=outward).outputs[0]
    # Profile X points out from the core, Y along the axis; the base sinks into the core.
    half = mul(tree, width, 0.5)
    flat = mul(tree, width, 0.04)
    sunk = mul(tree, depth, -0.25)
    ridge = polyline(tree, [(sunk, mul(tree, half, -1.0), 0.0),
                            (depth, mul(tree, flat, -1.0), 0.0), (depth, flat, 0.0),
                            (sunk, half, 0.0)], cyclic=True)
    return tree.n("GeometryNodeCurveToMesh", Curve=spiral, Profile_Curve=ridge,
                  Fill_Caps=True).outputs[0]


def extruded(tree, curve, height, z=0.0):
    """Fill a 2D curve (inner curves become holes) and extrude it up by `height`."""
    mesh = tree.n("GeometryNodeFillCurve", Curve=curve, Mode="Triangles",
                  Fill_Rule="Even-Odd").outputs[0]
    mesh = tree.n("GeometryNodeExtrudeMesh", Mesh=mesh, Offset_Scale=height,
                  Individual=False).outputs["Mesh"]
    # The bottom faces still face up after extruding; flip them.
    _, _, pz = tree.separate(position(tree))
    _, _, nz = tree.separate(tree.n("GeometryNodeInputNormal").outputs[0])
    bottom = tree.math("LESS_THAN", pz, mul(tree, height, 0.5))
    upward = tree.math("GREATER_THAN", nz, 0.5)
    mesh = tree.n("GeometryNodeFlipFaces", Mesh=mesh,
                  Selection=tree.math("MULTIPLY", bottom, upward)).outputs[0]
    if z != 0.0:
        mesh = tree.transform(mesh, translation=vector(tree, 0.0, 0.0, z))
    return mesh


def circle(tree, radius, resolution=48):
    return tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=resolution,
                  Radius=radius).outputs["Curve"]


def pitch_gizmo(tree, pitch, radius, start, turns=3.5):
    """A box on the -X side of a thread, `turns` turns above where it starts. Dragging it moves
    that turn, spreading or packing the turns below it."""
    value = tree.math("MULTIPLY", pitch, turns)
    where = vector(tree, mul(tree, radius, -1.0), 0.0, add(tree, start, mul(tree, pitch, turns)))
    tree.linear_gizmo(value, where, AXES["Z"], color="SECONDARY", style="BOX")


def finish(tree, mesh, material, angle=40):
    """Smooth by `angle` (None keeps the parts' own shading), set the material, ground it."""
    if angle is not None:
        mesh = tree.smooth(mesh, angle=angle * RADIANS)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))


# -----------------------------------------------------------------------------
# Fasteners.

def recess(tree, drive, top, diameter, depth):
    """Cutter for the screwdriver recess in a head whose top is at `top`."""
    width = mul(tree, diameter, 0.2)
    slot_length = mul(tree, diameter, 2.6)
    cross_length = mul(tree, diameter, 1.15)
    height = mul(tree, depth, 2.0)

    def bar(length, angle):
        box = tree.n("GeometryNodeMeshCube", Size=vector(tree, length, width, height)).outputs[0]
        return tree.transform(box, rotation=(0.0, 0.0, angle),
                              translation=vector(tree, 0.0, 0.0, top))
    slot = bar(slot_length, 0.0)
    cross = tree.join(bar(cross_length, 0.0), bar(cross_length, math.pi / 2))
    return tree.switch(drive, "GEOMETRY", {"Slot": slot, "Cross": cross, "None": None})


def hex_head(tree, flat_radius, bottom, height):
    """A hex head from `bottom` up by `height`, its top edge chamfered at 30 degrees."""
    corner = mul(tree, flat_radius, 1.0 / math.cos(math.pi / 6) * 1.01)
    top = add(tree, bottom, height)
    start = mul(tree, flat_radius, 0.9)
    cut = mul(tree, sub(tree, corner, start), 0.58)
    head = revolve(tree, [(0.0, bottom), (corner, bottom), (corner, sub(tree, top, cut)),
                          (start, top), (0.0, top)], segments=64)
    head = mill_polygon(tree, head, 6, flat_radius, bottom, top)
    return tree.smooth(head, angle=30 * RADIANS)


@asset("Screw", "object", CATALOG,
       "A wood screw with a real helical thread, a pointed tip and a flat, pan or hex head.",
       ("screw", "fastener", "thread", "hardware", "metal"))
def screw():
    tree = object_group("Screw")
    length = tree.input("Length", "FLOAT", 0.15, min=0.02, subtype="DISTANCE",
                        description="Overall length, head included")
    diameter = tree.input("Diameter", "FLOAT", 0.03, min=0.004, subtype="DISTANCE",
                          description="Outer diameter of the thread")
    pitch = tree.input("Thread Pitch", "FLOAT", 0.012, min=0.001, subtype="DISTANCE",
                       description="Distance between neighboring turns of the thread")
    head_type = tree.menu("Head", ("Flat", "Pan", "Hex"),
                          description="Countersunk flat head, rounded pan head or hex head")
    drive = tree.menu("Drive", ("Slot", "Cross", "None"),
                      description="Screwdriver recess in the head")
    material = tree.input("Material", "MATERIAL", brass())

    radius = mul(tree, diameter, 0.5)
    # A deep, sharp wood screw thread.
    depth = tree.math("MINIMUM", mul(tree, diameter, 0.17), mul(tree, pitch, 0.6))
    core = sub(tree, radius, depth)
    tip = mul(tree, diameter, 1.3)

    # Heads, each from its bottom (where it meets the shank) up to `length`.
    flat_height = mul(tree, diameter, 0.55)
    flat_bottom = sub(tree, length, flat_height)
    flat_radius = mul(tree, diameter, 1.0)
    rim = mul(tree, diameter, 0.06)
    flat = revolve(tree, [(0.0, flat_bottom), (mul(tree, radius, 0.9), flat_bottom),
                          (flat_radius, sub(tree, length, rim)), (flat_radius, length),
                          (0.0, length)])

    pan_height = mul(tree, diameter, 0.65)
    pan_bottom = sub(tree, length, pan_height)
    pan_radius = mul(tree, diameter, 0.95)
    dome = [(1.0, 0.45), (0.97, 0.66), (0.89, 0.82), (0.74, 0.93), (0.48, 0.99), (0.0, 1.0)]
    pan = revolve(tree, [(0.0, pan_bottom)] + [(mul(tree, pan_radius, r),
                                                add(tree, pan_bottom, mul(tree, pan_height, h)))
                                               for r, h in [(1.0, 0.0)] + dome])

    hex_height = mul(tree, diameter, 0.7)
    hex_bottom = sub(tree, length, hex_height)
    hexagon = hex_head(tree, mul(tree, diameter, 0.8), hex_bottom, hex_height)

    head_index = menu_choice(tree, head_type, ("Flat", "Pan", "Hex"))
    flat, pan = (tree.smooth(part, angle=40 * RADIANS) for part in (flat, pan))
    head = index_switch(tree, "GEOMETRY", head_index, [flat, pan, hexagon])
    head_bottom = index_switch(tree, "FLOAT", head_index, [flat_bottom, pan_bottom, hex_bottom])
    head_height = index_switch(tree, "FLOAT", head_index, [flat_height, pan_height, hex_height])
    cutter = recess(tree, drive, length, diameter, mul(tree, head_height, 0.45))
    head = tree.n("GeometryNodeMeshBoolean", operation="DIFFERENCE", solver="MANIFOLD",
                  Mesh_1=head, Mesh_2=cutter).outputs["Mesh"]

    # Shank: a pointed tip, the threaded core, then a plain neck under the head.
    thread_top = sub(tree, head_bottom, mul(tree, sub(tree, head_bottom, tip), 0.22))
    neck = mul(tree, radius, 0.9)
    shank = revolve(tree, [(0.0, 0.0), (mul(tree, core, 0.5), mul(tree, tip, 0.35)),
                           (core, tip), (core, thread_top),
                           (neck, add(tree, thread_top, mul(tree, pitch, 0.6))),
                           (neck, add(tree, head_bottom, mul(tree, diameter, 0.05))),
                           (0.0, add(tree, head_bottom, mul(tree, diameter, 0.05)))],
                    segments=32)

    def taper(z):
        return smoothstep(tree, z, 0.0, tip)
    thread = helix_thread(tree, core, depth, mul(tree, pitch, 0.75), pitch, mul(tree, tip, 0.12),
                          sub(tree, thread_top, mul(tree, pitch, 0.3)), scale=taper)
    shank = tree.smooth(tree.join(shank, thread), angle=40 * RADIANS)

    # Length on top of the head, diameter on the thread, pitch on the far side of the thread.
    size_gizmo(tree, length, "Z", centered=False)
    radius_gizmo(tree, tree.math("MULTIPLY", diameter, 0.5),
                 origin=vector(tree, 0.0, 0.0, mul(tree, length, 0.45)))
    pitch_gizmo(tree, pitch, radius, mul(tree, tip, 0.12))
    finish(tree, tree.join(head, shank), material, angle=None)
    return generator_object("Screw", tree)


@asset("Bolt", "object", CATALOG,
       "A hex bolt with a chamfered head and a helical thread along the end of its shank.",
       ("bolt", "fastener", "thread", "hex", "hardware", "metal"))
def bolt():
    tree = object_group("Bolt")
    length = tree.input("Length", "FLOAT", 0.16, min=0.02, subtype="DISTANCE",
                        description="Length of the shank, below the head")
    diameter = tree.input("Diameter", "FLOAT", 0.03, min=0.004, subtype="DISTANCE")
    pitch = tree.input("Thread Pitch", "FLOAT", 0.005, min=0.0005, subtype="DISTANCE",
                       description="Distance between neighboring turns of the thread")
    threaded = tree.input("Thread Length", "FLOAT", 0.6, min=0.05, max=1.0, subtype="FACTOR",
                          description="How much of the shank is threaded")
    material = tree.input("Material", "MATERIAL", steel())

    radius = mul(tree, diameter, 0.5)
    # A 60 degree machine thread.
    depth = tree.math("MINIMUM", mul(tree, pitch, 0.6), mul(tree, diameter, 0.12))
    core = sub(tree, radius, depth)
    thread_top = mul(tree, length, threaded)
    end = mul(tree, depth, 1.2)
    head_height = mul(tree, diameter, 0.65)
    head = hex_head(tree, mul(tree, diameter, 0.75), length, head_height)
    shank = revolve(tree, [(0.0, 0.0), (sub(tree, core, end), 0.0), (core, end),
                           (core, thread_top), (radius, add(tree, thread_top, pitch)),
                           (radius, add(tree, length, mul(tree, diameter, 0.05))),
                           (0.0, add(tree, length, mul(tree, diameter, 0.05)))], segments=32)
    thread = helix_thread(tree, core, depth, mul(tree, pitch, 0.9), pitch, mul(tree, end, 0.5),
                          thread_top)
    shank = tree.smooth(tree.join(shank, thread), angle=40 * RADIANS)

    # Length on top of the head, diameter on the plain part of the shank, the threaded part's
    # end on the front and the pitch at the far side of the thread.
    tree.linear_gizmo(tree.math("MULTIPLY", length, 1.0),
                      vector(tree, 0.0, 0.0, add(tree, length, head_height)), AXES["Z"],
                      color="Z")
    radius_gizmo(tree, tree.math("MULTIPLY", diameter, 0.5),
                 origin=vector(tree, 0.0, 0.0, mul(tree, length, 0.85)))
    tree.linear_gizmo(tree.math("MULTIPLY", threaded, length),
                      vector(tree, 0.0, mul(tree, radius, -1.0), thread_top), AXES["Z"])
    pitch_gizmo(tree, pitch, radius, mul(tree, end, 0.5))
    finish(tree, tree.join(head, shank), material, angle=None)
    return generator_object("Bolt", tree)


@asset("Nut", "object", CATALOG,
       "A hex nut with chamfered edges and a threaded hole, with any number of sides.",
       ("nut", "hex nut", "fastener", "thread", "hardware", "metal"))
def nut():
    tree = object_group("Nut")
    size = tree.input("Size", "FLOAT", 0.08, min=0.005, subtype="DISTANCE",
                      description="Width across the flats")
    pitch = tree.input("Thread", "FLOAT", 0.006, min=0.0005, subtype="DISTANCE",
                       description="Pitch of the thread in the hole")
    sides = tree.input("Sides", "INT", 6, min=3, max=12)
    material = tree.input("Material", "MATERIAL", steel())

    flat = mul(tree, size, 0.5)
    hole = div(tree, size, 3.2)
    height = mul(tree, size, 0.5)
    corner = div(tree, flat, tree.math("COSINE", div(tree, math.pi, sides)))
    # A 30 degree chamfer from just inside the flats out to the corners.
    rounded = mul(tree, flat, 0.93)
    chamfer = tree.math("MINIMUM", mul(tree, sub(tree, corner, rounded), 0.58),
                        mul(tree, height, 0.3))
    corner = mul(tree, corner, 1.01)
    depth = tree.math("MINIMUM", mul(tree, pitch, 0.55), mul(tree, hole, 0.25))
    # The hole's wall is a cylinder with rows dense enough to carve the thread into.
    rows = tree.math("CEIL", div(tree, height, mul(tree, pitch, 0.125)))
    segments = 64
    body = revolve(tree, [(hole, height), (hole, 0.0), (rounded, 0.0), (corner, chamfer),
                          (corner, sub(tree, height, chamfer)), (rounded, height)],
                   segments=segments)
    # Swap the revolved hole's single band of faces for the dense wall.
    _, center_radius = polar(tree)
    body = tree.n("GeometryNodeDeleteGeometry", Geometry=body, domain="FACE",
                  Selection=tree.math("LESS_THAN", center_radius,
                                      mul(tree, hole, 1.001))).outputs[0]
    wall = tree.n("GeometryNodeMeshCylinder", Vertices=segments, Side_Segments=rows,
                  Radius=hole, Depth=height, fill_type="NONE").outputs["Mesh"]
    wall = tree.n("GeometryNodeFlipFaces", Mesh=wall).outputs[0]
    wall = tree.transform(wall, translation=vector(tree, 0.0, 0.0, mul(tree, height, 0.5)))
    mesh = tree.n("GeometryNodeMergeByDistance", Geometry=tree.join(body, wall),
                  Distance=1e-5).outputs[0]

    # Carve a helical thread into the hole: the radius follows a triangle wave.
    angle, radius = polar(tree)
    _, _, z = tree.separate(position(tree))
    phase = sub(tree, div(tree, z, pitch), div(tree, angle, 2.0 * math.pi))
    wave = tree.math("PINGPONG", mul(tree, phase, 2.0), 1.0)
    in_hole = tree.math("LESS_THAN", radius, mul(tree, hole, 1.001))
    mesh = scale_radially(tree, mesh, div(tree, add(tree, hole, mul(tree, depth, wave)), hole),
                          in_hole)
    mesh = mill_polygon(tree, mesh, sides, flat, 0.0, height)

    # Size on a flat (a hex nut has one facing +Y), the pitch inside the hole and the number
    # of sides on a dial on top.
    size_gizmo(tree, size, "Y", origin=vector(tree, 0.0, 0.0, mul(tree, height, 0.5)))
    pitch_gizmo(tree, pitch, hole, 0.0, turns=4.0)
    resolution_gizmo(tree, sides, vector(tree, 0.0, 0.0, height), per_turn=6.0)
    finish(tree, mesh, material, angle=30)
    return generator_object("Nut", tree)


@asset("Washer", "object", CATALOG,
       "A flat washer with softly rounded edges, to go under a nut or bolt head.",
       ("washer", "ring", "fastener", "hardware", "metal"))
def washer():
    tree = object_group("Washer")
    inner = tree.input("Inner Radius", "FLOAT", 0.032, min=0.001, subtype="DISTANCE")
    outer = tree.input("Outer Radius", "FLOAT", 0.058, min=0.002, subtype="DISTANCE")
    thickness = tree.input("Thickness", "FLOAT", 0.006, min=0.0005, subtype="DISTANCE")
    material = tree.input("Material", "MATERIAL", palette("Starter Steel"))

    # Outer and inner edges on opposite sides, thickness on top in between.
    middle = vector(tree, 0.0, 0.0, mul(tree, thickness, 0.5))
    radius_gizmo(tree, outer, origin=middle)
    radius_gizmo(tree, inner, "-X", origin=middle)
    size_gizmo(tree, thickness, "Z", centered=False,
               origin=vector(tree, 0.0, mul(tree, add(tree, inner, outer), -0.5), 0.0))

    outer = tree.math("MAXIMUM", outer, add(tree, inner, mul(tree, thickness, 0.6)))
    edge = mul(tree, thickness, 0.22)
    profile = [(inner, edge), (add(tree, inner, edge), 0.0), (sub(tree, outer, edge), 0.0),
               (outer, edge), (outer, sub(tree, thickness, edge)),
               (sub(tree, outer, edge), thickness), (add(tree, inner, edge), thickness),
               (inner, sub(tree, thickness, edge))]
    finish(tree, revolve(tree, profile, segments=64), material, angle=50)
    return generator_object("Washer", tree)


# -----------------------------------------------------------------------------
# Machine parts.

# One tooth of a spur gear: (fraction of the tooth pitch, radius offset in modules).
# The flanks bulge like involutes; the tip is narrower than the root.
GEAR_TOOTH = ((0.0, -1.25), (0.17, -1.25), (0.215, -0.75), (0.245, 0.0), (0.285, 0.55),
              (0.355, 1.0), (0.645, 1.0), (0.715, 0.55), (0.755, 0.0), (0.785, -0.75),
              (0.83, -1.25))


@asset("Gear", "object", CATALOG,
       "A spur gear with involute-style teeth, a hub and an optional lightened web.",
       ("gear", "cog", "machine", "mechanical", "hardware", "metal"))
def gear():
    tree = object_group("Gear")
    teeth = tree.input("Teeth", "INT", 24, min=6, max=200)
    radius = tree.input("Radius", "FLOAT", 0.15, min=0.01, subtype="DISTANCE",
                        description="Pitch radius, where the teeth of two gears meet")
    thickness = tree.input("Thickness", "FLOAT", 0.04, min=0.001, subtype="DISTANCE")
    hole = tree.input("Hole Radius", "FLOAT", 0.02, min=0.0, subtype="DISTANCE")
    style = tree.menu("Style", ("Holes", "Solid"),
                      description="Lighten the web between hub and teeth with holes, or not")
    material = tree.input("Material", "MATERIAL", brass())

    # Pitch radius and hole on opposite sides, thickness on the web between two holes, and the
    # teeth on a dial at the front of the rim.
    radius_gizmo(tree, radius, origin=vector(tree, 0.0, 0.0, mul(tree, thickness, 0.5)))
    radius_gizmo(tree, hole, "-X", origin=vector(tree, 0.0, 0.0, thickness))
    size_gizmo(tree, thickness, "Z", centered=False,
               origin=vector(tree, 0.0, mul(tree, radius, 0.6), 0.0))
    resolution_gizmo(tree, teeth, vector(tree, 0.0, mul(tree, radius, -1.0), thickness),
                     per_turn=24.0)

    module = div(tree, mul(tree, radius, 2.0), teeth)
    per_tooth = len(GEAR_TOOTH)
    i = index(tree)
    tooth = tree.math("FLOOR", div(tree, i, per_tooth))
    k = sub(tree, i, mul(tree, tooth, per_tooth))
    fraction = index_switch(tree, "FLOAT", k, [f for f, _ in GEAR_TOOTH])
    offset = index_switch(tree, "FLOAT", k, [o for _, o in GEAR_TOOTH])
    angle = mul(tree, add(tree, tooth, fraction), div(tree, 2.0 * math.pi, teeth))
    r = add(tree, radius, mul(tree, module, offset))
    outline = tree.n("GeometryNodePoints", Count=mul(tree, teeth, per_tooth),
                     Position=tree.xyz(mul(tree, r, tree.math("COSINE", angle)),
                                       mul(tree, r, tree.math("SINE", angle)), 0.0)).outputs[0]
    outline = tree.n("GeometryNodePointsToCurves", Points=outline).outputs[0]
    outline = tree.n("GeometryNodeSetSplineCyclic", Curve=outline, Cyclic=True).outputs[0]

    root = sub(tree, radius, mul(tree, module, 1.25))
    hole = tree.math("MINIMUM", hole, mul(tree, root, 0.6))
    rim = sub(tree, root, mul(tree, module, 2.5))
    hub = tree.math("MINIMUM", tree.math("MAXIMUM", mul(tree, hole, 1.9),
                                         mul(tree, rim, 0.3)), mul(tree, rim, 0.6))
    ring = extruded(tree, tree.join(outline, circle(tree, rim, 96)), thickness)
    middle = extruded(tree, tree.join(circle(tree, hub), circle(tree, hole)), thickness)

    web_thickness = mul(tree, thickness, 0.45)
    web_z = mul(tree, sub(tree, thickness, web_thickness), 0.5)
    web_outer = circle(tree, add(tree, rim, mul(tree, module, 0.3)), 96)
    web_inner = circle(tree, sub(tree, hub, mul(tree, module, 0.3)))
    # Six round holes centered between hub and rim.
    gap = sub(tree, rim, hub)
    holes = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=6,
                   Radius=mul(tree, add(tree, hub, rim), 0.5)).outputs["Curve"]
    holes = tree.n("GeometryNodeInstanceOnPoints", Points=holes,
                   Instance=circle(tree, mul(tree, gap, 0.32), 32)).outputs[0]
    holes = tree.realize(holes)
    web = tree.switch(style, "GEOMETRY", {
        "Holes": extruded(tree, tree.join(web_outer, web_inner, holes), web_thickness, web_z),
        "Solid": extruded(tree, tree.join(web_outer, web_inner), thickness),
    })
    finish(tree, tree.join(ring, middle, web), material, angle=30)
    return generator_object("Gear", tree)


@asset("Spring", "object", CATALOG,
       "A compression spring with closed, ground-flat ends so it stands on its own.",
       ("spring", "coil", "helix", "mechanical", "hardware", "metal"))
def spring():
    tree = object_group("Spring")
    coils = tree.input("Coils", "FLOAT", 8.0, min=2.5, max=100.0,
                       description="Number of turns, the closed end turns included")
    radius = tree.input("Radius", "FLOAT", 0.06, min=0.002, subtype="DISTANCE",
                        description="Radius of the coil, to the middle of the wire")
    wire = tree.input("Wire Thickness", "FLOAT", 0.01, min=0.0005, subtype="DISTANCE")
    height = tree.input("Height", "FLOAT", 0.25, min=0.005, subtype="DISTANCE")
    material = tree.input("Material", "MATERIAL", palette("Starter Steel"))

    # Height on top, coil radius and wire on opposite sides, and the coils on a dial in the
    # middle: turning it winds more turns in.
    size_gizmo(tree, height, "Z", centered=False)
    middle = vector(tree, 0.0, 0.0, mul(tree, height, 0.5))
    radius_gizmo(tree, radius, origin=middle)
    tree.linear_gizmo(tree.math("MULTIPLY", wire, 0.5),
                      vector(tree, mul(tree, add(tree, radius, mul(tree, wire, 0.5)), -1.0),
                             0.0, mul(tree, height, 0.5)), AXES["-X"])
    resolution_gizmo(tree, coils, middle, per_turn=2.0)

    spiral = tree.n("GeometryNodeCurveSpiral", Resolution=32, Rotations=coils,
                    Start_Radius=radius, End_Radius=radius, Height=0.0).outputs[0]
    # Closed ends: the first and last turns rise only one wire thickness; the rest share
    # the remaining height, eased in so the pitch changes smoothly.
    turn = mul(tree, tree.n("GeometryNodeSplineParameter").outputs["Factor"], coils)
    height = tree.math("MAXIMUM", height, mul(tree, wire, add(tree, coils, 0.5)))
    closed = mul(tree, wire, turn)
    free = sub(tree, height, mul(tree, wire, add(tree, coils, 0.5)))
    open_part = mul(tree, free, smoothstep(tree, turn, 0.8, sub(tree, coils, 0.8)))
    x, y, _ = tree.separate(position(tree))
    z = add(tree, add(tree, closed, open_part), mul(tree, wire, 0.25))
    spiral = tree.n("GeometryNodeSetPosition", Geometry=spiral,
                    Position=tree.xyz(x, y, z)).outputs[0]
    profile = circle(tree, mul(tree, wire, 0.5), 12)
    mesh = tree.n("GeometryNodeCurveToMesh", Curve=spiral, Profile_Curve=profile,
                  Fill_Caps=True).outputs[0]
    # Grind both ends flat.
    px, py, pz = tree.separate(position(tree))
    flat_z = tree.math("MINIMUM", tree.math("MAXIMUM", pz, 0.0), height)
    mesh = tree.n("GeometryNodeSetPosition", Geometry=mesh,
                  Position=tree.xyz(px, py, flat_z)).outputs[0]
    finish(tree, mesh, material, angle=60)
    return generator_object("Spring", tree)


@asset("Chain", "object", CATALOG,
       "A chain of interlocking oval links lying in a gentle curve, with adjustable twist.",
       ("chain", "links", "metal", "industrial", "hardware"))
def chain():
    tree = object_group("Chain")
    size = tree.input("Link Size", "FLOAT", 0.06, min=0.005, subtype="DISTANCE",
                      description="Outer length of one link")
    count = tree.input("Link Count", "INT", 13, min=1, max=500)
    wire = tree.input("Wire Thickness", "FLOAT", 0.2, min=0.05, max=0.3, subtype="FACTOR",
                      description="Thickness of the wire as a fraction of the link size")
    twist = tree.input("Twist", "FLOAT", 0.0, subtype="ANGLE",
                       description="Extra rotation of each link around the chain")
    bend = tree.input("Bend", "FLOAT", 150 * RADIANS, min=-360 * RADIANS, max=360 * RADIANS,
                      subtype="ANGLE", description="How far the chain curves along its length")
    material = tree.input("Material", "MATERIAL", palette("Starter Steel"))

    thickness = mul(tree, size, wire)
    width = mul(tree, size, 0.62)
    # A stadium-shaped link along X, swept from its centerline.
    middle = tree.n("GeometryNodeCurvePrimitiveQuadrilateral",
                    Width=sub(tree, size, thickness),
                    Height=sub(tree, width, thickness)).outputs[0]
    middle = tree.n("GeometryNodeFilletCurve", Curve=middle,
                    Radius=mul(tree, sub(tree, width, thickness), 0.4999),
                    Limit_Radius=True, Mode="Poly", Count=6).outputs[0]
    link = tree.n("GeometryNodeCurveToMesh", Curve=middle,
                  Profile_Curve=circle(tree, mul(tree, thickness, 0.5), 10)).outputs[0]
    link = tree.smooth(link)

    # Links sit one inner length apart along an arc, alternating flat and upright.
    pitch = sub(tree, size, mul(tree, thickness, 2.0))
    i = index(tree)
    total = mul(tree, pitch, count)
    curvature = div(tree, tree.math("MAXIMUM", tree.math("ABSOLUTE", bend), 1e-4), total)
    # Positive bends curve toward -Y, so the chain faces a default front view.
    curvature = mul(tree, curvature, tree.math("SIGN", add(tree, bend, 1e-9)))
    curvature = mul(tree, curvature, -1.0)
    s = mul(tree, sub(tree, i, mul(tree, sub(tree, count, 1.0), 0.5)), pitch)
    heading = mul(tree, s, curvature)
    where = tree.xyz(div(tree, tree.math("SINE", heading), curvature),
                     div(tree, sub(tree, 1.0, tree.math("COSINE", heading)), curvature), 0.0)
    alternate = mul(tree, tree.math("FLOORED_MODULO", i, 2.0), math.pi / 2)
    roll = add(tree, alternate, mul(tree, i, twist))
    points = tree.n("GeometryNodePoints", Count=count, Position=where).outputs[0]

    def on_arc(s):
        """Where the chain's center line is `s` along it, and which way it runs there."""
        h = mul(tree, s, curvature)
        return (tree.xyz(div(tree, tree.math("SINE", h), curvature),
                         div(tree, sub(tree, 1.0, tree.math("COSINE", h)), curvature), 0.0),
                tree.xyz(tree.math("COSINE", h), tree.math("SINE", h), 0.0))

    # Each added link grows both ends by half a pitch: the count box sits on the +X end, the
    # twist dial on the -X end. Link size and wire thickness are on the middle link, which
    # lies flat; the bend dial is above it.
    reach = add(tree, mul(tree, sub(tree, count, 1.0), mul(tree, pitch, 0.5)),
                mul(tree, size, 0.5))
    end, heading = on_arc(reach)
    tree.linear_gizmo(tree.math("MULTIPLY", count, mul(tree, pitch, 0.5)), end, heading,
                      color="SECONDARY", style="BOX")
    start, heading = on_arc(mul(tree, reach, -1.0))
    tree.dial_gizmo(tree.math("MULTIPLY", twist, 1.0), start, heading, radius=0.8)
    tree.linear_gizmo(tree.math("MULTIPLY", size, 0.31),
                      vector(tree, 0.0, mul(tree, width, 0.5), 0.0), AXES["Y"])
    tree.linear_gizmo(tree.math("MULTIPLY", wire, mul(tree, size, 0.5)),
                      vector(tree, 0.0, mul(tree, sub(tree, width, thickness), -0.5),
                             mul(tree, thickness, 0.5)), AXES["Z"])
    tree.dial_gizmo(tree.math("MULTIPLY", bend, 1.0), vector(tree, 0.0, 0.0, mul(tree, size, 0.6)),
                    AXES["Z"], radius=0.8)
    links = tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=link,
                   Rotation=tree.xyz(roll, 0.0, heading)).outputs[0]
    finish(tree, tree.realize(links), material, angle=180)
    return generator_object("Chain", tree)


# -----------------------------------------------------------------------------
# Runs.

# Corner points of each pipe run, in units of the leg length.
PIPE_RUNS = {
    "L Bend": ((0, 0, 0), (1, 0, 0), (1, 0, 0.9)),
    "S Bend": ((0, 0, 0), (0.8, 0, 0), (0.8, 0, 0.7), (1.6, 0, 0.7)),
    "U Bend": ((0, 0, 0), (1, 0, 0), (1, 0, 0.6), (0, 0, 0.6)),
}


def flange(tree, pipe_radius, inner_radius):
    """A bolted flange around Z, centered on the origin."""
    outer = mul(tree, pipe_radius, 1.75)
    thick = mul(tree, pipe_radius, 0.3)
    half = mul(tree, thick, 0.5)
    edge = mul(tree, thick, 0.12)
    disc = revolve(tree, [(inner_radius, mul(tree, half, -1.0)),
                          (sub(tree, outer, edge), mul(tree, half, -1.0)),
                          (outer, sub(tree, edge, half)), (outer, sub(tree, half, edge)),
                          (sub(tree, outer, edge), half), (inner_radius, half)], segments=48)
    bolt = tree.n("GeometryNodeMeshCylinder", Vertices=6, Radius=mul(tree, pipe_radius, 0.13),
                  Depth=mul(tree, thick, 1.6)).outputs["Mesh"]
    ring = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=8,
                  Radius=mul(tree, pipe_radius, 1.42)).outputs["Curve"]
    bolts = tree.realize(tree.n("GeometryNodeInstanceOnPoints", Points=ring,
                                Instance=bolt).outputs[0])
    return tree.join(disc, bolts)


@asset("Pipe", "object", CATALOG,
       "A run of hollow pipe with smooth bends and bolted flanges at the joints.",
       ("pipe", "plumbing", "industrial", "tube", "flange", "hardware"))
def pipe():
    tree = object_group("Pipe")
    shape = tree.menu("Shape", tuple(PIPE_RUNS), description="Layout of the pipe run")
    leg = tree.input("Leg Length", "FLOAT", 0.7, min=0.05, subtype="DISTANCE",
                     description="Length of the straight runs between bends")
    radius = tree.input("Radius", "FLOAT", 0.05, min=0.002, subtype="DISTANCE")
    bend = tree.input("Bend Radius", "FLOAT", 0.12, min=0.0, subtype="DISTANCE",
                      description="Radius of the bends, measured to the pipe's center line")
    spacing = tree.input("Flange Spacing", "FLOAT", 0.5, min=0.02, subtype="DISTANCE",
                         description="Longest distance between flanges along a straight run")
    material = tree.input("Material", "MATERIAL", palette("Starter Copper"))

    inner = mul(tree, radius, 0.82)
    bend_radius = bend
    # Keep bends within the shortest leg.
    bend = tree.math("MINIMUM", tree.math("MAXIMUM", bend, mul(tree, radius, 1.1)),
                     mul(tree, leg, 0.35))
    runs = {}
    legs = {}
    for name, corners in PIPE_RUNS.items():
        points = [(mul(tree, leg, x), mul(tree, leg, y), mul(tree, leg, z))
                  for x, y, z in corners]
        runs[name] = polyline(tree, points)
        # Straight legs between the bends, for placing flanges.
        pieces = []
        for j in range(len(corners) - 1):
            a, b = corners[j], corners[j + 1]
            span = math.dist(a, b)
            direction = [(b[n] - a[n]) / span for n in range(3)]
            trim_start = bend if j > 0 else 0.0
            trim_end = bend if j < len(corners) - 2 else 0.0
            start = vector(tree, *(add(tree, mul(tree, leg, a[n]),
                                       mul(tree, trim_start, direction[n])) for n in range(3)))
            end = vector(tree, *(sub(tree, mul(tree, leg, b[n]),
                                     mul(tree, trim_end, direction[n])) for n in range(3)))
            pieces.append(tree.n("GeometryNodeCurvePrimitiveLine", Start=start,
                                 End=end).outputs[0])
        legs[name] = tree.join(*pieces)
    shape_index = menu_choice(tree, shape, tuple(PIPE_RUNS))
    run = index_switch(tree, "GEOMETRY", shape_index, list(runs.values()))
    run = tree.n("GeometryNodeFilletCurve", Curve=run, Radius=bend, Limit_Radius=True,
                 Mode="Poly", Count=10).outputs[0]
    wall = tree.join(circle(tree, radius, 32),
                     tree.n("GeometryNodeReverseCurve", Curve=circle(tree, inner, 32)).outputs[0])
    tube = tree.n("GeometryNodeCurveToMesh", Curve=run, Profile_Curve=wall).outputs[0]
    tube = tree.smooth(tube)

    straight = index_switch(tree, "GEOMETRY", shape_index, list(legs.values()))
    spline_length = tree.n("GeometryNodeSplineLength").outputs["Length"]
    flanges = tree.n("GeometryNodeResampleCurve", Curve=straight, Mode="Count",
                     Count=add(tree, tree.math("CEIL", div(tree, spline_length, spacing)),
                               1.0)).outputs[0]
    tangent = tree.n("GeometryNodeInputTangent").outputs[0]
    align = tree.n("FunctionNodeAlignRotationToVector", axis="Z", Vector=tangent).outputs[0]
    flanges = tree.n("GeometryNodeInstanceOnPoints", Points=flanges,
                     Instance=tree.smooth(flange(tree, radius, inner), angle=40 * RADIANS),
                     Rotation=align).outputs[0]
    mesh = tree.join(tube, tree.realize(flanges))

    # Leg length at the run's right end: grounding re-centers the run, so the end moves by half
    # its width. The bend radius is at the start of the first bend, the flange spacing one
    # spacing along the first leg, and the pipe radius on the side of the first leg.
    def right_end(corners):
        right = max(x for x, _, _ in corners)
        heights = [z for x, _, z in corners if x == right]
        return right, sum(heights) / len(heights)
    ends = [right_end(corners) for corners in PIPE_RUNS.values()]
    tree.linear_gizmo(
        tree.math("MULTIPLY", leg, index_switch(tree, "FLOAT", shape_index,
                                                [right * 0.5 for right, _ in ends])),
        index_switch(tree, "VECTOR", shape_index,
                     [vector(tree, mul(tree, leg, right), 0.0, mul(tree, leg, z))
                      for right, z in ends]), AXES["X"], color="X")
    corner = index_switch(tree, "FLOAT", shape_index,
                          [corners[1][0] for corners in PIPE_RUNS.values()])
    tree.linear_gizmo(tree.math("MULTIPLY", bend_radius, 1.0),
                      vector(tree, sub(tree, mul(tree, leg, corner), bend), 0.0,
                             mul(tree, radius, 2.0)), AXES["-X"])
    tree.linear_gizmo(tree.math("MULTIPLY", spacing, 1.0),
                      vector(tree, spacing, mul(tree, radius, 2.0), 0.0), AXES["X"],
                      color="SECONDARY", style="BOX")
    radius_gizmo(tree, radius, "-Y", origin=vector(tree, mul(tree, leg, 0.15), 0.0, 0.0))
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Pipe", tree)


def plug(tree, thickness, prongs, material):
    """A molded plug along +Z, the cable entering at the origin."""
    body = mul(tree, thickness, 1.9)
    length = mul(tree, thickness, 5.0)
    relief = mul(tree, thickness, 2.2)
    edge = mul(tree, body, 0.18)
    shape = revolve(tree, [(0.0, 0.0), (mul(tree, thickness, 0.62), 0.0),
                           (mul(tree, thickness, 0.85), relief),
                           (body, add(tree, relief, mul(tree, thickness, 0.6))),
                           (body, sub(tree, length, edge)),
                           (sub(tree, body, edge), length), (0.0, length)], segments=32)
    shape = tree.material(shape, material)
    if not prongs:
        return shape
    pin = tree.n("GeometryNodeMeshCube", Size=vector(tree, mul(tree, thickness, 0.18),
                                                     mul(tree, thickness, 0.7),
                                                     mul(tree, thickness, 2.2))).outputs[0]
    pins = tree.join(*(tree.transform(pin, translation=vector(
        tree, mul(tree, thickness, side), 0.0, add(tree, length, mul(tree, thickness, 1.0))))
        for side in (-0.55, 0.55)))
    return tree.join(shape, tree.material(pins, palette("Starter Brass")))


@asset("Cable", "object", CATALOG,
       "A power cable sagging between two plugs, with adjustable slack and thickness.",
       ("cable", "wire", "cord", "plug", "electric", "hardware"))
def cable():
    tree = object_group("Cable")
    length = tree.input("Length", "FLOAT", 0.7, min=0.05, subtype="DISTANCE",
                        description="Distance between the two plugs")
    slack = tree.input("Slack", "FLOAT", 0.4, min=0.0, max=2.0, subtype="FACTOR",
                       description="Extra cable length, as a fraction of the distance")
    thickness = tree.input("Thickness", "FLOAT", 0.018, min=0.001, subtype="DISTANCE")
    material = tree.input("Material", "MATERIAL", palette("Starter Dark"))
    plug_material = tree.input("Plug Material", "MATERIAL", palette("Starter White"))

    # A parabola is close enough to a catenary; its sag gives the cable the extra length.
    half = mul(tree, length, 0.5)
    sag = mul(tree, length, tree.math("SQRT", mul(tree, slack, 3.0 / 8.0)))
    line = tree.n("GeometryNodeCurvePrimitiveLine", Start=vector(tree, mul(tree, half, -1.0),
                                                                 0.0, 0.0),
                  End=vector(tree, half, 0.0, 0.0)).outputs[0]
    line = tree.n("GeometryNodeResampleCurve", Curve=line, Mode="Count", Count=64).outputs[0]
    x, y, _ = tree.separate(position(tree))
    u = div(tree, x, half)
    z = mul(tree, sag, sub(tree, mul(tree, u, u), 1.0))
    line = tree.n("GeometryNodeSetPosition", Geometry=line, Position=tree.xyz(x, y, z)).outputs[0]
    wire = tree.n("GeometryNodeCurveToMesh", Curve=line,
                  Profile_Curve=circle(tree, mul(tree, thickness, 0.5), 12)).outputs[0]
    wire = tree.material(tree.smooth(wire), material)

    # Plugs continue the cable's direction at each end.
    slope = div(tree, mul(tree, sag, 2.0), half)
    ends = []
    for side, prongs in ((1.0, True), (-1.0, False)):
        body = plug(tree, thickness, prongs, plug_material)
        direction = tree.xyz(side, 0.0, slope)
        rotation = tree.n("FunctionNodeAlignRotationToVector", axis="Z",
                          Vector=direction).outputs[0]
        ends.append(tree.transform(body, rotation=rotation,
                                   translation=vector(tree, mul(tree, half, side), 0.0, 0.0)))
    plugs = tree.smooth(tree.join(*ends), angle=40 * RADIANS)

    # Length at the pronged plug. Slack at the other plug: grounding keeps the sagging middle on
    # the floor, so more slack lifts the plugs; near the default it lifts them about half the
    # length per unit of slack. Thickness on the side of the low point.
    size_gizmo(tree, length, "X")
    tree.linear_gizmo(tree.math("MULTIPLY", slack, mul(tree, length, 0.5)),
                      vector(tree, mul(tree, half, -1.0), 0.0, 0.0), AXES["Z"])
    tree.linear_gizmo(tree.math("MULTIPLY", thickness, 0.5),
                      vector(tree, 0.0, mul(tree, thickness, -0.5), mul(tree, sag, -1.0)),
                      AXES["-Y"])
    tree.output(rest_on_ground(tree, tree.join(wire, plugs)))
    return generator_object("Cable", tree)
