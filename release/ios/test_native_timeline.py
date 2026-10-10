# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NativeTimelineTest(unittest.TestCase):
    def test_keyframes_in_real_blender(self):
        blender = Path(os.environ.get('BLENDER_TEST_BINARY', '/Applications/Blender.app/Contents/MacOS/Blender'))
        if not blender.exists():
            self.skipTest('Set BLENDER_TEST_BINARY to a Blender 5.1 executable')
        result = subprocess.run([str(blender), '--background', '--factory-startup',
                                 '--python-exit-code', '1', '--python',
                                 str(ROOT / 'release/ios/native_timeline_engine_test.py')],
                                text=True, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: native timeline keys', result.stdout)


if __name__ == '__main__':
    unittest.main()
