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

#include "asset_library_toml.hh"

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

static const toml::value *toml_find_item(const toml::value &data, const StringRef key)
{
  BLI_assert(!key.is_empty());

  if (!data.is_table()) {
    /* Only tables can contain key/value pairs. */
    return nullptr;
  }

  const toml::table &data_table = data.as_table();
  toml::table::const_iterator iter = data_table.find(key);
  if (iter == data_table.end()) {
    /* The key does not exist. */
    return nullptr;
  }
  return &iter->second;
}

static const toml::value *toml_find_item_in_section(const toml::value &root,
                                                    StringRef section_name,
                                                    StringRef item_key)
{
  BLI_assert(!section_name.is_empty());
  BLI_assert(!item_key.is_empty());

  const toml::value *section_value = toml_find_item(root, section_name);
  if (!section_value || !section_value->is_table()) {
    /* The named section is not a table. */
    return nullptr;
  }
  /* Search item in the section. */
  return toml_find_item(*section_value, item_key);
}

static ReadingResult<toml::value> user_asset_library_read_config_file(
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
  return ReadingResult<toml::value>::Success(default_asset_lib_config_get());
}

static ReadingResult<> user_asset_library_write_config_file(bUserAssetLibrary &custom_library,
                                                            toml::value asset_lib_toml_value)
{
  char blender_asset_lib_toml_path[FILE_MAX];

  BLI_path_join(blender_asset_lib_toml_path,
                sizeof(blender_asset_lib_toml_path),
                custom_library.resolved_dirpath,
                BLENDER_ASSET_LIB_TOML_FILENAME);

  std::string toml_as_string = toml::format(asset_lib_toml_value, TOML_SPEC_VERSION);
  FILE *file_handle = BLI_fopen(blender_asset_lib_toml_path, "w");
  if (file_handle == nullptr) {
    return ReadingResult<>::Failure(N_("Error while writing asset library configuration file."));
  }
  fputs(toml_as_string.c_str(), file_handle);
  fclose(file_handle);
  return ReadingResult<>::Success();
}

static ReadingResult<> user_asset_library_uuid_ensure_imlp(bUserAssetLibrary &custom_library)
{
  BLI_assert(!(custom_library.flag & ASSET_LIBRARY_PROJECT_DEFINED));

  custom_library.runtime->uuid = BLI_uuid_nil();
  custom_library.runtime->invalid_uuid.clear();

  ReadingResult<toml::value> read_result = user_asset_library_read_config_file(custom_library);
  if (read_result.is_failure()) {
    return read_result.without_success_value();
  }

  toml::value asset_lib_toml_value = *read_result;
  ReadingResult<> success_result = ReadingResult<>::Success();

  /* Set library identifier if is not already set, ensure its type is string. */
  if (const toml::value *uuid_value = toml_find_item_in_section(
          asset_lib_toml_value, "library", "uuid");
      !uuid_value || !uuid_value->is_string())
  {
    if (uuid_value && !uuid_value->is_string()) {
      success_result.warnings.append(
          N_("Configuration file entry [\"library\"][\"uuid\"] is not a string value."));
    }
    /* Ensure library section. */
    if (const toml::value *lib_value = toml_find_item(asset_lib_toml_value, "library");
        !lib_value || !lib_value->is_table())
    {
      if (lib_value && !lib_value->is_table()) {
        success_result.warnings.append(
            N_("Configuration file entry [\"library\"] is not a table value."));
      }
      asset_lib_toml_value["library"] = toml::table{};
    }
    asset_lib_toml_value["library"]["uuid"] = UUID(BLI_uuid_generate_random()).str();
    ReadingResult<> write_result = user_asset_library_write_config_file(custom_library,
                                                                        asset_lib_toml_value);
    if (write_result.is_failure()) {
      return write_result;
    }
  }

  bUUID lib_uuid;
  std::string uuid_str =
      toml_find_item_in_section(asset_lib_toml_value, "library", "uuid")->as_string();
  if (!BLI_uuid_parse_string(&lib_uuid, uuid_str.c_str())) {
    custom_library.runtime->invalid_uuid = uuid_str;
    return ReadingResult<>::Failure(N_("Illformed asset library UUID."));
  }

  custom_library.runtime->uuid = lib_uuid;
  return success_result;
}

void user_asset_library_uuid_ensure(bUserAssetLibrary &user_library, ReportList *reports)
{
  ReadingResult result = user_asset_library_uuid_ensure_imlp(user_library);
  if (result.is_failure()) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Asset Library '%s': %s",
                user_library.name,
                RPT_(result.failure_reason.c_str()));
  }
  else if (result.has_warnings()) {
    for (const std::string &warning : result.warnings) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Asset Library '%s': %s",
                  user_library.name,
                  RPT_(warning.c_str()));
    }
  }
}

/** \} */

}  // namespace blender::asset_system
