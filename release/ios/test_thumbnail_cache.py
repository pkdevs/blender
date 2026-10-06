# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class ThumbnailCacheTest(unittest.TestCase):
    def test_ios_uses_writable_cache_without_changing_macos(self):
        compiler = shutil.which('clang++')
        if not compiler:
            self.skipTest('clang++ is required')
        source = (ROOT / 'source/blender/imbuf/intern/thumbs.cc').read_text()
        function = source.split('static bool get_thumb_dir(', 1)[1].split('\n#undef THUMBNAILS', 1)[0]
        harness = r'''
#include <cassert>
#include <cstring>
#define FILE_MAX 4096
#define SEP_STR "/"
#define THUMBNAILS ".thumbnails"
enum ThumbSize { THB_NORMAL, THB_LARGE, THB_FAIL };
const char *home = "/sandbox";
const char *BLI_dir_home() { return home; }
size_t BLI_strncpy_rlen(char *dst, const char *src, size_t capacity) {
  const size_t size = strlen(src) < capacity ? strlen(src) : capacity - 1;
  memcpy(dst, src, size);
  dst[size] = '\0';
  return size;
}
static bool get_thumb_dir(''' + function + r'''
int main() {
  char path[FILE_MAX];
#if defined(APPLE_TARGET_IOS) && APPLE_TARGET_IOS
  const char *prefix = "/sandbox/Library/Caches/.thumbnails/";
#else
  const char *prefix = "/sandbox/.thumbnails/";
#endif
  const char *suffixes[] = {"normal/", "large/", "fail/blender/"};
  for (int i = 0; i < 3; ++i) {
    assert(get_thumb_dir(path, ThumbSize(i)));
    assert(strncmp(path, prefix, strlen(prefix)) == 0);
    assert(strcmp(path + strlen(prefix), suffixes[i]) == 0);
  }
  home = nullptr;
  assert(!get_thumb_dir(path, THB_LARGE));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = pathlib.Path(directory) / 'cache.cc'
            binary = pathlib.Path(directory) / 'cache-test'
            source_path.write_text(harness)
            for flags in ([], ['-DWITH_APPLE_CROSSPLATFORM', '-DAPPLE_TARGET_IOS=1']):
                with self.subTest(flags=flags):
                    result = subprocess.run(
                        [compiler, '-std=c++17', *flags, str(source_path), '-o', str(binary)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
