# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NativeAssetsTest(unittest.TestCase):
    def test_asset_pane_search_and_drag(self):
        sdk = subprocess.run(['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'],
                             text=True, capture_output=True)
        if sdk.returncode:
            self.skipTest('Requires an iOS Simulator SDK')
        devices = json.loads(subprocess.check_output(['xcrun', 'simctl', 'list', 'devices', 'booted', '--json']))
        candidates = [device for runtime, entries in devices['devices'].items()
                      if '.iOS-' in runtime for device in entries if device['isAvailable']]
        if not candidates:
            self.skipTest('Requires a booted iOS simulator')
        device = os.environ.get('IOS_TEST_SIMULATOR_UDID', candidates[0]['udid'])
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'assets-test'
            result = subprocess.run([
                'xcrun', '--sdk', 'iphonesimulator', 'clang++', '-std=c++17', '-fno-objc-arc',
                '-isysroot', sdk.stdout.strip(), '-target', f'{os.uname().machine}-apple-ios26.0-simulator',
                '-framework', 'UIKit', '-framework', 'Foundation', '-framework', 'QuartzCore',
                '-framework', 'CoreGraphics', '-I', str(ROOT / 'intern/ghost/intern'),
                str(ROOT / 'intern/ghost/intern/GHOST_IOSAssets.mm'),
                str(ROOT / 'release/ios/assets_ui_test.mm'), '-o', str(executable),
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(['xcrun', 'simctl', 'spawn', device, str(executable)],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS: native asset pane', result.stderr)


if __name__ == '__main__':
    unittest.main()
