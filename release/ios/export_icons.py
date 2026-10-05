# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Export Blender's native iPad icons to a template-image asset catalog.

Uses only the Python standard library. SVG interface icons retain their alpha;
VCO tool artwork is converted to SVG with luminance carrying its secondary detail.
The generated catalog is disposable and must live outside the source tree.
"""

import argparse
import itertools
import json
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET

SVG = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG)
POINT_SIZE = 24
TOOL_POINT_SIZE = 36


def element(name, attrs=None, parent=None):
    tag = "{%s}%s" % (SVG, name)
    return ET.Element(tag, attrs or {}) if parent is None else ET.SubElement(parent, tag, attrs or {})


def svg_template(path):
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
            elif key in {"fill", "stroke", "stop-color", "color"}:
                if node.attrib[key] not in {"none", "transparent", "inherit"}:
                    node.set(key, "#000")
    root.set("fill", "#000")
    root.set("width", str(POINT_SIZE))
    root.set("height", str(POINT_SIZE))
    root.set("preserveAspectRatio", "xMidYMid meet")
    return root


def vco_triangles(data):
    if len(data) < 8 or data[:4] != b"VCO\x00" or (len(data) - 8) % 18:
        raise ValueError("Invalid VCO icon")
    if data[4:8] != bytes((255, 255, 0, 0)):
        raise ValueError("Unsupported VCO coordinate range/origin")
    count = (len(data) - 8) // 18
    colors = 8 + count * 6
    for i in range(count):
        points = [(data[8 + i * 6 + j], 255 - data[8 + i * 6 + j + 1]) for j in (0, 2, 4)]
        rgba = [tuple(data[colors + i * 12 + j:colors + i * 12 + j + 4]) for j in (0, 4, 8)]
        # Value removes hue without making the colored tool accents disappear.
        # Dark outlines become cut-outs in the final alpha mask, not fixed black ink.
        tones = tuple(round(max(color[:3]) * color[3] / 255) for color in rgba)
        yield points, tones


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


def export_catalog(source, manifest, output):
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
    print(f"Exported {len(variants)} Blender template assets from {len(names)} icons to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "datafiles")
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("blender_icons.json"))
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    export_catalog(args.source, args.manifest, args.output)
