/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef WITH_BLENDER_UPDATES_NOTIFICATIONS

#  pragma once

#  include "BLI_span.hh"
#  include "BLI_string_ref.hh"
#  include "BLI_vector.hh"

#  include <chrono>
#  include <string>

namespace blender {
struct bContext;
}

namespace blender::bke {

/** Blender version. */
struct BlenderVersion {
  int version;
  int patch;
  friend auto operator<=>(const BlenderVersion &a, const BlenderVersion &b) = default;
  std::string to_string() const;
};

enum class TrackType {
  Latest,
  LatestLTS,
  CurrentRelease,
};

enum class CycleType {
  Release,
  Alpha,
  Beta,
  RC,
};

/** Version update notification. */
struct VersionUpdate {
  int64_t build_size;
  std::string checksum_hash;
  std::string commit_hash;
  std::string description;
  std::string download_url;
  CycleType cycle;
  bool is_lts;
  std::string platform;
  std::string release_notes_url;
  std::string timestamp;
  std::time_t time;

  BlenderVersion version;
  TrackType ui_track_type;

  /** UI string representation of #VersionUpdate::time. */
  std::string date() const;
};

/**
 * Checks in the background if there is any new available blender update, this resets previously
 * ignored notifications.
 */
void check_for_available_updates(bContext &C);

bool have_available_updates();

/** Loads available updates notifications from the cache file, it may check for updates if the
 * cache is expired. */
void load_available_updates_cache_file(bContext &C);

/** Return available blender updates. */
Vector<const VersionUpdate *> available_updates();

/**
 * Ignores an specific update, the update and prior updates from the same type will not be listed
 * again.
 */
void ignore_update(bContext &C, const VersionUpdate *update_info);

/**
 * Ignores all available updates, see #ignore_update.
 */
void ignore_all_updates(bContext &C);

/** Sets when the process downloading the file of the latest updates have beed finished
 * successfully. */
void check_for_updates_set_finished();

void blender_updates_clear_tracks(bContext *C);
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
                                      TrackType track_type);
void blender_updates_set_ignored_version(bContext *C,
                                         BlenderVersion version,
                                         TrackType track_type);
/** Sets when the process downloading the file of the latest available updates have encounter any
 * error. */
void check_for_updates_set_failed();

/** Checks if there is an active process downloading the file of the latest available updates. */
bool is_looking_for_updates();

/** Checks is there was as issue when downloading the file of the latest available updates. */
bool is_looking_for_updates_failed();

/**
 * Return enabled update notifications flags when `G_FLAG_INTERNET_ALLOW` is enabled, if the
 * current version is a non LTS version it will only return the
 * `USER_BLENDER_UPDATE_LATEST_RELEASE` flag.
 */
int updates_notifications_flags();

}  // namespace blender::bke

#endif
