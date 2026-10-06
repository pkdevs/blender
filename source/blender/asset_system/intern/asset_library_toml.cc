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
# Blender Asset Library Configuration file

[library]

)_delim_";
    toml::result default_result = toml::try_parse_str(default_str, TOML_SPEC_VERSION);
    if (default_result.is_err()) {
      asset_lib_config_print_errors(default_result.unwrap_err());
      return toml::value();
    }
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

void user_asset_library_uuid_ensure(bUserAssetLibrary &custom_library)
{
  if (custom_library.flag & ASSET_LIBRARY_PROJECT_DEFINED) {
    /* Projects stores libraries identifiers in their configurations files. */
    return;
  }

  char blender_asset_lib_toml_path[FILE_MAX];

  BLI_path_join(blender_asset_lib_toml_path,
                sizeof(blender_asset_lib_toml_path),
                custom_library.resolved_dirpath,
                BLENDER_ASSET_LIB_TOML_FILENAME);

  toml::value asset_lib_toml_value = default_asset_lib_config_get();

  if (BLI_exists(blender_asset_lib_toml_path)) {
    toml::result file_result = toml::try_parse(blender_asset_lib_toml_path, TOML_SPEC_VERSION);
    if (!file_result.is_err()) {
      custom_library.runtime->invalid_uuid = "Broken Asset Library Config File";
      asset_lib_config_print_errors(file_result.unwrap_err());
      return;
    }
    else {
      asset_lib_toml_value = file_result.unwrap();
    }
  }

  /* Set library identifier if is not already set, ensure its type is string. */
  if (const toml::value *uuid_value = toml_find_item_in_section(
          asset_lib_toml_value, "library", "uuid");
      !uuid_value || !uuid_value->is_string())
  {
    /* Ensure library section. */
    if (const toml::value *lib_value = toml_find_item(asset_lib_toml_value, "library");
        !lib_value || !lib_value->is_table())
    {
      asset_lib_toml_value["library"] = toml::table{};
    }
    toml::table &library_table = asset_lib_toml_value["library"].as_table();
    library_table["uuid"] = UUID(BLI_uuid_generate_random()).str();
    std::string toml_as_string = toml::format(asset_lib_toml_value, TOML_SPEC_VERSION);
    FILE *file_handle = BLI_fopen(blender_asset_lib_toml_path, "w");
    if (file_handle == nullptr) {
      return;
    }
    fputs(toml_as_string.c_str(), file_handle);
    fclose(file_handle);
  }

  bUUID lib_uuid;
  std::string uuid_str =
      toml_find_item_in_section(asset_lib_toml_value, "library", "uuid")->as_string();
  if (!BLI_uuid_parse_string(&lib_uuid, uuid_str.c_str())) {
    custom_library.runtime->invalid_uuid = uuid_str;
    return;
  }
  custom_library.runtime->uuid = lib_uuid;
}

/** \} */

}  // namespace blender::asset_system
