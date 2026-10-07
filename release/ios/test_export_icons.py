# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from export_icons import (SVG, export_catalog, geometry_color_pdf, geometry_template,
                          invert_lightness, svg_template, vco_color_triangles, vco_triangles)


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

    def test_color_geometry_and_blender_lightness_adjustment(self):
        data = b'VCO\x00\xff\xff\x00\x00' + bytes((0, 0, 255, 0, 0, 255))
        colors = ((63, 128, 255, 255), (255, 0, 0, 255), (0, 255, 0, 255))
        data += bytes(value for color in colors for value in color)
        self.assertEqual(next(vco_color_triangles(data))[1], colors)
        self.assertEqual(invert_lightness(colors[0]), (0, 65, 192, 255))
        self.assertEqual(invert_lightness((229, 229, 229, 128)), (26, 26, 26, 128))
        for color in colors:
            self.assertEqual(invert_lightness(invert_lightness(color)), color)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'color.dat'
            path.write_bytes(data)
            pdf = geometry_color_pdf(path)
            self.assertTrue(pdf.startswith(b'%PDF-1.4'))
            self.assertIn(b'/ShadingType 4', pdf)
            self.assertIn(bytes((0, 0, 0, 63, 128, 255)), pdf)
            self.assertIn(bytes((0, 0, 0, 0, 65, 192)), geometry_color_pdf(path, light=True))
            xref = int(pdf.split(b'startxref\n')[1].splitlines()[0])
            self.assertEqual(pdf[xref:xref + 4], b'xref')

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
            colored = list((release / 'datafiles/icons').glob('*.dat'))
            themed = list(output.glob("blender.themed.*.imageset"))
            multicolor = list(output.glob("blender.multicolor.*.imageset"))
            self.assertEqual(len(list(output.glob("*.imageset"))),
                             len(interface_names) + len(tool_names) + 2 * len(colored) + len(themed) + len(multicolor))
            for imageset in output.glob("*.imageset"):
                contents = json.loads((imageset / "Contents.json").read_text())
                if imageset.name.startswith(('blender.themed.', 'blender.multicolor.')):
                    self.assertEqual(contents['properties']['template-rendering-intent'], 'original')
                    self.assertEqual(contents['images'][1]['appearances'], [{'appearance': 'luminosity', 'value': 'dark'}])
                    continue
                if imageset.name.startswith('blender.tool.selected.'):
                    self.assertEqual(contents['properties']['template-rendering-intent'], 'original')
                    self.assertTrue(contents['properties']['preserves-vector-representation'])
                    self.assertEqual(contents['images'], [{'filename': 'icon.pdf', 'idiom': 'universal'}])
                    normal = output / imageset.name.replace('blender.tool.selected.', 'blender.tool.color.')
                    self.assertEqual((imageset / 'icon.pdf').read_bytes(), (normal / 'dark.pdf').read_bytes())
                    continue
                if imageset.name.startswith('blender.tool.color.'):
                    self.assertEqual(contents['properties']['template-rendering-intent'], 'original')
                    self.assertTrue(contents['properties']['preserves-vector-representation'])
                    self.assertEqual(contents['images'][1]['appearances'], [{'appearance': 'luminosity', 'value': 'dark'}])
                    self.assertTrue((imageset / 'light.pdf').read_bytes().startswith(b'%PDF-'))
                    self.assertTrue((imageset / 'dark.pdf').read_bytes().startswith(b'%PDF-'))
                    continue
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
            # Blender's dark and light themes color the same icons differently.
            light = (output / "blender.themed.modifier.imageset/light.svg").read_text()
            dark = (output / "blender.themed.modifier.imageset/dark.svg").read_text()
            # Light mode uses the iOS system palette; dark mode keeps Blender's theme.
            self.assertIn("#0088ff", light)
            self.assertIn("#74a2ff", dark)
            self.assertNotIn("#74a2ff", light)
            # Icons are not outlined, even where Blender's light theme draws an icon border.
            for svg in (light, dark):
                self.assertNotIn("stroke=\"#000\"", svg)
                self.assertNotIn('opacity="0.700"', svg)
            # Grey and white categories (scene, collection) follow the platform tint instead.
            self.assertFalse((output / "blender.themed.scene.imageset").exists())
            self.assertFalse((output / "blender.themed.group.imageset").exists())
            light = (output / "blender.multicolor.collection_color_01.imageset/light.svg").read_text()
            dark = (output / "blender.multicolor.collection_color_01.imageset/dark.svg").read_text()
            self.assertIn("#ff383c", light)
            self.assertIn("#e2605b", dark)
            # Collection icons drop Blender's black ring and cut the handle out of the box.
            for name in ("outliner_collection", "collection_color_01"):
                for variant in ("light", "dark"):
                    root = ET.parse(output / f"blender.multicolor.{name}.imageset/{variant}.svg").getroot()
                    paths = list(root.iter(f"{{{SVG}}}path"))
                    self.assertTrue(paths and all(
                        any(a.get("id", "").startswith("blender_collection") for a in root.iter() if path in list(a.iter()) and a is not path)
                        for path in paths), f"{name} {variant} keeps unpainted outline shapes")
                    self.assertTrue(any(path.get("fill-rule") == "evenodd" for path in paths))
            self.assertTrue((output / "blender.themed.outliner_ob_mesh.imageset").is_dir())
            self.assertFalse((output / "blender.themed.add.imageset").exists())
            stale = output / "blender.stale.imageset"
            stale.mkdir()
            export_catalog(release / "datafiles", manifest, output)
            self.assertFalse(stale.exists())

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires Apple CoreGraphics')
    def test_colored_pdf_rendering(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            geometry = root / 'triangle.dat'
            geometry.write_bytes(b'VCO\x00\xff\xff\x00\x00' + bytes((0, 0, 255, 0, 0, 255)) +
                                 bytes((63, 128, 255, 255)) * 3)
            dark, light, smooth, alpha = [root / (name + '.pdf') for name in ('dark', 'light', 'smooth', 'alpha')]
            dark.write_bytes(geometry_color_pdf(geometry))
            light.write_bytes(geometry_color_pdf(geometry, light=True))
            geometry.write_bytes(geometry.read_bytes()[:14] +
                                 bytes((255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255)))
            smooth.write_bytes(geometry_color_pdf(geometry))
            geometry.write_bytes(geometry.read_bytes()[:14] + bytes((229, 229, 229, 128)) * 3)
            alpha.write_bytes(geometry_color_pdf(geometry))
            renderer = Path(__file__).with_name('icon_pdf_render_test.swift')
            output = subprocess.check_output(['xcrun', 'swift', str(renderer), str(dark), str(light),
                                              str(smooth), str(alpha)], text=True, timeout=60)
            values = json.loads(output)
            for value in values:
                self.assertEqual((value['width'], value['height']), (36, 36))
                self.assertGreater(value['transparent'], 1000)
            self.assertEqual(values[0]['opaqueColors'], [0x3F80FF])
            self.assertEqual(values[1]['opaqueColors'], [0x0041C0])
            self.assertGreater(len(values[2]['opaqueColors']), 100)
            self.assertGreater(values[3]['translucent'], 1000)
            self.assertFalse(values[3]['opaqueColors'])


if __name__ == "__main__":
    unittest.main()
