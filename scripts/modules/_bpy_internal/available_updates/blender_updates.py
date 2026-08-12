# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from _bpy_internal.available_updates import blender_updates_cache_model as cache_model
from pathlib import Path
from datetime import datetime, timedelta
import cattrs
import bpy


def _cache_file_path():
    from bpy.utils import user_resource
    path = user_resource('CONFIG')
    if path:
        import os
        path = os.path.join(path, "blender_updates_cache.json")
    return Path(path)


def _timestamp_format():
    return "%Y-%m-%dT%H:%M:%SZ"


def _blender_version_from_str(version_str) -> cache_model.BlenderVersion:
    major_version, minor_version, patch = [item.strip() for item in version_str.split(".")]
    return cache_model.BlenderVersion(int(major_version) * 100 + int(minor_version), int(patch))


_current_app_version = cache_model.BlenderVersion(
    bpy.app.version[0] * 100 + bpy.app.version[1], bpy.app.version[2])

_cache: cache_model.UpdatesCache = cache_model.UpdatesCache(
    ignored_versions=cache_model.IgnoredVersions(
        latest_lts=_current_app_version,
        latest=_current_app_version,
        current_release=_current_app_version),
    updates=cache_model.AvailableUpdates(),
    last_time_check=datetime.now() - timedelta(days=2))


def _json_converter():
    converter = cattrs.preconf.json.JsonConverter()
    converter.register_structure_hook_func(
        lambda t: t is datetime, lambda v, _: datetime.strptime(v, _timestamp_format())
    )
    return converter


def reset_cache():
    global _cache
    _cache = cache_model.UpdatesCache(
        ignored_versions=cache_model.IgnoredVersions(
            latest_lts=_current_app_version,
            latest=_current_app_version,
            current_release=_current_app_version),
        updates=cache_model.AvailableUpdates(),
        last_time_check=datetime.now() - timedelta(days=2))


def _load_tracks():
    bpy.types.WindowManager.blender_updates_clear_tracks()
    global _cache
    if _cache.updates.latest:
        bpy.types.WindowManager.blender_updates_add_update_track(
            version_number=_cache.updates.latest.version.version,
            version_patch=_cache.updates.latest.version.patch,
            is_lts=_cache.updates.latest.is_lts,
            download_url=_cache.updates.latest.download_url,
            description=_cache.updates.latest.description,
            timestamp=_cache.updates.latest.timestamp.strftime(_timestamp_format()),
            track_type='LATEST'
        )
    if _cache.updates.latest_lts:
        bpy.types.WindowManager.blender_updates_add_update_track(
            version_number=_cache.updates.latest_lts.version.version,
            version_patch=_cache.updates.latest_lts.version.patch,
            is_lts=_cache.updates.latest_lts.is_lts,
            download_url=_cache.updates.latest_lts.download_url,
            description=_cache.updates.latest_lts.description,
            timestamp=_cache.updates.latest_lts.timestamp.strftime(_timestamp_format()),
            track_type='LATEST_LTS'
        )
    if _cache.updates.current_release:
        bpy.types.WindowManager.blender_updates_add_update_track(
            version_number=_cache.updates.current_release.version.version,
            version_patch=_cache.updates.current_release.version.patch,
            is_lts=_cache.updates.current_release.is_lts,
            download_url=_cache.updates.current_release.download_url,
            description=_cache.updates.current_release.description,
            timestamp=_cache.updates.current_release.timestamp.strftime(_timestamp_format()),
            track_type='CURRENT_RELEASE'
        )
    bpy.types.WindowManager.blender_updates_set_ignored_version(
        version_number=_cache.ignored_versions.current_release.version,
        version_patch=_cache.ignored_versions.current_release.patch,
        track_type='CURRENT_RELEASE')
    bpy.types.WindowManager.blender_updates_set_ignored_version(
        version_number=_cache.ignored_versions.latest.version,
        version_patch=_cache.ignored_versions.latest.patch,
        track_type='LATEST')
    bpy.types.WindowManager.blender_updates_set_ignored_version(
        version_number=_cache.ignored_versions.latest_lts.version,
        version_patch=_cache.ignored_versions.latest_lts.patch,
        track_type='LATEST_LTS')


def load_cache_file():
    reset_cache()
    cache_path = Path(_cache_file_path())
    json_cache = None
    if not cache_path.is_file():
        return
    with cache_path.open() as file:
        json_cache = file.read()

    from datetime import datetime
    import json
    json_doc = json.loads(json_cache)
    cache: cache_model.UpdatesCache = _json_converter().structure(json_doc, cache_model.UpdatesCache)

    global _cache
    _cache.ignored_versions.current_release = max(cache.ignored_versions.current_release, _current_app_version)
    _cache.ignored_versions.latest_lts = max(cache.ignored_versions.latest_lts, _current_app_version)
    _cache.ignored_versions.latest = max(
        cache.ignored_versions.latest,
        _cache.ignored_versions.latest_lts,
        _current_app_version)
    _cache.last_time_check = cache.last_time_check
    if cache.updates.current_release:
        _load_release(cache.updates.current_release)
    if cache.updates.latest_lts:
        _load_release(cache.updates.latest_lts)
    if cache.updates.latest:
        _load_release(cache.updates.latest)
    _load_tracks()


def is_cache_expired() -> bool:
    global _cache
    return _cache.last_time_check < (datetime.now() - timedelta(days=1))


def _load_release(release: cache_model.VersionUpdate):
    release_version = release.version
    if _current_app_version >= release_version:
        return

    ignore = _cache.ignored_versions
    releases = _cache.updates

    current_min = max(
        releases.current_release.version if releases.current_release else _current_app_version,
        ignore.current_release)
    latest_lts_min = max(
        releases.latest_lts.version if releases.latest_lts else _current_app_version,
        ignore.latest_lts)
    latest_min = max(
        releases.latest.version if releases.latest else _current_app_version,
        ignore.current_release,
        latest_lts_min)

    if _current_app_version.version == release_version.version:
        if current_min > release_version:
            return
        _cache.updates.current_release = release
        return

    if release.is_lts:
        if latest_lts_min > release_version:
            return
        _cache.updates.latest_lts = release
        if latest_min < release_version:
            _cache.updates.latest = None
        return
    if latest_min > release_version:
        return
    _cache.updates.latest = release


def _json_default_formatter(obj):
    if isinstance(obj, datetime):
        return obj.strftime(_timestamp_format())
    raise TypeError(f'Cannot serialize object of {type(obj)}')


def write_cache_file():
    cache_path = _cache_file_path()
    import dataclasses
    import json
    with cache_path.open('w') as file:
        global _cache
        file.write(json.dumps(dataclasses.asdict(_cache), default=_json_default_formatter))


def load_available_updates_json(json_text):
    from datetime import datetime
    import json

    json_doc = json.loads(json_text)
    converter = _json_converter()
    # Blender version is a raw string in downloaded jsons
    converter.register_structure_hook_func(
        lambda t: t is cache_model.BlenderVersion, lambda v, _: _blender_version_from_str(v)
    )
    new_releases = converter.structure(json_doc, list[cache_model.VersionUpdate])
    _cache.last_time_check = datetime.now()

    for release in new_releases:
        _load_release(release)
    write_cache_file()
    _load_tracks()
