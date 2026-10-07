# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Objects/Architecture: stairs, walls, openings and building parts for blocking out spaces."""

import math

import bpy

from .common import (RADIANS, generator_object, object_group, palette, rest_on_ground,
                     simple_material)
from .registry import asset

CATALOG = "Objects/Architecture"


# -----------------------------------------------------------------------------
# Building blocks: float math with operators, and boxes placed by fields.

def _is_socket(value):
    return isinstance(value, bpy.types.NodeSocket)


def _raw(value):
    return value.value if isinstance(value, _F) else value


class _F:
    """A float socket or constant with Python operators, so formulas read like formulas."""

    def __init__(self, tree, value):
        self.tree = tree
        self.value = value

    def _op(self, operation, other=None, reverse=False):
        a, b = (_raw(other), self.value) if reverse else (self.value, _raw(other))
        if not _is_socket(a) and not _is_socket(b) and operation in _CONSTANT_OPS:
            return _F(self.tree, _CONSTANT_OPS[operation](a, b))
        # Skip nodes that would do nothing.
        if operation == "MULTIPLY" and 0 in [v for v in (a, b) if not _is_socket(v)]:
            return _F(self.tree, 0.0)
        if operation in {"ADD", "SUBTRACT"} and not _is_socket(b) and b == 0:
            return _F(self.tree, a)
        return _F(self.tree, self.tree.math(operation, a, b))

    def __add__(self, other):
        return self._op("ADD", other)

    def __radd__(self, other):
        return self._op("ADD", other, True)

    def __sub__(self, other):
        return self._op("SUBTRACT", other)

    def __rsub__(self, other):
        return self._op("SUBTRACT", other, True)

    def __mul__(self, other):
        return self._op("MULTIPLY", other)

    def __rmul__(self, other):
        return self._op("MULTIPLY", other, True)

    def __truediv__(self, other):
        return self._op("DIVIDE", other)

    def __rtruediv__(self, other):
        return self._op("DIVIDE", other, True)

    def __neg__(self):
        return self._op("MULTIPLY", -1.0)

    def __lt__(self, other):
        return self._op("LESS_THAN", other)

    def __gt__(self, other):
        return self._op("GREATER_THAN", other)

    def min(self, other):
        return self._op("MINIMUM", other)

    def max(self, other):
        return self._op("MAXIMUM", other)

    def mod(self, other):
        return self._op("FLOORED_MODULO", other)

    def unary(self, operation):
        return _F(self.tree, self.tree.math(operation, self.value))


_CONSTANT_OPS = {
    "ADD": lambda a, b: a + b,
    "SUBTRACT": lambda a, b: a - b,
    "MULTIPLY": lambda a, b: a * b,
    "DIVIDE": lambda a, b: a / b if b else 0.0,
    "MINIMUM": min,
    "MAXIMUM": max,
}


class _Builder:
    """Shorthands for the boxes, cylinders and rails architecture is made of."""

    def __init__(self, tree):
        self.tree = tree
        self.index = _F(tree, tree.n("GeometryNodeInputIndex").outputs[0])
        self._unit = {}

    def position(self):
        position = self.tree.n("GeometryNodeInputPosition").outputs[0]
        return tuple(_F(self.tree, s) for s in self.tree.separate(position))

    def vec(self, x=0.0, y=0.0, z=0.0):
        return self.tree.xyz(_raw(x), _raw(y), _raw(z))

    def box(self, sx, sy, sz, x=0.0, y=0.0, z=0.0):
        cube = self.tree.n("GeometryNodeMeshCube", Size=self.vec(sx, sy, sz)).outputs["Mesh"]
        return self.tree.transform(cube, translation=self.vec(x, y, z))

    def cylinder(self, radius, depth, x=0.0, y=0.0, z=0.0, vertices=16, axis="Z"):
        mesh = self.tree.n("GeometryNodeMeshCylinder", Vertices=vertices, Radius=_raw(radius),
                           Depth=_raw(depth)).outputs["Mesh"]
        rotation = {"Z": (0.0, 0.0, 0.0), "X": (0.0, 90 * RADIANS, 0.0),
                    "Y": (90 * RADIANS, 0.0, 0.0)}[axis]
        return self.tree.transform(mesh, translation=self.vec(x, y, z), rotation=rotation)

    def unit(self, kind):
        """A shared unit mesh to instance: a cube, or cylinders along Z or X."""
        if kind not in self._unit:
            if kind == "cube":
                self._unit[kind] = self.box(1.0, 1.0, 1.0)
            else:
                self._unit[kind] = self.cylinder(0.5, 1.0, vertices=12, axis=kind)
        return self._unit[kind]

    def place(self, count, center, instance, rotation=None, scale=None, selection=None):
        """Instance geometry on `count` points positioned by the field `center` (x, y, z)."""
        points = self.tree.n("GeometryNodePoints", Count=_raw(count),
                             Position=self.vec(*center)).outputs[0]
        node = self.tree.n("GeometryNodeInstanceOnPoints", Points=points, Instance=instance)
        if rotation is not None:
            self.tree.set_input(node.inputs["Rotation"], self.vec(*rotation))
        if scale is not None:
            self.tree.set_input(node.inputs["Scale"], self.vec(*scale))
        if selection is not None:
            self.tree.set_input(node.inputs["Selection"], _raw(selection))
        return self.tree.realize(node.outputs[0])

    def boxes(self, count, center, size, selection=None, kind="cube"):
        """`count` boxes whose center and size are fields of the index."""
        return self.place(count, center, self.unit(kind), scale=size, selection=selection)

    def line(self, start, end):
        return self.tree.n("GeometryNodeCurvePrimitiveLine", Start=self.vec(*start),
                           End=self.vec(*end)).outputs[0]

    def tube(self, curves, radius, resolution=8):
        """Sweep curves into round tubes with closed ends."""
        profile = self.tree.n("GeometryNodeCurvePrimitiveCircle", Resolution=resolution,
                              Radius=_raw(radius)).outputs["Curve"]
        curve = self.tree.join(*curves) if isinstance(curves, (list, tuple)) else curves
        return self.tree.n("GeometryNodeCurveToMesh", Curve=curve, Profile_Curve=profile,
                           Fill_Caps=True).outputs[0]

    def when(self, condition, geometry, otherwise=None):
        node = self.tree.n("GeometryNodeSwitch", input_type="GEOMETRY", Switch=_raw(condition))
        self.tree.set_input(node.inputs["True"], geometry)
        if otherwise is not None:
            self.tree.set_input(node.inputs["False"], otherwise)
        return node.outputs[0]

    def finish(self, name, *parts):
        """Join the parts, rest them on the ground and make the object."""
        mesh = self.tree.join(*parts)
        self.tree.output(rest_on_ground(self.tree, mesh))
        return generator_object(name, self.tree)


def _input(tree, name, socket_type, default, **kwargs):
    return _F(tree, tree.input(name, socket_type, default, **kwargs))


def _distance(tree, name, default, low=0.001, high=None, description=""):
    return _input(tree, name, "FLOAT", default, min=low, max=high, subtype="DISTANCE",
                  description=description)


# -----------------------------------------------------------------------------
# Stairs.

RAIL_HEIGHT = 0.9


def _flight(b, origin, direction, side, first, count, rise, run, width, rail_z):
    """A straight flight of solid steps, with balusters and a handrail on its `side`."""
    ox, oy = origin
    dx, dy = direction
    sx, sy = side
    i = b.index
    t = (i + 0.5) * run
    height = (i + first + 1.0) * rise
    cx, cy = ox + t * dx, oy + t * dy
    along, across = (run, width) if dx else (width, run)
    steps = b.boxes(count, (cx, cy, height * 0.5), (along, across, height))

    inset = width * 0.5 - 0.05
    top = rail_z + t / run * rise
    post = top - height
    posts = b.boxes(count, (cx + inset * sx, cy + inset * sy, height + post * 0.5),
                    (0.025, 0.025, post))
    end = run * count
    start = (ox + inset * sx, oy + inset * sy, rail_z)
    finish = (ox + end * dx + inset * sx, oy + end * dy + inset * sy, rail_z + rise * count)
    return steps, posts, b.line(start, finish)


def _landing_rail(b, corners, z):
    lines = [b.line((*a, z), (*c, z)) for a, c in zip(corners, corners[1:])]
    newels = [b.box(0.05, 0.05, RAIL_HEIGHT, x, y, z - RAIL_HEIGHT * 0.5)
              for x, y in corners[1:-1]]
    return lines, newels


@asset("Stairs", "object", CATALOG,
       "A flight of solid steps with a handrail, straight or turning at a landing.",
       ("stairs", "staircase", "steps", "architecture", "building", "interior"))
def stairs():
    tree = object_group("Stairs")
    b = _Builder(tree)
    shape = tree.menu("Shape", ("Straight", "L-Shaped", "U-Shaped"),
                      description="Straight, or turning at a landing halfway up")
    steps = _input(tree, "Steps", "INT", 14, min=3, max=64)
    rise = _distance(tree, "Rise", 0.18, 0.05, 0.5, "Height of each step")
    run = _distance(tree, "Run", 0.28, 0.1, 1.0, "Depth of each step")
    width = _distance(tree, "Width", 1.0, 0.3)
    railing = tree.input("Railing", "BOOL", True)
    material = tree.input("Material", "MATERIAL", palette("Starter Concrete"))
    rail_material = tree.input("Railing Material", "MATERIAL", palette("Starter Dark"))

    e = 0.05
    first = (steps * 0.5).unary("FLOOR")
    second = steps - first - 1.0
    landing = (first + 1.0) * rise
    at = first * run
    rail = rise + RAIL_HEIGHT
    lz = landing + RAIL_HEIGHT

    def build(flights, landing_box, corners):
        solids, posts, rails = [], [], []
        for flight in flights:
            s, p, r = _flight(b, *flight)
            solids.append(s)
            posts.append(p)
            rails.append(r)
        if landing_box:
            cx, cy, sx, sy = landing_box
            solids.append(b.box(sx, sy, landing, cx, cy, landing * 0.5))
            lines, newels = _landing_rail(b, corners, lz)
            rails += lines
            posts += newels
        railing_mesh = tree.join(b.tube(rails, 0.025), *posts)
        return tree.join(tree.material(tree.join(*solids), material),
                         b.when(railing, tree.material(railing_mesh, rail_material)))

    straight = build([((0.0, 0.0), (0, 1), (-1, 0), 0.0, steps, rise, run, width, rail)],
                     None, None)
    half = width * 0.5
    l_shaped = build(
        [((0.0, 0.0), (0, 1), (-1, 0), 0.0, first, rise, run, width, rail),
         ((half, at + half), (1, 0), (0, 1), first + 1.0, second, rise, run, width, lz)],
        (0.0, at + half, width, width),
        [(-half + e, at), (-half + e, at + width - e), (half, at + width - e)])
    gap = 0.1
    across = width + gap
    u_shaped = build(
        [((0.0, 0.0), (0, 1), (-1, 0), 0.0, first, rise, run, width, rail),
         ((across, at), (0, -1), (1, 0), first + 1.0, second, rise, run, width, lz)],
        (across * 0.5, at + half, width * 2.0 + gap, width),
        [(-half + e, at), (-half + e, at + width - e), (across + half - e, at + width - e),
         (across + half - e, at)])
    mesh = tree.switch(shape, "GEOMETRY", {"Straight": straight, "L-Shaped": l_shaped,
                                           "U-Shaped": u_shaped})
    return b.finish("Stairs", mesh)


@asset("Spiral Staircase", "object", CATALOG,
       "A spiral staircase of wedge treads winding around a central column, with a handrail.",
       ("stairs", "spiral", "staircase", "helix", "architecture", "interior"))
def spiral_staircase():
    tree = object_group("Spiral Staircase")
    b = _Builder(tree)
    steps = _input(tree, "Steps", "INT", 16, min=3, max=128)
    height = _distance(tree, "Height", 3.0, 0.3, description="Height of the top step")
    radius = _distance(tree, "Radius", 1.0, 0.3)
    turn = _input(tree, "Turn", "FLOAT", 360 * RADIANS, min=30 * RADIANS, max=1440 * RADIANS,
                  subtype="ANGLE", description="How far the stairs wind from bottom to top")
    railing = tree.input("Railing", "BOOL", True)
    material = tree.input("Material", "MATERIAL", palette("Starter Light Wood"))
    frame_material = tree.input("Frame Material", "MATERIAL", palette("Starter Dark"))

    core = 0.09
    angle = turn / steps
    rise = height / steps
    # A wedge from the column out to the radius, top at Z = 0.
    wedge = tree.transform(b.box(1.0, 1.0, 1.0), translation=(0.5, 0.0, -0.5))
    px, py, pz = b.position()
    x = core * 0.5 + px * (radius - core * 0.5)
    half = (angle * 0.5).unary("TANGENT")
    wedge = tree.n("GeometryNodeSetPosition", Geometry=wedge,
                   Position=b.vec(x, py * x * half * 2.0, pz * 0.05)).outputs[0]
    i = b.index
    treads = b.place(steps, (0.0, 0.0, (i + 1.0) * rise), wedge, rotation=(0.0, 0.0, i * angle))

    top = height + RAIL_HEIGHT
    column = tree.smooth(b.cylinder(core, top, z=top * 0.5, vertices=24))
    reach = radius - 0.05
    posts = b.boxes(steps, ((i * angle).unary("COSINE") * reach, (i * angle).unary("SINE") * reach,
                            (i + 1.0) * rise + RAIL_HEIGHT * 0.5), (0.03, 0.03, RAIL_HEIGHT),
                    kind="Z")
    # Reversed, the spiral winds counter-clockwise like the treads.
    helix = tree.n("GeometryNodeCurveSpiral", Resolution=48, Rotations=_raw(
        (steps - 1.0) * angle / (2 * math.pi)), Start_Radius=_raw(reach), End_Radius=_raw(reach),
        Height=_raw((steps - 1.0) * rise), Reverse=True).outputs[0]
    helix = tree.transform(helix, translation=b.vec(z=rise + RAIL_HEIGHT))
    rail = tree.join(tree.smooth(b.tube(helix, 0.025)), posts)
    return b.finish("Spiral Staircase", tree.material(treads, material),
                    tree.material(column, frame_material),
                    b.when(railing, tree.material(rail, frame_material)))


# -----------------------------------------------------------------------------
# Buildings and walls.

def _building_window(b, width, height):
    """One window as seen on a facade at Y = 0: a frame, dark glass, a mullion and a sill."""
    tree = b.tree
    fw, fd = 0.08, 0.14
    frame = tree.join(
        b.box(fw, fd, height + fw * 2.0, width * -0.5 - fw * 0.5, -fd * 0.5),
        b.box(fw, fd, height + fw * 2.0, width * 0.5 + fw * 0.5, -fd * 0.5),
        b.box(width, fd, fw, 0.0, -fd * 0.5, height * 0.5 + fw * 0.5),
        b.box(width, fd, fw, 0.0, -fd * 0.5, height * -0.5 - fw * 0.5),
        b.box(0.05, fd * 0.7, height, 0.0, -fd * 0.35),
        b.box(width + fw * 2.0 + 0.08, 0.2, 0.06, 0.0, -0.1, height * -0.5 - fw - 0.03))
    glass = b.box(width, 0.04, height, 0.0, -0.03)
    return frame, glass


@asset("Building", "object", CATALOG,
       "A multi-story building with a grid of windows, an entrance and a roof ledge.",
       ("building", "house", "city", "architecture", "office", "apartment", "blockout"))
def building():
    tree = object_group("Building")
    b = _Builder(tree)
    floors = _input(tree, "Floors", "INT", 4, min=1, max=60)
    width = _distance(tree, "Width", 12.0, 3.0)
    depth = _distance(tree, "Depth", 10.0, 3.0)
    spacing = _distance(tree, "Window Spacing", 3.0, 1.0, description="Distance between windows")
    story = _distance(tree, "Floor Height", 3.2, 2.2, description="Height of each floor")
    material = tree.input("Material", "MATERIAL", simple_material(
        "Architecture Facade", (0.72, 0.68, 0.62), roughness=0.8))
    trim_material = tree.input("Trim Material", "MATERIAL", palette("Starter Concrete"))
    window_material = tree.input("Window Material", "MATERIAL", simple_material(
        "Architecture Window", (0.04, 0.06, 0.08), roughness=0.08))

    total = floors * story
    body = b.box(width, depth, total, z=total * 0.5)
    plinth = b.box(width + 0.12, depth + 0.12, 0.5, z=0.25)
    ledge = b.box(width + 0.5, depth + 0.5, 0.35, z=total + 0.175)
    roof_box = b.box(width * 0.25, depth * 0.3, 1.6, width * 0.2, depth * 0.15, total + 0.8)
    i = b.index
    bands = b.boxes(floors - 1.0, (0.0, 0.0, (i + 1.0) * story),
                    (width + 0.1, depth + 0.1, 0.18))

    window_width = (spacing * 0.55).min(1.6)
    window_height = story * 0.5
    frame, glass = _building_window(b, window_width, window_height)
    window = tree.join(tree.material(frame, trim_material), tree.material(glass, window_material))
    door_width = 2.0

    def facade(length, offset, angle, front=False):
        count = (length / spacing).unary("FLOOR").max(1.0)
        column = i.mod(count)
        row = (i / count).unary("FLOOR")
        x = (column - (count - 1.0) * 0.5) * spacing
        selection = None
        if front:
            # Leave room for the entrance on the ground floor.
            clear = door_width * 0.5 + 0.3 + window_width * 0.5
            selection = 1.0 - (row < 0.5) * (x.unary("ABSOLUTE") < clear)
        windows = b.place(count * floors, (x, -offset, row * story + story * 0.55), window,
                          selection=selection)
        return tree.transform(windows, rotation=(0.0, 0.0, angle))

    half_w, half_d = width * 0.5, depth * 0.5
    windows = tree.join(facade(width, half_d, 0.0, True), facade(width, half_d, math.pi),
                        facade(depth, half_w, 90 * RADIANS), facade(depth, half_w, -90 * RADIANS))
    door_height = 2.6
    entrance = tree.join(
        tree.material(tree.join(
            b.box(door_width + 0.3, 0.2, door_height + 0.15, 0.0, -half_d - 0.1,
                  (door_height + 0.15) * 0.5),
            b.box(door_width + 1.4, 1.6, 0.18, 0.0, -half_d - 0.8, door_height + 0.4)),
            trim_material),
        tree.material(b.box(door_width, 0.24, door_height, 0.0, -half_d - 0.1,
                            door_height * 0.5), window_material))
    return b.finish("Building", tree.material(body, material),
                    tree.material(tree.join(plinth, ledge, roof_box, bands), trim_material),
                    windows, entrance)


@asset("Wall", "object", CATALOG,
       "A straight wall, plastered or bricked, with evenly spaced window or door openings.",
       ("wall", "brick", "masonry", "architecture", "building", "blockout"))
def wall():
    tree = object_group("Wall")
    b = _Builder(tree)
    length = _distance(tree, "Length", 5.0, 0.2)
    height = _distance(tree, "Height", 2.6, 0.2)
    thickness = _distance(tree, "Thickness", 0.22, 0.02)
    style = tree.menu("Style", ("Bricks", "Plain"))
    openings = _input(tree, "Openings", "INT", 2, min=0, max=50,
                      description="Number of evenly spaced window or door openings")
    opening_width = _distance(tree, "Opening Width", 1.0, 0.1)
    opening_height = _distance(tree, "Opening Height", 1.3, 0.1)
    sill = _distance(tree, "Sill Height", 0.85, 0.0,
                     description="Height of the openings above the ground; 0 for doorways")
    material = tree.input("Material", "MATERIAL", palette("Starter Brick"))
    plaster_material = tree.input("Plaster Material", "MATERIAL", simple_material(
        "Architecture Plaster", (0.75, 0.72, 0.66), roughness=0.85),
        description="The plain wall, the mortar between bricks, and sills and lintels")

    n = openings.max(0.0)
    pitch = length / n.max(1.0)
    ow = opening_width.min(pitch - 0.1).max(0.05)
    bottom = sill.min(height - 0.2)
    oh = opening_height.min(height - 0.1 - bottom).max(0.05)
    top = bottom + oh
    i = b.index
    center = (i + 0.5) * pitch - length * 0.5

    def piers(t):
        """The plain wall around the openings, `t` thick."""
        left = ((i - 0.5) * pitch + ow * 0.5 - length * 0.5).max(length * -0.5)
        left = left * (i > 0.5) + length * -0.5 * (i < 0.5)
        right = ((i + 0.5) * pitch - ow * 0.5 - length * 0.5) * (i < n - 0.5) + \
            length * 0.5 * (i > n - 0.5)
        return tree.join(
            b.boxes(n + 1.0, ((left + right) * 0.5, 0.0, height * 0.5),
                    (right - left, t, height)),
            b.boxes(n, (center, 0.0, (top + height) * 0.5), (ow, t, height - top),
                    selection=height - top > 0.001),
            b.boxes(n, (center, 0.0, bottom * 0.5), (ow, t, bottom),
                    selection=bottom > 0.001))

    plain = tree.material(piers(thickness), plaster_material)

    # Bricks in running bond, clipped at the ends of the wall and the sides of the openings.
    course, brick, joint = 0.075, 0.225, 0.01
    columns = (length / brick).unary("CEIL") + 1.0
    rows = (height / course).unary("CEIL")
    column = i.mod(columns)
    row = (i / columns).unary("FLOOR")
    a = (column - row.mod(2.0) * 0.5) * brick - length * 0.5
    c = a + brick - joint
    za = row * course
    zc = (za + course - joint).min(height)
    nearest = ((a + c) * 0.5 + length * 0.5) / pitch
    nearest = nearest.unary("FLOOR").max(0.0).min(n - 1.0)
    middle = (nearest + 0.5) * pitch - length * 0.5
    inside = (zc > bottom) * (za < top) * (n > 0.5)
    left_of = (a + c) * 0.5 < middle
    a = a.max(length * -0.5)
    c = c.min(length * 0.5)
    # Clip toward whichever side of the opening the brick is on.
    c = c - inside * left_of * (c - c.min(middle - ow * 0.5))
    a = a + inside * (1.0 - left_of) * (a.max(middle + ow * 0.5) - a)
    keep = c - a > 0.02
    bricks = b.boxes(columns * rows, ((a + c) * 0.5, 0.0, (za + zc) * 0.5),
                     (c - a, thickness, zc - za), selection=keep)
    trims = tree.join(
        b.boxes(n, (center, 0.0, top + 0.06),
                (ow + 0.2, thickness + 0.04, 0.12), selection=height - top > 0.13),
        b.boxes(n, (center, 0.0, bottom - 0.03),
                (ow + 0.1, thickness + 0.06, 0.06), selection=bottom > 0.07))
    bricked = tree.join(tree.material(bricks, material),
                        tree.material(tree.join(piers(thickness - 0.012), trims),
                                      plaster_material))
    mesh = tree.switch(style, "GEOMETRY", {"Bricks": bricked, "Plain": plain})
    return b.finish("Wall", mesh)


# -----------------------------------------------------------------------------
# Openings.

@asset("Window", "object", CATALOG,
       "A framed window with glass panes divided by mullions, standing on its sill.",
       ("window", "glass", "frame", "pane", "architecture", "building"))
def window():
    tree = object_group("Window")
    b = _Builder(tree)
    width = _distance(tree, "Width", 1.2, 0.1)
    height = _distance(tree, "Height", 1.4, 0.1)
    panes_x = _input(tree, "Panes X", "INT", 2, min=1, max=20,
                     description="Number of panes across")
    panes_y = _input(tree, "Panes Y", "INT", 2, min=1, max=20, description="Number of panes up")
    frame = _distance(tree, "Frame", 0.07, 0.01, description="Width of the frame around the glass")
    depth = _distance(tree, "Depth", 0.12, 0.01)
    material = tree.input("Material", "MATERIAL", palette("Starter White"))
    glass_material = tree.input("Glass Material", "MATERIAL", palette("Starter Glass"))

    f = frame.min(width * 0.25).min(height * 0.25)
    inner_w, inner_h = width - f * 2.0, height - f * 2.0
    half_w = width * 0.5
    bar = f * 0.55
    i = b.index
    frame_mesh = tree.join(
        b.box(f, depth, height, -half_w + f * 0.5, 0.0, height * 0.5),
        b.box(f, depth, height, half_w - f * 0.5, 0.0, height * 0.5),
        b.box(inner_w, depth, f, 0.0, 0.0, f * 0.5),
        b.box(inner_w, depth, f, 0.0, 0.0, height - f * 0.5),
        b.boxes(panes_x - 1.0, (-half_w + f + (i + 1.0) * inner_w / panes_x, 0.0, height * 0.5),
                (bar, depth * 0.7, inner_h)),
        b.boxes(panes_y - 1.0, (0.0, 0.0, f + (i + 1.0) * inner_h / panes_y),
                (inner_w, depth * 0.7, bar)),
        b.box(width + 0.08, depth + 0.06, 0.035, 0.0, -0.03, -0.0175))
    glass = b.box(inner_w, 0.006, inner_h, z=height * 0.5)
    return b.finish("Window", tree.material(frame_mesh, material),
                    tree.material(glass, glass_material))


def _door_handles(b, width, thickness):
    """Lever handles on both sides of a door leaf, at the usual height."""
    x, z = width * 0.5 - 0.07, 1.0
    parts = [b.cylinder(0.028, thickness + 0.02, x, 0.0, z, axis="Y")]
    for side in (-1.0, 1.0):
        y = (thickness * 0.5 + 0.03) * side
        parts.append(b.cylinder(0.009, 0.05, x, y * 0.8, z, vertices=8, axis="Y"))
        parts.append(b.box(0.13, 0.02, 0.022, x - 0.055, y + 0.01 * side, z))
    return b.tree.join(*parts)


def _door_panel(b, width, height, thickness, x, z):
    """A moulding around a panel on both faces of a leaf, so the panel reads as recessed."""
    w = 0.025
    depth = thickness + 0.02
    return b.tree.join(
        b.box(w, depth, height, x - (width - w) * 0.5, 0.0, z),
        b.box(w, depth, height, x + (width - w) * 0.5, 0.0, z),
        b.box(width - w * 2.0, depth, w, x, 0.0, z - (height - w) * 0.5),
        b.box(width - w * 2.0, depth, w, x, 0.0, z + (height - w) * 0.5))


@asset("Door", "object", CATALOG,
       "A door in its frame, with a paneled, flat or glazed leaf and lever handles.",
       ("door", "entrance", "frame", "interior", "architecture", "building"))
def door():
    tree = object_group("Door")
    b = _Builder(tree)
    style = tree.menu("Style", ("Panel", "Flat", "Glass"))
    width = _distance(tree, "Width", 0.9, 0.3)
    height = _distance(tree, "Height", 2.1, 0.5)
    handle = tree.input("Handle", "BOOL", True)
    material = tree.input("Material", "MATERIAL", simple_material(
        "Architecture Door", (0.32, 0.12, 0.06), roughness=0.45))
    frame_material = tree.input("Frame Material", "MATERIAL", palette("Starter White"))
    handle_material = tree.input("Handle Material", "MATERIAL", palette("Starter Brass"))
    glass_material = tree.input("Glass Material", "MATERIAL", palette("Starter Glass"))

    t, fw, fd = 0.045, 0.07, 0.14
    half = width * 0.5
    frame = tree.join(
        b.box(fw, fd, height + fw, -half - fw * 0.5, 0.0, (height + fw) * 0.5),
        b.box(fw, fd, height + fw, half + fw * 0.5, 0.0, (height + fw) * 0.5),
        b.box(width, fd, fw, 0.0, 0.0, height + fw * 0.5))

    # Paneled: two columns of raised panels, short ones below a lock rail, tall ones above.
    m = 0.11
    pw = (width - m * 3.0) * 0.5
    px = (pw + m) * 0.5
    rail = height * 0.42
    low_h = rail - m * 0.5 - 0.2
    high_h = height - m - rail - m * 0.5
    low_z = 0.2 + low_h * 0.5
    high_z = rail + m * 0.5 + high_h * 0.5
    panels = [_door_panel(b, pw, low_h, t, x, low_z) for x in (-px, px)]
    paneled = tree.join(b.box(width, t, height, z=height * 0.5), *panels,
                        *[_door_panel(b, pw, high_h, t, x, high_z) for x in (-px, px)])
    flat = b.box(width, t, height, z=height * 0.5)
    glass_h = height - rail - m
    glazed = tree.join(
        tree.material(tree.join(
            b.box(width, t, rail, z=rail * 0.5), *panels,
            b.box(m, t, height - rail, -half + m * 0.5, 0.0, (height + rail) * 0.5),
            b.box(m, t, height - rail, half - m * 0.5, 0.0, (height + rail) * 0.5),
            b.box(width - m * 2.0, t, m, z=height - m * 0.5),
            b.box(0.03, t * 0.6, glass_h, z=rail + glass_h * 0.5)), material),
        tree.material(b.box(width - m * 2.0, 0.01, glass_h, z=rail + glass_h * 0.5),
                      glass_material))
    leaf = tree.switch(style, "GEOMETRY", {"Panel": tree.material(paneled, material),
                                           "Flat": tree.material(flat, material),
                                           "Glass": glazed})
    handles = tree.material(tree.smooth(_door_handles(b, width, t), angle=40 * RADIANS),
                            handle_material)
    return b.finish("Door", leaf, tree.material(frame, frame_material), b.when(handle, handles))


# -----------------------------------------------------------------------------
# Structure.

@asset("Column", "object", CATALOG,
       "A column with a base and capital, classical with fluted shaft or modern and plain.",
       ("column", "pillar", "classical", "greek", "architecture", "building"))
def column():
    tree = object_group("Column")
    b = _Builder(tree)
    style = tree.menu("Style", ("Classical", "Modern"))
    height = _distance(tree, "Height", 3.0, 0.3)
    radius = _distance(tree, "Radius", 0.2, 0.02)
    flutes = _input(tree, "Flutes", "INT", 20, min=0, max=64,
                    description="Number of grooves down a classical shaft")
    material = tree.input("Material", "MATERIAL", simple_material(
        "Architecture Marble", (0.78, 0.76, 0.72), roughness=0.35))

    r = radius
    # Classical: plinth and torus below, echinus and abacus above, a fluted tapering shaft.
    plinth_h, torus_h, echinus_h, abacus_h = (height * k for k in (0.05, 0.035, 0.05, 0.045))
    shaft_bottom = plinth_h + torus_h
    shaft_h = height - shaft_bottom - echinus_h - abacus_h
    taper = 0.85
    sides = flutes.max(1.0) * 8.0
    shaft = tree.n("GeometryNodeMeshCylinder", Vertices=_raw((flutes > 0.5) * sides +
                                                             (flutes < 0.5) * 48.0),
                   Side_Segments=8, Radius=1.0, Depth=1.0).outputs["Mesh"]
    px, py, pz = b.position()
    angle = _F(tree, tree.math("ARCTAN2", py.value, px.value))
    phase = (angle * flutes / (2 * math.pi)).unary("FRACT")
    groove = (phase * math.pi).unary("SINE") * (math.pi / flutes.max(1.0)) * 0.45
    # Entasis: the shaft swells slightly, then narrows toward the top.
    s = pz + 0.5
    swell = 1.0 - (1.0 - taper) * s * s + 0.02 * (s * math.pi).unary("SINE")
    scale = (1.0 - groove) * swell * r
    shaft = tree.n("GeometryNodeSetPosition", Geometry=shaft, Position=b.vec(
        px * scale, py * scale, shaft_bottom + s * shaft_h)).outputs[0]
    top_r = r * taper
    classical = tree.join(
        tree.smooth(shaft, angle=50 * RADIANS),
        b.box(r * 2.7, r * 2.7, plinth_h, z=plinth_h * 0.5),
        tree.smooth(b.cylinder(r * 1.18, torus_h, z=plinth_h + torus_h * 0.5, vertices=48)),
        tree.smooth(tree.transform(tree.n(
            "GeometryNodeMeshCone", Vertices=48, Radius_Bottom=_raw(top_r),
            Radius_Top=_raw(top_r * 1.45), Depth=_raw(echinus_h)).outputs["Mesh"],
            translation=b.vec(z=height - abacus_h - echinus_h * 0.5))),
        b.box(top_r * 3.0, top_r * 3.0, abacus_h, z=height - abacus_h * 0.5))

    plate = height * 0.03
    modern = tree.join(
        tree.smooth(b.cylinder(r, height - plate * 2.0, z=height * 0.5, vertices=48)),
        b.box(r * 2.4, r * 2.4, plate, z=plate * 0.5),
        b.box(r * 2.4, r * 2.4, plate, z=height - plate * 0.5))
    mesh = tree.switch(style, "GEOMETRY", {"Classical": classical, "Modern": modern})
    return b.finish("Column", tree.material(mesh, material))


def _arc_block(b, inner, outer, depth, start, sweep, resolution=6):
    """A curved block of an arch: an arc swept with a rectangle, in the XY plane."""
    tree = b.tree
    arc = tree.n("GeometryNodeCurveArc", Resolution=resolution, Radius=_raw((inner + outer) * 0.5),
                 Start_Angle=_raw(start), Sweep_Angle=_raw(sweep)).outputs["Curve"]
    profile = tree.n("GeometryNodeCurvePrimitiveQuadrilateral", Width=_raw(outer - inner),
                     Height=_raw(depth)).outputs["Curve"]
    return tree.n("GeometryNodeCurveToMesh", Curve=arc, Profile_Curve=profile,
                  Fill_Caps=True).outputs[0]


@asset("Arch", "object", CATALOG,
       "A round stone arch on two piers, built from wedge-shaped voussoirs and a keystone.",
       ("arch", "archway", "stone", "gate", "architecture", "ruin"))
def arch():
    tree = object_group("Arch")
    b = _Builder(tree)
    span = _distance(tree, "Span", 2.0, 0.2, description="Width of the opening")
    height = _distance(tree, "Height", 3.2, 0.3, description="Height to the top of the arch")
    thickness = _distance(tree, "Thickness", 0.45, 0.05,
                          description="Width of the piers and of the arch ring")
    depth = _distance(tree, "Depth", 0.6, 0.05)
    voussoirs = tree.input("Voussoirs", "BOOL", True,
                           description="Build the arch from separate wedge-shaped stones")
    blocks = _input(tree, "Blocks", "INT", 11, min=3, max=63,
                    description="Number of stones in the arch; odd numbers get a keystone")
    material = tree.input("Material", "MATERIAL", simple_material(
        "Architecture Sandstone", (0.55, 0.46, 0.34), roughness=0.85))
    mortar_material = tree.input("Mortar Material", "MATERIAL", simple_material(
        "Architecture Mortar", (0.3, 0.28, 0.25), roughness=0.95))

    inner = span * 0.5
    outer = inner + thickness
    spring = (height - outer).max(0.0)
    gap = 0.012
    step = math.pi / blocks
    keystone = blocks.mod(2.0) > 0.5
    middle = (blocks - 1.0) * 0.5
    block = _arc_block(b, inner, outer, depth, gap / inner * 0.5, step - gap / inner)
    i = b.index
    stones = b.place(blocks, (0.0, 0.0, 0.0), block, rotation=(0.0, 0.0, i * step),
                     selection=1.0 - keystone * ((i - middle).unary("ABSOLUTE") < 0.5))
    stones = tree.join(stones, b.when(keystone, _arc_block(
        b, inner, outer + thickness * 0.15, depth + 0.04,
        math.pi * 0.5 - step * 0.5 + gap / inner * 0.5, step - gap / inner, 2)))
    mortar = _arc_block(b, inner + 0.01, outer - 0.01, depth - 0.03, 0.0, math.pi, 32)
    solid = _arc_block(b, inner, outer, depth, 0.0, math.pi, 32)
    ring = b.when(voussoirs, tree.join(tree.material(stones, material),
                                       tree.material(mortar, mortar_material)),
                  tree.material(solid, material))
    ring = tree.transform(ring, translation=b.vec(z=spring), rotation=(90 * RADIANS, 0.0, 0.0))

    # Piers of stacked stones on a plinth, capped with an impost where the arch springs.
    impost = 0.14
    pier_top = spring - impost
    courses = (pier_top / 0.4).unary("ROUND").max(1.0)
    course = pier_top / courses
    x = inner + thickness * 0.5
    row = (i * 0.5).unary("FLOOR")
    side = i.mod(2.0) * 2.0 - 1.0
    coursed = b.boxes(courses * 2.0, (x * side, 0.0, (row + 0.5) * course),
                      (thickness, depth, course - gap))
    plain = tree.join(b.box(thickness, depth, pier_top, -x, 0.0, pier_top * 0.5),
                      b.box(thickness, depth, pier_top, x, 0.0, pier_top * 0.5))
    cores = tree.join(b.box(thickness - 0.02, depth - 0.03, pier_top, -x, 0.0, pier_top * 0.5),
                      b.box(thickness - 0.02, depth - 0.03, pier_top, x, 0.0, pier_top * 0.5))
    piers = b.when(voussoirs, tree.join(tree.material(coursed, material),
                                        tree.material(cores, mortar_material)),
                   tree.material(plain, material))
    caps = tree.join(*[b.box(thickness + 0.1, depth + 0.1, impost, x * s, 0.0,
                             pier_top + impost * 0.5) for s in (-1.0, 1.0)])
    return b.finish("Arch", ring, piers, tree.material(caps, material))


@asset("Roof", "object", CATALOG,
       "A pitched or flat roof with overhanging eaves, to put on top of a building.",
       ("roof", "gable", "hip", "house", "architecture", "building"))
def roof():
    tree = object_group("Roof")
    b = _Builder(tree)
    kind = tree.menu("Type", ("Gable", "Hip", "Flat"))
    width = _distance(tree, "Width", 8.0, 0.5, description="Width of the building below")
    depth = _distance(tree, "Depth", 6.0, 0.5, description="Depth of the building below")
    pitch = _input(tree, "Pitch", "FLOAT", 35 * RADIANS, min=5 * RADIANS, max=70 * RADIANS,
                   subtype="ANGLE", description="Slope of the roof")
    overhang = _distance(tree, "Overhang", 0.4, 0.0,
                         description="How far the roof reaches past the walls")
    material = tree.input("Material", "MATERIAL", palette("Starter Roof"))
    wall_material = tree.input("Wall Material", "MATERIAL", palette("Starter White"),
                               description="The gable ends and the trim under the eaves")

    t = 0.18
    tan = pitch.unary("TANGENT")
    cos = pitch.unary("COSINE")
    half_w, half_d = width * 0.5 + overhang, depth * 0.5 + overhang
    rise = half_d * tan

    # Gable: two slabs meeting at the ridge, over triangular gable walls.
    slope = half_d / cos + t * tan
    slab = b.box(width + overhang * 2.0, slope, t, 0.0, slope * 0.5, t * -0.5)
    # Courses of tiles: a lip every row down the slope.
    row = 0.32
    rows = (slope / row).unary("FLOOR")
    lips = b.boxes(rows, (0.0, (b.index + 1.0) * row - 0.03, 0.015),
                   (width + overhang * 2.0, 0.06, 0.03))
    slab = tree.join(slab, lips)
    slab = tree.transform(slab, rotation=b.vec(pitch * -1.0, 0.0, 0.0))
    slab = tree.transform(slab, translation=b.vec(z=rise))
    slabs = tree.join(slab, tree.transform(slab, rotation=(0.0, 0.0, math.pi)))
    ridge = b.cylinder(t * 0.6, width + overhang * 2.0 + 0.04, z=rise + t * 0.2, axis="X")
    under = rise - t / cos
    wall_low = overhang * tan - t / cos
    gable_walls = _wedge(b, width, depth, under - wall_low, 0.0, wall_low)
    gable = tree.join(tree.material(tree.join(slabs, tree.smooth(ridge)), material),
                      tree.material(gable_walls, wall_material))

    # Hip: every side slopes, the ridge is shorter than the roof by its depth.
    hip_solid = _wedge(b, half_w * 2.0, half_d * 2.0, rise, (half_w - half_d).max(0.0), t)
    fascia = b.box(half_w * 2.0 + 0.02, half_d * 2.0 + 0.02, t, z=t * 0.5)
    ridge_x = (half_w - half_d).max(0.0)
    top = t + rise
    hips = [b.line((ridge_x * sx, 0.0, top), (half_w * sx, half_d * sy, t))
            for sx in (-1.0, 1.0) for sy in (-1.0, 1.0)]
    hips.append(b.line((-ridge_x, 0.0, top), (ridge_x, 0.0, top)))
    caps = tree.smooth(b.tube(hips, 0.07))
    hip = tree.join(tree.material(tree.join(hip_solid, caps), material),
                    tree.material(fascia, wall_material))

    # Flat: a slab with a low parapet around it.
    p = 0.15
    slab_h = 0.3
    parapet_z = slab_h + 0.2
    flat = tree.join(
        tree.material(b.box(half_w * 2.0, half_d * 2.0, slab_h, z=slab_h * 0.5),
                      wall_material),
        tree.material(tree.join(
            b.box(half_w * 2.0, p, 0.4, 0.0, half_d - p * 0.5, parapet_z),
            b.box(half_w * 2.0, p, 0.4, 0.0, p * 0.5 - half_d, parapet_z),
            b.box(p, half_d * 2.0 - p * 2.0, 0.4, half_w - p * 0.5, 0.0, parapet_z),
            b.box(p, half_d * 2.0 - p * 2.0, 0.4, p * 0.5 - half_w, 0.0, parapet_z),
            b.box(half_w * 2.0 - p * 2.0, half_d * 2.0 - p * 2.0, 0.02, z=slab_h + 0.01)),
            material))
    mesh = tree.switch(kind, "GEOMETRY", {"Gable": gable, "Hip": hip, "Flat": flat})
    return b.finish("Roof", mesh)


def _wedge(b, width, depth, height, ridge, bottom):
    """A box whose top closes into a ridge `ridge` long either side of the middle."""
    tree = b.tree
    cube = tree.n("GeometryNodeMeshCube", Size=(1.0, 1.0, 1.0)).outputs["Mesh"]
    px, py, pz = b.position()
    up = pz > 0.0
    x = px * width * (1.0 - up) + (px * 2.0).unary("SIGN") * ridge * up
    y = py * depth * (1.0 - up)
    mesh = tree.n("GeometryNodeSetPosition", Geometry=cube,
                  Position=b.vec(x, y, bottom + up * height)).outputs[0]
    return tree.n("GeometryNodeMergeByDistance", Geometry=mesh, Distance=0.0001).outputs[0]


# -----------------------------------------------------------------------------
# Outdoors.

@asset("Fence", "object", CATALOG,
       "A straight run of fence, white pickets or horizontal rails between square posts.",
       ("fence", "picket", "garden", "yard", "railing", "outdoor"))
def fence():
    tree = object_group("Fence")
    b = _Builder(tree)
    style = tree.menu("Style", ("Picket", "Rail"))
    length = _distance(tree, "Length", 5.0, 0.2)
    spacing = _distance(tree, "Post Spacing", 1.8, 0.3, description="Distance between posts")
    height = _distance(tree, "Height", 1.0, 0.2)
    picket_spacing = _distance(tree, "Picket Spacing", 0.13, 0.03,
                               description="Distance between pickets")
    material = tree.input("Material", "MATERIAL", palette("Starter White"))

    post = 0.09
    i = b.index
    posts_count = (length / spacing).unary("CEIL").max(1.0) + 1.0
    x = i * length / (posts_count - 1.0) - length * 0.5
    posts = tree.join(
        b.boxes(posts_count, (x, 0.0, height * 0.5 + 0.025), (post, post, height + 0.05)),
        b.boxes(posts_count, (x, 0.0, height + 0.065), (post + 0.03, post + 0.03, 0.03)))

    # Pickets with pointed tops, nailed to two rails behind them.
    pw, pt = 0.075, 0.02
    ph = height - 0.06
    picket = tree.n("GeometryNodeMeshCube", Size=b.vec(pw, pt, ph), Vertices_X=3).outputs["Mesh"]
    px, _, pz = b.position()
    tip = (pz > 0.0) * (px.unary("ABSOLUTE") > 0.001)
    picket = tree.n("GeometryNodeSetPosition", Geometry=picket, Selection=_raw(tip),
                    Offset=b.vec(z=pw * -0.6)).outputs[0]
    count = (length / picket_spacing).unary("FLOOR")
    run = (count - 1.0) * picket_spacing
    front = -post * 0.5 - 0.03
    pickets = b.place(count, (i * picket_spacing - run * 0.5, front - pt * 0.5, 0.05 + ph * 0.5),
                      picket)
    rails = tree.join(*[b.box(length, 0.04, 0.09, 0.0, front + 0.02, z)
                        for z in (0.22, height - 0.25)])
    picketed = tree.join(posts, pickets, rails)

    boards = tree.join(*[b.box(length + post, 0.03, 0.12, 0.0, -post * 0.5 - 0.015, height * k)
                         for k in (0.3, 0.6, 0.9)])
    railed = tree.join(posts, boards)
    mesh = tree.switch(style, "GEOMETRY", {"Picket": picketed, "Rail": railed})
    return b.finish("Fence", tree.material(mesh, material))


@asset("Ladder", "object", CATALOG,
       "A straight wooden ladder with two rails and round rungs, standing upright.",
       ("ladder", "rungs", "climb", "wood", "tool", "construction"))
def ladder():
    tree = object_group("Ladder")
    b = _Builder(tree)
    rungs = _input(tree, "Rungs", "INT", 8, min=1, max=100)
    height = _distance(tree, "Height", 2.4, 0.2)
    width = _distance(tree, "Width", 0.45, 0.1, description="Width between the rails")
    material = tree.input("Material", "MATERIAL", palette("Starter Light Wood"))

    rail_w, rail_d = 0.032, 0.07
    x = width * 0.5 + rail_w * 0.5
    rails = tree.join(b.box(rail_w, rail_d, height, -x, 0.0, height * 0.5),
                      b.box(rail_w, rail_d, height, x, 0.0, height * 0.5))
    i = b.index
    gap = height / (rungs + 1.0)
    bars = b.boxes(rungs, (0.0, 0.0, (i + 1.0) * gap), (width + rail_w, 0.034, 0.034), kind="X")
    bars = tree.smooth(bars, angle=50 * RADIANS)
    return b.finish("Ladder", tree.material(tree.join(rails, bars), material))
