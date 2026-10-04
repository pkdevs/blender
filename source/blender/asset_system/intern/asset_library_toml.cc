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

constexpr toml::spec version = toml::spec::v(1, 1, 0);

/* -------------------------------------------------------------------- */
/** \name Blender Asset Library TOML Configuration
 * @code{.unparsed}
 * # Example blender_asset_lib.toml configuration file
 * title = "Asset Library Config"
 * name = "Blender"
 *
 * [library]
 * # Name of the library when registered; informational only. 
 * name = "Asset Library Name"
 * uuid = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 * # # Optional: Link to the remote library if this library is hosted online; informational only.
 * # remore_url = "url"
 * @endcode
 * \{ */

static const toml::value &default_asset_lib_config_get()
{
  static toml::value value = []() -> toml::value {
    const StringRef default_str = R"_delim_(
title = "Asset Library Config"
name = "Blender"

[library]

)_delim_";
    toml::result default_result = toml::try_parse_str(default_str, version);
    if (!default_result.is_ok()) {
      asset_lib_config_print_errors(default_result.unwrap_err());
      return toml::value();
    }
    return default_result.unwrap();
  }();
  return value;
}

static void asset_lib_config_print_errors(const std::vector<toml::error_info> &errors)
{
  for (const toml::error_info &error : errors) {
    std::string msg = toml::format_error(error);
    fprintf(stderr, "%s\n", msg.c_str());
  }
}

static const toml::value *toml_find_item(const toml::value &data, StringRef key)
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
                "blender_asset_lib.toml");

  toml::value asset_lib_toml_value = default_asset_lib_config_get();

  if (BLI_exists(blender_asset_lib_toml_path)) {
    toml::result file_result = toml::try_parse(blender_asset_lib_toml_path, version);
    if (file_result.is_ok()) {
      asset_lib_toml_value = file_result.unwrap();
    }
    else {
      custom_library.runtime->invalid_uuid = "Broken Asset Library Config File";
      asset_lib_config_print_errors(file_result.unwrap_err());
      return;
    }
  }

  /* Set library identifier if is not already set. */
  if (toml_find_item_in_section(asset_lib_toml_value, "library", "uuid") == nullptr) {
    asset_lib_toml_value["library"]["uuid"] = UUID(BLI_uuid_generate_random()).str();
    /* Copy general library information to the .toml file. */
    asset_lib_toml_value["library"]["name"] = custom_library.name;
    if (custom_library.remote_url[0]) {
      asset_lib_toml_value["library"]["remote_url"] = custom_library.remote_url;
    }
    std::string toml_as_string = toml::format(asset_lib_toml_value, version);
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
