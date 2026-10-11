# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Test native warning controls and the engine-owned asynchronous dialog lifecycle."""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeDialogsTest(unittest.TestCase):
    def test_native_warning_controls(self):
        device = os.environ.get("IOS_TEST_SIMULATOR_UDID")
        if not device:
            self.skipTest("Set IOS_TEST_SIMULATOR_UDID to an isolated booted simulator")
        sdk = subprocess.check_output(
            ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip()
        source = (ROOT / "source/blender/windowmanager/apple_ios/wm_apple_ios_alerts.mm").read_text()
        source = source.replace('#include "GHOST_IOSNativeInterface.hh"',
                                '#include <atomic>\n#include <memory>\n'
                                'class GHOST_WindowIOS { public: UIWindow *rootWindow; '
                                'std::shared_ptr<std::atomic<bool>> input_alive = '
                                'std::make_shared<std::atomic<bool>>(true); };\n'
                                'static UIWindow *GHOST_ios_window_native(GHOST_WindowIOS *w) '
                                '{ return w->rootWindow; }\n'
                                'static std::shared_ptr<std::atomic<bool>> '
                                'GHOST_ios_window_alive(GHOST_WindowIOS *w) { return w->input_alive; }')
        source += Path(__file__).with_name("native_dialogs_test.mm").read_text()
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "NativeDialogsTest.app"
            app.mkdir()
            bundle = "org.blender.tests.native-dialogs"
            (app / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": bundle, "CFBundleExecutable": "NativeDialogsTest",
                "CFBundleName": "NativeDialogsTest", "CFBundlePackageType": "APPL",
                "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
                "MinimumOSVersion": "26.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
                "UIApplicationSceneManifest": {"UISceneConfigurations": {
                    "UIWindowSceneSessionRoleApplication": [{
                        "UISceneConfigurationName": "Tests", "UISceneDelegateClassName": "DialogTestScene",
                    }],
                }},
            }))
            code = Path(directory) / "dialogs.mm"
            code.write_text(source)
            build = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fblocks", "-fno-objc-arc",
                "-I", str(ROOT / "source/blender/windowmanager/apple_ios"), "-isysroot", sdk,
                "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation",
                str(code), "-o", str(app / "NativeDialogsTest"),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True, capture_output=True)
            subprocess.run(["xcrun", "simctl", "install", device, str(app)], check=True, capture_output=True)
            try:
                result = subprocess.run(["xcrun", "simctl", "launch", "--console-pty", device, bundle],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS: native alerts", result.stdout + result.stderr)
            finally:
                artifacts = os.environ.get("IOS_TEST_ARTIFACT_DIR")
                if artifacts:
                    container = subprocess.check_output(
                        ["xcrun", "simctl", "get_app_container", device, bundle, "data"], text=True).strip()
                    shutil.copytree(Path(container) / "Documents", artifacts, dirs_exist_ok=True)
                subprocess.run(["xcrun", "simctl", "uninstall", device, bundle], capture_output=True)


if __name__ == "__main__":
    unittest.main()
