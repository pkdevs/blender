# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the committed Starter asset library: validate it with Blender, and make sure the shipped
files don't record where they were built."""

import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
LIBRARY = ROOT / "assets/ios_starter"
SOURCES = ROOT / "release/ios/starter_assets/sources"
BLENDER = os.environ.get("BLENDER_BIN", "")
# Absolute paths a build machine could leave behind in packed data or saved UI.
LOCAL_PATHS = (b"/Users/", b"/home/", b"/opt/", b"/private/", b"/var/folders/")


def blend_bytes(path):
    data = path.read_bytes()
    if data[:4] == b"\x28\xb5\x2f\xfd":
        zstd = shutil.which("zstd")
        if zstd is None:
            raise unittest.SkipTest("Requires zstd to read compressed .blend files")
        data = subprocess.run([zstd, "-dc", str(path)], capture_output=True, check=True).stdout
    return data


class StarterLibraryTest(unittest.TestCase):
    def test_library_validates(self):
        if not os.path.exists(BLENDER):
            self.skipTest("Requires Blender; set BLENDER_BIN")
        result = subprocess.run([BLENDER, "--background", "--factory-startup", "--python",
                                 str(ROOT / "release/ios/starter_assets/validate_starter_library.py"),
                                 "--", "--library", str(LIBRARY), "--strict-time"],
                                text=True, capture_output=True, timeout=900)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:] + result.stderr[-2000:])
        self.assertIn(" 0 problems, 0 warnings", result.stdout)

    def test_files_record_no_local_paths(self):
        files = sorted(LIBRARY.glob("*.blend")) + sorted(SOURCES.glob("*.blend"))
        self.assertTrue(files, "No library files found")
        for path in files:
            data = blend_bytes(path)
            found = [local.decode() for local in LOCAL_PATHS if local in data]
            self.assertFalse(found, f"{path.name} records local paths: {found}")

    def test_catalogs_are_installed_with_the_library(self):
        catalogs = (LIBRARY / "blender_assets.cats.txt").read_text(encoding="utf-8")
        self.assertIn("VERSION 1", catalogs)
        for root in ("Objects", "Materials", "Generate", "Deform"):
            self.assertIn(f":{root}:", catalogs)


if __name__ == "__main__":
    unittest.main()
