#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Download the sources the iPad's bundled templates and Starter library are built from.

They are not kept in the repository, only what the app installs is. Each file is checked against
the SHA-256 in `asset_sources.json`. The human meshes are extracted from Blender Studio's bundle,
which needs Blender:

    ./release/ios/fetch_asset_sources.py --blender /path/to/blender

Then rebuild with `templates/build_character.py` or `starter_assets/build_starter_library.py`.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent


def download(url: str, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url) as response, open(target, "wb") as file:
        while chunk := response.read(1 << 20):
            file.write(chunk)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        while chunk := file.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--blender", help="Blender, to extract the human meshes from their bundle")
    args = parser.parse_args()
    manifest = json.loads((HERE / "asset_sources.json").read_text())

    for entry in manifest["files"]:
        target = HERE / entry["path"]
        if target.exists() and sha256(target) == entry["sha256"]:
            continue
        print("Downloading", entry["path"])
        download(entry["url"], target)
        if sha256(target) != entry["sha256"]:
            target.unlink()
            raise SystemExit("Checksum mismatch: " + entry["url"])

    humans = manifest["human_base_meshes"]
    target = HERE / humans["path"]
    if target.exists():
        return
    if not args.blender:
        print("Skipping", humans["path"], "(pass --blender to extract it)")
        return
    with tempfile.TemporaryDirectory() as folder:
        archive = Path(folder) / "bundle.zip"
        print("Downloading", humans["bundle_url"])
        download(humans["bundle_url"], archive)
        with zipfile.ZipFile(archive) as bundle:
            member = next(name for name in bundle.namelist() if name.endswith(humans["bundle_blend"]))
            bundle_blend = Path(bundle.extract(member, folder))
        subprocess.run([args.blender, "--background", "--factory-startup",
                        "--python", str(HERE / humans["script"]), "--",
                        str(bundle_blend), "--output", str(target)], check=True)


if __name__ == "__main__":
    main()
