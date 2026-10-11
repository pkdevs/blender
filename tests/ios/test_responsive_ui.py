# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Thread handoff stress and real UIKit/Metal presentation during engine stalls."""

import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ResponsiveUITest(unittest.TestCase):
    def test_mailbox_ownership_ordering_and_stalled_consumer(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "mailbox"
            command = ["xcrun", "clang++", "-std=c++17", "-pthread", "-g",
                       "-I", str(ROOT / "intern/ghost"),
                       str(Path(__file__).with_name("responsive_mailbox_test.cc")),
                       "-o", str(executable)]
            if os.environ.get("IOS_TEST_TSAN"):
                command += ["-fsanitize=thread"]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == "__main__":
    unittest.main()
