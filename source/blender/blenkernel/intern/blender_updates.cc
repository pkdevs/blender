/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_BLENDER_UPDATES_NOTIFICATIONS

#  include "BKE_appdir.hh"
#  include "BKE_blender_updates.hh"
#  include "BKE_blender_version.h"
#  include "BKE_global.hh"
#  include "BKE_idprop.hh"

#  include "BLI_fileops.hh"
#  include "BLI_path_utils.hh"
#  include "BLI_serialize.hh"
#  include "BLI_string_ref.hh"

#  include "BLT_date_string.hh"
#  include "BLT_lang.hh"

#  include "CLG_log.h"

#  include "DNA_userdef_types.h"

#  include "WM_api.hh"

#  include <chrono>
#  include <ctime>
#  include <fmt/chrono.h>
#  include <fmt/format.h>

#  ifdef WITH_PYTHON
#    include "BPY_extern_run.hh"
#  endif

namespace blender::bke {

static CLG_LogRef LOG = {"blender.updates_notifications"};

struct BlenderUpdates {
  std::optional<VersionUpdate> latest;
  std::optional<VersionUpdate> latest_lts;
  std::optional<VersionUpdate> current_release;
};

struct IgnoredBlenderVersions {
  BlenderVersion latest;
  BlenderVersion latest_lts;
  BlenderVersion current_release;
};

std::string VersionUpdate::date() const
{
  const std::tm local_time = *std::localtime(&this->time);
  return date_string::date(local_time, BLT_lang_get(), date_string::DateFormat(U.date_format));
}

std::string BlenderVersion::to_string() const
{
  return fmt::format("{}.{}.{}", this->version / 100, this->version % 100, this->patch);
}

/** Parses `%Y-%m-%dT%H:%M:%SZ` formatted timestamps strings as #std::chrono::sys_seconds. */
static std::optional<std::chrono::sys_seconds> parse_timestamp_to_sys_seconds(
    StringRefNull timestamp)
{
#  ifdef __APPLE__
  std::tm time;
  if (!strptime(timestamp.data(), "%Y-%m-%dT%H:%M:%SZ", &time)) {
    return std::nullopt;
  }
  return std::chrono::sys_seconds(std::chrono::seconds(std::mktime(&time)));
#  else
  std::istringstream is(timestamp);
  std::chrono::sys_seconds time;
  if (!(is >> std::chrono::parse("%Y-%m-%dT%H:%M:%SZ", time))) {
    return std::nullopt;
  }
  return time;
#  endif
}

static IgnoredBlenderVersions &ignored_blender_updates()
{
  /* Ignore any update prior the current version. */
  static IgnoredBlenderVersions ignored_blender_updates{
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
  };
  return ignored_blender_updates;
}

static BlenderUpdates &available_blender_updates()
{
  static BlenderUpdates available_blender_updates;
  return available_blender_updates;
}

void blender_updates_clear_tracks(bContext *C)
{
  available_blender_updates() = {};
  ignored_blender_updates() = {};
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
}

void blender_updates_add_update_track(bContext *C,
                                      BlenderVersion version,
                                      int64_t build_size,
                                      StringRefNull checksum_hash,
                                      StringRefNull commit_hash,
                                      StringRefNull description,
                                      StringRefNull download_url,
                                      CycleType cycle,
                                      bool is_lts,
                                      StringRefNull platform,
                                      StringRefNull release_notes_url,
                                      StringRefNull timestamp,
                                      TrackType track_type)
{
  VersionUpdate update = {};
  update.version = version;
  update.build_size = build_size;
  update.checksum_hash = checksum_hash;
  update.commit_hash = commit_hash;
  update.description = description;
  update.download_url = download_url;
  update.cycle = cycle;
  update.is_lts = is_lts;
  update.platform = platform;
  update.release_notes_url = release_notes_url;
  update.timestamp = timestamp;

  std::optional<std::chrono::sys_seconds> time = parse_timestamp_to_sys_seconds(timestamp);
  if (!time) {
    return;
  }
  update.time = std::chrono::system_clock::to_time_t(*time);

  switch (track_type) {
    case TrackType::Latest: {
      available_blender_updates().latest = update;
      break;
    }
    case TrackType::LatestLTS: {
      available_blender_updates().latest_lts = update;
      break;
    }
    case TrackType::CurrentRelease: {
      available_blender_updates().current_release = update;
      break;
    }
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
}

void blender_updates_set_ignored_version(bContext *C, BlenderVersion version, TrackType track_type)
{
  switch (track_type) {
    case TrackType::Latest: {
      ignored_blender_updates().latest = version;
      break;
    }
    case TrackType::LatestLTS: {
      ignored_blender_updates().latest_lts = version;
      break;
    }
    case TrackType::CurrentRelease: {
      ignored_blender_updates().current_release = version;
      break;
    }
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
}

enum class CheckForUpdatesState {
  None,
  Loading,
  Failed,
};

static CheckForUpdatesState &check_for_updates_state()
{
  static CheckForUpdatesState check_for_updates_state = CheckForUpdatesState::None;
  return check_for_updates_state;
}

void check_for_updates_set_finished()
{
  check_for_updates_state() = CheckForUpdatesState::None;
}

void check_for_updates_set_failed()
{
  check_for_updates_state() = CheckForUpdatesState::Failed;
}

bool is_looking_for_updates()
{
  return check_for_updates_state() == CheckForUpdatesState::Loading;
}

bool is_looking_for_updates_failed()
{
  return check_for_updates_state() == CheckForUpdatesState::Failed;
}

static void download_available_updates_list(bContext &C)
{
  check_for_updates_state() = CheckForUpdatesState::Loading;

  const std::string expr =
      R"(
import _bpy_internal.available_updates.blender_updates as updates
updates.reset_cache()
updates.write_cache_file()

import _bpy_internal.available_updates.available_updates_list_downloader as downloader
downloader.download_available_updates_list()
)";
  std::unique_ptr locals = bke::idprop::create_group("locals");
  BPY_run_string_exec_with_locals(&C, expr, *locals);
}

static void load_available_updates_cache_file_impl(bContext &C)
{
  constexpr const char *expr =
      R"(
import _bpy_internal.available_updates.blender_updates as updates
updates.load_cache_file()
)";
  std::unique_ptr locals = bke::idprop::create_group("locals");
  std::optional<blender::IDProperty *> updates_ptr = BPY_run_string_exec_with_locals_return_idprop(
      &C, expr, *locals, "_result");
  if (!updates_ptr) {
    return;
  }
  IDP_FreeProperty(*updates_ptr);
  // #  endif
}

void check_for_available_updates_if_expired(bContext &C);

void load_available_updates_cache_file(bContext &C)
{
  load_available_updates_cache_file_impl(C);
  check_for_available_updates_if_expired(C);
}

int updates_notifications_flags()
{
  if (!(G.f & G_FLAG_INTERNET_ALLOW)) {
    return 0;
  }
  if (BKE_blender_version_is_lts()) {
    return (U.flag & (USER_BLENDER_UPDATE_LATEST_RELEASE | USER_BLENDER_UPDATE_LATEST_LTS_RELEASE |
                      USER_BLENDER_UPDATE_CURRENT_RELEASE));
  }
  return U.flag & USER_BLENDER_UPDATE_LATEST_RELEASE;
}

void check_for_available_updates_if_expired(bContext &C)
{
  constexpr const char *expr =
      R"(
import _bpy_internal.available_updates.available_updates_list_downloader as downloader
downloader.download_available_updates_if_expired()
)";
  std::unique_ptr locals = bke::idprop::create_group("locals");
  std::optional<blender::IDProperty *> updates_ptr = BPY_run_string_exec_with_locals_return_idprop(
      &C, expr, *locals, "_result");
  if (!updates_ptr) {
    return;
  }
  IDP_FreeProperty(*updates_ptr);
}

void check_for_available_updates(bContext &C)
{
  if (!updates_notifications_flags()) {
    return;
  }
  ignored_blender_updates() = {
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
  };
  available_blender_updates() = {};
  download_available_updates_list(C);
}

bool have_available_updates()
{
  if (!updates_notifications_flags()) {
    return false;
  }
  return !available_updates().is_empty();
}

Vector<const VersionUpdate *> available_updates()
{
  BlenderUpdates &updates = available_blender_updates();
  const IgnoredBlenderVersions &ignored_updates = ignored_blender_updates();
  const int notifications_flags = updates_notifications_flags();

  Vector<VersionUpdate *> result;
  /** Add Latest Release. */
  [&]() -> void {
    if (!updates.latest) {
      return;
    }
    if (!(notifications_flags & USER_BLENDER_UPDATE_LATEST_RELEASE)) {
      return;
    }
    if (updates.latest_lts && updates.latest->version < updates.latest_lts->version) {
      return;
    }
    result.append(&*updates.latest);
    result.last()->ui_track_type = TrackType::Latest;
  }();
  /** Add Latest LTS Release. */
  [&]() -> void {
    if (!updates.latest_lts) {
      return;
    }
    /* Add Latest LTS Release as Latest Release when there is no Latest Release available.  */
    if (notifications_flags & USER_BLENDER_UPDATE_LATEST_RELEASE && result.is_empty() &&
        updates.latest_lts->version > ignored_updates.latest)
    {
      result.append(&*updates.latest_lts);
      result.last()->ui_track_type = TrackType::Latest;
      return;
    }
    if (!(notifications_flags & USER_BLENDER_UPDATE_LATEST_LTS_RELEASE)) {
      return;
    }
    result.append(&*updates.latest_lts);
    result.last()->ui_track_type = TrackType::LatestLTS;
  }();
  [&]() -> void {
    if (!updates.current_release) {
      return;
    }
    if (notifications_flags & USER_BLENDER_UPDATE_CURRENT_RELEASE) {
      result.append(&*updates.current_release);
      result.last()->ui_track_type = TrackType::CurrentRelease;
      return;
    }
    /* Add current Release Update as Latest Release when there is no Latest Release or Latest LTS
     * Release available. */
    if ((notifications_flags & USER_BLENDER_UPDATE_LATEST_RELEASE) && result.is_empty() &&
        (updates.current_release->version > ignored_updates.latest))
    {
      result.append(&*updates.current_release);
      result.last()->ui_track_type = TrackType::Latest;
      return;
    }
    /* Add current Release Update as Latest LTS Release when the update is LTS and there is no
     * other Latest LTS Release. */
    if ((notifications_flags & USER_BLENDER_UPDATE_LATEST_LTS_RELEASE) &&
        updates.current_release->is_lts && (result.is_empty() || !result.last()->is_lts) &&
        updates.current_release->version > ignored_updates.latest_lts)
    {
      result.append(&*updates.current_release);
      result.last()->ui_track_type = TrackType::LatestLTS;
      return;
    }
  }();
  return result.as_span();
}

void ignore_update(bContext &C, const VersionUpdate *update)
{
  const std::string expr =
      R"(
import _bpy_internal.available_updates.blender_updates as updates
from _bpy_internal.available_updates import blender_updates_models as  models
updates.ignore_update(models.BlenderVersion(version_number,version_patch),is_lts)
)";
  std::unique_ptr locals = bke::idprop::create_group("locals");
  IDP_AddToGroup(locals.get(), IDP_NewInt(update->version.version, "version_number"));
  IDP_AddToGroup(locals.get(), IDP_NewInt(update->version.patch, "version_patch"));
  IDP_AddToGroup(locals.get(), IDP_NewInt(update->is_lts, "is_lts"));
  BPY_run_string_exec_with_locals(&C, expr, *locals);
}

void ignore_all_updates(bContext &C)
{
  BlenderUpdates &updates = available_blender_updates();
  if (updates.latest) {
    ignore_update(C, &*updates.latest);
  }
  if (updates.latest_lts) {
    ignore_update(C, &*updates.latest_lts);
  }
  if (updates.current_release) {
    ignore_update(C, &*updates.current_release);
  }
}

}  // namespace blender::bke

#endif /* WITH_BLENDER_UPDATES_NOTIFICATIONS */
