# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from _bpy_internal.available_updates import blender_updates_models as models
from pathlib import Path
from datetime import datetime, timedelta
import bpy


def _updates_cache_path() -> Path:
    from bpy.utils import user_resource
    path = user_resource('CONFIG')
    if path:
        import os
        path = os.path.join(path, "blender_updates_cache.json")
    return Path(path)


def _timestamp_format() -> str:
    return "%Y-%m-%dT%H:%M:%SZ"


_app_version = models.BlenderVersion(bpy.app.version[0] * 100 + bpy.app.version[1], bpy.app.version[2])


def _cache_default_init() -> models.UpdatesCache:
    return models.UpdatesCache(
        ignored_versions=models.IgnoredVersions(
            latest_lts=_app_version,
            latest=_app_version,
            current_release=_app_version),
        tracks=models.Tracks(),
        last_time_check=datetime.now() - timedelta(days=2)  # Expired by default
    )


_cache: models.UpdatesCache = _cache_default_init()


def _json_converter():
    import cattrs
    converter = cattrs.preconf.json.JsonConverter()
    converter.register_structure_hook_func(
        lambda t: t is datetime, lambda v, _: datetime.strptime(
            v, _timestamp_format()))
    return converter


def reset_cache():
    """Resets the updates cache, ignored versions are not longer remember, and updates should be downloaded again."""
    global _cache
    _cache = _cache_default_init()


def _update_blender_tracks():
    """Update blender tracks"""
    bpy.types.WindowManager.blender_updates_clear_tracks()

    def load_track(track: models.VersionUpdate, track_type: str):
        if not track:
            return
        bpy.types.WindowManager.blender_updates_add_update_track(
            version_number=track.version.version,
            version_patch=track.version.patch,
            build_size=track.build_size,
            checksum_hash=track.checksum_hash,
            commit_hash=track.commit_hash,
            description=track.description,
            download_url=track.download_url,
            cycle=track.cycle,
            is_lts=track.is_lts,
            platform=track.platform,
            release_notes_url=track.release_notes_url,
            timestamp=track.timestamp.strftime(_timestamp_format()),
            track_type=track_type
        )
    load_track(_cache.tracks.latest, 'LATEST')
    load_track(_cache.tracks.latest_lts, 'LATEST_LTS')
    load_track(_cache.tracks.current_release, 'CURRENT_RELEASE')

    def load_ignored_version(version: models.BlenderVersion, track_type: str):
        bpy.types.WindowManager.blender_updates_set_ignored_version(
            version_number=version.version,
            version_patch=version.patch,
            track_type=track_type)
    load_ignored_version(_cache.ignored_versions.current_release, 'CURRENT_RELEASE')
    load_ignored_version(_cache.ignored_versions.latest, 'LATEST')
    load_ignored_version(_cache.ignored_versions.latest_lts, 'LATEST_LTS')


def load_cache_file():
    """Loads the updates cache stored in disk."""
    reset_cache()

    cache_path = _updates_cache_path()
    if not cache_path.is_file():
        return

    json_cache = None
    with cache_path.open() as file:
        json_cache = file.read()

    from datetime import datetime
    import json
    json_doc = json.loads(json_cache)
    cache: models.UpdatesCache = _json_converter().structure(json_doc, models.UpdatesCache)

    _cache.ignored_versions.current_release = max(cache.ignored_versions.current_release, _app_version)
    _cache.ignored_versions.latest_lts = max(cache.ignored_versions.latest_lts, _app_version)
    _cache.ignored_versions.latest = max(cache.ignored_versions.latest, _app_version)
    # Make sure to ignore in the latest track versions ignored in the latest LTS track
    _cache.ignored_versions.latest = max(_cache.ignored_versions.latest, _cache.ignored_versions.latest_lts)

    _cache.last_time_check = cache.last_time_check

    if cache.tracks.current_release:
        _load_update(cache.tracks.current_release)
    if cache.tracks.latest_lts:
        _load_update(cache.tracks.latest_lts)
    if cache.tracks.latest:
        _load_update(cache.tracks.latest)
    _update_blender_tracks()


def is_cache_expired() -> bool:
    """Checks if the updates cache is expired, which happens every 24 hours."""
    return _cache.last_time_check < (datetime.now() - timedelta(days=1))


def _load_update(update: models.VersionUpdate):
    # Skip earlier versions
    if update.version <= _app_version:
        return

    ignore = _cache.ignored_versions
    tracks = _cache.tracks

    current_min = max(
        tracks.current_release.version if tracks.current_release else _app_version,
        ignore.current_release)

    latest_lts_min = max(tracks.latest_lts.version if tracks.latest_lts else _app_version, ignore.latest_lts)

    # Latest track should be newer than the latest LTS track
    latest_min = max(
        tracks.latest.version if tracks.latest else _app_version,
        ignore.current_release,
        latest_lts_min)

    # Add the update in the current release track if is newer that the current listed/ignored.
    if _app_version.version == update.version:
        if not (current_min < update.version):
            return
        _cache.tracks.current_release = update
        return

    # Add the update in the latest LTS track if is newer that the latest LTS listed/ignored.
    if update.is_lts:
        if not (latest_lts_min < update.version):
            return
        _cache.tracks.latest_lts = update
        if latest_min < update.version:
            _cache.tracks.latest = None
        return

    # Add the update in the latest track if is newer that the latest listed/ignored.
    if not (update.version < latest_min):
        return
    _cache.tracks.latest = update


def write_cache_file():
    """Write the updates cache into the disk in json format."""
    def datetime_default_formatter(obj) -> str:
        """datetime timestamp formatter"""
        if isinstance(obj, datetime):
            return obj.strftime(_timestamp_format())
        raise TypeError(f'Cannot serialize object of {type(obj)}')

    with _updates_cache_path().open('w') as file:
        import dataclasses
        import json
        file.write(json.dumps(dataclasses.asdict(_cache), default=datetime_default_formatter))


def load_json_updates_to_tracks(json_text):
    """Loads downloaded json updates to tracks."""
    from datetime import datetime
    import json

    json_doc = json.loads(json_text)
    converter = _json_converter()

    def str_blender_version_parser(version_str) -> models.BlenderVersion:
        major_version, minor_version, patch = [item.strip() for item in version_str.split(".")]
        return models.BlenderVersion(int(major_version) * 100 + int(minor_version), int(patch))

    # Blender version is a raw string in downloaded jsons
    converter.register_structure_hook_func(
        lambda t: t is models.BlenderVersion, lambda v, _: str_blender_version_parser(v)
    )

    updates = converter.structure(json_doc, list[models.VersionUpdate])

    _cache.last_time_check = datetime.now()

    for update in updates:
        _load_update(update)

    # Save cache to disk
    write_cache_file()

    _update_blender_tracks()


def ignore_update(version: models.BlenderVersion, is_lts: bool):
    """Ignores a update and reloads tracks discarding the new ignored version"""

    # A version can be ignored  in multiple tracks
    if _app_version == version:
        _cache.ignored_versions.current_release = max(_cache.ignored_versions.current_release, version)
    if is_lts:
        _cache.ignored_versions.latest_lts = max(_cache.ignored_versions.latest_lts, version)
    _cache.ignored_versions.latest = max(_cache.ignored_versions.latest, version)

    # Reload current tracks
    tracks = _cache.tracks
    _cache.tracks = models.Tracks()
    if tracks.current_release:
        _load_update(tracks.current_release)
    if tracks.latest_lts:
        _load_update(tracks.latest_lts)
    if tracks.latest:
        _load_update(tracks.latest)

    # Save cache changes
    write_cache_file()
    _update_blender_tracks()
