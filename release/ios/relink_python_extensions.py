# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Relink existing Meson/Ninja Python extension objects as iOS framework binaries.

The input build is unchanged. Use the output as prepare_testflight.py's replacement
directory. This is needed while the dependency build uses Python's Darwin ABI,
whose extension-module linker defaults to MH_BUNDLE rather than MH_DYLIB.
"""

import argparse
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--minimum-os', default='27.0')
    args = parser.parse_args()
    build = args.build.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    count = 0
    for binary in sorted(build.rglob('*.so')):
        target = str(binary.relative_to(build))
        command = subprocess.check_output(['ninja', '-t', 'commands', '-s', target], cwd=build, text=True).strip()
        words = shlex.split(command)
        if '-bundle' not in words or words.count('-o') != 1 or any(w in words for w in ('&&', ';', '|')):
            raise ValueError(f'Unsupported extension link command: {target}')
        destination = output / binary.relative_to(build)
        if destination.exists():
            raise ValueError(f'Output already exists: {destination}')
        destination.parent.mkdir(parents=True, exist_ok=True)
        words[words.index('-bundle')] = '-dynamiclib'
        words[words.index('-o') + 1] = str(destination)
        for i, word in enumerate(words):
            if word == '-isysroot':
                words[i + 1] = sdk
            elif word.startswith('-miphoneos-version-min='):
                words[i] = '-miphoneos-version-min=' + args.minimum_os
        subprocess.run(words, cwd=build, check=True)
        count += 1
    if count == 0:
        raise ValueError('No extension modules found')
    print(f'Relinked {count} Python extensions in {output}')


if __name__ == '__main__':
    main()
