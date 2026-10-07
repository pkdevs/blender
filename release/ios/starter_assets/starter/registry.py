# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Asset registry: every Starter asset is declared once with its catalog and metadata."""

import dataclasses
import uuid

AUTHOR = "Blender Foundation"
LICENSE = "CC0"
COPYRIGHT = ""

# Stable catalog UUIDs: derived from the catalog path, so rebuilds never change them.
CATALOG_NAMESPACE = uuid.UUID("6f3c2e0a-5b1d-4c8e-9a47-2d61b0e5c9f3")

# Which `.blend` file each kind of asset is written to.
KIND_FILES = {
    "object": "starter_objects.blend",
    "collection": "starter_scenes.blend",
    "material": "starter_materials.blend",
    "modifier": "starter_modifiers.blend",
}


@dataclasses.dataclass
class AssetSpec:
    name: str
    kind: str
    catalog: str
    description: str
    tags: tuple
    build: object
    # Mesh the preview applies a material or modifier to: "sphere", "cube", "monkey", "plane", "torus".
    preview_base: str = "sphere"
    # Camera direction for the preview: "front" (three-quarter), "top" or "side".
    preview_view: str = "front"
    module: str = ""


ASSETS = []


def catalog_uuid(path):
    return str(uuid.uuid5(CATALOG_NAMESPACE, path))


def asset(name, kind, catalog, description, tags, preview_base="sphere", preview_view="front"):
    """Register a builder function returning the asset's data-block."""
    if kind not in KIND_FILES:
        raise ValueError(f"Unknown asset kind {kind!r} for {name!r}")
    if not description.endswith("."):
        raise ValueError(f"Description of {name!r} must be a full sentence")
    if len(tags) < 2:
        raise ValueError(f"{name!r} needs at least two search tags")

    def register(function):
        ASSETS.append(AssetSpec(name, kind, catalog, description, tuple(tags), function,
                                preview_base, preview_view, function.__module__))
        return function
    return register


def catalog_paths(specs):
    """Every catalog path the given assets use, including parents, sorted."""
    paths = set()
    for spec in specs:
        parts = spec.catalog.split("/")
        for i in range(1, len(parts) + 1):
            paths.add("/".join(parts[:i]))
    return sorted(paths)


def write_catalog_file(path, specs):
    lines = [
        "# This is an Asset Catalog Definition file for Blender.",
        "#",
        "# Empty lines and lines starting with `#` will be ignored.",
        "# The first non-ignored line should be the version indicator.",
        '# Other lines are of the format "UUID:catalog/path/for/assets:simple catalog name"',
        "",
        "VERSION 1",
        "",
    ]
    for catalog in catalog_paths(specs):
        lines.append(f"{catalog_uuid(catalog)}:{catalog}:{catalog.replace('/', '-')}")
    with open(path, "w", encoding="utf-8") as file:
        file.write("\n".join(lines) + "\n")


def mark(id_data, spec):
    """Mark a built data-block as a Starter asset."""
    id_data.asset_mark()
    data = id_data.asset_data
    data.catalog_id = catalog_uuid(spec.catalog)
    data.description = spec.description
    data.author = AUTHOR
    data.license = LICENSE
    for tag in spec.tags:
        data.tags.new(tag, skip_if_exists=True)
