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

/** Parses blender version str into blender version and patch revision.  */
static std::optional<BlenderVersion> blender_version_from_version_str(std::string str)
{
  std::stringstream ss(str);
  int major;
  ss >> major;
  if (ss.fail()) {
    return std::nullopt;
  }
  char sep = '.';
  ss >> sep;
  if (ss.fail() || sep != '.') {
    return std::nullopt;
  }
  int minor;
  ss >> minor;
  if (ss.fail()) {
    return std::nullopt;
  }
  ss >> sep;
  if (ss.fail() || sep != '.') {
    return std::nullopt;
  }
  int patch;
  ss >> patch;
  if (ss.fail() || !ss.eof()) {
    return std::nullopt;
  }
  return BlenderVersion{major * 100 + minor, patch};
}

static std::optional<VersionUpdate> version_update_deserialize(io::serialize::Value *entry)
{
  using namespace io::serialize;
  if (!entry || entry->type() != eValueType::Dictionary) {
    return std::nullopt;
  }
  const DictionaryValue &dict = *entry->as_dictionary_value();
  std::optional<int64_t> build_size = dict.lookup_int("build_size");
  std::optional<StringRefNull> checksum_hash = dict.lookup_str("checksum_hash");
  std::optional<StringRefNull> commit_hash = dict.lookup_str("commit_hash");
  std::optional<StringRefNull> description = dict.lookup_str("description");
  std::optional<StringRefNull> download_url = dict.lookup_str("download_url");
  std::optional<StringRefNull> cycle = dict.lookup_str("cycle");
  std::optional<bool> is_lts = dict.lookup_bool("is_lts");
  std::optional<StringRefNull> platform = dict.lookup_str("platform");
  std::optional<StringRefNull> release_notes_url = dict.lookup_str("release_notes_url");
  std::optional<StringRefNull> timestamp = dict.lookup_str("timestamp");
  std::optional<StringRefNull> version_str = dict.lookup_str("version");
  if (!(build_size && checksum_hash && commit_hash && description && download_url && cycle &&
        is_lts && platform && release_notes_url && timestamp && version_str))
  {
    return std::nullopt;
  }
  std::optional<BlenderVersion> version = blender_version_from_version_str(*version_str);
  if (!version) {
    return std::nullopt;
  }
  std::optional<std::chrono::sys_seconds> time = parse_timestamp_to_sys_seconds(*timestamp);
  if (!time) {
    return std::nullopt;
  }
  return VersionUpdate{
      .build_size = *build_size,
      .checksum_hash = *checksum_hash,
      .commit_hash = *commit_hash,
      .description = *description,
      .download_url = *download_url,
      .cycle = *cycle,
      .is_lts = *is_lts,
      .platform = *platform,
      .release_notes_url = *release_notes_url,
      .timestamp = *timestamp,
      .version_str = *version_str,
      .time = std::chrono::system_clock::to_time_t(*time),
      .version = *version,
  };
}

static std::shared_ptr<io::serialize::DictionaryValue> version_update_serialize(
    const VersionUpdate &update)
{
  std::shared_ptr<io::serialize::DictionaryValue> dict =
      std::make_unique<io::serialize::DictionaryValue>();
  dict->append_int("build_size", update.build_size);
  dict->append_str("checksum_hash", update.checksum_hash);
  dict->append_str("commit_hash", update.commit_hash);
  dict->append_str("description", update.description);
  dict->append_str("download_url", update.download_url);
  dict->append_str("cycle", update.cycle);
  dict->append_bool("is_lts", update.is_lts);
  dict->append_str("platform", update.platform);
  dict->append_str("release_notes_url", update.release_notes_url);
  dict->append_str("timestamp", update.timestamp);
  dict->append_str("version", update.version_str);
  return dict;
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

static std::chrono::sys_seconds &last_time_version_update_check()
{
  static std::chrono::sys_seconds last_time_version_update_check;
  return last_time_version_update_check;
}

/**
 * Registers a new blender update notification.
 */
static void register_blender_update(VersionUpdate &&update)
{
  BLI_assert(blender_version_from_version_str(update.version_str) &&
             update.version == blender_version_from_version_str(update.version_str));

  IgnoredBlenderVersions &ignored_updates = ignored_blender_updates();
  BlenderUpdates &updates = available_blender_updates();

  /* If the update version matches current version add as current release notification. */
  if (update.version.version == BLENDER_VERSION) {
    if (ignored_updates.current_release.patch < update.version.patch &&
        (!updates.current_release || updates.current_release->version < update.version))
    {
      updates.current_release = std::move(update);
    }
    return;
  }
  /* If the update version is an LTS version add as #BlenderUpdates::latest_lst notification. */
  if (update.is_lts) {
    if (ignored_updates.latest_lts < update.version) {
      if (!updates.latest_lts || updates.latest_lts->version < update.version) {
        updates.latest_lts = std::move(update);
      }
      /* Discard #BlenderUpdates::latest when the latest LTS release is newer. */
      if (updates.latest && updates.latest->version < update.version) {
        updates.latest = std::nullopt;
      }
    }
    return;
  }
  /* Add the version as #BlenderUpdates::latest release. */
  if (ignored_updates.latest < update.version) {
    /* Ignore Latest releases prior to Latest LTS releases.  */
    if ((updates.latest_lts && updates.latest_lts->version >= update.version) ||
        ignored_updates.latest_lts >= update.version)
    {
      return;
    }
    if (!updates.latest || updates.latest->version < update.version) {
      updates.latest = std::move(update);
    }
  }
}

enum class CheckForUpdatesState {
  None,
  Loading,
  Done,
  Failed,
};

static CheckForUpdatesState &check_for_updates_state()
{
  static CheckForUpdatesState check_for_updates_state;
  return check_for_updates_state;
}

void check_for_updates_set_finished()
{
  check_for_updates_state() = CheckForUpdatesState::Done;
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
#  ifdef WITH_PYTHON
  if (check_for_updates_state() == CheckForUpdatesState::Loading) {
    return;
  }
  check_for_updates_state() = CheckForUpdatesState::Loading;

  const std::string expr =
      R"(
import _bpy_internal.available_updates.available_updates_list_downloader as downloader
downloader.download_available_updates_list()
)";
  std::unique_ptr locals = bke::idprop::create_group("locals");
  BPY_run_string_exec_with_locals(&C, expr, *locals);
#  endif /* WITH_PYTHON */
}

static void write_blender_updates_cache_file();

static void load_latest_available_updates_file(bContext &C)
{
#  ifdef WITH_PYTHON
  if (check_for_updates_state() != CheckForUpdatesState::Done) {
    return;
  }
  check_for_updates_state() = CheckForUpdatesState::None;

  constexpr const char *expr =
      R"(
import _bpy_internal.available_updates.available_updates_list_downloader as downloader
result = downloader.read_available_updates_list()
if result:
  _result = result 
)";
  std::unique_ptr locals = bke::idprop::create_group("locals");
  std::optional<blender::IDProperty *> updates_ptr = BPY_run_string_exec_with_locals_return_idprop(
      &C, expr, *locals, "_result");
  if (!updates_ptr) {
    return;
  }
  IDProperty *updates_idprop = *updates_ptr;

  /* Check the returned value. */
  if (updates_idprop == nullptr || updates_idprop->type != IDP_STRING) {
    IDP_FreeProperty(updates_idprop);
    return;
  }
  std::string updates_str = IDP_string_get(updates_idprop);
  IDP_FreeProperty(updates_idprop);

  using namespace io::serialize;

  std::istringstream updates_stream(updates_str);

  JsonFormatter json;
  std::unique_ptr<Value> updates_json = json.deserialize(updates_stream);

  if (!updates_json) {
    return;
  }
  if (updates_json->type() != eValueType::Array) {
    return;
  }

  /* Reset notifications before reading new list. */
  available_blender_updates() = {};

  for (const std::shared_ptr<Value> &entry : updates_json->as_array_value()->elements()) {
    std::optional<VersionUpdate> update = version_update_deserialize(entry.get());
    if (!update) {
      continue;
    }
    register_blender_update(std::move(*update));
  }

  last_time_version_update_check() = std::chrono::sys_seconds(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()));

  write_blender_updates_cache_file();
#  endif /* WITH_PYTHON */

  WM_event_add_notifier(&C, NC_WINDOW, nullptr);
}
#  undef TEST_JSON_ENTRY

#  define BLENDER_AVAILABLE_UPDATES_FILE "available_updates.json"

static void load_available_updates_cache_file_impl()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (!datafiles_path) {
    return;
  }
  std::string available_updates_file = *datafiles_path + SEP + BLENDER_AVAILABLE_UPDATES_FILE;
  size_t size = 0;
  std::unique_ptr<char, MEM_smart_ptr_deleter<char>> json_text = nullptr;
  json_text.reset(BLI_file_read_text_as_mem(available_updates_file.c_str(), 0, &size));

  printf("%s\n", available_updates_file.c_str());

  if (!json_text) {
    return;
  }

  std::istringstream available_updates_stream(std::string(json_text.get(), size));

  using namespace io::serialize;

  JsonFormatter json;
  std::unique_ptr<Value> file_json = json.deserialize(available_updates_stream);

  if (!file_json || file_json->type() != eValueType::Dictionary) {
    CLOG_WARN(&LOG, "corrupt `available_updates.json` file.");
    return;
  }
  const DictionaryValue *file_dict = file_json->as_dictionary_value();
  const DictionaryValue *ignored_versions_dict = file_dict->lookup_dict("ignored_versions");
  if (!ignored_versions_dict) {
    CLOG_WARN(&LOG, "missing `ignored_versions` entry.");
    return;
  }
  std::optional<StringRefNull> latest_lts_ignored_str = ignored_versions_dict->lookup_str(
      "latest_lts");
  std::optional<StringRefNull> latest_ignored_str = ignored_versions_dict->lookup_str("latest");
  std::optional<StringRefNull> current_release_ignored_str = ignored_versions_dict->lookup_str(
      "current_release");

  if (!latest_lts_ignored_str) {
    CLOG_WARN(&LOG, "missing `ignored_versions::latest_lts` entry.");
    return;
  }
  if (!latest_ignored_str) {
    CLOG_WARN(&LOG, "missing `ignored_versions::latest` entry.");
    return;
  }
  if (!current_release_ignored_str) {
    CLOG_WARN(&LOG, "missing `ignored_versions::current_release` entry.");
    return;
  }
  std::optional<BlenderVersion> latest_lts_ignored = blender_version_from_version_str(
      *latest_lts_ignored_str);
  std::optional<BlenderVersion> latest_ignored = blender_version_from_version_str(
      *latest_ignored_str);
  std::optional<BlenderVersion> current_release_ignored = blender_version_from_version_str(
      *current_release_ignored_str);

  if (!latest_lts_ignored) {
    CLOG_WARN(&LOG, "`ignored_versions::latest_lts` have a wrong blender version format.");
    return;
  }
  if (!latest_ignored) {
    CLOG_WARN(&LOG, "`ignored_versions::latest` have a wrong blender version format.");
    return;
  }
  if (!current_release_ignored) {
    CLOG_WARN(&LOG, "`ignored_versions::current_release` have a wrong blender version format.");
    return;
  }
  IgnoredBlenderVersions &ignored_updates = ignored_blender_updates();
  if (ignored_updates.latest_lts < *latest_lts_ignored) {
    ignored_updates.latest_lts = *latest_lts_ignored;
  }
  if (ignored_updates.latest < *latest_ignored) {
    ignored_updates.latest = *latest_ignored;
  }
  if (ignored_updates.current_release < *current_release_ignored) {
    ignored_updates.current_release = *current_release_ignored;
  }
  const ArrayValue *updates = file_dict->lookup_array("blender_updates");
  if (!updates) {
    CLOG_WARN(&LOG, "corrupt or missing `blender_updates` entry.");
    return;
  }
  for (const std::shared_ptr<Value> &entry : updates->as_array_value()->elements()) {
    std::optional<VersionUpdate> update = version_update_deserialize(entry.get());
    if (!update) {
      continue;
    }
    register_blender_update(*std::move(update));
  }
  std::optional<StringRefNull> last_time_check_str = file_dict->lookup_str("last_time_check");
  if (!last_time_check_str) {
    CLOG_WARN(&LOG, "missing `last_time_check` entry.");
    return;
  }
  std::optional<std::chrono::sys_seconds> last_time_check = parse_timestamp_to_sys_seconds(
      *last_time_check_str);
  if (!last_time_check) {
    CLOG_WARN(&LOG, "corrupt entry :`last_time_check`.");
    return;
  }

  last_time_version_update_check() = std::chrono::sys_seconds(*last_time_check);
}

void check_for_available_updates_if_expired(bContext &C);

void load_available_updates_cache_file(bContext &C)
{
  load_available_updates_cache_file_impl();
  check_for_available_updates_if_expired(C);
}

void write_blender_updates_cache_file()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (!datafiles_path) {
    return;
  }
  std::string available_updates_file = *datafiles_path + SEP + BLENDER_AVAILABLE_UPDATES_FILE;

  using namespace io::serialize;
  std::shared_ptr<DictionaryValue> dict = std::make_shared<DictionaryValue>();
  auto version_to_str = [](const BlenderVersion &version) {
    return fmt::format("{}.{}.{}", version.version / 100, version.version % 100, version.patch);
  };
  const IgnoredBlenderVersions &ignored_versions = ignored_blender_updates();
  std::shared_ptr<DictionaryValue> ignored_versions_dict = dict->append_dict("ignored_versions");
  ignored_versions_dict->append_str("current_release",
                                    version_to_str(ignored_versions.current_release));
  ignored_versions_dict->append_str("latest", version_to_str(ignored_versions.latest));
  ignored_versions_dict->append_str("latest_lts", version_to_str(ignored_versions.latest_lts));

  std::shared_ptr<ArrayValue> updates_array = dict->append_array("blender_updates");
  Vector<const VersionUpdate *> updates = available_updates();
  for (const VersionUpdate *update : updates) {
    updates_array->append(version_update_serialize(*update));
  }
  std::chrono::time_point time_seconds = std::chrono::time_point_cast<std::chrono::seconds>(
      last_time_version_update_check());
  dict->append_str("last_time_check", fmt::format("{:%FT%TZ}", time_seconds));
  io::serialize::write_json_file(available_updates_file, *dict);
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
  if (!updates_notifications_flags()) {
    return;
  }
  const int64_t days_since_last_check = std::chrono::duration_cast<std::chrono::days>(
                                            (std::chrono::system_clock::now() -
                                             std::chrono::system_clock::time_point(
                                                 last_time_version_update_check())))
                                            .count();

  if (days_since_last_check >= 1) {
    download_available_updates_list(C);
  }
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

bool have_available_updates(bContext &C)
{
  if (!updates_notifications_flags()) {
    return false;
  }
  if (check_for_updates_state() == CheckForUpdatesState::Done) {
    load_latest_available_updates_file(C);
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
    result.last()->track_type = TrackType::Latest;
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
      result.last()->track_type = TrackType::Latest;
      return;
    }
    if (!(notifications_flags & USER_BLENDER_UPDATE_LATEST_LTS_RELEASE)) {
      return;
    }
    result.append(&*updates.latest_lts);
    result.last()->track_type = TrackType::LatestLTS;
  }();
  [&]() -> void {
    if (!updates.current_release) {
      return;
    }
    if (notifications_flags & USER_BLENDER_UPDATE_CURRENT_RELEASE) {
      result.append(&*updates.current_release);
      result.last()->track_type = TrackType::CurrentRelease;
      return;
    }
    /* Add current Release Update as Latest Release when there is no Latest Release or Latest LTS
     * Release available. */
    if ((notifications_flags & USER_BLENDER_UPDATE_LATEST_RELEASE) && result.is_empty() &&
        (updates.current_release->version > ignored_updates.latest))
    {
      result.append(&*updates.current_release);
      result.last()->track_type = TrackType::Latest;
      return;
    }
    /* Add current Release Update as Latest LTS Release when the update is LTS and there is no
     * other Latest LTS Release. */
    if ((notifications_flags & USER_BLENDER_UPDATE_LATEST_LTS_RELEASE) &&
        updates.current_release->is_lts && (result.is_empty() || !result.last()->is_lts) &&
        updates.current_release->version > ignored_updates.latest_lts)
    {
      result.append(&*updates.current_release);
      result.last()->track_type = TrackType::LatestLTS;
      return;
    }
  }();
  return result.as_span();
}

static void ignore_update_impl(const VersionUpdate *update)
{
  IgnoredBlenderVersions &ignored_updates = ignored_blender_updates();

  BlenderUpdates &updates = available_blender_updates();

  if (update->version.version == BLENDER_VERSION &&
      update->version > ignored_updates.current_release)
  {
    ignored_updates.current_release = update->version;
  }

  if (update->is_lts && ignored_updates.latest_lts < update->version) {
    ignored_updates.latest_lts = update->version;
  }

  if (ignored_updates.latest < update->version) {
    ignored_updates.latest = update->version;
  }

  if (updates.latest && update == &*updates.latest) {
    updates.latest = std::nullopt;
  }
  if (updates.latest_lts && update == &*updates.latest_lts) {
    updates.latest_lts = std::nullopt;
  }
  if (updates.current_release && update == &*updates.current_release) {
    updates.current_release = std::nullopt;
  }
}

void ignore_update(const VersionUpdate *update)
{
  ignore_update_impl(update);
  write_blender_updates_cache_file();
}

void ignore_all_updates()
{
  BlenderUpdates &updates = available_blender_updates();
  if (updates.latest) {
    ignore_update_impl(&*updates.latest);
  }
  if (updates.latest_lts) {
    ignore_update_impl(&*updates.latest_lts);
  }
  if (updates.current_release) {
    ignore_update_impl(&*updates.current_release);
  }
  write_blender_updates_cache_file();
}

}  // namespace blender::bke

#endif /* WITH_BLENDER_UPDATES_NOTIFICATIONS */
