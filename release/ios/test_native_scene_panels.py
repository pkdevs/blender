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


class NativeScenePanelsTest(unittest.TestCase):
    def test_real_blender_data_and_edits(self):
        blender = Path(os.environ.get('BLENDER_TEST_BINARY', '/Applications/Blender.app/Contents/MacOS/Blender'))
        if not blender.exists():
            self.skipTest('Set BLENDER_TEST_BINARY to a Blender 5.1 executable')
        result = subprocess.run([str(blender), '--background', '--factory-startup',
                                 '--python-exit-code', '1', '--python',
                                 str(ROOT / 'release/ios/scene_panels_engine_test.py')],
                                text=True, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: native scene panels', result.stdout)

    def test_native_controls_and_floating_layout(self):
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
            executable = Path(directory) / 'scene-panels-test'
            result = subprocess.run([
                'xcrun', '--sdk', 'iphonesimulator', 'clang++', '-std=c++17', '-fno-objc-arc',
                '-isysroot', sdk.stdout.strip(), '-target', f'{os.uname().machine}-apple-ios26.0-simulator',
                '-framework', 'UIKit', '-framework', 'Foundation', '-framework', 'QuartzCore',
                '-framework', 'CoreGraphics', '-I', str(ROOT / 'intern/ghost/intern'),
                str(ROOT / 'intern/ghost/intern/GHOST_IOSScenePanels.mm'),
                str(ROOT / 'release/ios/scene_panels_ui_test.mm'), '-o', str(executable),
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(['xcrun', 'simctl', 'spawn', device, str(executable)],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS: floating scene panels', result.stderr)
