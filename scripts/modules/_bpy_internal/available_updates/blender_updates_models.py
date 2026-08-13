# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


from __future__ import annotations

import dataclasses

from datetime import datetime
from _bpy_internal.available_updates import blender_releases_openapi as open_api


@dataclasses.dataclass(eq=True, order=True)
class BlenderVersion:
    version: int = 0
    patch: int = 0


@dataclasses.dataclass
class IgnoredVersions:
    latest: BlenderVersion
    latest_lts: BlenderVersion
    current_release: BlenderVersion


@dataclasses.dataclass
class VersionUpdate:
    build_size: int
    checksum_hash: str
    commit_hash: str
    description: str
    download_url: str
    cycle: open_api.CycleType
    is_lts: bool
    platform: str
    release_notes_url: str
    timestamp: datetime
    version: BlenderVersion


@dataclasses.dataclass
class Tracks:
    latest: VersionUpdate | None = None
    latest_lts: VersionUpdate | None = None
    current_release: VersionUpdate | None = None


@dataclasses.dataclass
class UpdatesCache:
    ignored_versions: IgnoredVersions
    tracks: Tracks
    last_time_check: datetime
