# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
import os
from pathlib import Path
import subprocess
import unittest


class CharacterTemplateTest(unittest.TestCase):
    def test_script_free_rig_and_animation(self):
        binary = os.environ.get('BLENDER_TEST_BINARY', '/Applications/Blender.app/Contents/MacOS/Blender')
        test = Path(__file__).resolve().parent / 'templates/validate_character.py'
        result = subprocess.run([binary, '--background', '--factory-startup', '--disable-autoexec',
                                 '--python-exit-code', '1', '--python', str(test)],
                                text=True, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: Character Animation', result.stdout)
