# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The active brush's size, strength and color for the native iPad interface.

The iPad shows these in the sidebar beside the viewport and the color well above the brushes,
rather than in the tool settings, and sets them through `brush_set`. Each is read from and written
to what Blender's own header uses: the unified paint settings when they are shared, otherwise the
brush.
"""

import json

import bpy

__all__ = (
    "brush_json",
    "brush_set",
)

_GREASE_PENCIL_MODES = {
    'PAINT_GREASE_PENCIL', 'SCULPT_GREASE_PENCIL', 'WEIGHT_GREASE_PENCIL', 'VERTEX_GREASE_PENCIL',
}


def _settings(context):
    from bl_ui.properties_paint_common import UnifiedPaintPanel
    settings = UnifiedPaintPanel.paint_settings(context)
    if settings is None or settings.brush is None:
        return None, None
    return settings, settings.brush


def _unified_owner(settings, brush, name):
    ups = getattr(settings, "unified_paint_settings", None)
    return ups if ups is not None and getattr(ups, "use_unified_" + name, False) else brush


def _owners(context):
    """The owners of the brush's size, strength and color, each None where it has none."""
    settings, brush = _settings(context)
    if brush is None:
        return None
    # Only while painting with the brush, not with the mode's other tools, such as Box Mask.
    tool = context.workspace.tools.from_space_view3d_mode(context.mode, create=False)
    if tool is None or not tool.use_brushes:
        return None
    mode = context.mode
    draw_type = brush.gpencil_brush_type if mode == 'PAINT_GREASE_PENCIL' else None
    size = strength = color = None
    if draw_type != 'FILL':
        # Grease Pencil drawing sizes its strokes from the brush, as its header does.
        size = brush if draw_type else _unified_owner(settings, brush, "size")
        strength = brush if draw_type else _unified_owner(settings, brush, "strength")
    if draw_type in {'DRAW', 'FILL'}:
        if context.tool_settings.gpencil_paint.color_mode == 'VERTEXCOLOR':
            # Strokes take the brush's own color, never the unified one, as in the header.
            color = brush
    elif mode in {'PAINT_VERTEX', 'PAINT_TEXTURE', 'VERTEX_GREASE_PENCIL'} or draw_type == 'TINT':
        color = _unified_owner(settings, brush, "color")
    elif mode == 'SCULPT' and brush.sculpt_brush_type == 'PAINT':
        color = _unified_owner(settings, brush, "color")
    return size, strength, color


def _size_prop(owner):
    return "unprojected_size" if owner.use_locked_size == 'SCENE' else "size"


def _range(owner, prop):
    rna = owner.bl_rna.properties[prop]
    return rna.soft_min, rna.soft_max, rna.default


def brush_json(context):
    """The brush's size, strength and color, or null outside the modes that paint with brushes."""
    owners = _owners(context)
    if owners is None:
        return "null"
    size, strength, color = owners
    result = {}
    if size is not None:
        prop = _size_prop(size)
        low, high, default = _range(size, prop)
        result["size"] = {
            "value": float(getattr(size, prop)),
            "min": float(max(low, 1.0 if prop == "size" else 0.001)),
            "max": float(high),
            "default": float(default),
            # Pixels on screen, or a distance in the scene, which the iPad shows no circle for.
            "pixels": prop == "size",
            "label": "Size" if context.mode in _GREASE_PENCIL_MODES else "Radius",
        }
    if strength is not None:
        low, high, default = _range(strength, "strength")
        result["strength"] = {
            "value": float(strength.strength),
            "min": float(low),
            "max": float(high),
            "default": float(default),
        }
    if color is not None:
        result["color"] = [float(c) for c in color.color]
    return json.dumps(result)


def brush_set(context, payload):
    """Set one of `size`, `strength` or `color` from a JSON object with its `key` and `value`."""
    request = json.loads(payload)
    size, strength, color = _owners(context) or (None, None, None)
    key = request.get("key")
    value = request.get("value")
    if key == "size" and size is not None:
        prop = _size_prop(size)
        setattr(size, prop, int(round(value)) if prop == "size" else float(value))
    elif key == "strength" and strength is not None:
        strength.strength = float(value)
    elif key == "color" and color is not None:
        color.color = [float(c) for c in value[:3]]


classes = ()
