# Native iPad Icons

`blender_icons.json` maps native UI semantic keys to Blender source artwork.
Tools use their `builtin.*` identifiers, settings use their setting/value pair,
and `command.*` keys use the stable `WMIOSUICommand` values. Mode-specific tool
keys override the common tool key. SF Symbols remain the fallback for controls
without a mapped Blender equivalent. The bottom action strip is stricter: it uses
operator/property-specific Blender artwork, or explicitly approved SF fallbacks
for Undo, Redo, Duplicate, and Delete. Other unmapped actions and X/Y/Z constraints
are text-only, with no placeholder icon or reserved icon spacing.

Projected menus carry each Blender button's actual icon identifier, including
submenu headings and enum entries. UIKit resolves these directly to `blender.*`
template assets; entries without artwork stay iconless instead of guessing from
their operator. The same loader is used by the native menu bar, popup menus, and
Pencil menu. Tool groups similarly use their live `ToolDef.icon` names.

The iOS CMake build automatically runs `export_icons.py`, then compiles the
resulting asset catalog together with the app icon using Apple's `actool`.
Both catalogs must be compiled in one invocation to retain both in `Assets.car`.
The generated catalog is in `source/creator/BlenderIcons.xcassets` in the build
directory. No generated icon copies are checked into the repository.

For a standalone export, from the repository root:

```sh
python3 release/ios/export_icons.py --output /tmp/BlenderIcons.xcassets
python3 -m unittest discover -s release/ios -p 'test_export_icons.py'
```

- Interface artwork comes from `release/datafiles/icons_svg/*.svg`.
- Tool artwork comes from `release/datafiles/icons/*.dat`, the vector-triangle
  format documented in `release/datafiles/blender_icons_geom.py`.
- Menu/control images have a 24-point square canvas. The tool rail uses separate
  `blender.tool.*` assets at 36 points, retaining the same vector artwork and
  padding without enlarging menus or changing the 52-point tool buttons.
  Both sizes keep their aspect ratio intact.
  SVG transparency and secondary-detail opacity are retained. Tool colors are
  converted to monochrome value, with dark details becoming alpha cut-outs;
  triangle gradients and drawing order are retained in an SVG luminance mask.
- Catalog entries preserve vector data and specify template rendering. The
  UIKit loader also requests `UIImageRenderingModeAlwaysTemplate`; controls use
  dynamic `UIColor.labelColor`, not a baked light/dark RGB color. UIKit handles
  appearance, increased contrast, and control-state tinting.

The floating tool palette prefers separate `blender.tool.color.*` assets, including
its grouped-tool menus. These preserve the original VCO vertex colors, opacity,
and smooth color interpolation using PDF triangle-mesh shading. Vector PDF is
used because SVG does not have a CoreSVG-compatible Gouraud triangle primitive.
Light and dark asset variants follow Blender's own HSL lightness inversion for
light toolbars. UIKit renders these as original artwork, so the app's blue
selection highlight does not recolor them. Icons without colored tool artwork
continue to use the template fallback; other menus and controls stay monochrome.

All bundled SVG interface icons and vector-triangle tool icons are exported.
Add semantic mappings to the manifest for native controls that need named aliases. Missing
sources or unsupported SVG constructs fail the export instead of silently
shipping an empty asset. Source artwork and its original licensing stay in place.
