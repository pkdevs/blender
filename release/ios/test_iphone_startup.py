# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""A simulator can have no passwd entry and no USER environment variable."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class IPhoneStartupTest(unittest.TestCase):
    def test_username_lookup_without_a_record(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("Requires a C++ compiler")
        root = Path(__file__).resolve().parents[2]
        source = (root / "extern/glog/src/utilities.cc").read_text()
        start = source.index("static void MyUserNameInitializer()")
        method = source[start:source.index("REGISTER_MODULE_INITIALIZER", start)]
        harness = r'''
#include <string>
#include <cassert>
#include <cstdio>
#include <sys/types.h>
#define HAVE_PWD_H 1
#define HAVE_UNISTD_H 1
using std::string;
struct passwd { const char *pw_name; };
static string g_my_user_name;
static int scenario;
static const char *test_getenv(const char *) { return scenario == 3 ? "mobile" : nullptr; }
static uid_t test_geteuid() { return 501; }
static int test_getpwuid_r(uid_t, passwd *pwd, char *, size_t, passwd **result) {
  *result = nullptr;
  if (scenario == 1) return 1;
  if (scenario == 2) { pwd->pw_name = "simulator"; *result = pwd; }
  return 0;
}
#define getenv test_getenv
#define geteuid test_geteuid
#define getpwuid_r test_getpwuid_r
''' + method + r'''
int main() {
  const char *expected[] = {"uid501", "uid501", "simulator", "mobile"};
  for (scenario = 0; scenario < 4; scenario++) {
    MyUserNameInitializer();
    assert(g_my_user_name == expected[scenario]);
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / "startup.cc"
            executable = Path(directory) / "startup"
            source_file.write_text(harness)
            subprocess.run([compiler, "-std=c++17", str(source_file), "-o", str(executable)],
                           check=True, capture_output=True)
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
