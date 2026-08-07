# SPDX-FileCopyrightText: 2013-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ./blender.bin -b -X -P tests/python/bl_keymap_completeness.py
#

# simple script to test 'bl_keymap_utils.keymap_hierarchy' contains correct values.

# Needed for 'bl_keymap_utils.keymap_hierarchy' which inspects tools.
import os
import sys

sys.path.append(os.path.join(os.path.dirname(__file__), os.pardir, os.pardir, "scripts", "startup"))

del sys, os

from bl_keymap_utils import keymap_hierarchy

# Suffix for the key-map used while the tool acts as the fallback tool, see `_fallback_id`.
KEYMAP_SUFFIX_FALLBACK = " (fallback)"


def keymaps_missing(maps_py, maps_bl):
    return [
        km_id for km_id in sorted(maps_py - maps_bl)
        if not km_id[0].endswith(KEYMAP_SUFFIX_FALLBACK)
    ]


def keymaps_missing_fallback(maps_py, maps_bl):
    # `keymap_ui_hierarchy` yields a fallback for every tool keymap although only the select
    # tools have them. So the fallback may not exist, the keymap it falls back from must.
    return [
        km_id for km_id in sorted(maps_py - maps_bl)
        if km_id[0].endswith(KEYMAP_SUFFIX_FALLBACK)
        if (km_id[0].removesuffix(KEYMAP_SUFFIX_FALLBACK), *km_id[1:]) not in maps_bl
    ]


def check_maps():
    # A key-map is identified by its name, space-type & region-type, all three must match
    # for the key-map to be found, see `rna_keymap_ui.draw_entry`.
    maps_py = set()

    def fill_maps(seq):
        for km_name, km_space_type, km_region_type, km_sub in seq:
            maps_py.add((km_name, km_space_type, km_region_type))
            fill_maps(km_sub)

    fill_maps(keymap_hierarchy.generate())

    import bpy
    keyconf = bpy.context.window_manager.keyconfigs.active
    maps_bl = {(km.name, km.space_type, km.region_type) for km in keyconf.keymaps}

    err = False

    # Check the hierarchy only references keymaps that exist in blender.
    test = keymaps_missing(maps_py, maps_bl)
    test_fallback = keymaps_missing_fallback(maps_py, maps_bl)

    if test:
        print("Keymaps that are in 'bl_keymap_utils.keymap_hierarchy' but not blender")
        for km_id in test:
            print("    ('{:s}', '{:s}', '{:s}', []),".format(*km_id))
        # TODO #65963, broken keymap hierarchy tests disabled until fixed.
        # err = True

    if test_fallback:
        print("Fallback keymaps in 'bl_keymap_utils.keymap_hierarchy' with no keymap to fall back from")
        for km_id in test_fallback:
            print("    ('{:s}', '{:s}', '{:s}', []),".format(*km_id))
        # TODO #65963, broken keymap hierarchy tests disabled until fixed.
        # err = True

    # Check blender's keymaps can all be reached from the hierarchy.
    test = sorted(maps_bl - maps_py)
    if test:
        print("Keymaps that are in blender but not in 'bl_keymap_utils.keymap_hierarchy'")
        for km_id in test:
            print("    ('{:s}', '{:s}', '{:s}', []),".format(*km_id))
            # Listing the name with the wrong types is a common cause, report it as it's easy to miss.
            km_args_other = sorted(km_id_other[1:] for km_id_other in maps_py if km_id_other[0] == km_id[0])
            if km_args_other:
                km_args_text = ", ".join(["('{:s}', '{:s}')".format(*args) for args in km_args_other])
                print("        listed with: {:s}".format(km_args_text))
        # TODO #65963, broken keymap hierarchy tests disabled until fixed.
        # err = True

    return err


def main():
    import bpy

    # Not loaded in background mode.
    if bpy.app.background:
        bpy.utils.keyconfig_init()

    err = check_maps()

    if err and bpy.app.background:
        # alert CTest we failed
        import sys
        sys.exit(1)


if __name__ == "__main__":
    main()
