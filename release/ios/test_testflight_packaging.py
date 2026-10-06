# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise distribution packaging with real iOS Mach-O dependencies."""

import plistlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from prepare_testflight import dependencies, macho_type, prepare


@unittest.skipUnless(sys.platform == 'darwin', 'Requires the iPhoneOS SDK')
class TestFlightPackagingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'Input.app'
        self.lib = self.app / 'Assets/lib'
        self.lib.mkdir(parents=True)
        self.sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
        (self.app / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': 'org.blender.packaging-test',
            'CFBundleExecutable': 'Test',
            'CFBundleSupportedPlatforms': ['iPhoneOS'],
            'MinimumOSVersion': '27.0',
        }))

    def compile(self, name, source, *arguments):
        path = self.root / (name + '.c')
        path.write_text(source)
        subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64',
                        '-isysroot', self.sdk, '-miphoneos-version-min=27.0',
                        '-Wl,-headerpad_max_install_names', str(path), *map(str, arguments)],
                       check=True, capture_output=True)

    def test_relocates_dependency_graph_and_python_extensions(self):
        library = self.lib / 'libExample.1.dylib'
        self.compile('lib', 'int example(void) { return 42; }', '-dynamiclib',
                     '-Wl,-install_name,@rpath/libExample.1.dylib', '-o', library)
        (self.lib / 'libExample.dylib').symlink_to(library.name)
        self.compile('app', 'int example(void); int main(void) { return example(); }',
                     library, '-o', self.app / 'Test')
        modules = self.app / 'Assets/5.1/python/lib/python3.13/site-packages/demo'
        modules.mkdir(parents=True)
        module = modules / '_native.abi3.so'
        self.compile('module', 'int example(void); int native(void) { return example(); }',
                     '-dynamiclib', library, '-o', module)
        before = (self.app / 'Test').read_bytes()
        output = self.root / 'Output.app'
        prepare(self.app, output, {})
        self.assertEqual((self.app / 'Test').read_bytes(), before)
        self.assertTrue(module.exists())
        self.assertFalse(list(output.rglob('*.dylib')))
        self.assertFalse(list(output.rglob('*.so')))
        self.assertIn('@rpath/Example.1.framework/Example.1', dependencies(output / 'Test'))
        marker = output / module.relative_to(self.app).with_name('_native.fwork')
        extension = output / marker.read_text().strip()
        self.assertEqual(macho_type(extension), 6)
        self.assertEqual(extension.with_name(extension.name + '.origin').read_text().strip(),
                         str(marker.relative_to(output)))
        self.assertIn('@rpath/Example.1.framework/Example.1', dependencies(extension))
        with self.assertRaises(ValueError):
            prepare(self.app, output, {})

    def test_rejects_desktop_bundle_modules_without_modifying_source(self):
        self.compile('app', 'int main(void) { return 0; }', '-o', self.app / 'Test')
        self.compile('bundle', 'int example(void) { return 42; }', '-bundle',
                     '-o', self.lib / 'bad.so')
        with self.assertRaisesRegex(ValueError, 'Relink'):
            prepare(self.app, self.root / 'Rejected.app', {})
        self.assertTrue((self.lib / 'bad.so').exists())
        self.assertFalse((self.root / 'Rejected.app').exists())


if __name__ == '__main__':
    unittest.main()
