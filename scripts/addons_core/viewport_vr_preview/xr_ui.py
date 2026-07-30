# SPDX-FileCopyrightText: 2021-2023 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import (
    Operator,
    Panel,
)


class VIEW3D_PT_vr_world_space_panel(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'XR'
    bl_category = ""
    bl_xr_mount_point = 'HEAD_FOLLOW'


def vr_world_space_panel_id(panel_id):
    suffix = "_ws"
    max_base_len = 64 - len(suffix)
    return f"{panel_id[:max_base_len]}{suffix}"


def panel_clone_category(panel_cls):
    bl_context = getattr(panel_cls, "bl_context", None)
    if isinstance(bl_context, str) and bl_context.startswith("."):
        return "topbar_context"
    return "generic"


def adapt_panel_clone_attrs(panel_cls, world_space_attrs):
    if panel_clone_category(panel_cls) == "topbar_context":
        # Topbar-style tool panels rely on special dotted bl_context handling that does not
        # translate to the XR wrapper's VIEW_3D/XR host region.
        world_space_attrs.pop("bl_context", None)

    # PropertyPanel-derived polls expect these helper class attributes to exist on the cloned
    # panel type, even though they are not regular `bl_*` registration metadata.
    for attr_name in ("_context_path", "_property_type"):
        if hasattr(panel_cls, attr_name):
            world_space_attrs[attr_name] = getattr(panel_cls, attr_name)
    return world_space_attrs


# Copies an existing panel class and adapts its context to work in XR.
# This allows for arbitrary world space panels to be generated from a desktop panel class.
class _XRCloneContextProxy:
    def __init__(self, context, panel_cls):
        self._context = context
        self._panel_cls = panel_cls

    def _properties_space_data(self):
        window_manager = getattr(self._context, "window_manager", None)
        windows = getattr(window_manager, "windows", ())
        for window in windows:
            screen = getattr(window, "screen", None)
            areas = getattr(screen, "areas", ())
            for area in areas:
                if area.type == 'PROPERTIES':
                    spaces = getattr(area, "spaces", None)
                    active = getattr(spaces, "active", None) if spaces is not None else None
                    if active is not None:
                        return active
        return None

    def _properties_pinned_id(self):
        space_data = self._properties_space_data()
        if space_data is None or not getattr(space_data, "use_pin_id", False):
            return None
        return getattr(space_data, "pin_id", None)

    def _properties_active_object(self):
        pinned_id = self._properties_pinned_id()
        if isinstance(pinned_id, bpy.types.Object):
            return pinned_id
        view_layer = getattr(self._context, "view_layer", None)
        objects = getattr(view_layer, "objects", None)
        if objects is not None:
            return getattr(objects, "active", None)
        return None

    def __getattr__(self, name):
        if name == "space_data":
            space_data = self._properties_space_data()
            if space_data is not None:
                return space_data
        try:
            return getattr(self._context, name)
        except AttributeError:
            if name in {"object", "active_object"}:
                return self._properties_active_object()
            if name == "material":
                obj = self._properties_active_object()
                return getattr(obj, "active_material", None) if obj is not None else None
            if name == "world":
                scene = getattr(self._context, "scene", None)
                return getattr(scene, "world", None) if scene is not None else None
            raise


def adapt_panel_clone_context(panel_cls, context):
    if getattr(panel_cls, "bl_space_type", None) == 'PROPERTIES':
        return _XRCloneContextProxy(context, panel_cls)
    return context


def mirror_panel_to_xr(panel_cls, *, class_name=None, **attrs):
    if class_name is None:
        class_name = vr_world_space_panel_id(panel_cls.__name__)
    world_space_attrs = {
        "__module__": __name__,
        "bl_idname": class_name,
        "bl_category": "",
    }
    for attr_name in (
        "bl_label",
        "bl_options",
        "bl_order",
        "bl_ui_units_x",
        "bl_context",
        "bl_description",
        "bl_owner_id",
        "bl_translation_context",
    ):
        if hasattr(panel_cls, attr_name):
            world_space_attrs[attr_name] = getattr(panel_cls, attr_name)
    world_space_attrs = adapt_panel_clone_attrs(panel_cls, world_space_attrs)
    parent_id = getattr(panel_cls, "bl_parent_id", None)
    if parent_id and "bl_parent_id" not in attrs:
        world_space_attrs["bl_parent_id"] = vr_world_space_panel_id(parent_id)
    for base_cls in reversed(panel_cls.mro()):
        if base_cls in {object, Panel, bpy.types.Panel}:
            continue
        for attr_name, attr_value in base_cls.__dict__.items():
            if attr_name.startswith("__"):
                continue
            if attr_name in world_space_attrs:
                continue
            if attr_name in {
                "bl_rna",
                "rna_type",
                "is_registered",
                "bl_space_type",
                "bl_region_type",
                "bl_category",
                "bl_idname",
                "bl_parent_id",
                "bl_xr_mount_point",
            }:
                continue
            if attr_name.endswith("_panel_id") and isinstance(attr_value, str):
                world_space_attrs[attr_name] = vr_world_space_panel_id(attr_value)
                continue
            if callable(attr_value) or isinstance(attr_value, (classmethod, staticmethod)):
                world_space_attrs[attr_name] = attr_value
    def draw(self, context):
        return panel_cls.draw(self, adapt_panel_clone_context(panel_cls, context))

    world_space_attrs["draw"] = draw
    if hasattr(panel_cls, "poll"):
        @classmethod
        def poll(cls, context):
            return panel_cls.poll(adapt_panel_clone_context(panel_cls, context))

        world_space_attrs["poll"] = poll
    if hasattr(panel_cls, "draw_header"):
        def draw_header(self, context):
            return panel_cls.draw_header(self, adapt_panel_clone_context(panel_cls, context))

        world_space_attrs["draw_header"] = draw_header
    if hasattr(panel_cls, "draw_header_preset"):
        def draw_header_preset(self, context):
            return panel_cls.draw_header_preset(self, adapt_panel_clone_context(panel_cls, context))

        world_space_attrs["draw_header_preset"] = draw_header_preset
    world_space_attrs.update(attrs)
    return type(class_name, (VIEW3D_PT_vr_world_space_panel, Panel), world_space_attrs)


_world_space_panel_classes = {}


def track_world_space_panel_classes(panel_id, world_space_classes):
    _world_space_panel_classes[panel_id] = tuple(world_space_classes)


def panel_idname(panel_cls):
    return getattr(panel_cls, "bl_idname", panel_cls.__name__)


def resolve_panel_clone_root_class(panel_cls):
    panel_classes = {panel_idname(cls): cls for cls in iter_panel_subclasses()}
    current_cls = panel_cls
    while True:
        parent_id = getattr(current_cls, "bl_parent_id", None)
        if not parent_id:
            return current_cls
        parent_cls = panel_classes.get(parent_id)
        if parent_cls is None:
            return current_cls
        if getattr(parent_cls, "bl_space_type", None) != getattr(panel_cls, "bl_space_type", None):
            return current_cls
        if getattr(parent_cls, "bl_region_type", None) != getattr(panel_cls, "bl_region_type", None):
            return current_cls
        current_cls = parent_cls


def supported_source_panel(panel_id):
    panel_cls = bpy.types.Panel.bl_rna_get_subclass_py(panel_id)
    if panel_cls is None:
        return None, None

    panel_cls = resolve_panel_clone_root_class(panel_cls)
    panel_id = panel_idname(panel_cls)
    if not is_supported_source_panel_type(getattr(panel_cls, "bl_space_type", None),
                                          getattr(panel_cls, "bl_region_type", None)):
        return None, None
    return panel_id, panel_cls


def is_supported_source_panel_type(space_type, region_type):
    return (
        (space_type == 'VIEW_3D' and region_type == 'UI') or
        (space_type == 'PROPERTIES' and region_type == 'WINDOW') or
        (space_type == 'PREFERENCES' and region_type == 'WINDOW')
    )


def xr_panel_mirror_source_ids():
    return set(_world_space_panel_classes.keys())


def iter_panel_subclasses(base_cls=Panel):
    for subcls in base_cls.__subclasses__():
        yield subcls
        yield from iter_panel_subclasses(subcls)


def collect_panel_subtree(panel_cls):
    panel_classes = {panel_idname(cls): cls for cls in iter_panel_subclasses()}
    ordered = []
    source_space_type = getattr(panel_cls, "bl_space_type", None)
    source_region_type = getattr(panel_cls, "bl_region_type", None)

    def collect_recursive(cls):
        ordered.append(cls)
        parent_id = panel_idname(cls)
        child_classes = [
            child_cls for child_cls in panel_classes.values()
            if getattr(child_cls, "bl_parent_id", None) == parent_id and
            getattr(child_cls, "bl_space_type", None) == source_space_type and
            getattr(child_cls, "bl_region_type", None) == source_region_type
        ]
        for child_cls in sorted(child_classes, key=lambda child: panel_idname(child)):
            collect_recursive(child_cls)

    collect_recursive(panel_cls)
    return ordered


def world_space_classes_from_panel(panel_cls, *, default_mount_point='HEAD_FOLLOW', mount_overrides=None):
    mount_overrides = mount_overrides or {}
    world_space_classes = []
    for panel_tree_cls in collect_panel_subtree(panel_cls):
        mirror_attrs = {
            "bl_xr_mount_point": mount_overrides.get(
                panel_idname(panel_tree_cls),
                default_mount_point,
            ),
        }
        parent_id = getattr(panel_tree_cls, "bl_parent_id", None)
        if parent_id:
            mirror_attrs["bl_parent_id"] = vr_world_space_panel_id(parent_id)
        world_space_classes.append(mirror_panel_to_xr(panel_tree_cls, **mirror_attrs))
    return tuple(world_space_classes)


def register_default_world_space_panel(panel_id, *, default_mount_point='HEAD_FOLLOW', mount_overrides=None):
    panel_id, panel_cls = supported_source_panel(panel_id)
    if panel_cls is None:
        raise KeyError(panel_id)

    world_space_classes = world_space_classes_from_panel(
        panel_cls,
        default_mount_point=default_mount_point,
        mount_overrides=mount_overrides,
    )
    for world_space_cls in world_space_classes:
        bpy.utils.register_class(world_space_cls)
    track_world_space_panel_classes(panel_id, world_space_classes)
    return world_space_classes


def dynamic_world_space_toggle(panel_id, panel_cls=None):
    if panel_cls is None:
        panel_id, panel_cls = supported_source_panel(panel_id)
        if panel_cls is None:
            raise LookupError(panel_id)

    if panel_id in _world_space_panel_classes:
        return toggle_tracked_world_space_panel(panel_id)

    world_space_classes = world_space_classes_from_panel(panel_cls)
    for world_space_cls in world_space_classes:
        bpy.utils.register_class(world_space_cls)
    track_world_space_panel_classes(panel_id, world_space_classes)
    return True


def toggle_tracked_world_space_panel(panel_id):
    world_space_classes = _world_space_panel_classes.get(panel_id)
    if not world_space_classes:
        raise KeyError(panel_id)

    root_world_space_cls = world_space_classes[0]
    if root_world_space_cls.is_registered:
        for world_space_cls in reversed(world_space_classes):
            if world_space_cls.is_registered:
                bpy.utils.unregister_class(world_space_cls)
        return False

    for world_space_cls in world_space_classes:
        if not world_space_cls.is_registered:
            bpy.utils.register_class(world_space_cls)
    return True


def tag_redraw_panel_sources(context):
    for window in context.window_manager.windows:
        screen = getattr(window, "screen", None)
        areas = getattr(screen, "areas", None)
        if areas is None:
            continue
        for area in areas:
            if area.type in {'VIEW_3D', 'PROPERTIES'}:
                area.tag_redraw()


def refresh_xr_world_space_ui():
    xr_refresh = getattr(bpy.ops.wm, "xr_ui_refresh", None)
    if xr_refresh is None:
        return False

    window_manager = getattr(bpy.context, "window_manager", None)
    windows = getattr(window_manager, "windows", ())
    for window in windows:
        screen = getattr(window, "screen", None)
        areas = getattr(screen, "areas", None)
        if areas is None:
            continue

        area = next((candidate for candidate in areas if candidate.type == 'VIEW_3D'), None)
        region = None
        if area is not None:
            region = next((candidate for candidate in area.regions if candidate.type == 'WINDOW'),
                          None)

        override = {"window": window}
        if area is not None:
            override["area"] = area
        if region is not None:
            override["region"] = region

        with bpy.context.temp_override(**override):
            xr_refresh()
        return True

    xr_refresh()
    return True

def schedule_xr_world_space_ui_refresh():
    attempts_remaining = 20

    def _refresh():
        nonlocal attempts_remaining
        if refresh_xr_world_space_ui():
            return None
        attempts_remaining -= 1
        return 0.1 if attempts_remaining > 0 else None

    bpy.app.timers.register(_refresh, first_interval=0.0)


class VIEW3D_OT_xr_toggle_panel_clone(Operator):
    bl_idname = "view3d.xr_toggle_panel_clone"
    bl_label = "Toggle World Space Clone"
    bl_description = "Toggle a world space XR clone of this desktop panel"

    panel_id: bpy.props.StringProperty(options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        space = context.space_data
        region = context.region
        return (
            space is not None and region is not None and
            is_supported_source_panel_type(space.type, region.type)
        )

    def execute(self, context):
        source_panel_id, panel_cls = supported_source_panel(self.panel_id)
        if panel_cls is None:
            self.report(
                {'ERROR'},
                f"Unsupported panel: {self.panel_id} (panel is not available as a Python bpy.types class)",
            )
            return {'CANCELLED'}
        enabled = dynamic_world_space_toggle(source_panel_id, panel_cls)
        tag_redraw_panel_sources(context)
        refresh_xr_world_space_ui()
        message = "Enabled" if enabled else "Disabled"
        self.report({'INFO'}, f"{message} world space clone for {source_panel_id}")
        return {'FINISHED'}


def register():
    bpy.utils.register_class(VIEW3D_OT_xr_toggle_panel_clone)


def unregister_world_space_panels():
    for world_space_panel_id in tuple(_world_space_panel_classes.keys()):
        if _world_space_panel_classes[world_space_panel_id][0].is_registered:
            toggle_tracked_world_space_panel(world_space_panel_id)
    _world_space_panel_classes.clear()


def unregister():
    unregister_world_space_panels()
    bpy.utils.unregister_class(VIEW3D_OT_xr_toggle_panel_clone)
