#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Make pre-compiled libraries for another Apple platform from the iOS ones.

iOS and visionOS share the same arm64 ABI and C/C++ runtime; libraries differ only in the
platform their Mach-O files are marked as built for. This copies a library directory, such as
`lib/ios_arm64`, and marks every object, archive member, dylib and Python module for the target
platform, so the dependencies need not be rebuilt for visionOS:

    ./build_files/utils/apple_retarget_libs.py lib/ios_arm64 lib/visionos_arm64 --platform xros
    ./build_files/utils/apple_retarget_libs.py lib/ios-simulator_arm64 \\
        lib/visionos-simulator_arm64 --platform xrsimulator
"""

import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

MACHO_MAGICS = {b"\xcf\xfa\xed\xfe"}
FAT_MAGICS = {b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"}
AR_MAGIC = b"!<arch>\n"


def file_kind(path: Path) -> str:
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return ""
    if head == AR_MAGIC:
        return "archive"
    if head[:4] in MACHO_MAGICS or head[:4] in FAT_MAGICS:
        return "macho"
    return ""


LC_BUILD_VERSION = 0x32
PLATFORMS = {"xros": 11, "xrsimulator": 12}
IOS_PLATFORMS = {2, 7}
# Only code that is linked or loaded: build tools in the library folders run on the Mac.
MH_OBJECT, MH_DYLIB, MH_BUNDLE = 1, 6, 8


def encode_version(version: str) -> int:
    parts = [int(p) for p in version.split(".")] + [0, 0]
    return (parts[0] << 16) | (parts[1] << 8) | parts[2]


def retag_bytes(data: bytearray, platform: int, version: int) -> bool:
    """Rewrite the platform and versions of a thin 64-bit Mach-O's `LC_BUILD_VERSION` in place.
    The command keeps its size, so nothing else in the file moves."""
    if bytes(data[:4]) != b"\xcf\xfa\xed\xfe":
        raise ValueError("not a thin 64-bit Mach-O file")
    ncmds, = struct.unpack_from("<I", data, 16)
    offset = 32
    found = False
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, offset)
        if cmd == LC_BUILD_VERSION:
            current, = struct.unpack_from("<I", data, offset + 8)
            if current not in IOS_PLATFORMS:
                raise ValueError("not built for iOS: platform {:d}".format(current))
            struct.pack_into("<III", data, offset + 8, platform, version, version)
            found = True
        offset += size
    return found


def retag(path: Path, platform: int, version: str) -> None:
    """Mark a Mach-O file as built for `platform`, keeping it otherwise untouched."""
    data = bytearray(path.read_bytes())
    path.chmod(path.stat().st_mode | 0o200)
    if not retag_bytes(data, platform, encode_version(version)):
        raise ValueError("no LC_BUILD_VERSION in " + str(path))
    path.write_bytes(data)
    if data[12] in (6, 8):  # MH_DYLIB, MH_BUNDLE: the old signature no longer matches.
        subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True, capture_output=True)


def archive_members(path: Path):
    """The members of a BSD `ar` archive, as (name, data), without its symbol table."""
    data = path.read_bytes()
    offset = len(AR_MAGIC)
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        name = header[0:16].decode().strip()
        size = int(header[48:58].decode().strip())
        offset += 60
        body = data[offset:offset + size]
        offset += size + (size & 1)
        if name.startswith("#1/"):
            length = int(name[3:])
            name = body[:length].split(b"\0", 1)[0].decode()
            body = body[length:]
        if name.startswith("__.SYMDEF"):
            continue
        yield name, body


def retag_archive(path: Path, platform: int, version: str) -> None:
    with tempfile.TemporaryDirectory() as folder:
        members = []
        for index, (name, body) in enumerate(archive_members(path)):
            # Members may share a name: keep them apart, in their original order.
            member = Path(folder, "{:06d}_{:s}".format(index, os.path.basename(name)))
            member.write_bytes(body)
            if body[:4] in MACHO_MAGICS:
                data = bytearray(body)
                retag_bytes(data, platform, encode_version(version))
                member.write_bytes(data)
            members.append(member)
        listing = Path(folder, "members.txt")
        listing.write_text("\n".join(str(m) for m in members) + "\n")
        out = path.with_name(path.name + ".retag")
        subprocess.run(["libtool", "-static", "-no_warning_for_no_symbols", "-o", str(out),
                        "-filelist", str(listing)], check=True, capture_output=True)
        os.replace(out, path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path)
    parser.add_argument("target", type=Path)
    parser.add_argument("--platform", required=True, choices=sorted(PLATFORMS), help="target platform")
    parser.add_argument("--version", default="26.0", help="minimum OS version to mark")
    args = parser.parse_args()

    if args.target.exists():
        shutil.rmtree(args.target)
    shutil.copytree(args.source, args.target, symlinks=True)

    files = [p for p in args.target.rglob("*") if p.is_file() and not p.is_symlink()]
    jobs = [(p, kind) for p in files if (kind := file_kind(p))]
    # Leave executables (MH_EXECUTE) and fat files alone: they are tools for the build machine.
    jobs = [(p, kind) for p, kind in jobs
            if kind == "archive" or p.read_bytes()[:4] == b"\xcf\xfa\xed\xfe" and
            struct.unpack_from("<I", p.read_bytes(), 12)[0] in (MH_OBJECT, MH_DYLIB, MH_BUNDLE)]
    for path, kind in jobs:
        path.chmod(path.stat().st_mode | 0o200)

    platform = PLATFORMS[args.platform]

    def work(job):
        path, kind = job
        if kind == "archive":
            retag_archive(path, platform, args.version)
        else:
            retag(path, platform, args.version)
        return path

    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        futures = {pool.submit(work, job): job for job in jobs}
        for future in concurrent.futures.as_completed(futures):
            try:
                future.result()
            except subprocess.CalledProcessError as ex:
                failed.append((futures[future][0], ex.stderr.decode(errors="replace").strip()))
            except ValueError as ex:
                failed.append((futures[future][0], str(ex)))
    print("Retargeted {:d} files to {:s} in {:s}".format(len(jobs) - len(failed), args.platform, str(args.target)))
    for path, error in failed:
        print("FAILED:", path, error)
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
