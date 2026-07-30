# SPDX-FileCopyrightText: 2021-2023 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

if "bpy" in locals():
    import importlib
    importlib.reload(preferences)
    importlib.reload(properties)
    importlib.reload(xr_ui)
else:
    from . import preferences, properties, xr_ui

import bpy
from bpy.app.translations import (
    pgettext_n as n_,
    pgettext_iface as iface_,
    contexts as i18n_contexts,
)
from bpy.types import (
    Menu,
    Panel,
    UIList,
)
# Add space_view3d.py to module search path for VIEW3D_PT_object_type_visibility import.
import os.path
import sys
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '../../startup/bl_ui')))
from space_view3d import VIEW3D_PT_object_type_visibility


class VRButtonsPanel:
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "VR"

XR_DEFAULT_MOUNT_POINT = 'HEAD_FOLLOW'
XR_MOUNT_POINT_OVERRIDES = {
    "VIEW3D_PT_vr_session": 'LEFT_HAND',
}
XR_PANEL_EXCLUDE = {
    "VIEW3D_PT_vr_session_view_object_type_visibility",
}
world_space_source_classes = ()
world_space_classes = ()


def build_world_space_classes():
    source_panel_classes = tuple(
        cls for cls in classes
        if isinstance(cls, type) and issubclass(cls, Panel) and
        getattr(cls, "bl_space_type", None) == 'VIEW_3D' and
        getattr(cls, "bl_region_type", None) == 'UI' and
        xr_ui.panel_idname(cls) not in XR_PANEL_EXCLUDE and
        not getattr(cls, "bl_parent_id", None)
    )
    extra_source_panel_classes = (
        preferences.USERPREF_PT_vr_navigation,
    )
    return source_panel_classes + extra_source_panel_classes


# Session.
class VIEW3D_PT_vr_session(VRButtonsPanel, Panel):
    bl_label = "VR Session"

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings
        scene = context.scene

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        is_session_running = bpy.types.XrSessionState.is_running(context)

        # Using SNAP_FACE because it looks like a stop icon -- I shouldn't
        # have commit rights...
        toggle_info = ((iface_("Start VR Session"), 'PLAY') if not is_session_running
                       else (iface_("Stop VR Session"), 'SNAP_FACE'))
        layout.operator("wm.xr_session_toggle", text=toggle_info[0],
                        translate=False, icon=toggle_info[1])

        layout.separator()

        col = layout.column(align=True, heading="Tracking")
        col.prop(session_settings, "use_positional_tracking", text="Positional")
        col.prop(session_settings, "use_absolute_tracking", text="Absolute")

        col = layout.column(align=True, heading="Actions")
        col.prop(scene, "vr_actions_enable")


# View.
class VIEW3D_PT_vr_session_view(VRButtonsPanel, Panel):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        col = layout.column(align=True, heading="Show")
        col.prop(session_settings, "show_floor", text="Floor")
        col.prop(session_settings, "show_passthrough", text="Passthrough")
        col.prop(session_settings, "show_annotation", text="Annotations")

        col.prop(session_settings, "show_selection", text="Selection")
        col.prop(session_settings, "show_controllers", text="Controllers")
        col.prop(session_settings, "show_custom_overlays", text="Custom Overlays")
        col.prop(session_settings, "show_world_space_ui", text="World Space UI")
        col.prop(session_settings, "show_controller_ui", text="Controller UI")
        col.prop(session_settings, "show_object_extras", text="Object Extras")

        col = col.row(align=True, heading=" ")
        col.scale_x = 2.0
        col.popover(
            panel="VIEW3D_PT_vr_session_view_object_type_visibility",
            icon_value=session_settings.icon_from_show_object_viewport,
            text="",
        )

        col = layout.column(align=True)
        col.prop(session_settings, "controller_draw_style", text="Controller Style")

        col = layout.column(align=True)
        col.prop(session_settings, "clip_start", text="Clip Start")
        col.prop(session_settings, "clip_end", text="End", text_ctxt=i18n_contexts.id_camera)

        col = layout.column(align=True)
        col.prop(session_settings, "view_scale", text="View Scale")

        col = layout.column(align=True)
        col.prop(session_settings, "fly_speed", text="Fly Speed")


class VIEW3D_PT_vr_session_view_object_type_visibility(VIEW3D_PT_object_type_visibility):
    def draw(self, context):
        session_settings = context.window_manager.xr_session_settings
        self.draw_ex(context, session_settings, False)  # Pass session settings instead of 3D view.


# Location Scouting.
class VIEW3D_UL_vr_captures(UIList):
    def draw_item(self, context, layout, _data, item, icon, _active_data, _active_propname, index):
        capture = item

        layout.emboss = 'NONE'

        layout.label(icon='OUTLINER_OB_CAMERA')
        layout.prop(capture, "name", text="")


class VIEW3D_PT_vr_location_scouting(VRButtonsPanel, Panel):
    bl_label = "Location Scouting"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        pass


class VIEW3D_PT_vr_location_scouting_captures(VRButtonsPanel, Panel):
    bl_label = "Captured Shots"
    bl_parent_id = "VIEW3D_PT_vr_location_scouting"

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        row = layout.row()
        row.template_list("VIEW3D_UL_vr_captures", "", scene, "vr_captures", scene, "vr_captures_selected", rows=5)

        col = row.column(align=True)

        col.operator("view3d.vr_location_scouting_remove_capture", icon='REMOVE', text="")

        col.separator()

        col.operator("view3d.vr_location_scouting_add_camera_from_capture", icon='OUTLINER_OB_CAMERA', text="")
        col.operator("view3d.vr_location_scouting_add_marker_from_capture", icon='MARKER', text="")

        col.separator()

        browse_up = col.operator("view3d.vr_location_scouting_browse_captures", icon='TRIA_UP', text="")
        if browse_up is not None:
            browse_up.backward = True
        browse_down = col.operator("view3d.vr_location_scouting_browse_captures", icon='TRIA_DOWN', text="")
        if browse_down is not None:
            browse_down.backward = False

        is_reviewing = context.window_manager.vr_capture_review_running
        capture_review_text = "Review VR Captures" if not is_reviewing else "Exit Review"
        capture_review_icon = 'HIDE_OFF' if not is_reviewing else 'CANCEL'
        layout.operator(
            "view3d.vr_location_scouting_capture_review",
            text=capture_review_text,
            icon=capture_review_icon,
            depress=is_reviewing)


class VIEW3D_PT_vr_location_scouting_viewfinder(VRButtonsPanel, Panel):
    bl_label = "VR Viewfinder"
    bl_parent_id = "VIEW3D_PT_vr_location_scouting"

    def draw_header(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.prop(session_settings, "viewfinder_enabled", text="")

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.enabled = session_settings.viewfinder_enabled
        session_settings = context.window_manager.xr_session_settings

        layout.use_property_split = True

        layout.prop(session_settings, "viewfinder_hand", text="Hand", expand=True)
        layout.prop(session_settings, "viewfinder_scale", text="Scale")

        col = layout.column(align=True, heading="Display")
        col.prop(session_settings, "viewfinder_crosshair_enabled", text="Crosshair")


class VIEW3D_PT_vr_location_scouting_viewfinder_passepartout(VRButtonsPanel, Panel):
    bl_label = "Passepartout"
    bl_parent_id = "VIEW3D_PT_vr_location_scouting_viewfinder"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.enabled = session_settings.viewfinder_enabled
        session_settings = context.window_manager.xr_session_settings

        layout.use_property_split = True

        layout.prop(session_settings, "viewfinder_passepartout_overscan", text="Overscan")
        layout.prop(session_settings, "viewfinder_passepartout_opacity", text="Opacity")


# Landmarks.
class VIEW3D_MT_vr_landmark_menu(Menu):
    bl_label = "Landmark Controls"

    def draw(self, _context):
        layout = self.layout

        layout.operator("view3d.vr_camera_landmark_from_session")
        layout.operator("view3d.vr_landmark_from_camera")
        layout.operator("view3d.update_vr_landmark")
        layout.separator()
        layout.operator("view3d.cursor_to_vr_landmark")
        layout.operator("view3d.camera_to_vr_landmark")
        layout.operator("view3d.add_camera_from_vr_landmark")


class VIEW3D_UL_vr_landmarks(UIList):
    def draw_item(self, context, layout, _data, item, icon, _active_data,
                  _active_propname, index):
        landmark = item
        landmark_active_idx = context.scene.vr_landmarks_active

        layout.emboss = 'NONE'

        layout.prop(landmark, "name", text="")

        icon = (
            'RADIOBUT_ON' if (index == landmark_active_idx) else 'RADIOBUT_OFF'
        )
        props = layout.operator(
            "view3d.vr_landmark_activate", text="", icon=icon)
        props.index = index


class VIEW3D_PT_vr_landmarks(VRButtonsPanel, Panel):
    bl_label = "Landmarks"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        landmark_selected = properties.VRLandmark.get_selected_landmark(context)

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        row = layout.row()

        row.template_list("VIEW3D_UL_vr_landmarks", "", scene, "vr_landmarks",
                          scene, "vr_landmarks_selected", rows=3)

        col = row.column(align=True)
        col.operator("view3d.vr_landmark_add", icon='ADD', text="")
        col.operator("view3d.vr_landmark_remove", icon='REMOVE', text="")
        col.operator("view3d.vr_landmark_from_session", icon='PLUS', text="")

        col.menu("VIEW3D_MT_vr_landmark_menu", icon='DOWNARROW_HLT', text="")

        if landmark_selected:
            layout.prop(landmark_selected, "type")

            if landmark_selected.type == 'OBJECT':
                layout.prop(landmark_selected, "base_pose_object")
                layout.prop(landmark_selected, "base_scale", text="Scale")
            elif landmark_selected.type == 'CUSTOM':
                layout.prop(landmark_selected,
                            "base_pose_location", text="Location")
                layout.prop(landmark_selected,
                            "base_pose_angle", text="Angle")
                layout.prop(landmark_selected,
                            "base_scale", text="Scale")


# Actions.
class VIEW3D_PT_vr_actionmaps(VRButtonsPanel, Panel):
    bl_label = "Action Maps"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        col = layout.column(align=True)
        col.prop(scene, "vr_actions_use_gamepad", text="Gamepad")

        col = layout.column(align=True, heading="Extensions")
        col.prop(scene, "vr_actions_enable_reverb_g2", text="HP Reverb G2")
        col.prop(scene, "vr_actions_enable_vive_cosmos", text="HTC Vive Cosmos")
        col.prop(scene, "vr_actions_enable_vive_focus", text="HTC Vive Focus")
        col.prop(scene, "vr_actions_enable_huawei", text="Huawei")


# Viewport feedback.
class VIEW3D_PT_vr_viewport_feedback(VRButtonsPanel, Panel):
    bl_label = "Viewport Feedback"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        view3d = context.space_data

        col = layout.column(align=True)
        col.label(icon='STATUS_WARNING', text="Note:")
        col.label(text="Settings here may have a significant")
        col.label(text="performance impact!")

        layout.separator()

        layout.prop(view3d.shading, "vr_show_virtual_camera")
        layout.prop(view3d.shading, "vr_show_controllers")
        layout.prop(view3d.shading, "vr_show_landmarks")
        layout.prop(view3d.shading, "vr_show_captures")
        layout.prop(view3d, "mirror_xr_session")


# Info.
class VIEW3D_PT_vr_info(VRButtonsPanel, Panel):
    bl_label = "VR Info"

    @classmethod
    def poll(cls, context):
        return not bpy.app.build_options.xr_openxr

    def draw(self, context):
        import platform
        layout = self.layout
        missing_support_string = n_("Built without VR/OpenXR features")
        layout.label(icon='STATUS_ERROR', text=missing_support_string)


classes = (
    VIEW3D_PT_vr_session,
    VIEW3D_PT_vr_session_view,
    VIEW3D_PT_vr_session_view_object_type_visibility,
    VIEW3D_PT_vr_location_scouting,
    VIEW3D_PT_vr_location_scouting_captures,
    VIEW3D_PT_vr_location_scouting_viewfinder,
    VIEW3D_PT_vr_location_scouting_viewfinder_passepartout,
    VIEW3D_PT_vr_landmarks,
    VIEW3D_PT_vr_actionmaps,
    VIEW3D_PT_vr_viewport_feedback,

    VIEW3D_UL_vr_landmarks,
    VIEW3D_UL_vr_captures,
    VIEW3D_MT_vr_landmark_menu,
)


def register():
    global world_space_source_classes
    global world_space_classes
    world_space_source_classes = build_world_space_classes()
    xr_ui.register()
    for cls in classes:
        bpy.utils.register_class(cls)
    world_space_classes = tuple(
        world_space_cls
        for source_panel_cls in world_space_source_classes
        for world_space_cls in xr_ui.register_default_world_space_panel(
            xr_ui.panel_idname(source_panel_cls),
            default_mount_point=XR_DEFAULT_MOUNT_POINT,
            mount_overrides=XR_MOUNT_POINT_OVERRIDES,
        )
    )
    xr_ui.schedule_xr_world_space_ui_refresh()

    # View3DShading is the only per 3D-View struct with custom property
    # support, so "abusing" that to get a per 3D-View option.
    bpy.types.View3DShading.vr_show_virtual_camera = bpy.props.BoolProperty(
        name="Show VR Camera",
        default=False
    )
    bpy.types.View3DShading.vr_show_controllers = bpy.props.BoolProperty(
        name="Show VR Controllers",
        default=False
    )
    bpy.types.View3DShading.vr_show_landmarks = bpy.props.BoolProperty(
        name="Show Landmarks",
        default=False
    )
    bpy.types.View3DShading.vr_show_captures = bpy.props.BoolProperty(
        name="Show Location Scouting Captures",
        default=True
    )


def unregister():
    global world_space_source_classes
    global world_space_classes
    xr_ui.unregister()
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
    world_space_source_classes = ()
    world_space_classes = ()

    del bpy.types.View3DShading.vr_show_virtual_camera
    del bpy.types.View3DShading.vr_show_controllers
    del bpy.types.View3DShading.vr_show_landmarks
    del bpy.types.View3DShading.vr_show_captures
