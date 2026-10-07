# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""A small, explicit API for building node trees from Python.

`Tree` wraps a Geometry Nodes or shader node tree. Nodes are added with `tree.n()`, whose
arguments set node properties (when the node has an attribute of that name) or inputs (by name,
or by index for positional arguments). A value is either a constant or an output socket, which is
linked. Trees are laid out left to right by dependency depth, so they stay readable for anyone who
opens them in the node editor.
"""

import re

import bpy

SOCKET_TYPES = {
    "FLOAT": "NodeSocketFloat",
    "INT": "NodeSocketInt",
    "BOOL": "NodeSocketBool",
    "VECTOR": "NodeSocketVector",
    "COLOR": "NodeSocketColor",
    "GEOMETRY": "NodeSocketGeometry",
    "MATERIAL": "NodeSocketMaterial",
    "OBJECT": "NodeSocketObject",
    "COLLECTION": "NodeSocketCollection",
    "MENU": "NodeSocketMenu",
    "SHADER": "NodeSocketShader",
}


# Node attributes that only affect the node editor, never what the node computes.
_UI_ATTRIBUTES = {"name", "label", "width", "height", "location", "color", "use_custom_color",
                  "hide", "mute", "select", "parent", "dimensions", "show_options",
                  "show_preview", "show_texture"}


def _normalized(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def _is_socket(value):
    return isinstance(value, bpy.types.NodeSocket)


class Tree:
    def __init__(self, node_tree, group_io=True):
        self.nt = node_tree
        self.nodes = node_tree.nodes
        self.links = node_tree.links
        self._menus = []
        # Group menu inputs: identifier -> item names, and the one index each menu drives.
        self._menu_items = {}
        self._menu_indices = {}
        if group_io:
            self.group_in = self.nodes.new("NodeGroupInput")
            self.group_out = self.nodes.new("NodeGroupOutput")

    # -------------------------------------------------------------------------
    # Construction.

    @classmethod
    def geometry(cls, name, modifier=True, geometry_input=False):
        """A Geometry Nodes group. Object generators need no geometry input; modifiers do."""
        tree = cls(bpy.data.node_groups.new(name, "GeometryNodeTree"))
        tree.nt.is_modifier = modifier
        if geometry_input:
            tree.input("Geometry", "GEOMETRY")
        tree.nt.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
        return tree

    @classmethod
    def shader_group(cls, name):
        """A shader node group, so a material's tweakable inputs show in Properties."""
        tree = cls(bpy.data.node_groups.new(name, "ShaderNodeTree"))
        tree.nt.interface.new_socket("Shader", in_out="OUTPUT", socket_type="NodeSocketShader")
        return tree

    @classmethod
    def for_material(cls, material):
        """Wraps a material's own node tree, emptied."""
        material.use_nodes = True
        material.node_tree.nodes.clear()
        return cls(material.node_tree, group_io=False)

    # -------------------------------------------------------------------------
    # Interface.

    def input(self, name, socket_type, default=None, min=None, max=None, subtype=None,
              description=""):
        """Add a group input and return its socket on the Group Input node."""
        item = self.nt.interface.new_socket(name, in_out="INPUT",
                                            socket_type=SOCKET_TYPES[socket_type])
        if subtype is not None:
            item.subtype = subtype
        if description:
            item.description = description
        if min is not None:
            item.min_value = min
        if max is not None:
            item.max_value = max
        if default is not None and socket_type != "MENU":
            item.default_value = default
        return self.group_in.outputs[name]

    def menu(self, name, items, default=None, description=""):
        """A drop-down input. Use `switch()` to choose values with it."""
        socket = self.input(name, "MENU", description=description)
        self._menus.append((name, list(items), default or items[0]))
        self._menu_items[socket.identifier] = list(items)
        return socket

    def switch(self, menu_socket, data_type, values):
        """Choose between `values` (item name -> value or socket) with a menu input.

        A menu input may feed only one Menu Switch: with more, Blender can't tell which defines
        its items and the menu ends up empty. So each group menu drives a single Menu Switch that
        outputs the chosen item's index, and every switch on it is an Index Switch.
        """
        items = self._menu_items.get(getattr(menu_socket, "identifier", None))
        if items is None or menu_socket.node != self.group_in:
            node = self.nodes.new("GeometryNodeMenuSwitch")
            node.data_type = data_type
            node.enum_items.clear()
            for name in values:
                node.enum_items.new(name)
            self.links.new(menu_socket, node.inputs[0])
            for name, value in values.items():
                self.set_input(node.inputs[name], value)
            return node.outputs[0]
        unknown = [name for name in values if name not in items]
        missing = [name for name in items if name not in values]
        if unknown or missing:
            raise ValueError(f"Menu {menu_socket.name!r} switch: unknown items {unknown}, "
                             f"missing items {missing}")
        node = self.nodes.new("GeometryNodeIndexSwitch")
        node.data_type = data_type
        node.index_switch_items.clear()
        for _ in items:
            node.index_switch_items.new()
        self.links.new(self.menu_index(menu_socket), node.inputs["Index"])
        for position, name in enumerate(items):
            self.set_input(node.inputs[str(position)], values[name])
        return node.outputs[0]

    def menu_index(self, menu_socket):
        """The chosen item's position in a group menu input, as an integer."""
        key = menu_socket.identifier
        if key not in self._menu_indices:
            node = self.nodes.new("GeometryNodeMenuSwitch")
            node.data_type = "INT"
            node.enum_items.clear()
            for name in self._menu_items[key]:
                node.enum_items.new(name)
            self.links.new(menu_socket, node.inputs[0])
            for position, name in enumerate(self._menu_items[key]):
                node.inputs[name].default_value = position
            self._menu_indices[key] = node.outputs[0]
        return self._menu_indices[key]

    def finish_menus(self):
        """Menu defaults can only be set once a switch defines the items."""
        for name, items, default in self._menus:
            item = self.nt.interface.items_tree[name]
            try:
                item.default_value = default
            except (TypeError, ValueError):
                pass

    def output(self, value, name=None):
        socket = self.group_out.inputs[name] if name else self.group_out.inputs[0]
        self.set_input(socket, value)

    # -------------------------------------------------------------------------
    # Nodes.

    def set_input(self, socket, value):
        if value is None:
            return
        if isinstance(value, bpy.types.Node):
            value = value.outputs[0]
        if _is_socket(value):
            self.links.new(value, socket)
        elif socket.type == "MENU" and isinstance(value, str):
            # Menu sockets take the item's display name ("N-gons"); accept identifiers ("NGONS").
            try:
                socket.default_value = value
                return
            except TypeError as error:
                items = re.findall(r"'([^']*)'", str(error).rsplit(" in ", 1)[-1])
            wanted = _normalized(value)
            for item in items:
                if _normalized(item) == wanted:
                    socket.default_value = item
                    return
            raise ValueError(f"{socket.node.bl_idname}.{socket.name}: no menu item {value!r} in {items}")
        else:
            socket.default_value = value

    def n(self, idname, *args, **kwargs):
        """Add a node. Positional args set inputs by index; keywords set properties or inputs."""
        node = self.nodes.new(idname)
        inputs = {}
        for key, value in kwargs.items():
            attribute = key.lower()
            # An input of that name always wins: nodes also have UI attributes such as
            # `width`, `height`, `location` and `color` that must never be set by accident.
            if not (value is None or _is_socket(value) or isinstance(value, bpy.types.Node)) and \
                    not self._has_input(node, key) and hasattr(node, attribute) and \
                    attribute not in _UI_ATTRIBUTES:
                try:
                    setattr(node, attribute, value)
                    continue
                except (TypeError, AttributeError):
                    pass
            inputs[key] = value
        for index, value in enumerate(args):
            self.set_input(self._enabled_inputs(node)[index], value)
        for key, value in inputs.items():
            self.set_input(self._input(node, key), value)
        return node

    @staticmethod
    def _has_input(node, key):
        wanted = {key.lower(), key.replace("_", " ").lower()}
        return any(socket.name.lower() in wanted or socket.identifier == key
                   for socket in node.inputs)

    @staticmethod
    def _enabled_inputs(node):
        return [socket for socket in node.inputs if socket.enabled]

    @staticmethod
    def _input(node, key):
        """Input by name, preferring enabled sockets (nodes reuse names across data types)."""
        wanted = {key.lower(), key.replace("_", " ").lower()}
        for socket in node.inputs:
            if socket.enabled and (socket.name.lower() in wanted or socket.identifier == key):
                return socket
        for socket in node.inputs:
            if socket.name.lower() in wanted or socket.identifier == key:
                return socket
        raise KeyError(f"{node.bl_idname} has no input {key!r}: "
                       f"{[socket.name for socket in node.inputs]}")

    @staticmethod
    def out(node, name):
        """Output by name, preferring enabled sockets."""
        for socket in node.outputs:
            if socket.enabled and socket.name == name:
                return socket
        return node.outputs[name]

    # -------------------------------------------------------------------------
    # Common shorthands.

    def math(self, operation, a, b=None, c=None, clamp=False):
        node = self.nodes.new("ShaderNodeMath")
        node.operation = operation
        node.use_clamp = clamp
        for index, value in enumerate((a, b, c)):
            if value is not None:
                self.set_input(node.inputs[index], value)
        return node.outputs[0]

    def vmath(self, operation, a, b=None, scale=None):
        node = self.nodes.new("ShaderNodeVectorMath")
        node.operation = operation
        self.set_input(node.inputs[0], a)
        if b is not None:
            self.set_input(node.inputs[1], b)
        if scale is not None:
            self.set_input(node.inputs["Scale"], scale)
        output = "Value" if operation in {"DOT_PRODUCT", "LENGTH", "DISTANCE"} else "Vector"
        return self.out(node, output)

    def xyz(self, x=0.0, y=0.0, z=0.0):
        return self.n("ShaderNodeCombineXYZ", X=x, Y=y, Z=z).outputs[0]

    def separate(self, vector):
        node = self.n("ShaderNodeSeparateXYZ", vector)
        return node.outputs[0], node.outputs[1], node.outputs[2]

    def compare(self, operation, a, b, data_type="FLOAT"):
        node = self.n("FunctionNodeCompare", data_type=data_type, operation=operation)
        enabled = self._enabled_inputs(node)
        self.set_input(enabled[0], a)
        self.set_input(enabled[1], b)
        return self.out(node, "Result")

    def join(self, *geometries):
        node = self.nodes.new("GeometryNodeJoinGeometry")
        for geometry in geometries:
            self.set_input(node.inputs[0], geometry)
        return node.outputs[0]

    def transform(self, geometry, translation=None, rotation=None, scale=None):
        node = self.n("GeometryNodeTransform", Geometry=geometry)
        if translation is not None:
            self.set_input(node.inputs["Translation"], translation)
        if rotation is not None:
            self.set_input(node.inputs["Rotation"], rotation)
        if scale is not None:
            self.set_input(node.inputs["Scale"], scale)
        return node.outputs[0]

    def material(self, geometry, material, selection=None):
        node = self.n("GeometryNodeSetMaterial", Geometry=geometry, Material=material)
        if selection is not None:
            self.set_input(node.inputs["Selection"], selection)
        return node.outputs[0]

    def smooth(self, geometry, smooth=True, angle=None):
        """Shade smooth; with `angle` (radians), smooth by angle like Blender's Shade Auto Smooth."""
        if angle is None:
            return self.n("GeometryNodeSetShadeSmooth", Geometry=geometry, Shade_Smooth=smooth).outputs[0]
        edge_angle = self.n("GeometryNodeInputMeshEdgeAngle")
        sharp = self.compare("GREATER_THAN", self.out(edge_angle, "Unsigned Angle"), angle)
        geometry = self.n("GeometryNodeSetShadeSmooth", Geometry=geometry, domain="FACE",
                          Shade_Smooth=True).outputs[0]
        return self.n("GeometryNodeSetShadeSmooth", Geometry=geometry, domain="EDGE",
                      Selection=sharp, Shade_Smooth=False).outputs[0]

    def realize(self, geometry):
        return self.n("GeometryNodeRealizeInstances", Geometry=geometry).outputs[0]

    # -------------------------------------------------------------------------
    # Layout.

    def layout(self):
        """Place nodes in columns by dependency depth, outputs on the right."""
        self.finish_menus()
        depth = {node: 0 for node in self.nodes}
        changed = True
        guard = 0
        while changed and guard < len(self.nodes) + 2:
            changed = False
            guard += 1
            for link in self.links:
                wanted = depth[link.from_node] + 1
                if depth[link.to_node] < wanted:
                    depth[link.to_node] = wanted
                    changed = True
        columns = {}
        for node in self.nodes:
            columns.setdefault(depth[node], []).append(node)
        for column, nodes in columns.items():
            y = 0.0
            for node in nodes:
                node.location = (column * 260.0, y)
                y -= 60.0 + 26.0 * (len([s for s in node.inputs if s.enabled]) +
                                     len([s for s in node.outputs if s.enabled]))
