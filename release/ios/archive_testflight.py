# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sign a prepared device bundle and place it in an Xcode archive for export.

The profile and signing identity must already be available. Xcode's exportArchive
step handles App Store distribution signing; this script creates a local archive.
"""

import argparse
import datetime
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path


def archive(app, output, profile, identity, dsym=None):
    app, output = app.resolve(), output.resolve()
    if output.exists() or app in output.parents:
        raise ValueError('Archive must be a new path outside the input app')
    info = plistlib.loads((app / 'Info.plist').read_bytes())
    provisioning = plistlib.loads(subprocess.check_output(['security', 'cms', '-D', '-i', str(profile)]))
    team = provisioning['TeamIdentifier'][0]
    prefix = provisioning['ApplicationIdentifierPrefix'][0]
    identifier = prefix + '.' + info['CFBundleIdentifier']
    allowed = provisioning['Entitlements']['application-identifier']
    if identifier != allowed and not (allowed.endswith('.*') and identifier.startswith(allowed[:-1])):
        raise ValueError('Provisioning profile does not cover the app identifier')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='archive-', dir=output.parent) as temporary:
        staged = Path(temporary) / output.name
        bundle = staged / 'Products/Applications' / app.name
        shutil.copytree(app, bundle, symlinks=True)
        shutil.copy2(profile, bundle / 'embedded.mobileprovision')
        entitlements = {
            'application-identifier': identifier,
            'com.apple.developer.team-identifier': team,
            'get-task-allow': provisioning['Entitlements'].get('get-task-allow', False),
        }
        if 'keychain-access-groups' in provisioning['Entitlements']:
            entitlements['keychain-access-groups'] = [identifier]
        entitlement_path = Path(temporary) / 'entitlements.plist'
        entitlement_path.write_bytes(plistlib.dumps(entitlements))
        for framework in sorted((bundle / 'Frameworks').glob('*.framework')):
            subprocess.run(['codesign', '--force', '--sign', identity, '--timestamp=none', str(framework)], check=True)
        subprocess.run(['codesign', '--force', '--sign', identity, '--timestamp=none',
                        '--entitlements', str(entitlement_path), '--generate-entitlement-der', str(bundle)], check=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
        signed = subprocess.run(['codesign', '-dvv', str(bundle)], capture_output=True, text=True, check=True)
        authority = next(line.removeprefix('Authority=') for line in signed.stderr.splitlines()
                         if line.startswith('Authority='))
        metadata = {
            'ArchiveVersion': 2,
            'CreationDate': datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None),
            'Name': info.get('CFBundleName', 'Blender'),
            'SchemeName': 'blender',
            'ApplicationProperties': {
                'ApplicationPath': 'Applications/' + app.name,
                'Architectures': ['arm64'],
                'CFBundleIdentifier': info['CFBundleIdentifier'],
                'CFBundleShortVersionString': info['CFBundleShortVersionString'],
                'CFBundleVersion': info['CFBundleVersion'],
                'SigningIdentity': authority,
                'Team': team,
            },
        }
        (staged / 'Info.plist').write_bytes(plistlib.dumps(metadata))
        if dsym:
            shutil.copytree(dsym, staged / 'dSYMs' / dsym.name)
        staged.rename(output)
    print(f'Created and verified {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--dsym', type=Path)
    args = parser.parse_args()
    archive(args.app, args.output, args.profile, args.identity, args.dsym)


if __name__ == '__main__':
    main()
