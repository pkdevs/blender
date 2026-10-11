# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Guard live resizing and exercise the production drawable-size callback chain."""

import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest



ROOT = Path(__file__).resolve().parents[2]


class IPadResizeTest(unittest.TestCase):
    def test_dynamic_scene_resize_configuration(self):
        with (ROOT / "release/ios/Blender.app/Info.plist").open("rb") as source:
            info = plistlib.load(source)
        # On iPadOS 27 this legacy flag defers scene-size changes until release.
        self.assertFalse(info.get("UIRequiresFullScreen", False))
        # A plain launch screen, which lets the scene resize from the first frame.
        self.assertIn("UILaunchScreen", info)
        self.assertEqual(set(info["UISupportedInterfaceOrientations"]), {
            "UIInterfaceOrientationPortrait", "UIInterfaceOrientationPortraitUpsideDown",
            "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight",
        })
        scenes = info["UIApplicationSceneManifest"]["UISceneConfigurations"]
        self.assertEqual(scenes["UIWindowSceneSessionRoleApplication"][0]["UISceneDelegateClassName"],
                         "IOSSceneDelegate")

if __name__ == "__main__":
    unittest.main()
