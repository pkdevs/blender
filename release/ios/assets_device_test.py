# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Drop every Starter asset into a factory-startup scene on an iPad, through the asset pane's path.

Copy into the app's Documents directory and launch with `--factory-startup --python-expr`:

    from pathlib import Path
    exec(compile((Path.home() / 'Documents/assets_device_test.py').read_text(),
                 'assets_device_test.py', 'exec'))

Each asset goes through `WM_apple_ios_ui_asset_drag`, the entry point the native pane uses: a tap
adds it (objects at the 3D cursor, materials and modifiers onto the active object), and one asset
is also dragged and dropped at a viewport point, and one drag is cancelled. Blender's own drag and
drop does the rest. Results are written to `Documents/assets-device-test.json`.
"""

import ctypes
import json
import os
from pathlib import Path
import time
import traceback

import bpy
from bpy_extras import view3d_utils

ASSET_DRAG_BEGIN, ASSET_DRAG_MOVE, ASSET_DRAG_DROP, ASSET_DRAG_CANCEL, ASSET_ADD = range(5)
LIBRARY = "Starter"
CUSTOM_LIBRARY = 100

engine = ctypes.CDLL(None)
drag = engine.WM_apple_ios_ui_asset_drag
drag.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
setting = engine.WM_apple_ios_ui_setting_enqueue
setting.argtypes = [ctypes.c_char_p, ctypes.c_char_p]

output = Path.home() / "Documents/assets-device-test.json"
report = {"checks": [], "failures": [], "passed": False}


def check(ok, title):
    report["checks"].append({"ok": bool(ok), "title": title})
    if not ok:
        report["failures"].append(title)


def library():
    for item in bpy.context.preferences.filepaths.asset_libraries:
        if item.name == LIBRARY:
            return item
    return None


def assets_in(path):
    names = {}
    with bpy.data.libraries.load(path, assets_only=True) as (source, _):
        for kind in ("objects", "materials", "node_groups", "collections"):
            names[kind] = list(getattr(source, kind))
    return names


def key(filename, id_type, name):
    return f"{CUSTOM_LIBRARY}:{LIBRARY}:{filename}/{id_type}/{name}".encode()


def viewport():
    for area in bpy.context.window.screen.areas:
        if area.type == "VIEW_3D":
            for region in area.regions:
                if region.type == "WINDOW":
                    return area, region
    return None, None


class Calibrate(bpy.types.Operator):
    """Record where a synthetic pointer move lands, to convert to GHOST client coordinates."""
    bl_idname = "wm.starter_assets_calibrate"
    bl_label = "Calibrate"
    seen = []

    def invoke(self, context, event):
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

    def modal(self, context, event):
        if event.type == "MOUSEMOVE":
            Calibrate.seen.append((event.mouse_x, event.mouse_y))
            return {"FINISHED"}
        return {"PASS_THROUGH"}


def to_client(window, scale, x, y):
    """Window pixels to GHOST client coordinates (the inverse of Blender's conversion)."""
    return int(round(x / scale)), int(round((window.height - 1) - y / scale))


def new_ids(collection, before):
    return [id_data for id_data in collection if id_data not in before]


def tests():
    started = time.time()
    starter = library()
    check(starter is not None, "The Starter asset library is registered at launch")
    if starter is None:
        return
    check(os.path.isdir(starter.path), f"Starter library path exists: {starter.path}")
    report["import_method"] = starter.import_method
    files = sorted(f for f in os.listdir(starter.path) if f.endswith(".blend"))
    check(len(files) >= 4, f"Starter library has its .blend files: {files}")
    check(os.path.exists(os.path.join(starter.path, "blender_assets.cats.txt")),
          "Starter library has its catalog file")

    setting(b"assets", b"show")
    yield 2.0

    scene = bpy.context.scene
    cube = bpy.data.objects.get("Cube")
    check(cube is not None, "Factory startup has the default cube to drop onto")
    window = bpy.context.window
    area, region = viewport()
    check(region is not None, "A 3D viewport is open")

    contents = {name: assets_in(os.path.join(starter.path, name)) for name in files}

    def select_cube():
        for obj in scene.objects:
            obj.select_set(False)
        cube.select_set(True)
        bpy.context.view_layer.objects.active = cube

    # Asset lists load in the background once the pane asks for them; wait until one drops.
    first = next((f, n) for f, names in contents.items() for n in names["objects"])
    loaded = False
    for _ in range(60):
        before = set(bpy.data.objects)
        drag(ASSET_ADD, key(first[0], "Object", first[1]), 0, 0)
        yield 0.5
        added = new_ids(bpy.data.objects, before)
        for obj in added:
            bpy.data.objects.remove(obj)
        if added:
            loaded = True
            break
    report["library_ready_seconds"] = round(time.time() - started, 1)
    check(loaded, "Starter assets become droppable once the asset libraries load")

    # Objects: tap to add at the 3D cursor.
    for filename, names in contents.items():
        for name in names["objects"]:
            before = set(bpy.data.objects)
            drag(ASSET_ADD, key(filename, "Object", name), 0, 0)
            yield 0.5
            added = [obj for obj in new_ids(bpy.data.objects, before) if obj.name.startswith(name)]
            obj = added[0] if added else None
            ok = obj is not None and obj.name in scene.objects
            if ok:
                evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
                polygons = len(evaluated.data.polygons) if evaluated.type == "MESH" else 1
                ok = polygons > 0 and all(m.node_group for m in obj.modifiers if m.type == "NODES")
            check(ok, f"Object '{name}' drops into the scene and evaluates")
            for obj in new_ids(bpy.data.objects, before):
                bpy.data.objects.remove(obj)

    # Materials: tap to apply to the active object.
    for filename, names in contents.items():
        for name in names["materials"]:
            select_cube()
            drag(ASSET_ADD, key(filename, "Material", name), 0, 0)
            yield 0.5
            materials = [slot.material.name for slot in cube.material_slots if slot.material]
            check(any(m.startswith(name) for m in materials),
                  f"Material '{name}' drops onto the active object ({materials})")

    # Dropping a material again appends it again but reuses its packed images.
    scanned = next(((f, n) for f, names in contents.items() for n in names["materials"]
                    if n == "Red Brick Wall"), None)
    if scanned:
        select_cube()
        images = len(bpy.data.images)
        drag(ASSET_ADD, key(scanned[0], "Material", scanned[1]), 0, 0)
        yield 0.5
        check(len(bpy.data.images) == images,
              "Dropping a material twice reuses its images instead of loading them again")

    # Modifiers: tap to add to the active object.
    for filename, names in contents.items():
        for name in names["node_groups"]:
            select_cube()
            before = set(cube.modifiers)
            drag(ASSET_ADD, key(filename, "NodeTree", name), 0, 0)
            yield 0.5
            added = [m for m in cube.modifiers if m not in before and m.type == "NODES"]
            check(added and added[0].node_group and added[0].node_group.name.startswith(name),
                  f"Modifier '{name}' drops onto the active object")
            for modifier in added:
                cube.modifiers.remove(modifier)

    # Scenes: collections are appended into the scene with their camera and lights.
    for filename, names in contents.items():
        for name in names["collections"]:
            before = set(bpy.data.collections)
            before_objects = set(bpy.data.objects)
            drag(ASSET_ADD, key(filename, "Collection", name), 0, 0)
            yield 0.8
            added = [c for c in new_ids(bpy.data.collections, before) if c.name.startswith(name)]
            check(added, f"Scene '{name}' drops into the scene")
            for obj in new_ids(bpy.data.objects, before_objects):
                bpy.data.objects.remove(obj)
            for collection in new_ids(bpy.data.collections, before):
                bpy.data.collections.remove(collection)

    # A real drag: begin, move across the viewport, drop. First measure the client scale.
    bpy.utils.register_class(Calibrate)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.starter_assets_calibrate("INVOKE_DEFAULT")
    filename = next(f for f, names in contents.items() if "Rounded Box" in names["objects"])
    box = key(filename, "Object", "Rounded Box")
    drag(ASSET_DRAG_BEGIN, box, 100, 100)
    yield 0.5
    drag(ASSET_DRAG_CANCEL, None, 0, 0)
    yield 0.5
    check(Calibrate.seen, "A synthetic drag move reaches Blender's event handlers")
    if Calibrate.seen:
        scale = Calibrate.seen[0][0] / 100.0
        report["client_scale"] = scale
        center_x = region.x + region.width * 0.5
        center_y = region.y + region.height * 0.5
        before = set(bpy.data.objects)
        x, y = to_client(window, scale, center_x - 200, center_y)
        drag(ASSET_DRAG_BEGIN, box, x, y)
        yield 0.3
        for step in range(1, 5):
            x, y = to_client(window, scale, center_x - 200 + 50 * step, center_y)
            drag(ASSET_DRAG_MOVE, None, x, y)
            yield 0.1
        drag(ASSET_DRAG_DROP, None, x, y)
        yield 0.8
        added = [obj for obj in new_ids(bpy.data.objects, before) if obj.name.startswith("Rounded Box")]
        check(added, "Dragging an asset into the viewport and releasing drops it")
        if added:
            # The drop lands where the finger lifted: project the object back to the screen.
            point = view3d_utils.location_3d_to_region_2d(region, area.spaces.active.region_3d,
                                                         added[0].location)
            if point is not None:
                report["drop_screen_offset"] = [point.x - region.width * 0.5, point.y - region.height * 0.5]
                check(abs(point.x - region.width * 0.5) < 40 and abs(point.y - region.height * 0.5) < 40,
                      f"The dropped asset lands under the release point ({point.x:.0f}, {point.y:.0f})")
        for obj in new_ids(bpy.data.objects, before):
            bpy.data.objects.remove(obj)

        before = set(bpy.data.objects)
        drag(ASSET_DRAG_BEGIN, box, *to_client(window, scale, center_x, center_y))
        yield 0.3
        drag(ASSET_DRAG_CANCEL, None, 0, 0)
        yield 0.5
        check(not new_ids(bpy.data.objects, before), "A cancelled drag adds nothing")
    bpy.utils.unregister_class(Calibrate)

    setting(b"assets", b"hide")
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
