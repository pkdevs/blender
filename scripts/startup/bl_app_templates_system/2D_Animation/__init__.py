# SPDX-FileCopyrightText: 2020-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Initialization script for 2D Animation template

import bpy
from bpy.app.handlers import persistent

SIDEBAR_TAB_RETRY_INTERVAL = 0.05
SIDEBAR_TAB_RETRY_COUNT = 100

def make_sidebar_tool_tab_timer(screen_name):
    remaining = SIDEBAR_TAB_RETRY_COUNT

    def timer():
        nonlocal remaining
        screen = bpy.data.screens.get(screen_name)
        if screen is None:
            return None
        for area in screen.areas:
            if area.type != 'VIEW_3D':
                continue
            for region in area.regions:
                if region.type != 'UI':
                    continue
                try:
                    region.active_panel_category = 'Tool'
                except (AttributeError, TypeError):
                    # Region has not drawn its panels yet, retry shortly.
                    remaining -= 1
                    return SIDEBAR_TAB_RETRY_INTERVAL if remaining > 0 else None
        return None

    return timer


def update_factory_startup_screens():
    # 2D Animation.
    screen = bpy.data.screens["2D Animation"]
    for area in screen.areas:
        if area.type == 'PROPERTIES':
            # Set Tool settings as default in properties panel.
            space = area.spaces.active
            space.context = 'TOOL'
        elif area.type == 'DOPESHEET_EDITOR':
            # Open sidebar in Dope-sheet.
            space = area.spaces.active
            space.show_region_ui = True
        elif area.type == 'VIEW_3D':
            # Open sidebar in the 3D viewport.
            space = area.spaces.active
            space.show_region_ui = True

    bpy.app.timers.register(
        make_sidebar_tool_tab_timer("2D Animation"),
        first_interval=SIDEBAR_TAB_RETRY_INTERVAL,
    )

    # 2D Full Canvas.
    screen = bpy.data.screens["2D Full Canvas"]
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            space = area.spaces.active
            space.shading.type = 'MATERIAL'
            space.shading.use_scene_world = True


def update_factory_startup_scenes():
    for scene in bpy.data.scenes:
        scene.tool_settings.use_keyframe_insert_auto = True
        scene.tool_settings.gpencil_sculpt.use_scale_thickness = True


def update_factory_startup_grease_pencils():
    for grease_pencil in bpy.data.grease_pencils:
        grease_pencil.onion_keyframe_type = 'ALL'


@persistent
def load_handler(_):
    update_factory_startup_screens()
    update_factory_startup_scenes()
    update_factory_startup_grease_pencils()


def register():
    bpy.app.handlers.load_factory_startup_post.append(load_handler)


def unregister():
    bpy.app.handlers.load_factory_startup_post.remove(load_handler)
