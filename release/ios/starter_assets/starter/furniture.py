# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Furniture and Props: everyday furniture and tabletop props at real-world sizes."""

import math

import bpy

from .common import (AXES, RADIANS, count_gizmo, generator_object, object_group, palette,
                     radius_gizmo, resolution_gizmo, rest_on_ground, simple_material, size_gizmo)
from .registry import asset

CATALOG = "Objects/Furniture and Props"

SQRT2 = math.sqrt(2.0)


# -----------------------------------------------------------------------------
# Small building blocks.

def _ops(tree):
    """Shorthands for the arithmetic every builder needs."""
    return (lambda a, b: tree.math("ADD", a, b),
            lambda a, b: tree.math("SUBTRACT", a, b),
            lambda a, b: tree.math("MULTIPLY", a, b),
            lambda a, b: tree.math("DIVIDE", a, b))


def _position(tree):
    return tree.n("GeometryNodeInputPosition").outputs[0]


def _index(tree):
    return tree.n("GeometryNodeInputIndex").outputs[0]


def _random(tree, low, high, seed, offset=0, ident=None, data_type="FLOAT"):
    """A random value per element; `offset` decorrelates several values from one seed."""
    if offset:
        seed = tree.math("ADD", seed, offset)
    node = tree.n("FunctionNodeRandomValue", data_type=data_type, Seed=seed, ID=ident)
    if data_type == "BOOLEAN":
        tree.set_input(tree._input(node, "Probability"), low)
    else:
        tree.set_input(tree._input(node, "Min"), low)
        tree.set_input(tree._input(node, "Max"), high)
    return tree.out(node, "Value")


def _box(tree, size, center):
    cube = tree.n("GeometryNodeMeshCube", Size=size).outputs["Mesh"]
    return tree.transform(cube, translation=center)


def _index_switch(tree, data_type, index, values):
    node = tree.nodes.new("GeometryNodeIndexSwitch")
    node.data_type = data_type
    while len(node.index_switch_items) < len(values):
        node.index_switch_items.new()
    tree.set_input(node.inputs[0], index)
    for i, value in enumerate(values):
        tree.set_input(node.inputs[i + 1], value)
    return node.outputs[0]


def _switch(tree, flag, false, true, data_type="GEOMETRY"):
    node = tree.n("GeometryNodeSwitch", input_type=data_type, Switch=flag)
    tree.set_input(tree._input(node, "False"), false)
    tree.set_input(tree._input(node, "True"), true)
    return tree.out(node, "Output")


def _menu_index(tree, menu, items):
    """The chosen item's position, so one menu can drive any number of switches."""
    return tree.switch(menu, "INT", {name: i for i, name in enumerate(items)})


def _instance(tree, points, instance, rotation=None, scale=None, realize=True):
    node = tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=instance,
                  Rotation=rotation, Scale=scale)
    return tree.realize(node.outputs[0]) if realize else node.outputs[0]


def _points(tree, count, position):
    """`count` points placed by a position field, which may depend on the other points."""
    points = tree.n("GeometryNodePoints", Count=count).outputs[0]
    return tree.n("GeometryNodeSetPosition", Geometry=points, Position=position).outputs[0]


def _euler(tree, x=0.0, y=0.0, z=0.0):
    return tree.n("FunctionNodeEulerToRotation", Euler=tree.xyz(x, y, z)).outputs[0]


def _store(tree, geometry, name, value, data_type="FLOAT", domain="POINT"):
    return tree.n("GeometryNodeStoreNamedAttribute", data_type=data_type, domain=domain,
                  Geometry=geometry, Name=name, Value=value).outputs[0]


def _lathe(tree, rings, segments, level=0, creases=(), fill="NGON"):
    """Revolve a profile around Z. `rings` are (radius, height) pairs from one end to the other.

    With caps, the first and last rings are closed with flat faces, so a profile that goes up the
    outside and back down the inside makes a hollow vessel with a floor. `level` smooths the
    profile with subdivision; `creases` lists ring indices that stay sharp.
    """
    count = len(rings)
    mesh = tree.n("GeometryNodeMeshCylinder", fill_type=fill, Vertices=segments,
                  Side_Segments=count - 1, Radius=1.0, Depth=1.0).outputs["Mesh"]
    x, y, z = tree.separate(_position(tree))
    ring = tree.math("ROUND", tree.math("MULTIPLY", tree.math("ADD", z, 0.5), count - 1))
    radius = _index_switch(tree, "FLOAT", ring, [r for r, _ in rings])
    height = _index_switch(tree, "FLOAT", ring, [h for _, h in rings])
    if creases:
        mesh = _store(tree, mesh, "ring", ring)
    mesh = tree.n("GeometryNodeSetPosition", Geometry=mesh, Position=tree.xyz(
        tree.math("MULTIPLY", x, radius), tree.math("MULTIPLY", y, radius), height)).outputs[0]
    if level:
        crease = 0.0
        if creases:
            ring_attribute = tree.out(tree.n("GeometryNodeInputNamedAttribute", data_type="FLOAT",
                                             Name="ring"), "Attribute")
            for index in creases:
                near = tree.math("LESS_THAN", tree.math("ABSOLUTE", tree.math(
                    "SUBTRACT", ring_attribute, index)), 0.01)
                crease = tree.math("MAXIMUM", crease, near)
        mesh = tree.n("GeometryNodeSubdivisionSurface", Mesh=mesh, Level=level,
                      Edge_Crease=crease).outputs[0]
    if creases:
        mesh = tree.n("GeometryNodeRemoveAttribute", Geometry=mesh, Name="ring").outputs[0]
    return mesh


def _leg(tree, style, thickness, length):
    """A leg standing on Z = 0: straight square, tapered square or round, by `style` index."""
    add, sub, mul, div = _ops(tree)
    center = tree.xyz(0.0, 0.0, mul(length, 0.5))
    straight = _box(tree, tree.xyz(thickness, thickness, length), center)
    corner = div(thickness, SQRT2)
    tapered = tree.n("GeometryNodeMeshCone", fill_type="NGON", Vertices=4, Radius_Top=corner,
                     Radius_Bottom=mul(corner, 0.55), Depth=length).outputs["Mesh"]
    # Cones start at Z = 0.
    tapered = tree.transform(tapered, rotation=(0.0, 0.0, 45 * RADIANS))
    rounded = tree.n("GeometryNodeMeshCylinder", fill_type="NGON", Vertices=16,
                     Radius=mul(thickness, 0.5), Depth=length).outputs["Mesh"]
    rounded = tree.transform(rounded, translation=center)
    return _index_switch(tree, "GEOMETRY", style, [straight, tapered, rounded])


def _corners(tree, half_x, half_y):
    """Four points at (+-half_x, +-half_y, 0)."""
    return tree.n("GeometryNodeMeshGrid", Size_X=tree.math("MULTIPLY", half_x, 2.0),
                  Size_Y=tree.math("MULTIPLY", half_y, 2.0), Vertices_X=2,
                  Vertices_Y=2).outputs["Mesh"]


def _tinted_material(name, color, roughness=0.6, attribute="tint"):
    """A material whose color is varied per part by a float attribute around 0."""
    material = bpy.data.materials.get(name)
    if material:
        return material
    material = simple_material(name, color, roughness=roughness)
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    bsdf = nodes["Principled BSDF"]
    tint = nodes.new("ShaderNodeAttribute")
    tint.attribute_name = attribute
    scale = nodes.new("ShaderNodeMath")
    scale.operation = "ADD"
    scale.inputs[1].default_value = 1.0
    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    mix.inputs["Factor"].default_value = 1.0
    mix.inputs["A"].default_value = (*color, 1.0)
    links.new(tint.outputs["Fac"], scale.inputs[0])
    links.new(scale.outputs[0], mix.inputs["B"])
    links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    tint.location = (-700, 200)
    scale.location = (-500, 200)
    mix.location = (-300, 200)
    return material


def _attribute_color_material(name, color, roughness=0.5, attribute="color"):
    """A material colored by a color attribute, for parts that each get their own color."""
    material = bpy.data.materials.get(name)
    if material:
        return material
    material = simple_material(name, color, roughness=roughness)
    nodes = material.node_tree.nodes
    source = nodes.new("ShaderNodeAttribute")
    source.attribute_name = attribute
    source.location = (-400, 200)
    material.node_tree.links.new(source.outputs["Color"],
                                 nodes["Principled BSDF"].inputs["Base Color"])
    return material


# -----------------------------------------------------------------------------
# Furniture.

@asset("Table", "object", CATALOG,
       "A wooden table with a rectangular or round top and straight, tapered or pedestal legs.",
       ("table", "furniture", "dining", "desk", "wood", "interior"))
def table():
    tree = object_group("Table")
    add, sub, mul, div = _ops(tree)
    shapes = ("Rectangle", "Round")
    shape = _menu_index(tree, tree.menu("Shape", shapes, description="Outline of the top"),
                        shapes)
    length = tree.input("Length", "FLOAT", 1.6, min=0.3, max=6.0, subtype="DISTANCE")
    width = tree.input("Width", "FLOAT", 0.9, min=0.3, max=3.0, subtype="DISTANCE")
    height = tree.input("Height", "FLOAT", 0.75, min=0.2, max=1.5, subtype="DISTANCE")
    top = tree.input("Top Thickness", "FLOAT", 0.04, min=0.01, max=0.2, subtype="DISTANCE")
    styles = ("Straight", "Tapered", "Pedestal")
    style = _menu_index(tree, tree.menu("Leg Style", styles), styles)
    leg = tree.input("Leg Thickness", "FLOAT", 0.07, min=0.02, max=0.3, subtype="DISTANCE")
    material = tree.input("Material", "MATERIAL", palette("Starter Wood"))

    top_center = tree.xyz(0.0, 0.0, sub(height, mul(top, 0.5)))
    rectangle = _box(tree, tree.xyz(length, width, top), top_center)
    disc = tree.n("GeometryNodeMeshCylinder", fill_type="NGON", Vertices=64, Radius=0.5,
                  Depth=1.0).outputs["Mesh"]
    disc = tree.transform(disc, translation=top_center, scale=tree.xyz(length, width, top))
    board = _index_switch(tree, "GEOMETRY", shape, [rectangle, disc])

    leg_length = sub(height, top)
    inset = add(mul(leg, 0.5), 0.05)
    half_x = _index_switch(tree, "FLOAT", shape, [sub(mul(length, 0.5), inset),
                                                   mul(length, 0.33)])
    half_y = _index_switch(tree, "FLOAT", shape, [sub(mul(width, 0.5), inset),
                                                   mul(width, 0.33)])
    legs = _instance(tree, _corners(tree, half_x, half_y), _leg(tree, style, leg, leg_length))
    # An apron joins the legs under the top.
    apron_height = 0.1
    apron = 0.022
    apron_z = sub(leg_length, apron_height * 0.5)
    face_y = sub(add(half_y, mul(leg, 0.5)), apron * 0.5 + 0.008)
    face_x = sub(add(half_x, mul(leg, 0.5)), apron * 0.5 + 0.008)
    long_side = tree.xyz(mul(half_x, 2.0), apron, apron_height)
    short_side = tree.xyz(apron, mul(half_y, 2.0), apron_height)
    aprons = tree.join(
        _box(tree, long_side, tree.xyz(0.0, face_y, apron_z)),
        _box(tree, long_side, tree.xyz(0.0, mul(face_y, -1.0), apron_z)),
        _box(tree, short_side, tree.xyz(face_x, 0.0, apron_z)),
        _box(tree, short_side, tree.xyz(mul(face_x, -1.0), 0.0, apron_z)))
    legs = tree.join(legs, aprons)

    foot = mul(tree.math("MINIMUM", length, width), 0.3)
    column = mul(leg, 0.6)
    plate = mul(tree.math("MINIMUM", length, width), 0.22)
    pedestal = _lathe(tree, [
        (mul(foot, 0.97), 0.0), (foot, 0.012), (foot, 0.03), (mul(foot, 0.5), 0.06),
        (mul(column, 1.5), 0.11), (column, 0.2), (column, sub(leg_length, 0.12)),
        (mul(column, 1.4), sub(leg_length, 0.05)), (plate, sub(leg_length, 0.02)),
        (plate, leg_length)], 32, level=2, creases=(1, 2, 8, 9))
    legs = _index_switch(tree, "GEOMETRY", style, [legs, legs, pedestal])

    top_mid = tree.xyz(0.0, 0.0, sub(height, mul(top, 0.5)))
    size_gizmo(tree, length, "X", origin=top_mid)
    size_gizmo(tree, width, "Y", origin=top_mid)
    size_gizmo(tree, height, "Z", centered=False)
    # The top keeps its upper face and grows down, so its arrow points down from the underside.
    tree.linear_gizmo(mul(top, 1.0), tree.xyz(0.0, mul(width, -0.5), leg_length), AXES["-Z"])
    # Corner legs keep their outer faces, so the inner face of a front leg moves with the drag;
    # the pedestal's column grows outward.
    leg_z = mul(leg_length, 0.5)
    tree.linear_gizmo(mul(leg, 1.0), _index_switch(tree, "VECTOR", style, [
        tree.xyz(sub(half_x, mul(leg, 0.5)), mul(half_y, -1.0), leg_z)] * 2 + [
        tree.xyz(column, 0.0, leg_z)]), _index_switch(tree, "VECTOR", style, [
            AXES["-X"], AXES["-X"], AXES["X"]]))

    mesh = tree.smooth(tree.join(board, legs), angle=35 * RADIANS)
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Table", tree)


@asset("Chair", "object", CATALOG,
       "A wooden dining chair with an optional slatted back and seat cushion.",
       ("chair", "seat", "furniture", "dining", "wood", "interior"))
def chair():
    tree = object_group("Chair")
    add, sub, mul, div = _ops(tree)
    seat_height = tree.input("Seat Height", "FLOAT", 0.46, min=0.1, max=1.2, subtype="DISTANCE",
                             description="Height of the top of the seat")
    seat = tree.input("Seat Size", "FLOAT", 0.44, min=0.2, max=1.2, subtype="DISTANCE",
                      description="Width and depth of the seat")
    has_back = tree.input("Back", "BOOL", True)
    back_height = tree.input("Back Height", "FLOAT", 0.42, min=0.1, max=1.5, subtype="DISTANCE",
                             description="Height of the back above the seat")
    styles = ("Straight", "Tapered", "Round")
    style = _menu_index(tree, tree.menu("Leg Style", styles), styles)
    leg = tree.input("Leg Thickness", "FLOAT", 0.036, min=0.01, max=0.15, subtype="DISTANCE")
    has_cushion = tree.input("Cushion", "BOOL", True)
    material = tree.input("Material", "MATERIAL", palette("Starter Wood"))
    cushion_material = tree.input("Cushion Material", "MATERIAL", palette("Starter Fabric"))

    thickness = 0.035
    half = sub(mul(seat, 0.5), add(mul(leg, 0.5), 0.012))
    seat_bottom = sub(seat_height, thickness)
    board = _box(tree, tree.xyz(seat, seat, thickness),
                 tree.xyz(0.0, 0.0, sub(seat_height, thickness * 0.5)))
    legs = _instance(tree, _corners(tree, half, half), _leg(tree, style, leg, seat_bottom))
    rail = mul(leg, 0.55)
    span = mul(half, 2.0)
    stretchers = tree.join(
        _box(tree, tree.xyz(rail, span, rail), tree.xyz(half, 0.0, 0.16)),
        _box(tree, tree.xyz(rail, span, rail), tree.xyz(mul(half, -1.0), 0.0, 0.16)),
        _box(tree, tree.xyz(span, rail, rail), tree.xyz(0.0, mul(half, -1.0), 0.22)),
        _box(tree, tree.xyz(span, rail, rail), tree.xyz(0.0, half, 0.22)))

    # Back posts continue the rear legs; tapered legs get straight posts.
    post_style = _index_switch(tree, "INT", style, [0, 0, 2])
    post = tree.transform(_leg(tree, post_style, leg, back_height),
                          translation=tree.xyz(0.0, 0.0, seat_height))
    posts = _instance(tree, tree.transform(_corners(tree, half, 0.0),
                                           translation=tree.xyz(0.0, half, 0.0)), post)
    top_rail = 0.07
    back_top = add(seat_height, back_height)
    rail_box = _box(tree, tree.xyz(span, mul(leg, 0.7), top_rail),
                    tree.xyz(0.0, half, sub(back_top, top_rail * 0.5 + 0.01)))
    slat_length = sub(back_height, top_rail + 0.01)
    slat = _box(tree, tree.xyz(0.032, mul(leg, 0.45), slat_length),
                tree.xyz(0.0, half, add(seat_height, mul(slat_length, 0.5))))
    step = div(span, 4.0)
    slat_points = tree.n("GeometryNodeMeshLine", Count=3,
                         Start_Location=tree.xyz(mul(step, -1.0), 0.0, 0.0),
                         Offset=tree.xyz(step, 0.0, 0.0)).outputs[0]
    back = tree.join(posts, rail_box, _instance(tree, slat_points, slat))
    wood = tree.join(board, legs, stretchers, _switch(tree, has_back, None, back))
    wood = tree.material(tree.smooth(wood, angle=35 * RADIANS), material)

    pad = tree.n("GeometryNodeMeshCube", Size=tree.xyz(sub(seat, 0.03), sub(seat, 0.03),
                                                        0.05)).outputs["Mesh"]
    pad = tree.n("GeometryNodeSubdivisionSurface", Mesh=pad, Level=3,
                 Edge_Crease=0.75).outputs[0]
    pad = tree.transform(pad, translation=tree.xyz(0.0, -0.005, add(seat_height, 0.018)))
    pad = tree.material(tree.smooth(pad), cushion_material)

    size_gizmo(tree, seat, "X", origin=tree.xyz(0.0, 0.0, seat_bottom))
    tree.linear_gizmo(mul(seat_height, 1.0), tree.xyz(0.0, mul(seat, -0.5), seat_height),
                      AXES["Z"], color="Z")
    tree.linear_gizmo(mul(back_height, 1.0), tree.xyz(0.0, half, back_top), AXES["Z"], color="Z")
    # The legs keep their outer faces, so the inner face of a front leg moves with the drag.
    tree.linear_gizmo(mul(leg, 1.0), tree.xyz(sub(half, mul(leg, 0.5)), mul(half, -1.0),
                                              mul(seat_bottom, 0.6)), AXES["-X"])

    mesh = tree.join(wood, _switch(tree, has_cushion, None, pad))
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Chair", tree)


@asset("Shelf", "object", CATALOG,
       "A wooden bookcase with any number of shelves and compartments.",
       ("shelf", "bookcase", "bookshelf", "storage", "furniture", "interior"))
def shelf():
    tree = object_group("Shelf")
    add, sub, mul, div = _ops(tree)
    rows = tree.input("Rows", "INT", 5, min=1, max=20)
    columns = tree.input("Columns", "INT", 2, min=1, max=12)
    width = tree.input("Width", "FLOAT", 1.2, min=0.2, max=6.0, subtype="DISTANCE")
    height = tree.input("Height", "FLOAT", 1.8, min=0.2, max=4.0, subtype="DISTANCE")
    depth = tree.input("Depth", "FLOAT", 0.32, min=0.1, max=1.0, subtype="DISTANCE")
    board = tree.input("Board Thickness", "FLOAT", 0.022, min=0.005, max=0.08,
                       subtype="DISTANCE")
    has_back = tree.input("Back Panel", "BOOL", True)
    material = tree.input("Material", "MATERIAL", palette("Starter Light Wood"))

    plinth = 0.07
    half_w = mul(width, 0.5)
    inner_w = sub(width, mul(board, 2.0))
    side = tree.xyz(board, depth, height)
    side_x = sub(half_w, mul(board, 0.5))
    sides = tree.join(
        _box(tree, side, tree.xyz(side_x, 0.0, mul(height, 0.5))),
        _box(tree, side, tree.xyz(mul(side_x, -1.0), 0.0, mul(height, 0.5))))
    shelf_size = tree.xyz(inner_w, sub(depth, 0.01), board)
    top = _box(tree, tree.xyz(inner_w, depth, board),
               tree.xyz(0.0, 0.0, sub(height, mul(board, 0.5))))
    bottom_z = add(plinth, mul(board, 0.5))
    bottom = _box(tree, shelf_size, tree.xyz(0.0, 0.005, bottom_z))
    kick = _box(tree, tree.xyz(inner_w, board, plinth),
                tree.xyz(0.0, sub(mul(depth, -0.5), mul(board, -1.5)), plinth * 0.5))

    # Shelves split the space between the bottom and top boards into equal rows.
    low = add(plinth, board)
    high = sub(height, board)
    pitch = div(add(sub(high, low), board), rows)
    shelf_points = tree.n("GeometryNodeMeshLine", Count=sub(rows, 1),
                          Start_Location=tree.xyz(0.0, 0.005, add(bottom_z, pitch)),
                          Offset=tree.xyz(0.0, 0.0, pitch)).outputs[0]
    shelves = _instance(tree, shelf_points, _box(tree, shelf_size, (0.0, 0.0, 0.0)))
    column_pitch = div(add(inner_w, board), columns)
    divider_points = tree.n("GeometryNodeMeshLine", Count=sub(columns, 1),
                            Start_Location=tree.xyz(add(mul(half_w, -1.0),
                                                        add(mul(board, 0.5), column_pitch)),
                                                    0.005, mul(add(low, high), 0.5)),
                            Offset=tree.xyz(column_pitch, 0.0, 0.0)).outputs[0]
    divider = _box(tree, tree.xyz(board, sub(depth, 0.01), sub(high, low)), (0.0, 0.0, 0.0))
    dividers = _instance(tree, divider_points, divider)
    back = _box(tree, tree.xyz(inner_w, 0.006, sub(height, plinth)),
                tree.xyz(0.0, sub(mul(depth, 0.5), 0.003), mul(add(height, plinth), 0.5)))

    front = mul(depth, -0.5)
    size_gizmo(tree, width, "X", origin=tree.xyz(0.0, 0.0, mul(height, 0.5)))
    size_gizmo(tree, height, "Z", centered=False)
    size_gizmo(tree, depth, "-Y", origin=tree.xyz(mul(width, 0.25), 0.0, height))
    # Rows and columns divide the case, so their handles sit at its top and right end and move
    # one row or column per spacing.
    count_gizmo(tree, rows, pitch, "Z", origin=tree.xyz(mul(width, -0.25), front, 0.0),
                extra=add(board, plinth))
    count_gizmo(tree, columns, column_pitch, "X",
                origin=tree.xyz(add(mul(half_w, -1.0), mul(board, 0.5)), front, plinth * 0.5))
    # The bottom board sits on the plinth and grows up.
    tree.linear_gizmo(mul(board, 1.0), tree.xyz(mul(width, 0.25), front, add(plinth, board)),
                      AXES["Z"])

    mesh = tree.join(sides, top, bottom, kick, shelves, dividers,
                     _switch(tree, has_back, None, back))
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Shelf", tree)


@asset("Crate", "object", CATALOG,
       "A slatted wooden crate with corner posts, a plank floor and adjustable wear.",
       ("crate", "box", "wood", "storage", "container", "prop"))
def crate():
    tree = object_group("Crate")
    add, sub, mul, div = _ops(tree)
    size = tree.input("Size", "VECTOR", (0.6, 0.4, 0.34), min=0.05, subtype="XYZ")
    planks = tree.input("Planks per Side", "INT", 3, min=1, max=16)
    gap = tree.input("Gap", "FLOAT", 0.018, min=0.0, max=0.2, subtype="DISTANCE",
                     description="Space between the side planks")
    wear = tree.input("Wear", "FLOAT", 0.35, min=0.0, max=1.0, subtype="FACTOR",
                      description="How crooked, uneven and weathered the planks are")
    seed = tree.input("Seed", "INT", 0)
    material = tree.input("Material", "MATERIAL",
                          _tinted_material("Starter Crate Wood", (0.6, 0.42, 0.24), 0.75))

    sx, sy, sz = tree.separate(size)
    thick = 0.016
    post = 0.04
    plank_h = div(sub(sz, mul(gap, sub(planks, 1))), planks)
    total = mul(planks, 4)
    index = _index(tree)
    side = tree.math("FLOOR", div(index, planks))
    row = tree.math("FLOORED_MODULO", index, planks)
    odd = tree.math("FLOORED_MODULO", side, 2.0)
    angle = mul(side, 90 * RADIANS)
    distance = mul(sub(tree.math("MAXIMUM", mul(sy, sub(1.0, odd)), mul(sx, odd)), thick), 0.5)
    length = sub(_index_switch(tree, "FLOAT", tree.math("ROUND", odd), [sx, sy]),
                 mul(odd, thick * 2.0))
    z = add(mul(row, add(plank_h, gap)), mul(plank_h, 0.5))
    # Wear shifts, tilts and shortens each plank a little.
    sag = mul(wear, _random(tree, -0.006, 0.006, seed, 1))
    position = tree.xyz(mul(tree.math("SINE", angle), distance),
                        mul(tree.math("COSINE", angle), mul(distance, -1.0)), add(z, sag))
    rotation = _euler(tree, mul(wear, _random(tree, -0.05, 0.05, seed, 2)),
                      mul(wear, _random(tree, -0.04, 0.04, seed, 3)), angle)
    scale = tree.xyz(sub(length, mul(wear, _random(tree, 0.0, 0.03, seed, 4))), thick,
                     plank_h)
    points = _points(tree, total, position)
    plank = tree.n("GeometryNodeMeshCube", Size=(1.0, 1.0, 1.0)).outputs["Mesh"]
    sides = _instance(tree, points, plank, rotation=rotation, scale=scale, realize=False)
    # Very worn crates lose a plank or two.
    missing = _random(tree, tree.math("MAXIMUM", mul(sub(wear, 0.55), 0.35), 0.0), None, seed,
                      5, data_type="BOOLEAN")
    sides = tree.n("GeometryNodeDeleteGeometry", domain="INSTANCE", Geometry=sides,
                   Selection=tree.math("MULTIPLY", missing,
                                       tree.math("GREATER_THAN", row, 0.5))).outputs[0]
    sides = _store(tree, sides, "tint", mul(_random(tree, -0.18, 0.12, seed, 6),
                                            add(mul(wear, 0.8), 0.4)), domain="INSTANCE")

    post_points = _corners(tree, sub(mul(sx, 0.5), thick + post * 0.5),
                           sub(mul(sy, 0.5), thick + post * 0.5))
    posts = _instance(tree, post_points, plank, scale=tree.xyz(post, post, sub(sz, 0.002)),
                      realize=False)
    posts = tree.n("GeometryNodeTranslateInstances", Instances=posts,
                   Translation=tree.xyz(0.0, 0.0, mul(sz, 0.5)), Local_Space=False).outputs[0]
    posts = _store(tree, posts, "tint", -0.12, domain="INSTANCE")
    floor_width = div(sub(sy, thick * 2.0), 4.0)
    floor_points = tree.n("GeometryNodeMeshLine", Count=4,
                          Start_Location=tree.xyz(0.0, mul(add(mul(floor_width, 1.5), 0.0), -1.0),
                                                  thick * 0.5),
                          Offset=tree.xyz(0.0, floor_width, 0.0)).outputs[0]
    floor = _instance(tree, floor_points, plank, realize=False,
                      scale=tree.xyz(sub(sx, thick * 2.0), sub(floor_width, 0.006), thick))
    floor = _store(tree, floor, "tint", mul(_random(tree, -0.15, 0.05, seed, 7), wear),
                   domain="INSTANCE")

    size_gizmo(tree, sx, "X", origin=tree.xyz(0.0, 0.0, mul(sz, 0.5)))
    size_gizmo(tree, sy, "Y", origin=tree.xyz(0.0, 0.0, mul(sz, 0.5)))
    size_gizmo(tree, sz, "Z", centered=False)
    count_gizmo(tree, planks, add(plank_h, gap), "Z",
                origin=tree.xyz(mul(sx, 0.25), mul(sy, -0.5), 0.0), extra=mul(gap, -1.0))
    # The first gap's top rises by gap / planks as the planks get thinner, so scale the drag.
    tree.linear_gizmo(div(gap, planks), tree.xyz(mul(sx, -0.25), mul(sy, -0.5),
                                                 add(plank_h, gap)), AXES["Z"])

    mesh = tree.realize(tree.join(sides, posts, floor))
    mesh = tree.material(mesh, material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Crate", tree)


@asset("Barrel", "object", CATALOG,
       "A staved wooden barrel bound with iron hoops, with a bulge you can adjust.",
       ("barrel", "cask", "keg", "wood", "container", "prop"))
def barrel():
    tree = object_group("Barrel")
    add, sub, mul, div = _ops(tree)
    height = tree.input("Height", "FLOAT", 0.9, min=0.1, max=4.0, subtype="DISTANCE")
    radius = tree.input("Radius", "FLOAT", 0.27, min=0.03, max=2.0, subtype="DISTANCE",
                        description="Radius of the barrel at its ends")
    bulge = tree.input("Bulge", "FLOAT", 0.06, min=0.0, max=1.0, subtype="DISTANCE",
                       description="How much wider the barrel is in the middle than at its ends")
    staves = tree.input("Staves", "INT", 20, min=6, max=64)
    hoops = tree.input("Hoops", "INT", 4, min=0, max=12)
    seed = tree.input("Seed", "INT", 0, description="Varies the color of the staves")
    material = tree.input("Material", "MATERIAL",
                          _tinted_material("Starter Oak", (0.42, 0.24, 0.11), 0.7))
    hoop_material = tree.input("Hoop Material", "MATERIAL", simple_material(
        "Starter Iron", (0.07, 0.07, 0.075), metallic=0.85, roughness=0.45))

    def profile(v):
        """Radius at height fraction `v`: widest in the middle."""
        centered = sub(mul(v, 2.0), 1.0)
        return add(radius, mul(bulge, sub(1.0, mul(centered, centered))))

    cylinder = tree.n("GeometryNodeMeshCylinder", fill_type="NGON", Vertices=mul(staves, 8),
                      Side_Segments=16, Radius=1.0, Depth=1.0)
    x, y, z = tree.separate(_position(tree))
    v = add(z, 0.5)
    turns = div(tree.math("ARCTAN2", y, x), 2.0 * math.pi)
    stave = mul(tree.math("FRACT", turns), staves)
    edge = tree.math("ABSOLUTE", sub(stave, tree.math("ROUND", stave)))
    groove = mul(tree.math("LESS_THAN", edge, 0.01), 0.004)
    r = sub(profile(v), groove)
    body = tree.n("GeometryNodeSetPosition", Geometry=cylinder.outputs["Mesh"],
                  Position=tree.xyz(mul(x, r), mul(y, r), mul(v, height))).outputs[0]
    # Recess the lids inside a rim, like the chime of a real barrel.
    ends = tree.math("MAXIMUM", tree.out(cylinder, "Top"), tree.out(cylinder, "Bottom"))
    rim = tree.n("GeometryNodeExtrudeMesh", Mesh=body, Selection=ends, Offset_Scale=0.0)
    body = tree.n("GeometryNodeScaleElements", Geometry=tree.out(rim, "Mesh"),
                  Selection=tree.out(rim, "Top"), Scale=0.93).outputs[0]
    body = tree.out(tree.n("GeometryNodeExtrudeMesh", Mesh=body, Selection=tree.out(rim, "Top"),
                           Offset_Scale=-0.02), "Mesh")
    body = _store(tree, body, "tint", _random(tree, -0.22, 0.18, seed,
                                              ident=tree.math("FLOOR", stave)), domain="FACE")
    body = tree.material(tree.smooth(body, angle=50 * RADIANS), material)

    i = _index(tree)
    last = tree.math("MAXIMUM", sub(hoops, 1), 1.0)
    t = add(div(i, last), mul(tree.math("COMPARE", hoops, 1.0, 0.1), 0.5))
    hoop_v = add(0.07, mul(t, 0.86))
    hoop_r = add(profile(hoop_v), 0.003)
    hoop_points = _points(tree, hoops, tree.xyz(0.0, 0.0, mul(hoop_v, height)))
    circle = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=96, Radius=1.0).outputs[0]
    rings = _instance(tree, hoop_points, circle, scale=tree.xyz(hoop_r, hoop_r, 1.0))
    band = tree.n("GeometryNodeCurvePrimitiveQuadrilateral", Width=0.006,
                  Height=0.045).outputs[0]
    rings = tree.n("GeometryNodeCurveToMesh", Curve=rings, Profile_Curve=band,
                   Fill_Caps=True).outputs[0]
    rings = tree.material(tree.smooth(rings, angle=40 * RADIANS), hoop_material)

    size_gizmo(tree, height, "Z", centered=False)
    radius_gizmo(tree, radius, origin=tree.xyz(0.0, 0.0, height))
    tree.linear_gizmo(mul(bulge, 1.0), tree.xyz(0.0, mul(add(radius, bulge), -1.0),
                                                mul(height, 0.5)), AXES["-Y"])
    resolution_gizmo(tree, staves, tree.xyz(0.0, 0.0, mul(height, 0.5)), per_turn=20.0)
    # Hoops spread over the barrel; one more per spacing dragged up from the top one.
    hoop_spacing = div(mul(height, 0.86), tree.math("MAXIMUM", hoops, 1.0))
    tree.linear_gizmo(mul(hoops, hoop_spacing),
                      tree.xyz(mul(add(radius, mul(bulge, 0.25)), -1.0), 0.0, mul(height, 0.93)),
                      AXES["Z"], color="SECONDARY", style="BOX")

    tree.output(rest_on_ground(tree, tree.join(body, rings)))
    return generator_object("Barrel", tree)


# -----------------------------------------------------------------------------
# Tableware and decoration.

@asset("Bottle", "object", CATALOG,
       "A hollow glass bottle with a long neck and an optional cork.",
       ("bottle", "glass", "wine", "drink", "kitchen", "prop"))
def bottle():
    tree = object_group("Bottle")
    add, sub, mul, div = _ops(tree)
    height = tree.input("Height", "FLOAT", 0.3, min=0.05, max=1.0, subtype="DISTANCE")
    radius = tree.input("Radius", "FLOAT", 0.037, min=0.01, max=0.3, subtype="DISTANCE")
    neck = tree.input("Neck Length", "FLOAT", 0.085, min=0.01, max=0.5, subtype="DISTANCE")
    neck_radius = tree.input("Neck Radius", "FLOAT", 0.0135, min=0.004, max=0.2,
                             subtype="DISTANCE")
    has_cork = tree.input("Cork", "BOOL", True)
    material = tree.input("Material", "MATERIAL", simple_material(
        "Starter Bottle Glass", (0.3, 0.62, 0.36), roughness=0.04, Transmission_Weight=1.0,
        IOR=1.5))
    cork_material = tree.input("Cork Material", "MATERIAL", simple_material(
        "Starter Cork", (0.55, 0.38, 0.22), roughness=0.85))

    wall = 0.003
    shoulder = mul(sub(radius, neck_radius), 1.4)
    neck_base = sub(height, neck)
    body_top = sub(neck_base, shoulder)
    lip = add(neck_radius, 0.0025)
    inner = sub(neck_radius, wall)
    glass = _lathe(tree, [
        (mul(radius, 0.8), 0.0), (radius, 0.004), (radius, 0.02), (radius, body_top),
        (add(neck_radius, mul(sub(radius, neck_radius), 0.35)), add(body_top,
                                                                    mul(shoulder, 0.75))),
        (neck_radius, neck_base), (neck_radius, sub(height, 0.014)),
        (lip, sub(height, 0.011)), (lip, sub(height, 0.002)),
        (sub(neck_radius, wall * 0.3), height), (inner, sub(height, 0.004)),
        (inner, neck_base), (sub(radius, wall), body_top), (sub(radius, wall), 0.022),
        (mul(radius, 0.55), 0.014)], 24, level=2)
    glass = tree.material(tree.smooth(glass), material)

    cork = tree.n("GeometryNodeMeshCone", fill_type="NGON", Vertices=24,
                  Radius_Top=add(inner, 0.001), Radius_Bottom=sub(inner, 0.0005),
                  Depth=0.036).outputs["Mesh"]
    cork = tree.transform(cork, translation=tree.xyz(0.0, 0.0, sub(height, 0.022)))
    cork = tree.material(tree.smooth(cork, angle=40 * RADIANS), cork_material)

    size_gizmo(tree, height, "Z", centered=False)
    radius_gizmo(tree, radius, origin=tree.xyz(0.0, 0.0, mul(body_top, 0.5)))
    # A longer neck lowers the shoulder.
    tree.linear_gizmo(mul(neck, 1.0), tree.xyz(mul(neck_radius, -1.0), 0.0, neck_base),
                      AXES["-Z"])
    radius_gizmo(tree, neck_radius, "Y", origin=tree.xyz(0.0, 0.0, sub(height, mul(neck, 0.5))))

    tree.output(rest_on_ground(tree, tree.join(glass, _switch(tree, has_cork, None, cork))))
    return generator_object("Bottle", tree)


@asset("Mug", "object", CATALOG,
       "A glazed ceramic mug, hollow inside, with an optional handle.",
       ("mug", "cup", "coffee", "tea", "kitchen", "ceramic", "prop"))
def mug():
    tree = object_group("Mug")
    add, sub, mul, div = _ops(tree)
    height = tree.input("Height", "FLOAT", 0.095, min=0.02, max=0.5, subtype="DISTANCE")
    radius = tree.input("Radius", "FLOAT", 0.041, min=0.01, max=0.3, subtype="DISTANCE")
    wall = tree.input("Wall Thickness", "FLOAT", 0.004, min=0.001, max=0.03, subtype="DISTANCE")
    has_handle = tree.input("Handle", "BOOL", True)
    material = tree.input("Material", "MATERIAL", simple_material(
        "Starter Glazed Ceramic", (0.1, 0.32, 0.42), roughness=0.12))

    inner = sub(radius, wall)
    body = _lathe(tree, [
        (sub(radius, 0.006), 0.0), (sub(radius, 0.002), 0.0005), (radius, 0.006),
        (radius, sub(height, 0.004)), (sub(radius, mul(wall, 0.5)), height),
        (inner, sub(height, 0.004)), (inner, add(wall, 0.008)),
        (sub(inner, 0.006), wall)], 32, level=2)

    loop = mul(height, 0.3)
    arc = tree.n("GeometryNodeCurveArc", mode="RADIUS", Resolution=24, Radius=loop,
                 Start_Angle=-110 * RADIANS, Sweep_Angle=220 * RADIANS).outputs["Curve"]
    arc = tree.transform(arc, rotation=(90 * RADIANS, 0.0, 0.0),
                         translation=tree.xyz(add(radius, mul(loop, 0.2)), 0.0,
                                              mul(height, 0.52)))
    arc = tree.n("GeometryNodeSetCurveNormal", Curve=arc, Mode="Free",
                 Normal=(0.0, 1.0, 0.0)).outputs[0]
    section = tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=12, Radius=1.0).outputs[0]
    section = tree.transform(section, scale=tree.xyz(0.0065, mul(wall, 1.6), 1.0))
    handle = tree.n("GeometryNodeCurveToMesh", Curve=arc, Profile_Curve=section,
                    Fill_Caps=True).outputs[0]

    tree.linear_gizmo(mul(height, 1.0), tree.xyz(0.0, mul(radius, -1.0), height), AXES["Z"],
                      color="Z")
    radius_gizmo(tree, radius, "-X", origin=tree.xyz(0.0, 0.0, mul(height, 0.5)))
    # The wall grows inward from the outside, so its arrow points in from the inner rim.
    tree.linear_gizmo(mul(wall, 1.0), tree.xyz(0.0, inner, height), AXES["-Y"])

    mesh = tree.join(body, _switch(tree, has_handle, None, handle))
    mesh = tree.material(tree.smooth(mesh), material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Mug", tree)


@asset("Vase", "object", CATALOG,
       "A hollow turned vase whose belly, neck and lip are shaped with a few sliders.",
       ("vase", "pottery", "ceramic", "decoration", "flower", "interior"))
def vase():
    tree = object_group("Vase")
    add, sub, mul, div = _ops(tree)
    height = tree.input("Height", "FLOAT", 0.32, min=0.03, max=2.0, subtype="DISTANCE")
    radius = tree.input("Radius", "FLOAT", 0.1, min=0.01, max=1.0, subtype="DISTANCE",
                        description="Radius at the widest point")
    belly = tree.input("Belly", "FLOAT", 0.38, min=0.1, max=0.75, subtype="FACTOR",
                       description="Height of the widest point, as a fraction of the height")
    neck = tree.input("Neck", "FLOAT", 0.38, min=0.1, max=1.0, subtype="FACTOR",
                      description="Width of the neck relative to the belly")
    lip = tree.input("Lip", "FLOAT", 0.55, min=0.1, max=1.2, subtype="FACTOR",
                     description="Width of the opening relative to the belly")
    foot = tree.input("Foot", "FLOAT", 0.5, min=0.1, max=1.0, subtype="FACTOR",
                      description="Width of the base relative to the belly")
    smoothness = tree.input("Smoothness", "INT", 2, min=0, max=3,
                            description="Subdivision levels of the turned profile")
    material = tree.input("Material", "MATERIAL", simple_material(
        "Starter Terracotta", (0.55, 0.22, 0.11), roughness=0.55))

    wall = 0.005
    belly_z = mul(height, belly)
    neck_z = mul(height, 0.8)
    rings = [
        (mul(mul(radius, foot), 0.95), 0.0),
        (mul(radius, foot), 0.008),
        (mul(radius, add(mul(foot, 0.4), 0.6)), mul(belly_z, 0.45)),
        (radius, belly_z),
        (mul(radius, add(mul(neck, 0.45), 0.55)), add(belly_z, mul(sub(neck_z, belly_z), 0.55))),
        (mul(radius, neck), neck_z),
        (mul(radius, lip), sub(height, 0.006)),
    ]
    outer = rings[1:]
    rings.append((sub(mul(radius, lip), wall * 0.5), height))
    for r, z in reversed(outer[1:]):
        rings.append((sub(r, wall), z))
    rings.append((sub(mul(mul(radius, foot), 0.9), wall), wall * 2.0))
    size_gizmo(tree, height, "Z", centered=False)
    radius_gizmo(tree, radius, origin=tree.xyz(0.0, 0.0, belly_z))
    # Shape factors are fractions of the height or the belly radius, so scale them to meters.
    tree.linear_gizmo(mul(belly, height), tree.xyz(mul(radius, -1.0), 0.0, belly_z), AXES["Z"])
    tree.linear_gizmo(mul(neck, radius), tree.xyz(0.0, mul(radius, neck), neck_z), AXES["Y"])
    tree.linear_gizmo(mul(lip, radius), tree.xyz(0.0, mul(mul(radius, lip), -1.0), height),
                      AXES["-Y"])
    tree.linear_gizmo(mul(foot, radius), tree.xyz(mul(mul(radius, foot), -1.0), 0.0, 0.008),
                      AXES["-X"])
    resolution_gizmo(tree, smoothness, tree.xyz(0.0, 0.0, mul(belly_z, 0.5)), per_turn=4.0)

    mesh = _lathe(tree, rings, 24, level=smoothness)
    mesh = tree.material(tree.smooth(mesh), material)
    tree.output(rest_on_ground(tree, mesh))
    return generator_object("Vase", tree)


@asset("Book Stack", "object", CATALOG,
       "A stack of hardcover books in assorted sizes and cover colors.",
       ("book", "books", "stack", "library", "reading", "prop", "interior"))
def book_stack():
    tree = object_group("Book Stack")
    add, sub, mul, div = _ops(tree)
    count = tree.input("Count", "INT", 6, min=1, max=40)
    variation = tree.input("Size Variation", "FLOAT", 0.5, min=0.0, max=1.0, subtype="FACTOR",
                           description="How much the books differ in size and placement")
    seed = tree.input("Seed", "INT", 0)
    cover_material = tree.input("Cover Material", "MATERIAL", _attribute_color_material(
        "Starter Book Cover", (0.4, 0.1, 0.08), roughness=0.55, attribute="cover_color"))
    page_material = tree.input("Page Material", "MATERIAL", simple_material(
        "Starter Paper", (0.85, 0.8, 0.68), roughness=0.8))

    # One book, 1 m on each side, scaled per copy: two boards and a spine around the pages.
    covers = tree.join(
        _box(tree, (1.0, 1.0, 0.09), (0.0, 0.0, 0.455)),
        _box(tree, (1.0, 1.0, 0.09), (0.0, 0.0, -0.455)),
        _box(tree, (0.06, 1.0, 1.0), (-0.47, 0.0, 0.0)))
    pages = _box(tree, (0.93, 0.95, 0.82), (0.0, 0.0, 0.0))
    book = tree.join(tree.material(covers, cover_material), tree.material(pages, page_material))

    spread = mul(variation, 0.35)
    size = add(1.0, mul(spread, _random(tree, -1.0, 1.0, seed, 1)))
    width = mul(0.16, size)
    length = mul(mul(0.235, size), add(1.0, mul(spread, _random(tree, -0.15, 0.15, seed, 2))))
    thickness = _random(tree, 0.022, 0.048, seed, 3)
    below = tree.out(tree.n("GeometryNodeAccumulateField", Value=thickness), "Trailing")
    jitter = mul(variation, 0.012)
    position = tree.xyz(mul(jitter, _random(tree, -1.0, 1.0, seed, 4)),
                        mul(jitter, _random(tree, -1.0, 1.0, seed, 5)),
                        add(below, mul(thickness, 0.5)))
    flip = mul(tree.math("ROUND", _random(tree, 0.0, 1.0, seed, 6)), math.pi)
    turn = add(flip, mul(add(mul(variation, 0.3), 0.04), _random(tree, -1.0, 1.0, seed, 7)))
    points = _points(tree, count, position)
    books = _instance(tree, points, book, rotation=_euler(tree, z=turn),
                      scale=tree.xyz(width, length, thickness), realize=False)

    colors = [(0.42, 0.05, 0.05), (0.04, 0.1, 0.28), (0.05, 0.22, 0.12), (0.62, 0.42, 0.08),
              (0.25, 0.05, 0.12), (0.05, 0.28, 0.3), (0.3, 0.16, 0.08), (0.08, 0.08, 0.09),
              (0.55, 0.5, 0.42)]
    pick = _random(tree, 0, len(colors) - 1, seed, 8, data_type="INT")
    color = _index_switch(tree, "RGBA", pick, [(*c, 1.0) for c in colors])
    books = _store(tree, books, "cover_color", color, data_type="FLOAT_COLOR",
                   domain="INSTANCE")
    books = tree.realize(books)
    # Books average 35 mm thick; the handle rides on top of the stack.
    _, _, stack_top = tree.separate(tree.out(tree.n("GeometryNodeBoundBox", Geometry=books),
                                             "Max"))
    tree.linear_gizmo(mul(count, 0.035), tree.xyz(0.0, 0.0, stack_top), AXES["Z"],
                      color="SECONDARY", style="BOX")
    tree.output(rest_on_ground(tree, books))
    return generator_object("Book Stack", tree)


@asset("Lamp", "object", CATALOG,
       "A desk or floor lamp with a fabric shade and a glowing bulb.",
       ("lamp", "light", "shade", "desk", "floor", "lighting", "interior"))
def lamp():
    tree = object_group("Lamp")
    add, sub, mul, div = _ops(tree)
    styles = ("Desk", "Floor")
    style = _menu_index(tree, tree.menu("Style", styles), styles)
    extra = tree.input("Extra Height", "FLOAT", 0.0, min=-0.3, max=2.5, subtype="DISTANCE",
                       description="Added to the style's usual height")
    shade_size = tree.input("Shade Size", "FLOAT", 1.0, min=0.3, max=3.0,
                            description="Size of the shade relative to the style's usual one")
    material = tree.input("Material", "MATERIAL", palette("Starter Brass"))
    shade_material = tree.input("Shade Material", "MATERIAL", simple_material(
        "Starter Lamp Shade", (0.9, 0.84, 0.7), roughness=0.85,
        Emission_Color=(1.0, 0.82, 0.55), Emission_Strength=0.5))
    bulb_material = tree.input("Bulb Material", "MATERIAL", palette("Starter Emission"))

    def pick(desk, floor, data_type="FLOAT"):
        return _index_switch(tree, data_type, style, [desk, floor])

    # An offset rather than a height, so each style keeps its own and the gizmo can drag it.
    shade_h = mul(pick(0.21, 0.3), shade_size)
    height = tree.math("MAXIMUM", add(pick(0.55, 1.6), extra), add(shade_h, 0.15))
    bottom_r = mul(pick(0.165, 0.22), shade_size)
    top_r = mul(pick(0.12, 0.17), shade_size)
    shade_z = sub(height, shade_h)
    thick = 0.003
    shade = _lathe(tree, [(bottom_r, shade_z), (top_r, height), (sub(top_r, thick), height),
                          (sub(bottom_r, thick), shade_z), (bottom_r, shade_z)], 64,
                   fill="NONE")
    shade = tree.n("GeometryNodeMergeByDistance", Geometry=shade, Distance=0.0001).outputs[0]
    shade = tree.material(tree.smooth(shade, angle=50 * RADIANS), shade_material)

    bulb_z = sub(height, mul(shade_h, 0.6))
    bulb_r = pick(0.03, 0.04)
    bulb = tree.n("GeometryNodeMeshUVSphere", Segments=24, Rings=12, Radius=bulb_r).outputs[0]
    bulb = tree.transform(bulb, translation=tree.xyz(0.0, 0.0, bulb_z), scale=(1.0, 1.0, 1.2))
    bulb = tree.material(tree.smooth(bulb), bulb_material)

    # Desk: a turned ginger jar base. Floor: a weighted disc and a slender pole.
    jar_top = mul(shade_z, 0.82)
    jar = _lathe(tree, [
        (0.07, 0.0), (0.075, 0.006), (0.075, 0.018), (0.055, 0.028),
        (0.085, mul(jar_top, 0.45)), (0.05, mul(jar_top, 0.85)), (0.02, mul(jar_top, 0.95)),
        (0.022, jar_top)], 32, level=2, creases=(1, 2))
    desk_stem_from = jar_top
    disc = _lathe(tree, [(0.15, 0.0), (0.16, 0.008), (0.16, 0.022), (0.12, 0.04),
                         (0.025, 0.05), (0.02, 0.055)], 48, level=2, creases=(1, 2))
    base = pick(jar, disc, "GEOMETRY")
    stem_from = pick(desk_stem_from, 0.05)
    stem_to = sub(bulb_z, mul(bulb_r, 1.1))
    stem_r = pick(0.007, 0.012)
    stem = tree.n("GeometryNodeMeshCylinder", fill_type="NGON", Vertices=16, Radius=stem_r,
                  Depth=sub(stem_to, stem_from)).outputs["Mesh"]
    stem = tree.transform(stem, translation=tree.xyz(0.0, 0.0, mul(add(stem_to, stem_from),
                                                                   0.5)))
    socket = tree.n("GeometryNodeMeshCylinder", fill_type="NGON", Vertices=16,
                    Radius=mul(bulb_r, 0.55), Depth=mul(bulb_r, 0.8)).outputs["Mesh"]
    socket = tree.transform(socket, translation=tree.xyz(0.0, 0.0, sub(bulb_z,
                                                                      mul(bulb_r, 1.2))))
    # A spider: crossed wires holding the shade from its top.
    wire_z = sub(height, 0.01)
    wire_span = mul(sub(top_r, thick), 2.0)
    wires = tree.join(
        _box(tree, tree.xyz(wire_span, 0.003, 0.003), tree.xyz(0.0, 0.0, wire_z)),
        _box(tree, tree.xyz(0.003, wire_span, 0.003), tree.xyz(0.0, 0.0, wire_z)))
    rod_from = add(bulb_z, mul(bulb_r, 1.2))
    rod = tree.n("GeometryNodeMeshCylinder", fill_type="NGON", Vertices=8, Radius=0.003,
                 Depth=sub(wire_z, rod_from)).outputs["Mesh"]
    rod = tree.transform(rod, translation=tree.xyz(0.0, 0.0, mul(add(wire_z, rod_from), 0.5)))
    metal = tree.join(base, stem, socket, wires, rod)
    metal = tree.material(tree.smooth(metal, angle=40 * RADIANS), material)

    size_gizmo(tree, extra, "Z", origin=tree.xyz(0.0, 0.0, sub(height, extra)), centered=False)
    tree.linear_gizmo(mul(shade_size, pick(0.165, 0.22)), tree.xyz(bottom_r, 0.0, shade_z),
                      AXES["X"])

    tree.output(rest_on_ground(tree, tree.join(metal, shade, bulb)))
    return generator_object("Lamp", tree)


@asset("Coin Stack", "object", CATALOG,
       "A leaning stack of gold coins with raised rims.",
       ("coin", "coins", "gold", "money", "treasure", "prop"))
def coin_stack():
    tree = object_group("Coin Stack")
    add, sub, mul, div = _ops(tree)
    count = tree.input("Count", "INT", 12, min=1, max=60)
    lean = tree.input("Lean", "FLOAT", 0.35, min=0.0, max=1.0, subtype="FACTOR",
                      description="How far the coins drift sideways as the stack rises")
    seed = tree.input("Seed", "INT", 0)
    radius = tree.input("Coin Radius", "FLOAT", 0.0125, min=0.003, max=0.1, subtype="DISTANCE")
    thickness = tree.input("Coin Thickness", "FLOAT", 0.0022, min=0.0005, max=0.02,
                           subtype="DISTANCE")
    material = tree.input("Material", "MATERIAL", palette("Starter Gold"))

    edge = mul(thickness, 0.22)
    rim = mul(radius, 0.1)
    relief = mul(thickness, 0.18)
    coin = _lathe(tree, [
        (sub(radius, edge), 0.0), (radius, edge), (radius, sub(thickness, edge)),
        (sub(radius, edge), thickness), (sub(radius, rim), thickness),
        (sub(radius, add(rim, edge)), sub(thickness, relief)),
        (mul(radius, 0.55), sub(thickness, relief)), (mul(radius, 0.5), sub(thickness,
                                                                         mul(relief, 0.3))),
        (mul(radius, 0.42), sub(thickness, mul(relief, 0.3))),
        (mul(radius, 0.4), sub(thickness, relief))], 36)
    coin = tree.material(tree.smooth(coin, angle=40 * RADIANS), material)

    drift = mul(mul(radius, 0.22), lean)
    step = tree.xyz(mul(drift, _random(tree, -1.0, 1.0, seed, 1)),
                    mul(drift, _random(tree, -1.0, 1.0, seed, 2)), thickness)
    accumulate = tree.n("GeometryNodeAccumulateField", data_type="FLOAT_VECTOR", Value=step)
    position = tree.vmath("ADD", tree.out(accumulate, "Trailing"),
                          tree.xyz(mul(drift, 0.6), 0.0, 0.0))
    tilt = mul(lean, 0.03)
    rotation = _euler(tree, mul(tilt, _random(tree, -1.0, 1.0, seed, 3)),
                      mul(tilt, _random(tree, -1.0, 1.0, seed, 4)),
                      _random(tree, 0.0, 2.0 * math.pi, seed, 5))
    points = _points(tree, count, position)
    coins = _instance(tree, points, coin, rotation=rotation)
    stack = mul(count, thickness)
    count_gizmo(tree, count, thickness, "Z")
    # Thicker coins raise the top of the stack by the change times the count.
    tree.linear_gizmo(mul(thickness, count), tree.xyz(mul(radius, -1.0), 0.0, stack), AXES["Z"],
                      color="Z")
    radius_gizmo(tree, radius, origin=tree.xyz(0.0, 0.0, mul(stack, 0.5)))
    tree.output(rest_on_ground(tree, coins))
    return generator_object("Coin Stack", tree)


def _face_down(tree, mesh):
    """Turn a centrally symmetric solid so its first face points up and its opposite one down."""
    normal = tree.n("GeometryNodeInputNormal").outputs[0]
    sample = tree.n("GeometryNodeSampleIndex", data_type="FLOAT_VECTOR", domain="FACE",
                    Geometry=mesh, Value=normal, Index=0)
    align = tree.n("FunctionNodeAlignRotationToVector", axis="Z", Vector=sample.outputs[0])
    rotation = tree.n("FunctionNodeInvertRotation", Rotation=align.outputs[0]).outputs[0]
    return tree.transform(mesh, rotation=rotation)


# Pips of a six-sided die on a 1 m cube: (face normal, u axis, v axis, pips).
_PIP_LAYOUTS = {
    1: ((0, 0),),
    2: ((-1, -1), (1, 1)),
    3: ((-1, -1), (0, 0), (1, 1)),
    4: ((-1, -1), (-1, 1), (1, -1), (1, 1)),
    5: ((-1, -1), (-1, 1), (1, -1), (1, 1), (0, 0)),
    6: ((-1, -1), (-1, 0), (-1, 1), (1, -1), (1, 0), (1, 1)),
}
# Opposite faces add up to seven; the preview sees 5, 3 and 6.
_DIE_FACES = (
    ((0, 0, 1), (1, 0, 0), (0, 1, 0), 5),
    ((0, 0, -1), (1, 0, 0), (0, 1, 0), 2),
    ((0, -1, 0), (1, 0, 0), (0, 0, 1), 3),
    ((0, 1, 0), (1, 0, 0), (0, 0, 1), 4),
    ((1, 0, 0), (0, 1, 0), (0, 0, 1), 6),
    ((-1, 0, 0), (0, 1, 0), (0, 0, 1), 1),
)


def _pips(spacing):
    """Positions and normals of every pip on a die 1 m across."""
    positions, normals = [], []
    for normal, u, v, value in _DIE_FACES:
        for a, b in _PIP_LAYOUTS[value]:
            positions.append(tuple(0.5 * n + spacing * (a * uu + b * vv)
                                   for n, uu, vv in zip(normal, u, v)))
            normals.append(tuple(float(n) for n in normal))
    return positions, normals


@asset("Dice", "object", CATALOG,
       "A rounded game die with 4, 6, 8, 12 or 20 faces; the six-sided one has pips.",
       ("dice", "die", "game", "board game", "tabletop", "prop"))
def dice():
    tree = object_group("Dice")
    add, sub, mul, div = _ops(tree)
    faces = ("4", "6", "8", "12", "20")
    kind = _menu_index(tree, tree.menu("Faces", faces, default="6"), faces)
    size = tree.input("Size", "FLOAT", 0.016, min=0.003, max=1.0, subtype="DISTANCE")
    rounding = tree.input("Rounding", "FLOAT", 0.3, min=0.0, max=1.0, subtype="FACTOR",
                          description="How rounded the edges and corners are")
    material = tree.input("Material", "MATERIAL", simple_material(
        "Starter Dice", (0.6, 0.04, 0.035), roughness=0.18, Coat_Weight=0.5))
    pip_material = tree.input("Pip Material", "MATERIAL", simple_material(
        "Starter Dice Pips", (0.9, 0.88, 0.84), roughness=0.4))

    # Six sides: a cube cage pulled in near its edges, then subdivided into a rounded cube.
    cage = tree.n("GeometryNodeMeshCube", Size=(1.0, 1.0, 1.0), Vertices_X=4, Vertices_Y=4,
                  Vertices_Z=4).outputs["Mesh"]
    inset = add(0.005, mul(rounding, 0.3))

    def pull(c):
        outer = tree.math("GREATER_THAN", tree.math("ABSOLUTE", c), 0.3)
        return mul(tree.math("SIGN", c), sub(0.5, mul(inset, sub(1.0, outer))))

    cx, cy, cz = tree.separate(_position(tree))
    cage = tree.n("GeometryNodeSetPosition", Geometry=cage,
                  Position=tree.xyz(pull(cx), pull(cy), pull(cz))).outputs[0]
    cube = tree.n("GeometryNodeSubdivisionSurface", Mesh=cage, Level=3).outputs[0]
    positions, normals = _pips(0.26)
    pip_index = _index(tree)
    pip_normal = _index_switch(tree, "VECTOR", pip_index, normals)
    pip_points = _points(tree, len(positions),
                         _index_switch(tree, "VECTOR", pip_index, positions))
    pip = tree.n("GeometryNodeMeshCylinder", fill_type="NGON", Vertices=20, Radius=0.085,
                 Depth=0.02).outputs["Mesh"]
    pip = tree.transform(pip, translation=(0.0, 0.0, -0.007))
    align = tree.n("FunctionNodeAlignRotationToVector", axis="Z", Vector=pip_normal)
    pips = _instance(tree, pip_points, pip, rotation=align.outputs[0])
    pips = tree.material(tree.smooth(pips, angle=40 * RADIANS), pip_material)
    six = tree.join(tree.material(tree.smooth(cube), material), pips)

    # Other dice: polyhedra with their corners rounded off against a sphere.
    tetra_height = SQRT2
    tetra = tree.n("GeometryNodeMeshCone", fill_type="NGON", Vertices=3, Radius_Top=0.0,
                   Radius_Bottom=1.0, Depth=tetra_height).outputs["Mesh"]
    tetra = tree.transform(tetra, translation=(0.0, 0.0, tetra_height * -0.25))
    octa = tree.n("GeometryNodeMeshUVSphere", Segments=4, Rings=2, Radius=1.0).outputs[0]
    octa = _face_down(tree, octa)
    ico = tree.n("GeometryNodeMeshIcoSphere", Radius=1.0, Subdivisions=1).outputs[0]
    dodeca = tree.n("GeometryNodeDualMesh", Mesh=ico).outputs[0]
    icosa = _face_down(tree, ico)
    # (circumradius, inradius, scale) of each solid, so they read as one size.
    solids = ((tetra, 3.0 * tetra_height / 4.0, tetra_height / 4.0, 0.8),
              (octa, 1.0, 1.0 / math.sqrt(3.0), 0.92),
              (dodeca, 0.7947, 0.6312, 0.85),
              (icosa, 1.0, 0.7947, 0.66))
    shaped = []
    for mesh, outer_r, inner_r, scale in solids:
        mesh = tree.n("GeometryNodeSubdivideMesh", Mesh=mesh, Level=3).outputs[0]
        clip = sub(outer_r, mul(rounding, (outer_r - inner_r) * 0.45))
        length = tree.vmath("LENGTH", _position(tree))
        factor = tree.math("MINIMUM", div(clip, length), 1.0)
        mesh = tree.n("GeometryNodeSetPosition", Geometry=mesh,
                      Position=tree.vmath("SCALE", _position(tree), scale=factor)).outputs[0]
        mesh = tree.transform(mesh, scale=(scale / outer_r,) * 3)
        shaped.append(tree.material(tree.smooth(mesh, angle=30 * RADIANS), material))
    die = _index_switch(tree, "GEOMETRY", kind, [shaped[0], six] + shaped[1:])
    die = tree.transform(die, scale=tree.xyz(size, size, size))
    size_gizmo(tree, size, "Z", origin=tree.xyz(0.0, 0.0, mul(size, -0.5)), centered=False)
    tree.output(rest_on_ground(tree, die))
    return generator_object("Dice", tree)
