# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Blender preferences shown in the native iPad Settings."""

import json

import bpy

__all__ = (
    "preferences_json",
    "keymap_set",
)

# Keymap presets offered on the iPad, by preset file name.
KEYMAPS = ("Industry_Compatible", "Blender")


def preferences_json(context):
    return json.dumps({
        "keymap": context.window_manager.keyconfigs.active.name,
    })


def keymap_set(context, name):
    if name not in KEYMAPS:
        return
    if context.window_manager.keyconfigs.active.name == name:
        return
    filepath = bpy.utils.preset_find(name, "keyconfig")
    if not filepath or not bpy.utils.keyconfig_set(filepath):
        return
    # The iPad may end the app without warning: keep the choice now rather than on quit.
    if context.preferences.use_preferences_save:
        bpy.ops.wm.save_userpref()


classes = ()
