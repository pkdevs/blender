# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Export Blender's native iPad icons to an image asset catalog.

Uses only the Python standard library. SVG interface icons retain their alpha;
VCO tool artwork is converted to SVG with luminance carrying its secondary detail.
Colored toolbar variants retain the original mesh colors in vector PDFs.
Icons that Blender colors through its theme get light and dark variants with the
colors of Blender's default dark and light themes, mirroring interface_icons.cc:
multicolor icons fill their `blender_*` groups, and object, modifier and other
category icons are tinted as a whole where Blender tints them. The light
appearance replaces Blender's light theme icon colors with the iOS system
palette. Icon outlines (`icon_border_intensity`) are not exported, and neutral
grey or white category colors are left to the platform tint.
The generated catalog is disposable and must live outside the source tree.
"""

import argparse
import itertools
import json
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

SVG = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG)
POINT_SIZE = 24
ROOT = Path(__file__).resolve().parents[2]
ICON_HEADER = ROOT / "source/blender/editors/include/UI_icons.hh"
DARK_THEME = ROOT / "release/datafiles/userdef/userdef_default_theme.c"
LIGHT_THEME = ROOT / "scripts/presets/interface_theme/Blender_Light.xml"
TOOL_POINT_SIZE = 36


def element(name, attrs=None, parent=None):
    tag = "{%s}%s" % (SVG, name)
    return ET.Element(tag, attrs or {}) if parent is None else ET.SubElement(parent, tag, attrs or {})


def svg_clean(path):
    root = ET.parse(path).getroot()
    if "viewBox" not in root.attrib:
        width = float(root.attrib["width"].removesuffix("px"))
        height = float(root.attrib["height"].removesuffix("px"))
        if width <= 0 or height <= 0:
            raise ValueError(f"{path}: invalid SVG dimensions")
        root.set("viewBox", f"0 0 {width:g} {height:g}")
    # Editor metadata is not artwork and is not understood by CoreSVG.
    for parent in root.iter():
        for child in list(parent):
            if not child.tag.startswith("{%s}" % SVG) or child.tag == "{%s}metadata" % SVG:
                parent.remove(child)
    for node in root.iter():
        if node.tag in {"{%s}%s" % (SVG, tag) for tag in ("style", "image", "mask", "filter")}:
            raise ValueError(f"{path}: unsupported template element {node.tag}")
        if "style" in node.attrib:
            for declaration in node.attrib.pop("style").split(";"):
                if declaration.strip():
                    key, value = declaration.split(":", 1)
                    # Editor-only CSS properties are not valid SVG presentation attributes.
                    if not key.strip().startswith("-"):
                        node.set(key.strip(), value.strip())
        for key in list(node.attrib):
            if key.startswith("{"):
                del node.attrib[key]
    root.set("width", str(POINT_SIZE))
    root.set("height", str(POINT_SIZE))
    root.set("preserveAspectRatio", "xMidYMid meet")
    return root


def svg_template(path, color="#000"):
    """One-color artwork: every painted part takes `color`, keeping its alpha."""
    root = svg_clean(path)
    for node in root.iter():
        for key in ("fill", "stroke", "stop-color", "color"):
            if key in node.attrib and node.attrib[key] not in {"none", "transparent", "inherit"}:
                node.set(key, color)
    root.set("fill", color)
    return root


def theme_color(value):
    """`#rrggbb[aa]` to an SVG color and opacity."""
    value = value.strip().lstrip("#")
    if len(value) not in (6, 8):
        raise ValueError(f"Invalid theme color: {value}")
    alpha = int(value[6:8], 16) / 255 if len(value) == 8 else 1.0
    return "#" + value[:6].lower(), alpha


def dark_theme_values(path):
    """Flatten the default theme initializer into `tui/icon_scene`-style paths."""
    values, stack, counters = {}, [], [0]
    token = re.compile(r"\.(\w+)\s*=\s*RGBA\(0x([0-9a-fA-F]{8})\)|\.(\w+)\s*=\s*\{|(\{)|(\})")
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
    border = re.search(r"\.icon_border_intensity\s*=\s*([0-9.]+)", source)
    values["tui/icon_border_intensity"] = border[1] if border else "0"
    for match in token.finditer(source):
        name, rgba, block, anonymous, close = match.groups()
        if rgba:
            values["/".join(stack[1:] + [name])] = "#" + rgba
        elif block or anonymous:
            stack.append(block or str(counters[-1]))
            counters[-1] += 1
            counters.append(0)
        elif close and stack:
            stack.pop()
            counters.pop()
    return values


def light_theme_values(path):
    """The same paths, read from Blender's light theme preset."""
    theme = ET.parse(path).getroot().find("Theme")
    values = {}

    def read(prefix, node, names):
        for name in names:
            if node is not None and node.get(name):
                values[f"{prefix}/{name}"] = node.get(name)

    ui = theme.find("user_interface/ThemeUserInterface")
    read("tui", ui, ("icon_scene", "icon_collection", "icon_object", "icon_object_data", "icon_modifier",
                     "icon_shading", "icon_folder", "icon_autokey", "panel_text", "icon_border_intensity"))
    read("tui/wcol_regular", ui.find("wcol_regular/ThemeWidgetColors"), ("inner",))
    read("tui/wcol_state", ui.find("wcol_state/ThemeWidgetStateColors"), ("error", "warning", "info"))
    read("space_view3d", theme.find("view_3d/ThemeView3D"), ("bevel", "crease", "seam", "sharp", "vertex_select"))
    read("space_action", theme.find("dopesheet_editor/ThemeDopeSheet"),
         ("anim_interpolation_linear", "anim_interpolation_constant", "anim_interpolation_other"))
    read("space_properties", theme.find("properties/ThemeProperties/space/ThemeSpaceGeneric"), ("back",))
    for index, item in enumerate(theme.findall("collection_color/ThemeCollectionColor")):
        read(f"collection_color/{index}", item, ("color",))
    return values


# Light appearance uses the iOS 27 system palette rather than Blender's light theme, whose pale
# icon colors depend on an icon outline. Hues follow Blender's so icons keep their identity.
IOS_LIGHT_COLORS = {
    "tui/icon_scene": "#3c3c43ff", "tui/icon_collection": "#3c3c43ff",  # Secondary label.
    "tui/icon_object": "#ff8d28ff",  # Orange.
    "tui/icon_object_data": "#00c8b3ff",  # Mint.
    "tui/icon_modifier": "#0088ffff",  # Blue.
    "tui/icon_shading": "#ff2d55ff",  # Pink.
    "tui/icon_folder": "#ac7f5eff",  # Brown.
    "tui/icon_autokey": "#ff383cff",  # Red.
    "tui/wcol_state/error": "#ff383cff", "tui/wcol_state/warning": "#ff8d28ff",
    "tui/wcol_state/info": "#0088ffff",
    # Collection color tags: red, orange, yellow, green, blue, violet, pink, brown.
    **{f"collection_color/{i}/color": color for i, color in enumerate((
        "#ff383cff", "#ff8d28ff", "#ffcc00ff", "#34c759ff",
        "#0088ffff", "#6155f5ff", "#ff2d55ff", "#ac7f5eff"))},
}

# `blender_*` SVG group ids and the theme color Blender fills them with (interface_icons.cc).
GROUP_COLORS = {
    "blender_white": "#ffffffff", "blender_black": "#000000ff",
    "blender_logo_orange": "#e87d0dff", "blender_logo_blue": "#265787ff",
    "blender_selected": "tui/wcol_regular/inner", "blender_mesh_selected": "space_view3d/vertex_select",
    "blender_back": "space_properties/back", "blender_text": "tui/panel_text",
    "blender_red_alert": "tui/wcol_state/error", "blender_error": "tui/wcol_state/error",
    "blender_warning": "tui/wcol_state/warning", "blender_info": "tui/wcol_state/info",
    "blender_scene": "tui/icon_scene", "blender_collection": "tui/icon_collection",
    **{f"blender_collection_color_{i + 1:02d}": f"collection_color/{i}/color" for i in range(8)},
    "blender_object": "tui/icon_object", "blender_object_data": "tui/icon_object_data",
    "blender_modifier": "tui/icon_modifier", "blender_shading": "tui/icon_shading",
    "blender_folder": "tui/icon_folder", "blender_fund": "#cc3048ff", "blender_autokey": "tui/icon_autokey",
    "blender_tool_add": "#75ffafff", "blender_tool_remove": "#f56b5bff", "blender_tool_select": "#ffb02bff",
    "blender_tool_transform": "#d9aff5ff", "blender_tool_white": "#ffffffff", "blender_tool_red": "#d62d30ff",
    "blender_bevel": "space_view3d/bevel", "blender_crease": "space_view3d/crease",
    "blender_seam": "space_view3d/seam", "blender_sharp": "space_view3d/sharp",
    "blender_ipo_linear": "space_action/anim_interpolation_linear",
    "blender_ipo_constant": "space_action/anim_interpolation_constant",
    "blender_ipo_other": "space_action/anim_interpolation_other",
}
# UI_icons.hh icon classes tinted as a whole with a theme color.
CATEGORY_COLORS = {
    "SCENE": "tui/icon_scene", "COLLECTION": "tui/icon_collection", "OBJECT": "tui/icon_object",
    "OBJECT_DATA": "tui/icon_object_data", "MODIFIER": "tui/icon_modifier", "SHADING": "tui/icon_shading",
    "FOLDER": "tui/icon_folder", "FUND": "#cc3048ff",
}


def resolve(theme, key):
    return key if key.startswith("#") else theme.get(key)


def icon_classes(header):
    """Map icon file names to their UI_icons.hh class: COLOR, a category, or plain."""
    classes = {}
    for kind, name in re.findall(r"^DEF_ICON(?:_(\w+?))?\((\w+)\)", header.read_text(), flags=re.M):
        classes[name.lower()] = kind
    return classes


def paint(node, color):
    svg_color, alpha = theme_color(color)
    node.set("fill", svg_color)
    if alpha < 1:
        node.set("fill-opacity", f"{alpha:.3f}")


def translation(node, ancestors):
    """Summed translation of `node` and its ancestors; collection artwork is only translated."""
    tx = ty = 0.0
    for item in ancestors + [node]:
        for kind, args in re.findall(r"(\w+)\s*\(([^)]*)\)", item.get("transform", "")):
            values = [float(v) for v in re.split(r"[\s,]+", args.strip()) if v]
            if kind == "translate":
                tx, ty = tx + values[0], ty + (values[1] if len(values) > 1 else 0.0)
            elif kind == "matrix" and len(values) == 6:
                tx, ty = tx + values[4], ty + values[5]
    return tx, ty


def collection_artwork(root):
    """Blender draws collection icons with a translucent black ring around the colored box and a
    black handle on it. Drop the ring and cut the handle out of the box instead, so the icon
    reads cleanly on light and dark backgrounds without an outline."""
    painted = None
    for parent in root.iter():
        if parent.get("id", "").startswith("blender_collection"):
            # The last painted path is the box body that the handle sits on.
            def visit(node, chain):
                nonlocal painted
                if node.tag == "{%s}path" % SVG:
                    painted = (node, chain)
                for child in node:
                    visit(child, chain + [node])
            visit(parent, [])
            break
    if painted is None:
        return
    box, ancestors = painted
    tx, ty = translation(box, [a for a in ancestors if a is not root])
    handle = None
    for parent in list(root.iter()):
        for child in list(parent):
            if parent.get("id", "").startswith("blender_") or child.get("id", "").startswith("blender_"):
                continue
            if child.tag != "{%s}path" % SVG:
                continue
            if child.get("id") == "handhold":
                handle = child
            parent.remove(child)
    # Drop now-empty unpainted groups.
    for parent in list(root.iter()):
        for child in list(parent):
            if child.tag == "{%s}g" % SVG and not len(child) and not child.get("id", "").startswith("blender_"):
                parent.remove(child)
    if handle is not None:
        match = re.match(r"\s*m\s*(-?[\d.]+)[\s,]*(-?[\d.]+)(.*)", handle.get("d"), re.S | re.I)
        if match:
            x, y = float(match[1]) - tx, float(match[2]) - ty
            box.set("d", box.get("d") + f" M{x:g} {y:g}" + match[3])
            box.set("fill-rule", "evenodd")


def svg_multicolor(path, theme):
    """Keep authored colors and fill `blender_*` groups with the theme, as Blender does."""
    root = svg_clean(path)
    if path.stem == "outliner_collection" or path.stem.startswith("collection_color_"):
        collection_artwork(root)
    for group in root.iter():
        name = group.get("id", "")
        color = resolve(theme, GROUP_COLORS[name]) if name in GROUP_COLORS else None
        if not color:
            continue
        for node in group.iter():
            if "fill" in node.attrib and node.attrib["fill"] not in {"none", "transparent"}:
                paint(node, color)
    return root


def neutral(color):
    """Grey and white theme colors only mean "the icon color"; tinting follows the platform."""
    svg_color, _ = theme_color(color)
    channels = [int(svg_color[i:i + 2], 16) for i in (1, 3, 5)]
    return max(channels) - min(channels) < 16


def write_appearances(image_set, light, dark, info):
    image_set.mkdir(exist_ok=True)
    ET.ElementTree(light).write(image_set / "light.svg", encoding="utf-8", xml_declaration=True)
    ET.ElementTree(dark).write(image_set / "dark.svg", encoding="utf-8", xml_declaration=True)
    (image_set / "Contents.json").write_text(json.dumps({
        "images": [
            {"filename": "light.svg", "idiom": "universal"},
            {"filename": "dark.svg", "idiom": "universal",
             "appearances": [{"appearance": "luminosity", "value": "dark"}]},
        ],
        "info": info,
        "properties": {"preserves-vector-representation": True, "template-rendering-intent": "original"},
    }, indent=2) + "\n")


def vco_color_triangles(data):
    if len(data) < 8 or data[:4] != b"VCO\x00" or (len(data) - 8) % 18:
        raise ValueError("Invalid VCO icon")
    if data[4:8] != bytes((255, 255, 0, 0)):
        raise ValueError("Unsupported VCO coordinate range/origin")
    count = (len(data) - 8) // 18
    colors = 8 + count * 6
    for i in range(count):
        points = [(data[8 + i * 6 + j], 255 - data[8 + i * 6 + j + 1]) for j in (0, 2, 4)]
        rgba = [tuple(data[colors + i * 12 + j:colors + i * 12 + j + 4]) for j in (0, 4, 8)]
        yield points, tuple(rgba)


def vco_triangles(data):
    for points, rgba in vco_color_triangles(data):
        # Value removes hue without making the colored tool accents disappear.
        # Dark outlines become cut-outs in the final alpha mask, not fixed black ink.
        tones = tuple(round(max(color[:3]) * color[3] / 255) for color in rgba)
        yield points, tones


def invert_lightness(color):
    # Exact byte-space equivalent of Blender's HSL L -> 1-L toolbar adjustment.
    offset = 255 - max(color[:3]) - min(color[:3])
    return tuple(value + offset for value in color[:3]) + (color[3],)


def geometry_color_pdf(path, light=False):
    """Preserve Gouraud vertex colors with PDF's native free-form triangle shading."""
    objects = [b'', b'', b'']  # Catalog, page tree, page.

    def add(value):
        objects.append(value)
        return len(objects)

    def stream(dictionary, data):
        return b'<< ' + dictionary + b' /Length ' + str(len(data)).encode() + b' >>\nstream\n' + data + b'\nendstream'

    shading_resources, alpha_resources, commands = [], [], []
    triangles = vco_color_triangles(path.read_bytes())
    # Consecutive alpha batches retain painter order, including translucent details.
    for index, (alpha, group) in enumerate(itertools.groupby(triangles, key=lambda item: item[1][0][3])):
        mesh = bytearray()
        for points, colors in group:
            if any(color[3] != alpha for color in colors):
                raise ValueError(f'{path}: varying vertex alpha is not supported')
            for (x, y), color in zip(points, colors):
                color = invert_lightness(color) if light else color
                mesh.extend((0, x, 255 - y, *color[:3]))
        shading = add(stream(b'/ShadingType 4 /ColorSpace /DeviceRGB /BitsPerCoordinate 8 '
                             b'/BitsPerComponent 8 /BitsPerFlag 8 /Decode [0 255 0 255 0 1 0 1 0 1]', bytes(mesh)))
        opacity = add(f'<< /Type /ExtGState /ca {alpha / 255:.8f} >>'.encode())
        shading_resources.append(f'/S{index} {shading} 0 R')
        alpha_resources.append(f'/A{index} {opacity} 0 R')
        commands.append(f'q /A{index} gs /S{index} sh Q')
    scale = TOOL_POINT_SIZE / 256
    content = f'q {scale} 0 0 {scale} 0 0 cm\n' + '\n'.join(commands) + '\nQ'
    content_id = add(stream(b'', content.encode()))
    objects[0] = b'<< /Type /Catalog /Pages 2 0 R >>'
    objects[1] = b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>'
    objects[2] = (f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {TOOL_POINT_SIZE} {TOOL_POINT_SIZE}] '
                  f'/Resources << /Shading << {" ".join(shading_resources)} >> '
                  f'/ExtGState << {" ".join(alpha_resources)} >> >> /Contents {content_id} 0 R >>').encode()
    pdf = bytearray(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n')
    offsets = [0]
    for number, obj in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf.extend(f'{number} 0 obj\n'.encode() + obj + b'\nendobj\n')
    xref = len(pdf)
    pdf.extend(f'xref\n0 {len(offsets)}\n0000000000 65535 f \n'.encode())
    for offset in offsets[1:]:
        pdf.extend(f'{offset:010d} 00000 n \n'.encode())
    pdf.extend(f'trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode())
    return bytes(pdf)


def gradient_for_triangle(defs, name, points, tones):
    (x0, y0), (x1, y1), (x2, y2) = points
    a, b, c = (tone / 255 for tone in tones)
    det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if not det:
        return "#000"
    gx = ((b - a) * (y2 - y0) - (c - a) * (y1 - y0)) / det
    gy = ((x1 - x0) * (c - a) - (x2 - x0) * (b - a)) / det
    length_squared = gx * gx + gy * gy
    start_x, start_y = x0 - a * gx / length_squared, y0 - a * gy / length_squared
    gradient = element("linearGradient", {
        "id": name, "gradientUnits": "userSpaceOnUse",
        "x1": str(start_x), "y1": str(start_y),
        "x2": str(start_x + gx / length_squared), "y2": str(start_y + gy / length_squared),
    }, defs)
    element("stop", {"offset": "0", "stop-color": "#000"}, gradient)
    element("stop", {"offset": "1", "stop-color": "#fff"}, gradient)
    return f"url(#{name})"


def geometry_template(path):
    root = element("svg", {"viewBox": "0 0 256 256", "width": str(POINT_SIZE), "height": str(POINT_SIZE)})
    defs = element("defs", parent=root)
    mask = element("mask", {
        "id": "artwork", "maskUnits": "userSpaceOnUse",
        "x": "0", "y": "0", "width": "256", "height": "256",
        "color-interpolation": "sRGB",
    }, defs)
    # One compound path per consecutive flat-tone run avoids hairline cracks at
    # internal triangle edges. Keep paint order: later dark details cut earlier faces.
    for index, (tones, group) in enumerate(itertools.groupby(vco_triangles(path.read_bytes()), key=lambda t: t[1])):
        triangles = list(group)
        if len(set(tones)) == 1:
            commands = []
            for points, _ in triangles:
                commands.append("M%d %dL%d %dL%d %dZ" % tuple(v for point in points for v in point))
            element("path", {
                "d": "".join(commands), "fill": "rgb(%d,%d,%d)" % (tones[0], tones[0], tones[0]),
            }, mask)
        else:
            for j, (points, _) in enumerate(triangles):
                fill = gradient_for_triangle(defs, f"tone{index}_{j}", points, tones)
                element("path", {
                    "d": "M%d %dL%d %dL%d %dZ" % tuple(v for point in points for v in point),
                    "fill": fill,
                }, mask)
    element("rect", {"width": "256", "height": "256", "fill": "#000", "mask": "url(#artwork)"}, root)
    return root


def export_catalog(source, manifest, output, header=ICON_HEADER, dark_theme=DARK_THEME, light_theme=LIGHT_THEME):
    mapping = json.loads(manifest.read_text())
    names = set(mapping.values())
    # Projected menus carry Blender icon names, including icons absent from the toolbar.
    names.update(path.stem for path in (source / "icons_svg").glob("*.svg"))
    tool_names = {name for key, name in mapping.items() if key.startswith("tool.")}
    # Native tool groups use ToolDef.icon directly, including context-dependent tools.
    tool_names.update(path.stem for path in (source / "icons").glob("*.dat"))
    variants = [(f"blender.{name}", name, POINT_SIZE) for name in sorted(names)]
    variants += [(f"blender.tool.{name}", name, TOOL_POINT_SIZE) for name in sorted(tool_names)]
    output.mkdir(parents=True, exist_ok=True)
    info = {"author": "Blender", "version": 1}
    (output / "Contents.json").write_text(json.dumps({"info": info}, indent=2) + "\n")
    expected = {f"{asset}.imageset" for asset, _, _ in variants}
    colored = sorted((source / 'icons').glob('*.dat'))
    expected.update(f'blender.tool.color.{path.stem}.imageset' for path in colored)
    expected.update(f'blender.tool.selected.{path.stem}.imageset' for path in colored)
    classes = icon_classes(header)
    themes = {"light": {**light_theme_values(light_theme), **IOS_LIGHT_COLORS},
              "dark": dark_theme_values(dark_theme)}
    for theme in themes.values():
        missing = sorted({key for key in list(GROUP_COLORS.values()) + list(CATEGORY_COLORS.values())
                          if not key.startswith("#") and key not in theme} - {"space_properties/back"})
        if missing:
            raise ValueError(f"Theme is missing icon colors: {missing}")
    svgs = {path.stem: path for path in (source / "icons_svg").glob("*.svg")}
    multicolor = sorted(name for name, kind in classes.items() if kind == "COLOR" and name in svgs)
    themed = sorted(name for name, kind in classes.items() if kind in CATEGORY_COLORS and name in svgs and
                    not all(neutral(resolve(theme, CATEGORY_COLORS[kind])) for theme in themes.values()))
    expected.update(f"blender.multicolor.{name}.imageset" for name in multicolor)
    expected.update(f"blender.themed.{name}.imageset" for name in themed)
    for stale in output.glob("blender.*.imageset"):
        if stale.name not in expected:
            shutil.rmtree(stale)
    for asset, name, point_size in variants:
        svg = source / "icons_svg" / f"{name}.svg"
        geometry = source / "icons" / f"{name}.dat"
        if svg.is_file():
            root = svg_template(svg)
        elif geometry.is_file():
            root = geometry_template(geometry)
        else:
            raise ValueError(f"No Blender source icon: {name}")
        root.set("width", str(point_size))
        root.set("height", str(point_size))
        image_set = output / f"{asset}.imageset"
        image_set.mkdir(exist_ok=True)
        ET.ElementTree(root).write(image_set / "icon.svg", encoding="utf-8", xml_declaration=True)
        (image_set / "Contents.json").write_text(json.dumps({
            "images": [{"filename": "icon.svg", "idiom": "universal"}],
            "info": info,
            "properties": {"preserves-vector-representation": True, "template-rendering-intent": "template"},
        }, indent=2) + "\n")
    for name in multicolor:
        write_appearances(output / f"blender.multicolor.{name}.imageset",
                          svg_multicolor(svgs[name], themes["light"]),
                          svg_multicolor(svgs[name], themes["dark"]), info)
    for name in themed:
        key = CATEGORY_COLORS[classes[name]]
        light, dark = (svg_template(svgs[name], theme_color(resolve(theme, key))[0])
                       for theme in (themes["light"], themes["dark"]))
        write_appearances(output / f"blender.themed.{name}.imageset", light, dark, info)
    for path in colored:
        image_set = output / f'blender.tool.color.{path.stem}.imageset'
        image_set.mkdir(exist_ok=True)
        (image_set / 'light.pdf').write_bytes(geometry_color_pdf(path, light=True))
        (image_set / 'dark.pdf').write_bytes(geometry_color_pdf(path))
        (image_set / 'Contents.json').write_text(json.dumps({
            'images': [
                {'filename': 'light.pdf', 'idiom': 'universal'},
                {'filename': 'dark.pdf', 'idiom': 'universal',
                 'appearances': [{'appearance': 'luminosity', 'value': 'dark'}]},
            ],
            'info': info,
            'properties': {'preserves-vector-representation': True, 'template-rendering-intent': 'original'},
        }, indent=2) + '\n')
        selected = output / f'blender.tool.selected.{path.stem}.imageset'
        selected.mkdir(exist_ok=True)
        (selected / 'icon.pdf').write_bytes((image_set / 'dark.pdf').read_bytes())
        (selected / 'Contents.json').write_text(json.dumps({
            'images': [{'filename': 'icon.pdf', 'idiom': 'universal'}],
            'info': info,
            'properties': {'preserves-vector-representation': True, 'template-rendering-intent': 'original'},
        }, indent=2) + '\n')
    # The app's accent colour, Blender's orange: system controls take it, such as the document
    # launcher's buttons. Named by `NSAccentColorName` in the app's Info.plist.
    accent = output / "AccentColor.colorset"
    accent.mkdir(exist_ok=True)
    (accent / "Contents.json").write_text(json.dumps({
        "colors": [{
            "idiom": "universal",
            "color": {"color-space": "srgb",
                      "components": {"red": "0.910", "green": "0.490", "blue": "0.051", "alpha": "1.000"}},
        }],
        "info": info,
    }, indent=2) + "\n")
    print(f"Exported {len(variants)} template, {len(multicolor)} multicolor, {len(themed)} themed, "
          f"{len(colored)} colored, and {len(colored)} selection Blender assets to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "datafiles")
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("blender_icons.json"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--icon-header", type=Path, default=ICON_HEADER)
    parser.add_argument("--dark-theme", type=Path, default=DARK_THEME)
    parser.add_argument("--light-theme", type=Path, default=LIGHT_THEME)
    args = parser.parse_args()
    export_catalog(args.source, args.manifest, args.output, args.icon_header, args.dark_theme, args.light_theme)
