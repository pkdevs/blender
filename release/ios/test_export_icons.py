# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
from pathlib import Path
import re
import tempfile
import unittest
import xml.etree.ElementTree as ET

from export_icons import SVG, export_catalog, geometry_template, svg_template, vco_triangles


class ExportIconsTest(unittest.TestCase):
    def test_svg_preserves_alpha_and_aspect_ratio(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.svg"
            path.write_text(f'<svg xmlns="{SVG}" viewBox="0 0 1600 1500" width="1600" height="1500">'
                            '<path style="fill:#ffffff;opacity:.6;stroke:none;-inkscape-stroke:none" d="M0 0h100v100Z"/>'
                            '<path fill="none" stroke="#ff0000" stroke-opacity=".3" d="M0 0L100 100"/>'
                            '</svg>')
            root = svg_template(path)
            self.assertEqual(root.attrib["width"], "24")
            self.assertEqual(root.attrib["height"], "24")
            self.assertEqual(root.attrib["viewBox"], "0 0 1600 1500")
            self.assertEqual(root[0].attrib["opacity"], ".6")
            self.assertEqual(root[0].attrib["fill"], "#000")
            self.assertEqual(root[0].attrib["stroke"], "none")
            self.assertNotIn("-inkscape-stroke", root[0].attrib)
            self.assertEqual(root[1].attrib["fill"], "none")
            self.assertEqual(root[1].attrib["stroke-opacity"], ".3")

    def test_svg_intrinsic_dimensions_supply_viewbox(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.svg"
            path.write_text(f'<svg xmlns="{SVG}" width="1400px" height="1600"><path d="M0 0h10v10Z"/></svg>')
            self.assertEqual(svg_template(path).attrib["viewBox"], "0 0 1400 1600")

    def test_vco_coordinates_tones_and_validation(self):
        data = b"VCO\x00\xff\xff\x00\x00" + bytes((0, 0, 255, 0, 0, 255))
        data += bytes((255, 128, 0, 255, 128, 128, 128, 128, 0, 0, 0, 255))
        points, tones = next(vco_triangles(data))
        self.assertEqual(points, [(0, 255), (255, 255), (0, 0)])
        self.assertEqual(tones, (255, 64, 0))
        for invalid in (b"", data[:-1], b"BAD!" + data[4:]):
            with self.assertRaises(ValueError):
                list(vco_triangles(invalid))

    def test_geometry_preserves_knockouts_and_gradients(self):
        source = Path(__file__).resolve().parents[1] / "datafiles" / "icons"
        root = geometry_template(source / "ops.paint.weight_gradient.dat")
        self.assertIsNotNone(root.find(f".//{{{SVG}}}mask"))
        self.assertIsNotNone(root.find(f".//{{{SVG}}}linearGradient"))
        self.assertEqual(root[-1].attrib["mask"], "url(#artwork)")
        self.assertEqual(root[-1].attrib["fill"], "#000")

    def test_complete_catalog_and_toolbar_coverage(self):
        release = Path(__file__).resolve().parents[1]
        manifest = release / "ios" / "blender_icons.json"
        mapping = json.loads(manifest.read_text())
        bridge = release.parent / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc"
        tool_ids = set(re.findall(r'"(builtin(?:_brush)?\.[a-z_]+)"', bridge.read_text()))
        for tool_id in tool_ids:
            self.assertIn("tool." + tool_id, mapping)
        native = release.parent / "intern/ghost/intern/GHOST_WindowIOS.mm"
        for setting, value in re.findall(r'setting:@"([^"]+)"\s+value:@"([^"]+)"', native.read_text()):
            self.assertIn(f"setting.{setting}.{value}", mapping)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "Icons.xcassets"
            export_catalog(release / "datafiles", manifest, output)
            tool_names = {name for key, name in mapping.items() if key.startswith("tool.")}
            tool_names.update(path.stem for path in (release / "datafiles/icons").glob("*.dat"))
            interface_names = set(mapping.values()) | {
                path.stem for path in (release / "datafiles/icons_svg").glob("*.svg")}
            self.assertEqual(len(list(output.glob("*.imageset"))), len(interface_names) + len(tool_names))
            for imageset in output.glob("*.imageset"):
                contents = json.loads((imageset / "Contents.json").read_text())
                self.assertEqual(contents["properties"]["template-rendering-intent"], "template")
                self.assertTrue(contents["properties"]["preserves-vector-representation"])
                expected_size = "36" if imageset.name.startswith("blender.tool.") else "24"
                root = ET.parse(imageset / "icon.svg").getroot()
                self.assertEqual(root.attrib["width"], expected_size)
                self.assertEqual(root.attrib["height"], expected_size)
            for name in tool_names.intersection(mapping.values()):
                regular = ET.parse(output / f"blender.{name}.imageset/icon.svg").getroot()
                enlarged = ET.parse(output / f"blender.tool.{name}.imageset/icon.svg").getroot()
                enlarged.set("width", "24")
                enlarged.set("height", "24")
                self.assertEqual(ET.tostring(regular), ET.tostring(enlarged))
            stale = output / "blender.stale.imageset"
            stale.mkdir()
            export_catalog(release / "datafiles", manifest, output)
            self.assertFalse(stale.exists())


if __name__ == "__main__":
    unittest.main()
