# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the SwiftUI Properties, Outliner, shading and tool settings panels in a simulator.

Builds `swiftui_panels_harness` with the production Swift sources, feeds it snapshots shaped
like `interface_native_properties.cc` and `native_panels.py` produce, and screenshots each
scenario. The harness answers edits as Blender does, so a scenario can also check round trips.

Screenshots are written to `IOS_TEST_ARTIFACT_DIR` (default: a temporary directory). Set
`IOS_TEST_ICON_CATALOG` to an iOS build's `BlenderIcons.xcassets` to show Blender's icons.
"""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SWIFT_SOURCES = sorted((ROOT / "intern/ghost/intern/swift").glob("*.swift"))
BRIDGE = ROOT / "release/ios/swiftui_panels_harness/HarnessBridge.h"
# The UIKit panels, built in for comparison (`-uikit YES`).
UIKIT_SOURCES = [ROOT / "intern/ghost/intern/GHOST_IOSScenePanels.mm",
                 ROOT / "intern/ghost/intern/GHOST_IOSNumericInput.mm",
                 ROOT / "intern/ghost/intern/GHOST_IOSRedoStrip.mm"]
HARNESS = ROOT / "release/ios/swiftui_panels_harness/Harness.swift"
BUNDLE_ID = "org.blender.swiftui-panels-harness"


# --------------------------------------------------------------------
# Fixtures, as Blender projects its panels.

def prop(key, label, kind, value, **extra):
    field = {"key": key, "label": label, "type": kind, "value": value, "editable": 1,
             "panel": extra.pop("panel", "PANEL"), "tab": extra.pop("tab", "OBJECT"), "native": 1}
    field.update(extra)
    return field


def vector(key, group, values, panel, labels="XYZ", lock=True, **extra):
    fields = []
    for index, (label, value) in enumerate(zip(labels, values)):
        field = prop(f"{key}.{index}", label, "FLOAT", value, group=group, group_key=key,
                     panel=panel, index=index, property=key.split(".")[-1], owner="object",
                     dragStep=0.01, min=-1e9, max=1e9, **extra)
        if lock:
            field["lock"] = prop(f"lock_{key}.{index}", "Lock", "BOOLEAN", 1 if index == 2 else 0,
                                 panel=panel)
        field["animation"] = [{"id": "INSERT", "title": "Insert Keyframe", "icon": "KEY_HLT"}]
        fields.append(field)
    return fields


def tab_list():
    tabs = [("TOOL", "Tool", "TOOL_SETTINGS"), ("RENDER", "Render", "SCENE"),
            ("OUTPUT", "Output", "OUTPUT"), ("VIEW_LAYER", "View Layer", "RENDERLAYERS"),
            ("SCENE", "Scene", "SCENE_DATA"), ("WORLD", "World", "WORLD"),
            ("COLLECTION", "Collection", "OUTLINER_COLLECTION"), ("OBJECT", "Object", "OBJECT_DATA"),
            ("MODIFIER", "Modifiers", "MODIFIER"), ("PARTICLES", "Particles", "PARTICLES"),
            ("PHYSICS", "Physics", "PHYSICS"), ("CONSTRAINT", "Constraints", "CONSTRAINT"),
            ("DATA", "Data", "MESH_DATA"), ("MATERIAL", "Material", "MATERIAL"),
            ("TEXTURE", "Texture", "TEXTURE")]
    return [{"id": i, "title": t, "icon": icon} for i, t, icon in tabs]


def object_tab():
    transform = "OBJECT_PT_transform"
    rotation = vector("rotation_euler", "Rotation", [0, 0, 0.785398], transform, angle=1)
    rotation[0]["group_menu"] = prop("rotation_mode", "Mode", "ENUM", "XYZ", panel=transform, options=[
        {"value": "QUATERNION", "label": "Quaternion (WXYZ)"}, {"value": "XYZ", "label": "XYZ Euler"},
        {"value": "XZY", "label": "XZY Euler"}, {"value": "AXIS_ANGLE", "label": "Axis Angle"}])
    location = vector("location", "Location", [0, 0, 1.25], transform)
    location[2]["anim_state"] = "KEYED"
    return {"name": "Cube", "sections": [
        {"id": transform, "title": "Transform", "open": 1, "properties":
            location + rotation + vector("scale", "Scale", [1, 1, 1], transform) + [
            prop("delta", "", "SUBPANEL", 0, panel=transform, section="Delta Transform",
                 subpanel="OBJECT_PT_delta_transform", open=0, depth=1)]},
        {"id": "OBJECT_PT_relations", "title": "Relations", "open": 0, "properties": []},
        {"id": "OBJECT_PT_collections", "title": "Collections", "open": 0, "properties": []},
        {"id": "OBJECT_PT_visibility", "title": "Visibility", "open": 1, "properties": [
            prop("hide_select", "Selectable", "BOOLEAN", 1, panel="OBJECT_PT_visibility"),
            prop("show_viewport", "Viewports", "BOOLEAN", 1, panel="OBJECT_PT_visibility",
                 section="Show In", subpanel="OBJECT_PT_visibility"),
            prop("show_render", "Renders", "BOOLEAN", 0, panel="OBJECT_PT_visibility",
                 section="Show In", subpanel="OBJECT_PT_visibility"),
            prop("display_type", "Display As", "ENUM", "TEXTURED", panel="OBJECT_PT_visibility",
                 options=[{"value": "BOUNDS", "label": "Bounds"}, {"value": "WIRE", "label": "Wire"},
                          {"value": "SOLID", "label": "Solid"},
                          {"value": "TEXTURED", "label": "Textured"}])]},
        {"id": "OBJECT_PT_viewport_display", "title": "Viewport Display", "open": 0, "properties": []},
    ]}


def shape_key_rows():
    names = ["Basis", "Smile", "Blink Left", "Blink Right", "Brow Up", "Jaw Open", "Cheek Puff",
             "Squint", "Frown", "Pucker"]
    rows = []
    for index, name in enumerate(names):
        controls = []
        if index:
            controls.append(prop(f"key_blocks[{index}].value", "Value", "FLOAT", round(0.1 * index, 2),
                                 panel="DATA_PT_shape_keys", tab="DATA", min=0, max=1, dragStep=0.01))
            # Blender draws the mute toggle inverted: checked while the key is not muted.
            muted = index == 4
            controls.append(prop(f"key_blocks[{index}].mute", "Mute", "BOOLEAN", 1 if muted else 0,
                                 panel="DATA_PT_shape_keys", tab="DATA",
                                 icon="CHECKBOX_DEHLT" if muted else "CHECKBOX_HLT"))
        rows.append({"index": index, "label": name, "icon": "SHAPEKEY_DATA",
                     "rename_key": f"key_blocks[{index}].name", "controls": controls})
    return rows


def data_tab():
    ops = lambda prefix, panel: {
        "add": {"key": f"{prefix}_add", "label": "Add", "icon": "ADD", "editable": 1},
        "remove": {"key": f"{prefix}_remove", "label": "Remove", "icon": "REMOVE", "editable": 1}}
    groups = [{"index": i, "label": n, "icon": "GROUP_VERTEX",
               "rename_key": f"vertex_groups[{i}].name",
               "controls": [prop(f"vertex_groups[{i}].lock_weight", "Lock", "BOOLEAN", 1 if i == 1 else 0,
                                 panel="DATA_PT_vertex_groups", tab="DATA",
                                 icon="LOCKED" if i == 1 else "UNLOCKED")]}
              for i, n in enumerate(["Head", "Jaw", "Eyes"])]
    vertex_groups = dict(prop("vertex_groups.active_index", "", "LIST", 0, panel="DATA_PT_vertex_groups",
                              tab="DATA", rows=groups, reorder=1, menu="MESH_MT_vertex_group_context_menu"),
                         **ops("object.vertex_group", "DATA_PT_vertex_groups"))
    shape_keys = dict(prop("shape_keys.active_index", "", "LIST", 2, panel="DATA_PT_shape_keys", tab="DATA",
                           rows=shape_key_rows(), reorder=1, menu="MESH_MT_shape_key_context_menu"),
                      **ops("object.shape_key", "DATA_PT_shape_keys"))
    uv_maps = dict(prop("uv_layers.active_index", "", "LIST", 0, panel="DATA_PT_uv_texture", tab="DATA",
                        rows=[{"index": 0, "label": "UVMap", "icon": "GROUP_UVS", "rename_key": "uv.0",
                               "controls": [prop("uv_layers[0].active_render", "Active Render", "BOOLEAN", 1,
                                                 panel="DATA_PT_uv_texture", tab="DATA",
                                                 icon="RESTRICT_RENDER_OFF")]}]),
                   **ops("mesh.uv_texture", "DATA_PT_uv_texture"))
    return {"name": "Cube", "sections": [
        {"id": "DATA_PT_context_mesh", "title": "", "open": 1, "header_hidden": 1, "properties": [
            prop("data", "", "ID", "Cube", panel="DATA_PT_context_mesh", tab="DATA", id_type="Mesh",
                 name="Cube", icon="MESH_DATA", users=1, can_new=1, can_unlink=0, fake_user=0,
                 options=[{"value": "Cube", "label": "Cube", "users": 1},
                          {"value": "Cube.001", "label": "Cube.001", "users": 0}])]},
        {"id": "DATA_PT_vertex_groups", "title": "Vertex Groups", "open": 1, "properties": [vertex_groups]},
        {"id": "DATA_PT_shape_keys", "title": "Shape Keys", "open": 1, "properties": [
            shape_keys,
            prop("relative_key", "Relative To", "STRING", "Basis", panel="DATA_PT_shape_keys", tab="DATA"),
            prop("shape_key_value", "Value", "FLOAT", 0.2, panel="DATA_PT_shape_keys", tab="DATA",
                 slider={"min": 0, "max": 1}, min=0, max=1)]},
        {"id": "DATA_PT_uv_texture", "title": "UV Maps", "open": 1, "properties": [uv_maps]},
        {"id": "DATA_PT_normals", "title": "Normals", "open": 0, "properties": []},
        {"id": "DATA_PT_texture_space", "title": "Texture Space", "open": 0, "properties": []},
    ]}


def modifier_tab():
    panel = "MOD_PT_subsurf:modifiers[\"Subdivision\"]"
    return {"name": "Cube", "sections": [
        {"id": "DATA_PT_modifiers", "title": "", "open": 1, "header_hidden": 1, "properties": [
            prop("add_modifier", "Add Modifier", "OPERATOR_MENU", "", panel="DATA_PT_modifiers",
                 tab="MODIFIER", icon="ADD", options=[
                     {"heading": "Edit"}, {"value": "DATA_TRANSFER", "label": "Data Transfer",
                                           "icon": "MOD_DATA_TRANSFER"},
                     {"heading": "Generate"}, {"value": "ARRAY", "label": "Array", "icon": "MOD_ARRAY"},
                     {"value": "BEVEL", "label": "Bevel", "icon": "MOD_BEVEL"},
                     {"value": "SUBSURF", "label": "Subdivision Surface", "icon": "MOD_SUBSURF"},
                     {"heading": "Deform"}, {"value": "CAST", "label": "Cast", "icon": "MOD_CAST"}])]},
        {"id": panel, "title": "Subdivision", "open": 1, "active": 1, "icon": "MOD_SUBSURF", "properties": [
            prop("show_viewport", "Realtime", "BOOLEAN", 1, panel=panel, tab="MODIFIER", header=1),
            prop("object.modifier_remove", "Remove", "OPERATOR", "", panel=panel, tab="MODIFIER",
                 header=1, icon="X"),
            prop("subdivision_type", "Type", "ENUM", "CATMULL_CLARK", panel=panel, tab="MODIFIER",
                 options=[{"value": "CATMULL_CLARK", "label": "Catmull-Clark"},
                          {"value": "SIMPLE", "label": "Simple"}]),
            prop("levels", "Levels Viewport", "INT", 1, panel=panel, tab="MODIFIER", min=0, max=11,
                 dragStep=1),
            prop("render_levels", "Render", "INT", 2, panel=panel, tab="MODIFIER", min=0, max=11,
                 dragStep=1),
            prop("show_only_control_edges", "Optimal Display", "BOOLEAN", 1, panel=panel, tab="MODIFIER"),
            prop("adv", "", "SUBPANEL", 0, panel=panel, tab="MODIFIER", section="Advanced",
                 subpanel="MOD_PT_subsurf_advanced", open=0, depth=1)]},
        {"id": "MOD_PT_bevel:modifiers[\"Bevel\"]", "title": "Bevel", "open": 0, "icon": "MOD_BEVEL",
         "properties": [prop("bevel_show", "Realtime", "BOOLEAN", 0, panel="bevel", tab="MODIFIER",
                             header=1)]},
    ]}


def material_tab():
    slots = [{"index": i, "label": n, "preview": "", "icon": "MATERIAL"}
             for i, n in enumerate(["Skin", "Eyes", "Teeth"])]
    return {"name": "Cube", "sections": [
        {"id": "EEVEE_MATERIAL_PT_context_material", "title": "", "open": 1, "header_hidden": 1,
         "properties": [
             dict(prop("material_slots.active_index", "", "LIST", 0, panel="EEVEE_MATERIAL_PT_context_material",
                       tab="MATERIAL", rows=slots, reorder=1),
                  add={"key": "object.material_slot_add", "label": "Add Material Slot", "icon": "ADD"},
                  remove={"key": "object.material_slot_remove", "label": "Remove Material Slot",
                          "icon": "REMOVE"}),
             prop("material", "", "ID", "Skin", panel="EEVEE_MATERIAL_PT_context_material",
                  tab="MATERIAL", id_type="Material", name="Skin", icon="MATERIAL", users=2, can_new=1,
                  can_unlink=1, fake_user=0, can_single_user=1,
                  options=[{"value": n, "label": n, "users": 1} for n in ["Skin", "Eyes", "Teeth", "Metal"]])]},
        {"id": "EEVEE_MATERIAL_PT_surface", "title": "Surface", "open": 1, "properties": [
            prop("base_color", "Base Color", "COLOR", [0.8, 0.45, 0.35, 1.0], panel="surface",
                 tab="MATERIAL", alpha=1, exposure=1),
            prop("metallic", "Metallic", "FLOAT", 0.0, panel="surface", tab="MATERIAL",
                 slider={"min": 0, "max": 1}, min=0, max=1),
            prop("roughness", "Roughness", "FLOAT", 0.5, panel="surface", tab="MATERIAL",
                 slider={"min": 0, "max": 1}, min=0, max=1, anim_state="ANIMATED"),
            prop("ior", "IOR", "FLOAT", 1.5, panel="surface", tab="MATERIAL", min=1, max=1000)]},
        {"id": "EEVEE_MATERIAL_PT_settings", "title": "Settings", "open": 0, "properties": []},
    ]}


def physics_tab():
    add = [prop(f"physics.{name}", label, "OPERATOR", 1 if name == "cloth" else 0, panel="PHYSICS_PT_add",
                tab="PHYSICS", checkbox=1, physics_panel=f"PHYSICS_PT_{name}", leading_icon=icon)
           for name, label, icon in [("cloth", "Cloth", "MOD_CLOTH"), ("collision", "Collision", "MOD_PHYSICS"),
                                     ("softbody", "Soft Body", "MOD_SOFT"), ("fluid", "Fluid", "MOD_FLUIDSIM")]]
    return {"name": "Cube", "sections": [
        {"id": "PHYSICS_PT_add", "title": "Enable Physics For", "open": 1, "properties": add},
        {"id": "PHYSICS_PT_cloth", "title": "Cloth", "open": 1, "properties": [
            prop("quality", "Quality Steps", "INT", 5, panel="PHYSICS_PT_cloth", tab="PHYSICS", min=1,
                 max=80, dragStep=1),
            prop("mass", "Vertex Mass", "FLOAT", 0.3, panel="PHYSICS_PT_cloth", tab="PHYSICS", min=0, max=50)]},
    ]}


def hierarchy():
    rows = [("scene", "", 0, "collection", "Scene Collection", "OUTLINER_COLLECTION", {}),
            ("scene/c1", "scene", 1, "collection", "Character", "OUTLINER_COLLECTION", {}),
            ("scene/c1/body", "scene/c1", 2, "object", "Body", "OUTLINER_OB_MESH",
             {"selected": 1, "active": 1}),
            ("scene/c1/eyes", "scene/c1", 2, "object", "Eyes", "OUTLINER_OB_MESH", {"selected": 1}),
            ("scene/c1/rig", "scene/c1", 2, "object", "Rig", "OUTLINER_OB_ARMATURE", {}),
            ("scene/c2", "scene", 1, "collection", "Environment", "OUTLINER_COLLECTION", {}),
            ("scene/c2/floor", "scene/c2", 2, "object", "Floor", "OUTLINER_OB_MESH", {}),
            ("scene/c2/tree", "scene/c2", 2, "object", "Tree", "OUTLINER_OB_MESH", {"hidden": 1}),
            ("scene/camera", "scene", 1, "object", "Camera", "OUTLINER_OB_CAMERA", {}),
            ("scene/light", "scene", 1, "object", "Key Light", "OUTLINER_OB_LIGHT", {})]
    nodes = []
    for key, parent, depth, kind, name, icon, extra in rows:
        node = {"key": key, "parent": parent, "depth": depth, "kind": kind, "id": key, "name": name,
                "icon": icon, "enabled": 1, "active": 0, "selected": 0, "hidden": 0,
                "actions": ["rename", "duplicate", "delete", "visibility", "move"] if depth else [],
                "accepts": 1 if kind == "collection" else 0}
        node.update(extra)
        nodes.append(node)
    return nodes


def shading():
    panel = "VIEW3D_PT_shading"
    return {"name": "Viewport Shading", "tab": "SHADING", "id": "SHADING:1", "sections": [
        {"id": panel, "title": "Shading", "open": 1, "properties": [
            prop("type", "Shading", "ENUM", "SOLID", panel=panel, tab="SHADING", property="type",
                 options=[{"value": "WIREFRAME", "label": "Wireframe", "icon": "SHADING_WIRE"},
                          {"value": "SOLID", "label": "Solid", "icon": "SHADING_SOLID"},
                          {"value": "MATERIAL", "label": "Material Preview", "icon": "SHADING_TEXTURE"},
                          {"value": "RENDERED", "label": "Rendered", "icon": "SHADING_RENDERED"}])]},
        {"id": "VIEW3D_PT_shading_lighting", "title": "Lighting", "open": 1, "properties": [
            prop("light", "Lighting", "ENUM", "STUDIO", panel="lighting", tab="SHADING", expand=1,
                 options=[{"value": "FLAT", "label": "Flat"}, {"value": "STUDIO", "label": "Studio"},
                          {"value": "MATCAP", "label": "MatCap"}]),
            prop("show_specular", "Specular Lighting", "BOOLEAN", 1, panel="lighting", tab="SHADING")]},
        {"id": "VIEW3D_PT_shading_color", "title": "Color", "open": 1, "properties": [
            prop("color_type", "Color", "ENUM", "MATERIAL", panel="color", tab="SHADING",
                 options=[{"value": "MATERIAL", "label": "Material"}, {"value": "OBJECT", "label": "Object"},
                          {"value": "RANDOM", "label": "Random"}, {"value": "SINGLE", "label": "Single"}])]},
        {"id": "VIEW3D_PT_shading_options", "title": "Options", "open": 1, "properties": [
            prop("show_xray", "X-Ray", "BOOLEAN", 0, panel="options", tab="SHADING"),
            prop("xray_alpha", "Alpha", "FLOAT", 0.5, panel="options", tab="SHADING",
                 slider={"min": 0, "max": 1}, min=0, max=1),
            prop("show_shadows", "Shadow", "BOOLEAN", 0, panel="options", tab="SHADING"),
            prop("show_cavity", "Cavity", "BOOLEAN", 0, panel="options", tab="SHADING")]},
    ]}


def tool():
    panel = "VIEW3D_PT_active_tool"
    mirror = [prop(f"use_mirror_{axis.lower()}", axis, "BOOLEAN", 1 if axis == "X" else 0, panel=panel,
                   tab="TOOL", toggle_group=f"{panel}:0", toggle_caption="Mirror") for axis in "XYZ"]
    return {"name": "Tool Settings", "tab": "TOOL", "id": "TOOL:1:builtin.brush:SCULPT", "sections": [
        {"id": panel, "title": "Active Tool", "open": 1, "properties": [
            prop("size", "Radius", "INT", 50, panel=panel, tab="TOOL", toolbar_label="Radius", min=1, max=500,
                 slider={"min": 1, "max": 500}),
            prop("strength", "Strength", "FLOAT", 0.5, panel=panel, tab="TOOL", toolbar_label="Strength",
                 min=0, max=1, slider={"min": 0, "max": 1}),
            prop("direction", "Direction", "ENUM", "ADD", panel=panel, tab="TOOL", expand=1,
                 toolbar_label="Direction", options=[{"value": "ADD", "label": "Add", "icon": "ADD"},
                                                     {"value": "SUBTRACT", "label": "Subtract",
                                                      "icon": "REMOVE"}]),
            prop("use_frontface", "Front Faces Only", "BOOLEAN", 0, panel=panel, tab="TOOL",
                 toolbar_label="Front Faces")] + mirror},
    ]}


def redo():
    """The last operation's settings, as `native_operator_redo_query` sends them."""
    move = [prop(f"value.{i}", axis, "FLOAT", value, group="Move", index=i, dragStep=0.01, min=-1e9, max=1e9)
            for i, (axis, value) in enumerate(zip("XYZ", [0.5, 0.0, 1.25]))]
    return {"id": "op:1", "operator": "TRANSFORM_OT_translate", "title": "Move", "ack": 0, "error": "",
            "properties": move + [
                prop("orient_type", "Orientation", "ENUM", "GLOBAL", options=[
                    {"value": "GLOBAL", "label": "Global", "checked": 1},
                    {"value": "LOCAL", "label": "Local", "checked": 0},
                    {"value": "VIEW", "label": "View", "checked": 0}]),
                prop("use_proportional_edit", "Proportional Editing", "BOOLEAN", 0),
                prop("proportional_size", "Proportional Size", "FLOAT", 1.0,
                     slider={"min": 0.001, "max": 10.0}, min=0.001, max=100)]}


def fixtures():
    return {"tab_list": tab_list(),
            "tabs": {"OBJECT": object_tab(), "DATA": data_tab(), "MODIFIER": modifier_tab(),
                     "MATERIAL": material_tab(), "PHYSICS": physics_tab(),
                     **{tab["id"]: {"name": "Cube", "sections": []} for tab in tab_list()
                        if tab["id"] not in ("OBJECT", "DATA", "MODIFIER", "MATERIAL", "PHYSICS")}},
            "hierarchy": hierarchy(), "shading": shading(), "tool": tool(), "redo": redo()}


# --------------------------------------------------------------------
# Build and run.

def simctl(*args, **kwargs):
    return subprocess.run(["xcrun", "simctl", *args], check=True, capture_output=True, text=True, **kwargs)


def build(app):
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip()
    app.mkdir(parents=True)
    objects = []
    for source in UIKIT_SOURCES:
        output = app.parent / (source.stem + ".o")
        subprocess.run(["xcrun", "clang++", "-x", "objective-c++", "-std=c++17", "-fno-objc-arc",
                        "-target", "arm64-apple-ios26.0-simulator", "-isysroot", sdk,
                        "-c", str(source), "-o", str(output)], check=True)
        objects.append(str(output))
    subprocess.run(["xcrun", "swiftc", "-target", "arm64-apple-ios26.0-simulator", "-sdk", sdk,
                    "-module-name", "Harness", "-parse-as-library", "-swift-version", "5", "-Onone",
                    "-import-objc-header", str(BRIDGE), "-o", str(app / "Harness"),
                    *map(str, SWIFT_SOURCES), str(HARNESS), *objects, "-lc++"], check=True)
    # Blender's icons: `BlenderIcons.xcassets` from an iOS build's `source/creator` folder.
    catalog = Path(os.environ.get("IOS_TEST_ICON_CATALOG", ""))
    if catalog.name and catalog.exists():
        subprocess.run(["xcrun", "actool", "--compile", str(app), "--platform", "iphonesimulator",
                        "--minimum-deployment-target", "26.0", "--accent-color", "AccentColor",
                        "--output-partial-info-plist", str(app.parent / "assets.plist"), str(catalog)],
                       check=True, capture_output=True)
    (app / "fixtures.json").write_text(json.dumps(fixtures()))
    info = {"CFBundleIdentifier": BUNDLE_ID, "CFBundleExecutable": "Harness", "CFBundleName": "Panels",
            "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
            "MinimumOSVersion": "26.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
            "UIRequiresFullScreen": False, "LSRequiresIPhoneOS": True,
            "UISupportedInterfaceOrientations~ipad": ["UIInterfaceOrientationLandscapeLeft",
                                                      "UIInterfaceOrientationLandscapeRight",
                                                      "UIInterfaceOrientationPortrait"],
            "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False,
                                           "UISceneConfigurations": {"UIWindowSceneSessionRoleApplication": [
                                               {"UISceneConfigurationName": "Default",
                                                "UISceneDelegateClassName": "Harness.HarnessSceneDelegate"}]}}}
    with open(app / "Info.plist", "wb") as handle:
        plistlib.dump(info, handle)
    subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True, capture_output=True)


def run_scenario(device, artifacts, name, *arguments, wait=3.0, before_screenshot=None):
    subprocess.run(["xcrun", "simctl", "terminate", device, BUNDLE_ID], capture_output=True)
    simctl("launch", device, BUNDLE_ID, *arguments)
    time.sleep(wait)
    if before_screenshot:
        before_screenshot()
    path = artifacts / f"{name}.png"
    simctl("io", device, "screenshot", str(path))
    container = Path(simctl("get_app_container", device, BUNDLE_ID, "data").stdout.strip())
    log = container / "Documents/requests.jsonl"
    requests = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
    return path, requests


class SwiftUIPanelsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != "darwin":
            raise unittest.SkipTest("Requires Xcode and an iOS simulator")
        devices = json.loads(subprocess.check_output(
            ["xcrun", "simctl", "list", "devices", "booted", "--json"], text=True))
        candidates = [device for runtime, entries in devices["devices"].items()
                      if ".iOS-" in runtime for device in entries if "iPad" in device["name"]]
        if not candidates and "IOS_TEST_SIMULATOR_UDID" not in os.environ:
            raise unittest.SkipTest("No booted iPad simulator")
        cls.device = os.environ.get("IOS_TEST_SIMULATOR_UDID", candidates[0]["udid"] if candidates else "")
        cls.temp = tempfile.TemporaryDirectory()
        cls.artifacts = Path(os.environ.get("IOS_TEST_ARTIFACT_DIR", cls.temp.name))
        cls.artifacts.mkdir(parents=True, exist_ok=True)
        app = Path(cls.temp.name) / "build/Harness.app"
        build(app)
        simctl("install", cls.device, str(app))

    @classmethod
    def tearDownClass(cls):
        subprocess.run(["xcrun", "simctl", "terminate", cls.device, BUNDLE_ID], capture_output=True)
        cls.temp.cleanup()

    def scenario(self, name, *arguments, **kwargs):
        path, requests = run_scenario(self.device, self.artifacts, name, *arguments, **kwargs)
        self.assertGreater(path.stat().st_size, 10000)
        return requests

    def test_properties_tabs(self):
        for tab in ("OBJECT", "DATA", "MODIFIER", "MATERIAL", "PHYSICS"):
            with self.subTest(tab=tab):
                requests = self.scenario(f"properties_{tab.lower()}", "-scenario", "properties", "-tab", tab)
                self.assertIn(("native_panels", "1"), [(r["setting"], str(r["value"])) for r in requests])

    def test_light_appearance(self):
        self.scenario("properties_data_light", "-scenario", "properties", "-tab", "DATA",
                      "-appearance", "light")

    def test_hierarchy(self):
        requests = self.scenario("hierarchy", "-scenario", "hierarchy")
        self.assertIn("2", [str(r["value"]) for r in requests if r["setting"] == "native_panels"])

    def test_both_panels(self):
        self.scenario("both", "-scenario", "both", "-tab", "DATA")

    def test_shading(self):
        requests = self.scenario("shading", "-scenario", "shading")
        self.assertIn("native_shading_visible", [r["setting"] for r in requests])

    def test_redo_strip(self):
        requests = self.scenario("redo", "-scenario", "redo")
        self.assertNotIn("native_redo_edit", [r["setting"] for r in requests])

    def test_tool_strip(self):
        self.scenario("tool", "-scenario", "tool")


if __name__ == "__main__":
    unittest.main()
