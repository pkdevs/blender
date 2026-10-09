# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Copy a device app and package its native libraries as iOS frameworks.

Run before signing the output. Never modifies the input app. Python extensions
must already be linked as MH_DYLIB, rather than desktop MH_BUNDLE modules.
"""

import argparse
import hashlib
import plistlib
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def macho_type(path):
    with path.open('rb') as stream:
        header = stream.read(16)
    if len(header) == 16 and header[:4] == b'\xcf\xfa\xed\xfe':
        return struct.unpack('<4I', header)[3]
    return None


def framework_name(path, app):
    if path.suffix == '.so':
        parts = path.relative_to(app).parts
        base = parts.index('site-packages') + 1 if 'site-packages' in parts else parts.index('lib-dynload') + 1
        return '.'.join((*parts[base:-1], path.name.split('.')[0]))
    if path.name.startswith('libOpenImageDenoise'):
        return re.sub(r'(?:\.\d+)*\.dylib$', '', path.name).removeprefix('lib')
    # Preserve versions where multiple ABIs are shipped together.
    return path.name.removesuffix('.dylib').removeprefix('lib')


def dependencies(path):
    return [line.strip().split(' (compatibility version', 1)[0]
            for line in run('xcrun', 'otool', '-L', str(path)).splitlines()[1:]]


def verify_platform(binary, minimum_os):
    if run('xcrun', 'lipo', '-archs', str(binary)) != 'arm64':
        raise ValueError(f'Expected an arm64 device binary: {binary}')
    commands = run('xcrun', 'vtool', '-show-build', str(binary))
    platform = re.search(r'platform (\S+)', commands)
    minimum = re.search(r'minos (\d+(?:\.\d+)*)', commands)
    if not platform or platform.group(1) != 'IOS' or not minimum:
        raise ValueError(f'Expected an iOS device build: {binary}')
    version = lambda text: tuple((list(map(int, text.split('.'))) + [0, 0])[:3])
    if version(minimum.group(1)) > version(minimum_os):
        raise ValueError(f'{binary} requires {minimum.group(1)}, above app minimum {minimum_os}')


def _prepare(source, output, replacements):
    source, output = source.resolve(), output.resolve()
    if output.exists() or source == output or source in output.parents:
        raise ValueError('Output must be a new directory outside the input app')
    info = plistlib.loads((source / 'Info.plist').read_bytes())
    if info.get('CFBundleSupportedPlatforms') != ['iPhoneOS']:
        raise ValueError('Expected an iPhoneOS device application')
    verify_platform(source / info['CFBundleExecutable'], info['MinimumOSVersion'])
    candidates = [p for p in source.rglob('*') if p.is_file() and p.suffix in ('.dylib', '.so')]
    libraries = {}
    aliases = {}
    used_names = {}
    canonical = {}
    for path in candidates:
        real = path.resolve()
        if source not in real.parents:
            raise ValueError(f'Library escapes app: {path}')
        if real not in canonical:
            if 'Probe' in real.name or 'UITest' in real.name:
                raise ValueError(f'Development test library must not be distributed: {real}')
            replacement = replacements.get(real.name, real)
            if real.suffix == '.dylib' and replacement == real:
                matches = [p for p in replacements.values()
                           if p.suffix == '.dylib' and framework_name(p, source) == framework_name(real, source)]
                if len(matches) == 1:
                    replacement = matches[0]
            if macho_type(replacement) != 6:
                raise ValueError(f'Relink as an iOS dynamic library before packaging: {replacement}')
            verify_platform(replacement, info['MinimumOSVersion'])
            name = framework_name(real, source)
            if name in used_names:
                previous = used_names[name]
                old_binary = libraries[previous][1]
                if hashlib.sha256(old_binary.read_bytes()).digest() != hashlib.sha256(replacement.read_bytes()).digest():
                    raise ValueError(f'Conflicting framework binaries: {name}')
                canonical[real] = previous
            else:
                used_names[name] = real
                libraries[real] = (name, replacement)
                canonical[real] = real
        aliases[path.name] = libraries[canonical[real]][0]
    shutil.copytree(source, output, symlinks=True)
    frameworks = output / 'Frameworks'
    frameworks.mkdir(exist_ok=True)
    binaries = [output / info['CFBundleExecutable']]
    # App extensions, such as the thumbnailer, load the same libraries from the app's Frameworks.
    extensions = []
    for appex in sorted((output / 'PlugIns').glob('*.appex')):
        appex_info = plistlib.loads((appex / 'Info.plist').read_bytes())
        extensions.append(appex / appex_info['CFBundleExecutable'])
    binaries.extend(extensions)
    for real, (name, replacement) in libraries.items():
        framework = frameworks / (name + '.framework')
        framework.mkdir()
        binary = framework / name
        shutil.copy2(replacement, binary)
        metadata = {
            'CFBundleExecutable': name,
            'CFBundleIdentifier': info['CFBundleIdentifier'] + '.lib.' + re.sub(r'[^A-Za-z0-9.-]', '-', name),
            'CFBundleName': name,
            'CFBundlePackageType': 'FMWK',
            'CFBundleInfoDictionaryVersion': '6.0',
            'CFBundleShortVersionString': '1.0',
            'CFBundleVersion': '1',
            'CFBundleSupportedPlatforms': ['iPhoneOS'],
            'MinimumOSVersion': info['MinimumOSVersion'],
        }
        (framework / 'Info.plist').write_bytes(plistlib.dumps(metadata))
        run('xcrun', 'install_name_tool', '-id', f'@rpath/{name}.framework/{name}', str(binary))
        binaries.append(binary)
        if real.suffix == '.so':
            # The dependency build's ABI tag may differ from embedded Python's
            # iPhoneOS tag. The generic .fwork suffix works with either runtime.
            marker = output / real.relative_to(source).with_name(real.name.split('.')[0] + '.fwork')
            marker.write_text(str(binary.relative_to(output)) + '\n')
            binary.with_name(binary.name + '.origin').write_text(str(marker.relative_to(output)) + '\n')
    for binary in binaries:
        changes = []
        for dependency in dependencies(binary):
            name = aliases.get(Path(dependency).name)
            if name:
                changes.extend(['-change', dependency, f'@rpath/{name}.framework/{name}'])
            elif not dependency.startswith(('/System/Library/', '/usr/lib/', '@rpath/')):
                raise ValueError(f'Unresolved dependency {dependency} in {binary}')
        if changes:
            run('xcrun', 'install_name_tool', *changes, str(binary))
    executable = binaries[0]
    load_commands = run('xcrun', 'otool', '-l', str(executable))
    if 'path @executable_path/Frameworks ' not in load_commands:
        run('xcrun', 'install_name_tool', '-add_rpath', '@executable_path/Frameworks', str(executable))
    for extension in extensions:
        # `Blender.app/PlugIns/<name>.appex/<executable>` reaches `Blender.app/Frameworks`.
        load_commands = run('xcrun', 'otool', '-l', str(extension))
        if 'path @executable_path/../../Frameworks ' not in load_commands:
            run('xcrun', 'install_name_tool', '-add_rpath', '@executable_path/../../Frameworks',
                str(extension))
    for path in candidates:
        (output / path.relative_to(source)).unlink()
    # Python's development configuration can include linker objects and static archives.
    # They are not runtime resources and cannot be shipped as loose executable material.
    for path in (output / 'Assets').rglob('*'):
        if path.is_file() and (path.suffix == '.a' or macho_type(path) == 1):
            path.unlink()
    for signature in list(output.rglob('_CodeSignature')):
        shutil.rmtree(signature)
    # A distribution app must not contain development probes or an injected test library.
    for path in output.rglob('*'):
        if path.is_file() and macho_type(path) is not None:
            if path != executable and path not in extensions and frameworks not in path.parents:
                raise ValueError(f'Unexpected executable outside Frameworks: {path}')
    for binary in binaries:
        for dependency in dependencies(binary):
            if dependency.startswith('@rpath/'):
                relative = dependency.removeprefix('@rpath/')
                if not (frameworks / relative).is_file() and not relative.startswith('libswift'):
                    raise ValueError(f'Missing packaged dependency: {dependency}')
    return len(libraries)


def prepare(source, output, replacements):
    source, output = source.resolve(), output.resolve()
    if output.exists() or source == output or source in output.parents:
        raise ValueError('Output must be a new directory outside the input app')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='testflight-', dir=output.parent) as temporary:
        staged = Path(temporary) / output.name
        count = _prepare(source, staged, replacements)
        staged.rename(output)
    print(f'Packaged {count} frameworks into {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--replacement-directory', type=Path)
    args = parser.parse_args()
    replacements = {}
    if args.replacement_directory:
        for path in args.replacement_directory.rglob('*'):
            if path.is_file() and path.suffix in ('.so', '.dylib'):
                if path.name in replacements:
                    raise ValueError(f'Duplicate replacement: {path.name}')
                replacements[path.name] = path
    prepare(args.source, args.output, replacements)


if __name__ == '__main__':
    main()
