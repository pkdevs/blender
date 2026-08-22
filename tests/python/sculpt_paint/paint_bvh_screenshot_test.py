#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import sys

BASE_DIR = os.path.dirname(os.path.realpath(__file__))


def screenshot():
    sys.path.insert(0, BASE_DIR)
    from modules.test_helpers import set_view3d_context_override
    import bpy

    output_path = sys.argv[-1]

    # Force redraw and take screenshot.
    bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)

    context_override = bpy.context.copy()
    set_view3d_context_override(context_override)
    with bpy.context.temp_override(**context_override):
        bpy.ops.screen.screenshot_area(filepath=output_path)

    bpy.ops.wm.quit_blender()


# When run from inside Blender, take screenshot and exit.
try:
    import bpy
    inside_blender = True
except ImportError:
    inside_blender = False

if inside_blender:
    bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
    # Run on a delay to allow EEVEE shaders to compile
    bpy.app.timers.register(screenshot, first_interval=2.0)


def get_arguments(filepath, output_filepath):
    return [
        "--no-window-focus",
        "--window-geometry",
        "0", "0", "1024", "768",
        "-noaudio",
        "--factory-startup",
        "--enable-autoexec",
        "--debug-memory",
        "--console-crash-handler",
        "--debug-exit-on-error",
        filepath,
        "-P",
        os.path.realpath(__file__),
        "--",
        output_filepath + '0001.png']


def create_argparse():
    parser = argparse.ArgumentParser(
        description="Run test script for each blend file in TESTDIR, comparing the render result with known output."
    )
    parser.add_argument("--blender", required=True)
    parser.add_argument("--testdir", required=True)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--oiiotool", required=True)
    parser.add_argument('--batch', default=False, action='store_true')
    return parser


def main():
    sys.path.insert(0, os.path.join(BASE_DIR, ".."))

    parser = create_argparse()
    args = parser.parse_args()

    from modules import render_report
    report = render_report.Report("Screenshot", args.outdir, args.oiiotool)
    ok = report.run(args.testdir, args.blender, get_arguments)

    sys.exit(not ok)


if __name__ == "__main__" and not inside_blender:
    main()
