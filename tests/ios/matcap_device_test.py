# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Switch through every matcap and studio light with the native Shading panel open, on an iPad.

Copy into the app's Documents directory and launch with `--factory-startup --python-expr`:

    from pathlib import Path
    exec(compile((Path.home() / 'Documents/matcap_device_test.py').read_text(),
                 'matcap_device_test.py', 'exec'))

The panel lists matcaps and studio lights with previews, which Blender renders in background
jobs while the viewport loads the same lights. Each light is picked in turn, slowly and then
quickly, and the panel must keep publishing previews for all of them. Reloading the lights
renders every preview again, to catch races between the panel and the preview jobs. Results are written to
`Documents/matcap-device-test.json`; a crash leaves the file unfinished.
"""

import ctypes
import json
from pathlib import Path
import time
import traceback

import bpy

engine = ctypes.CDLL(None)
setting = engine.WM_apple_ios_ui_setting_enqueue
setting.argtypes = [ctypes.c_char_p, ctypes.c_char_p]

output = Path.home() / "Documents/matcap-device-test.json"
report = {"checks": [], "failures": [], "passed": False, "progress": ""}


def check(ok, title):
    report["checks"].append({"ok": bool(ok), "title": title})
    if not ok:
        report["failures"].append(title)


def preview_files():
    """Previews the native panels have written for the menus they show."""
    folder = Path(bpy.app.tempdir)
    return [path for path in folder.glob("native_preview_*.png") if path.stat().st_size > 0]


def lights(kind):
    return [light for light in bpy.context.preferences.studio_lights if light.type == kind]


def tests():
    started = time.time()
    space = next((area.spaces.active for area in bpy.context.screen.areas
                  if area.type == "VIEW_3D"), None)
    check(space is not None, "A 3D viewport is open")
    if space is None:
        return
    shading = space.shading
    setting(b"native_shading_visible", b"1")
    shading.type = "SOLID"
    shading.light = "MATCAP"
    yield 0.5

    matcaps = lights("MATCAP")
    check(len(matcaps) > 1, f"There are matcaps to switch between ({len(matcaps)})")

    # Pick each matcap in turn while the panel renders their previews.
    for delay in (0.25, 0.03):
        for light in matcaps:
            report["progress"] = f"matcap {light.name} every {delay} s"
            shading.studio_light = light.name
            yield delay
    check(True, f"Switching through {len(matcaps)} matcaps, slowly and quickly, doesn't crash")

    # The panel ends up with a preview for every matcap, from Blender's own preview jobs.
    for _ in range(100):
        if len(preview_files()) >= len(matcaps):
            break
        yield 0.1
    report["previews"] = len(preview_files())
    check(len(preview_files()) >= len(matcaps),
          f"The panel shows a preview for every matcap ({len(preview_files())} of {len(matcaps)})")

    shading.light = "STUDIO"
    for light in lights("STUDIO"):
        report["progress"] = f"studio light {light.name}"
        shading.studio_light = light.name
        yield 0.1
    check(True, "Switching through the studio lights doesn't crash")

    shading.type = "MATERIAL"
    for light in lights("WORLD"):
        report["progress"] = f"world {light.name}"
        shading.studio_light = light.name
        yield 0.2
    check(True, "Switching through the HDRIs in Material Preview doesn't crash")

    shading.type = "SOLID"
    shading.light = "MATCAP"
    for light in matcaps:
        report["progress"] = f"back to matcap {light.name}"
        shading.studio_light = light.name
        yield 0.03
    check(True, "Switching back to matcaps after other lights doesn't crash")

    # Reloading the studio lights gives them new icons, so every preview renders again while
    # Blender's own preview jobs render them too: the two used to free each other's images.
    for attempt in range(40):
        report["progress"] = f"reload {attempt}"
        names = [light.name for light in lights("MATCAP")]
        bpy.context.preferences.studio_lights.refresh()
        shading.studio_light = names[attempt % len(names)]
        # Long enough for the panel to list the reloaded lights, which it does every 250 ms.
        yield 0.4
    report["previews_after_reloads"] = len(preview_files())
    check(len(preview_files()) > report["previews"],
          "Reloaded lights get new previews in the panel")
    check(True, "Reloading the studio lights with the panel open doesn't crash")

    setting(b"native_shading_visible", b"0")
    report["progress"] = "done"
    report["seconds"] = round(time.time() - started, 1)
    report["passed"] = not report["failures"]


steps = tests()


def tick():
    try:
        delay = next(steps)
    except StopIteration:
        delay = None
    except Exception:
        report["error"] = traceback.format_exc()
        delay = None
    output.write_text(json.dumps(report, indent=2))
    return delay


bpy.app.timers.register(tick, first_interval=5.0)
