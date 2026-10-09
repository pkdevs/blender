# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TemplatesTest(unittest.TestCase):
    def test_real_blender_seeds(self):
        binary = os.environ.get('BLENDER_TEST_BINARY', '/Applications/Blender.app/Contents/MacOS/Blender')
        result = subprocess.run([binary, '--background', '--factory-startup', '--python-exit-code', '1',
                                 '--python', str(ROOT / 'release/ios/templates/validate_templates.py')],
                                text=True, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: iPad template seeds', result.stdout)

    def test_native_chooser_and_creation(self):
        sdk = subprocess.check_output(['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'], text=True).strip()
        device = os.environ.get('IOS_TEST_SIMULATOR_UDID', '1EBB41D0-4CDC-4E24-BD9A-0F4818699324')
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'TemplatesTest.app'
            app.mkdir()
            bundle = 'org.blender.tests.templates'
            (app / 'Info.plist').write_bytes(plistlib.dumps({
                'CFBundleIdentifier': bundle, 'CFBundleExecutable': 'TemplatesTest',
                'CFBundleName': 'TemplatesTest', 'CFBundlePackageType': 'APPL',
                'CFBundleVersion': '1', 'CFBundleShortVersionString': '5.1.2',
                'MinimumOSVersion': '26.0', 'UIDeviceFamily': [1, 2], 'UILaunchScreen': {},
                'UIApplicationSceneManifest': {'UISceneConfigurations': {
                    'UIWindowSceneSessionRoleApplication': [{'UISceneConfigurationName': 'Tests',
                        'UISceneDelegateClassName': 'TemplatesTestScene'}]}},
            }))
            shutil.copytree(ROOT / 'release/ios/templates/library', app / 'Assets/5.1/datafiles/templates')
            result = subprocess.run(['xcrun', '--sdk', 'iphonesimulator', 'clang++', '-std=c++17',
                '-fno-objc-arc', '-isysroot', sdk, '-target', f'{os.uname().machine}-apple-ios26.0-simulator',
                '-I', str(ROOT / 'intern/ghost/intern'), '-framework', 'UIKit', '-framework', 'Foundation',
                '-framework', 'UniformTypeIdentifiers', '-framework', 'CoreGraphics', '-framework', 'QuartzCore',
                str(ROOT / 'intern/ghost/intern/GHOST_IOSDocument.mm'),
                str(ROOT / 'intern/ghost/intern/GHOST_IOSLauncher.mm'),
                str(ROOT / 'release/ios/templates_ui_test.mm'), '-o', str(app / 'TemplatesTest')],
                text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run(['codesign', '--force', '--sign', '-', str(app)], check=True, capture_output=True)
            subprocess.run(['xcrun', 'simctl', 'install', device, str(app)], check=True, capture_output=True)
            try:
                for name, arguments in [('dark', []), ('light-sheet', ['--light', '--sheet']),
                                        ('large-text', ['--large-text', '--sheet'])]:
                    with self.subTest(appearance=name):
                        result = subprocess.run(['xcrun', 'simctl', 'launch', '--console-pty', device, bundle, *arguments],
                            text=True, capture_output=True, timeout=60)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn('PASS: native templates', result.stdout + result.stderr)
                        artifacts = os.environ.get('IOS_TEST_ARTIFACT_DIR')
                        if artifacts:
                            container = subprocess.check_output(['xcrun', 'simctl', 'get_app_container', device, bundle, 'data'], text=True).strip()
                            Path(artifacts).mkdir(parents=True, exist_ok=True)
                            shutil.copy(Path(container) / 'Documents/templates.png', Path(artifacts) / (name + '.png'))
            finally:
                artifacts = os.environ.get('IOS_TEST_ARTIFACT_DIR')
                if artifacts:
                    container = subprocess.check_output(['xcrun', 'simctl', 'get_app_container', device, bundle, 'data'], text=True).strip()
                    shutil.copytree(Path(container) / 'Documents', artifacts, dirs_exist_ok=True)
                subprocess.run(['xcrun', 'simctl', 'uninstall', device, bundle], capture_output=True)
