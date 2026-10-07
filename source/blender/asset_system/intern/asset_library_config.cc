/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#include "toml.hpp"

#include "AS_asset_library.hh"

#include "BLI_fileops.hh"  // IWYU pragma: keep
#include "BLI_path_utils.hh"
#include "BLI_string_ref.hh"

#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "WM_api.hh"

#include "asset_library_config.hh"

namespace blender::asset_system {

constexpr char BLENDER_ASSET_LIB_TOML_FILENAME[] = "blender_asset_lib.toml";

constexpr toml::spec TOML_SPEC_VERSION = toml::spec::v(1, 1, 0);

/* -------------------------------------------------------------------- */
/** \name Blender Asset Library TOML Configuration
 * @code{.unparsed}
 * # Blender Asset Library Configuration file
 *
 * [library]
 * uuid = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 * @endcode
 * \{ */

static void asset_lib_config_print_errors(const std::vector<toml::error_info> &errors)
{
  for (const toml::error_info &error : errors) {
    std::string msg = toml::format_error(error);
    fprintf(stderr, "%s\n", msg.c_str());
  }
}

static const toml::value &default_asset_lib_config_get()
{
  static toml::value value = []() -> toml::value {
    const StringRef default_str = R"_delim_(
# Blender Asset Library Configuration File

[library]

)_delim_";
    toml::result default_result = toml::try_parse_str(default_str, TOML_SPEC_VERSION);
    BLI_assert(default_result.is_ok());
    return default_result.unwrap();
  }();
  return value;
}

static toml::value *toml_find_item(toml::value &data, const StringRef key)
{
  BLI_assert(!key.is_empty());

  if (!data.is_table()) {
    /* Only tables can contain key/value pairs. */
    return nullptr;
  }

  toml::table &data_table = data.as_table();
  toml::table::iterator iter = data_table.find(key);
  if (iter == data_table.end()) {
    /* The key does not exist. */
    return nullptr;
  }
  return &iter->second;
}

/** Read the Asset Library Configutation file. */
static ReadingResult<toml::value> user_asset_library_config_read_file(
    bUserAssetLibrary &custom_library)
{
  char blender_asset_lib_toml_path[FILE_MAX];

  BLI_path_join(blender_asset_lib_toml_path,
                sizeof(blender_asset_lib_toml_path),
                custom_library.resolved_dirpath,
                BLENDER_ASSET_LIB_TOML_FILENAME);

  if (BLI_exists(blender_asset_lib_toml_path)) {
    toml::result file_result = toml::try_parse(blender_asset_lib_toml_path, TOML_SPEC_VERSION);
    if (file_result.is_err()) {
      asset_lib_config_print_errors(file_result.unwrap_err());
      return ReadingResult<>::Failure(
          N_("Error while reading asset library configuration toml file."));
    }
    return ReadingResult<toml::value>::Success(file_result.unwrap());
  }
  /* No file found, load an empty Asset Library Configuration. */
  return ReadingResult<toml::value>::Success(default_asset_lib_config_get());
}

/** Write the Asset Library Configutation file. */
static ReadingResult<> user_asset_library_config_write_file(bUserAssetLibrary &custom_library,
                                                            toml::value asset_lib_config)
{
  char blender_asset_lib_toml_path[FILE_MAX];

  BLI_path_join(blender_asset_lib_toml_path,
                sizeof(blender_asset_lib_toml_path),
                custom_library.resolved_dirpath,
                BLENDER_ASSET_LIB_TOML_FILENAME);

  std::string toml_as_string = toml::format(asset_lib_config, TOML_SPEC_VERSION);
  FILE *file_handle = BLI_fopen(blender_asset_lib_toml_path, "w");
  if (file_handle == nullptr) {
    return ReadingResult<>::Failure(N_("Error while writing asset library configuration file."));
  }
  fputs(toml_as_string.c_str(), file_handle);
  fclose(file_handle);
  return ReadingResult<>::Success();
}

/** Sets a new UUID in the Asset Library Configuration if the value is missing. */
static ReadingResult<> user_asset_library_config_file_uuid_ensure(
    bUserAssetLibrary &custom_library, toml::value &asset_lib_config)
{
  /* Ensure library section. */
  if (const toml::value *lib_value = toml_find_item(asset_lib_config, "library");
      !lib_value || !lib_value->is_table())
  {
    /* Raise error if the config `[library]` field is set but is not a table. */
    if (lib_value && !lib_value->is_table()) {
      return ReadingResult<>::Failure(
          N_("Configuration file entry [\"library\"] was not a table value."));
    }
    asset_lib_config["library"] = toml::table{};
  }

  toml::value &library_value = *toml_find_item(asset_lib_config, "library");

  if (const toml::value *uuid_value = toml_find_item(library_value, "uuid");
      !uuid_value || !uuid_value->is_string())
  {
    /* Raise error if the config `[library][uuid]` field is set but is not a string. */
    if (uuid_value && !uuid_value->is_string()) {
      return ReadingResult<>::Failure(
          N_("Configuration file entry [\"library\"][\"uuid\"] was not a table value."));
    }

    library_value["uuid"] = UUID(BLI_uuid_generate_random()).str();
    return user_asset_library_config_write_file(custom_library, asset_lib_config);
  }
  return ReadingResult<>::Success();
}

static ReadingResult<> user_asset_library_parse_uuid(bUserAssetLibrary &custom_library,
                                                     toml::value &asset_lib_config)
{
  bUUID lib_uuid;
  std::string uuid_str = asset_lib_config["library"]["uuid"].as_string();
  if (!BLI_uuid_parse_string(&lib_uuid, uuid_str.c_str())) {
    custom_library.runtime->invalid_uuid = uuid_str;
    return ReadingResult<>::Failure(N_("Invalid Asset Library UUID in configuration files."));
  }
  custom_library.runtime->uuid = lib_uuid;
  return ReadingResult<>::Success();
}

void user_asset_library_uuid_ensure(bUserAssetLibrary &custom_library)
{
  BLI_assert(!(custom_library.flag & ASSET_LIBRARY_PROJECT_DEFINED));

  const ReadingResult<> result = [&]() {
    custom_library.runtime->uuid = BLI_uuid_nil();
    custom_library.runtime->invalid_uuid.clear();

    ReadingResult<toml::value> read_result = user_asset_library_config_read_file(custom_library);
    if (read_result.is_failure()) {
      return read_result.without_success_value();
    }

    toml::value asset_lib_config_value = *read_result;

    if (ReadingResult<> ensure_uuid_config_result = user_asset_library_config_file_uuid_ensure(
            custom_library, asset_lib_config_value);
        ensure_uuid_config_result.is_failure())
    {
      return ensure_uuid_config_result;
    }

    return user_asset_library_parse_uuid(custom_library, asset_lib_config_value);
  }();
  if (result.is_failure()) {
    WM_global_reportf(RPT_ERROR,
                      "Asset Library '%s': %s",
                      custom_library.name,
                      RPT_(result.failure_reason.c_str()));
  }
}

/** \} */

}  // namespace blender::asset_system
