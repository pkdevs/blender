# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise native Files presentation and Blender's asynchronous selector bridge."""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeFilesTest(unittest.TestCase):
    def test_blender_selector_bridge(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        source = (ROOT / "source/blender/windowmanager/intern/wm_apple_ios_file.cc").read_text()
        source = source[source.index("namespace {"):source.rindex("}  // namespace blender")]
        harness = Path(__file__).with_name("native_files_bridge_test.cc").read_text()
        harness = harness.replace("// BRIDGE_IMPLEMENTATION", source)
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "bridge.cc"
            executable = Path(directory) / "bridge"
            code.write_text(harness)
            build = subprocess.run([compiler, "-std=c++17", "-I", str(ROOT / "intern/ghost"),
                                    str(code), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_uikit_document_picker(self):
        if sys.platform != "darwin":
            self.skipTest("Requires Xcode and an iOS simulator")
        sdk = subprocess.check_output(
            ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip()
        devices = json.loads(subprocess.check_output(
            ["xcrun", "simctl", "list", "devices", "booted", "--json"], text=True))
        candidates = [d for runtime, entries in devices["devices"].items()
                      if ".iOS-" in runtime for d in entries if d["isAvailable"]]
        if not candidates:
            self.skipTest("No booted simulator")
        device = os.environ.get("IOS_TEST_SIMULATOR_UDID", candidates[0]["udid"])
        source = (ROOT / "intern/ghost/intern/GHOST_IOSDocumentPicker.mm").read_text()
        source = source.replace('#include "GHOST_WindowIOS.hh"',
                                '#include <atomic>\n#include <memory>\n#include <string>\n'
                                'class GHOST_WindowIOS { public: UIWindow *rootWindow; '
                                'std::shared_ptr<std::atomic<bool>> input_alive = '
                                'std::make_shared<std::atomic<bool>>(true); };')
        source += Path(__file__).with_name("native_files_test.mm").read_text()
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "NativeFilesTest.app"
            app.mkdir()
            bundle = "org.blender.tests.native-files"
            (app / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": bundle, "CFBundleExecutable": "NativeFilesTest",
                "CFBundleName": "NativeFilesTest", "CFBundlePackageType": "APPL",
                "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
                "MinimumOSVersion": "26.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
                "UIApplicationSceneManifest": {"UISceneConfigurations": {
                    "UIWindowSceneSessionRoleApplication": [{
                        "UISceneConfigurationName": "Tests", "UISceneDelegateClassName": "FilesTestScene",
                    }],
                }},
            }))
            code = Path(directory) / "files.mm"
            code.write_text(source)
            build = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fblocks", "-fno-objc-arc",
                "-I", str(ROOT / "intern/ghost"), "-isysroot", sdk,
                "-I", str(ROOT / "intern/ghost/intern"),
                "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation", "-framework", "UniformTypeIdentifiers",
                str(code), "-o", str(app / "NativeFilesTest"),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True, capture_output=True)
            subprocess.run(["xcrun", "simctl", "install", device, str(app)], check=True, capture_output=True)
            try:
                result = subprocess.run(["xcrun", "simctl", "launch", "--console-pty", device, bundle],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS: native document picker", result.stdout + result.stderr)
            finally:
                artifacts = os.environ.get("IOS_TEST_ARTIFACT_DIR")
                if artifacts:
                    container = subprocess.check_output(
                        ["xcrun", "simctl", "get_app_container", device, bundle, "data"], text=True).strip()
                    shutil.copytree(Path(container) / "Documents", artifacts, dirs_exist_ok=True)
                subprocess.run(["xcrun", "simctl", "uninstall", device, bundle], capture_output=True)


if __name__ == "__main__":
    unittest.main()
