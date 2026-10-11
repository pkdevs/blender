# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The native interface's Objective-C sources, for the tests that read them."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GHOST = ROOT / "intern/ghost/intern"
INTERFACE = ROOT / "source/blender/windowmanager/apple_ios"


def native_window_source() -> str:
    """The window, its input and the native interface's view controller, split over several files."""
    files = [GHOST / "GHOST_IOSWindowInput.hh", INTERFACE / "wm_apple_ios_view_controller.hh",
             GHOST / "GHOST_WindowIOS.mm", INTERFACE / "wm_apple_ios_interface.mm"]
    files += sorted(INTERFACE.glob("wm_apple_ios_view_controller*.mm"))
    return "\n".join(path.read_text() for path in files)
