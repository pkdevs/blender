# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Quick actions for the native iPad interface.

A short, ordered list of the operations people reach for in the current context: the mode,
the active tool, the mesh select mode and what is selected. Every entry must make sense without
a pointer location (no loop or ring picking) and must not destroy data the user did not ask to
change. Operators run directly; the Adjust Last Operation strip tunes them afterwards. The strip
shows as many as fit, in this order, and scrolls for the rest.
"""

import json
import bpy

__all__ = (
    "actions_json",
    "run",
)

class _State:
    """The context signals the rules read, gathered once per query."""

    def __init__(self, context, tool):
        self.context = context
        self.mode = context.mode
        self.tool = tool or ""
        self.selecting = self.tool.startswith("builtin.select")
        self.obj = context.active_object
        self.selected = list(context.selected_objects) if self.mode == 'OBJECT' else []
        self.verts = self.edges = self.faces = 0
        self.select_mode = (False, False, False)
        if self.mode == 'EDIT_MESH':
            self.select_mode = tuple(context.tool_settings.mesh_select_mode)
            for ob in context.objects_in_mode_unique_data:
                mesh = ob.data
                self.verts += mesh.total_vert_sel
                self.edges += mesh.total_edge_sel
                self.faces += mesh.total_face_sel

    @property
    def size(self):
        """A length on the scale of the active object, for initial bevel and inset sizes."""
        if self.obj is None:
            return 1.0
        return max(max(self.obj.dimensions), 1e-4)

    def selected_of_type(self, *types):
        return [ob for ob in self.selected if ob.type in types]

    def xray(self):
        space = self.context.space_data
        return bool(space and space.type == 'VIEW_3D' and space.shading.show_xray)


class Action:
    __slots__ = ("id", "label", "op", "icon", "when", "props", "checked")

    def __init__(self, id, label, op, icon="", when=None, props=None, checked=None):
        self.id = id
        self.label = label
        self.op = op
        self.icon = icon
        self.when = when or (lambda s: True)
        self.props = props or {}
        self.checked = checked

    def properties(self, state):
        return self.props(state) if callable(self.props) else dict(self.props)

    def operator(self):
        module, name = self.op.split(".")
        return getattr(getattr(bpy.ops, module), name)


def _unapplied_scale(s):
    """Non-unit scale silently skews bevels, modifiers and physics, so offer to apply it."""
    return s.mode == 'OBJECT' and any(any(abs(v - 1.0) > 1e-6 for v in ob.scale) for ob in s.selected)


def _mesh(*modes, verts=0, edges=0, faces=0):
    """Edit Mesh rule: one of the select modes is active and enough elements are selected."""
    index = {"VERT": 0, "EDGE": 1, "FACE": 2}

    def when(s):
        if s.mode != 'EDIT_MESH':
            return False
        if modes and not any(s.select_mode[index[mode]] for mode in modes):
            return False
        return s.verts >= verts and s.edges >= edges and s.faces >= faces
    return when


def _object(min_selected=1, types=None, tools=None):
    def when(s):
        if s.mode != 'OBJECT' or (tools and s.tool not in tools):
            return False
        chosen = s.selected_of_type(*types) if types else s.selected
        return len(chosen) >= min_selected
    return when


# Ordered by how often each is needed in its context; the first ones stay visible on narrow screens.
ACTIONS = [
    # Object Mode.
    Action("object.select_all", "Select All", "object.select_all", "SELECT_EXTEND",
           lambda s: s.mode == 'OBJECT' and not s.selected, {"action": 'SELECT'}),
    Action("object.duplicate", "Duplicate", "object.duplicate", "DUPLICATE", _object(), {"linked": False}),
    Action("object.delete", "Delete", "object.delete", "TRASH", _object(), {"use_global": False, "confirm": False}),
    Action("object.hide", "Hide", "object.hide_view_set", "HIDE_ON", _object(), {"unselected": False}),
    Action("object.apply_scale", "Apply Scale", "object.transform_apply", "CHECKMARK", _unapplied_scale,
           {"location": False, "rotation": False, "scale": True}),
    Action("object.join", "Join", "object.join", "",
           lambda s: s.mode == 'OBJECT' and s.obj in s.selected and s.obj and
           len(s.selected_of_type(s.obj.type)) >= 2 and len(s.selected) == len(s.selected_of_type(s.obj.type))),
    Action("object.parent", "Parent", "object.parent_set", "CON_CHILDOF", _object(2), {"type": 'OBJECT'}),
    Action("object.shade_smooth", "Shade Smooth", "object.shade_smooth", "",
           _object(types=('MESH',), tools=("builtin.select", "builtin.select_box", "builtin.select_circle",
                                            "builtin.select_lasso"))),
    Action("object.shade_flat", "Shade Flat", "object.shade_flat", "",
           _object(types=('MESH',), tools=("builtin.select", "builtin.select_box", "builtin.select_circle",
                                            "builtin.select_lasso"))),
    Action("object.active_camera", "Set Active Camera", "view3d.object_as_camera", "VIEW_CAMERA",
           lambda s: s.mode == 'OBJECT' and s.obj and s.obj.type == 'CAMERA' and s.obj in s.selected and
           s.context.scene.camera != s.obj),
    Action("object.keyframe", "Insert Keyframe", "anim.keyframe_insert", "DECORATE_KEYFRAME",
           lambda s: s.mode in {'OBJECT', 'POSE'} and s.obj and s.obj.animation_data and
           s.obj.animation_data.action and (s.selected or s.mode == 'POSE')),
    Action("object.select_none", "Select None", "object.select_all", "SELECT_SUBTRACT",
           lambda s: s.mode == 'OBJECT' and s.selecting and s.selected, {"action": 'DESELECT'}),
    Action("object.select_invert", "Invert", "object.select_all", "SELECT_DIFFERENCE",
           lambda s: s.mode == 'OBJECT' and s.selecting and s.selected, {"action": 'INVERT'}),

    # 3D cursor tool.
    Action("cursor.to_selected", "Cursor to Selected", "view3d.snap_cursor_to_selected", "PIVOT_CURSOR",
           lambda s: s.tool == "builtin.cursor" and (s.selected or s.verts)),
    Action("cursor.selected_to", "Selection to Cursor", "view3d.snap_selected_to_cursor", "CURSOR",
           lambda s: s.tool == "builtin.cursor" and (s.selected or s.verts), {"use_offset": True}),
    Action("cursor.to_origin", "Cursor to World Origin", "view3d.snap_cursor_to_center", "OBJECT_ORIGIN",
           lambda s: s.tool == "builtin.cursor"),

    # Edit Mesh: nothing selected.
    Action("mesh.select_all", "Select All", "mesh.select_all", "SELECT_EXTEND",
           lambda s: s.mode == 'EDIT_MESH' and not s.verts, {"action": 'SELECT'}),
    # Edit Mesh: faces.
    Action("mesh.extrude_faces", "Extrude", "view3d.edit_mesh_extrude_move_normal", "",
           _mesh("FACE", faces=1)),
    Action("mesh.inset", "Inset", "mesh.inset", "", _mesh("FACE", faces=1),
           lambda s: {"thickness": s.size * 0.05}),
    # Edit Mesh: edges.
    Action("mesh.bevel_edges", "Bevel", "mesh.bevel", "MOD_BEVEL", _mesh("EDGE", edges=1),
           lambda s: {"offset": s.size * 0.05, "segments": 2, "affect": 'EDGES'}),
    Action("mesh.bridge", "Bridge Loops", "mesh.bridge_edge_loops", "", _mesh("EDGE", edges=2)),
    Action("mesh.loop_select", "Select Loops", "mesh.loop_multi_select", "", _mesh("EDGE", edges=1),
           {"ring": False}),
    # Edit Mesh: vertices.
    Action("mesh.connect", "Connect", "mesh.vert_connect_path", "", _mesh("VERT", verts=2)),
    Action("mesh.merge_center", "Merge", "mesh.merge", "AUTOMERGE_OFF", _mesh("VERT", verts=2),
           {"type": 'CENTER'}),
    Action("mesh.bevel_vertices", "Bevel Vertices", "mesh.bevel", "MOD_BEVEL",
           _mesh("VERT", verts=1),
           lambda s: {"offset": s.size * 0.05, "segments": 2, "affect": 'VERTICES'}),
    # Edit Mesh: any selection.
    Action("mesh.fill", "Make Edge/Face", "mesh.edge_face_add", "", _mesh("VERT", "EDGE", verts=2)),
    Action("mesh.subdivide", "Subdivide", "mesh.subdivide", "", _mesh("EDGE", "FACE", edges=1)),
    Action("mesh.duplicate", "Duplicate", "mesh.duplicate", "DUPLICATE", _mesh(verts=1)),
    Action("mesh.separate", "Separate", "mesh.separate", "", _mesh("FACE", faces=1),
           {"type": 'SELECTED'}),
    Action("mesh.delete_verts", "Delete", "mesh.delete", "TRASH", _mesh("VERT", verts=1),
           {"type": 'VERT'}),
    Action("mesh.delete_edges", "Delete", "mesh.delete", "TRASH",
           lambda s: _mesh("EDGE", edges=1)(s) and not s.select_mode[0], {"type": 'EDGE'}),
    Action("mesh.delete_faces", "Delete", "mesh.delete", "TRASH",
           lambda s: _mesh("FACE", faces=1)(s) and not any(s.select_mode[:2]), {"type": 'FACE'}),
    Action("mesh.dissolve_edges", "Dissolve", "mesh.dissolve_edges", "",
           lambda s: _mesh("EDGE", edges=1)(s) and not s.select_mode[0]),
    Action("mesh.merge_distance", "Merge by Distance", "mesh.remove_doubles", "",
           _mesh("VERT", verts=2)),
    # Edit Mesh: growing and shrinking the selection, for every tool.
    Action("mesh.select_linked", "Select Linked", "mesh.select_linked", "", _mesh(verts=1)),
    Action("mesh.select_more", "More", "mesh.select_more", "SELECT_EXTEND", _mesh(verts=1)),
    Action("mesh.select_less", "Less", "mesh.select_less", "SELECT_SUBTRACT", _mesh(verts=1)),
    Action("mesh.select_none", "Select None", "mesh.select_all", "SELECT_SUBTRACT",
           lambda s: _mesh(verts=1)(s) and s.selecting, {"action": 'DESELECT'}),
    Action("mesh.hide", "Hide", "mesh.hide", "HIDE_ON", _mesh(verts=1), {"unselected": False}),

    # Selection tools in Object and Edit Mode.
    Action("view.xray", "X-Ray", "view3d.toggle_xray", "XRAY",
           lambda s: s.selecting and s.mode in {'OBJECT', 'EDIT_MESH'}, checked=lambda s: s.xray()),

    # Sculpt Mode: whole-mesh operations that otherwise need a menu.
    Action("sculpt.mask_invert", "Invert Mask", "paint.mask_flood_fill", "", lambda s: s.mode == 'SCULPT',
           {"mode": 'INVERT'}),
    Action("sculpt.mask_clear", "Clear Mask", "paint.mask_flood_fill", "", lambda s: s.mode == 'SCULPT',
           {"mode": 'VALUE', "value": 0.0}),
    Action("sculpt.reveal", "Reveal", "paint.hide_show_all", "HIDE_OFF", lambda s: s.mode == 'SCULPT',
           {"action": 'SHOW'}),
    Action("sculpt.symmetrize", "Symmetrize", "sculpt.symmetrize", "MOD_MIRROR", lambda s: s.mode == 'SCULPT'),
    Action("sculpt.remesh", "Remesh", "object.voxel_remesh", "MOD_REMESH", lambda s: s.mode == 'SCULPT'),

    # Pose Mode.
    Action("pose.clear", "Clear Transforms", "pose.transforms_clear", "LOOP_BACK", lambda s: s.mode == 'POSE'),
    Action("pose.copy", "Copy Pose", "pose.copy", "COPYDOWN", lambda s: s.mode == 'POSE'),
    Action("pose.paste", "Paste Pose", "pose.paste", "PASTEDOWN", lambda s: s.mode == 'POSE', {"flipped": False}),
    Action("pose.paste_flipped", "Paste Flipped", "pose.paste", "PASTEFLIPDOWN", lambda s: s.mode == 'POSE',
           {"flipped": True}),

    # Weight and Vertex Paint.
    Action("weight.normalize", "Normalize All", "object.vertex_group_normalize_all", "",
           lambda s: s.mode == 'PAINT_WEIGHT', {"lock_active": False}),
    Action("weight.smooth", "Smooth", "object.vertex_group_smooth", "MOD_SMOOTH",
           lambda s: s.mode == 'PAINT_WEIGHT'),
    Action("weight.clean", "Clean", "object.vertex_group_clean", "", lambda s: s.mode == 'PAINT_WEIGHT'),
    Action("vertex.fill", "Fill", "paint.vertex_color_set", "", lambda s: s.mode == 'PAINT_VERTEX'),
    Action("vertex.smooth", "Smooth", "paint.vertex_color_smooth", "MOD_SMOOTH", lambda s: s.mode == 'PAINT_VERTEX'),
    Action("vertex.invert", "Invert", "paint.vertex_color_invert", "", lambda s: s.mode == 'PAINT_VERTEX'),

    # Grease Pencil drawing.
    Action("grease.blank_frame", "Insert Blank Frame", "grease_pencil.insert_blank_frame", "ADD",
           lambda s: s.mode == 'PAINT_GREASE_PENCIL'),
    Action("grease.layer", "New Layer", "grease_pencil.layer_add", "OUTLINER_DATA_GP_LAYER",
           lambda s: s.mode == 'PAINT_GREASE_PENCIL'),

    # Curves, armatures and Grease Pencil strokes in Edit Mode.
    Action("curve.subdivide", "Subdivide", "curve.subdivide", "", lambda s: s.mode == 'EDIT_CURVE'),
    Action("curve.switch_direction", "Switch Direction", "curve.switch_direction", "ARROW_LEFTRIGHT",
           lambda s: s.mode == 'EDIT_CURVE'),
    Action("armature.subdivide", "Subdivide", "armature.subdivide", "", lambda s: s.mode == 'EDIT_ARMATURE'),
    Action("armature.duplicate", "Duplicate", "armature.duplicate", "DUPLICATE", lambda s: s.mode == 'EDIT_ARMATURE'),
    Action("grease.duplicate", "Duplicate", "grease_pencil.duplicate", "DUPLICATE",
           lambda s: s.mode == 'EDIT_GREASE_PENCIL'),
    Action("grease.delete", "Delete", "grease_pencil.delete", "TRASH", lambda s: s.mode == 'EDIT_GREASE_PENCIL'),
]

_BY_ID = {action.id: action for action in ACTIONS}
classes = ()


def _available(state):
    for action in ACTIONS:
        try:
            if action.when(state) and action.operator().poll():
                yield action
        except (AttributeError, KeyError, RuntimeError, ReferenceError):
            continue


def actions_json(context, tool):
    state = _State(context, tool)
    items = []
    for action in _available(state):
        items.append({
            "title": action.label, "action": action.id, "operator": action.operator().idname(),
            "icon": action.icon, "symbol": "",
            "selected": bool(action.checked(state)) if action.checked else False,
        })
    return json.dumps(items, ensure_ascii=True)


def run(context, action_id, tool=""):
    """Run a quick action if it still applies; the button may predate a context change."""
    action = _BY_ID.get(action_id)
    if action is None:
        return False
    state = _State(context, tool)
    if not action.when(state):
        return False
    operator = action.operator()
    if not operator.poll():
        return False
    return 'FINISHED' in operator('EXEC_REGION_WIN', True, **action.properties(state))
