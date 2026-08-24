/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#include "AS_production_library.hh"

#include "BKE_appdir.hh"

#include "CLG_log.h"

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "on_disk_library.hh"
#include "production_library.hh"
#include "utils.hh"

namespace blender::asset_system {

static CLG_LogRef LOG = {"asset.library.production"};

Vector<std::string> production_datafiles_directory_paths()
{
  return BKE_appdir_folder_id_production_datafiles("assets");
}

StringRefNull production_directory_path()
{
  static std::string path = []() {
    Vector<std::string> paths = production_datafiles_directory_paths();
    return paths.is_empty() ? std::string() : paths.first();
  }();
  return path;
}

ProductionAssetLibrary::ProductionAssetLibrary()
    : OnDiskAssetLibrary(ASSET_LIBRARY_PRODUCTION,
                         {},
                         utils::normalize_directory_path(production_directory_path()),
                         /*is_read_only=*/true)
{
}

std::optional<AssetLibraryReference> ProductionAssetLibrary::library_reference() const
{
  AssetLibraryReference library_ref{};
  library_ref.custom_library_index = -1;
  library_ref.type = ASSET_LIBRARY_PRODUCTION;
  return library_ref;
}

std::optional<eAssetImportMethod> ProductionAssetLibrary::import_method() const
{
  /* Match Essentials' behavior: prefer packing linked data so production data files aren't left
   * as external dependencies, unless data-block packing is disabled experimentally. */
  if (U.experimental.no_data_block_packing) {
    return ASSET_IMPORT_APPEND_REUSE;
  }
  return ASSET_IMPORT_PACK;
}

Vector<std::string> ProductionAssetLibrary::additional_root_paths() const
{
  Vector<std::string> paths = production_datafiles_directory_paths();
  if (!paths.is_empty()) {
    paths.remove(0);
  }
  return paths;
}

void ProductionAssetLibrary::refresh_catalogs()
{
  /* Start with empty catalog storage. Don't do this directly in #this.catalog_service to avoid
   * race conditions. Rather build into a new service and replace the current one when done. */
  std::unique_ptr<AssetCatalogService> new_catalog_service = std::make_unique<AssetCatalogService>(
      AssetCatalogService::read_only_tag());

  const auto load_catalogs_fn = [&](const AssetLibrary &library) {
    library.catalog_service().reload_catalogs();

    new_catalog_service->add_from_existing(
        library.catalog_service(),
        /*on_duplicate_items=*/[](const AssetCatalog &existing,
                                  const AssetCatalog &to_be_ignored) {
          if (existing.path == to_be_ignored.path) {
            CLOG_DEBUG(&LOG,
                       "multiple definitions of catalog %s (path: %s), ignoring duplicate",
                       existing.catalog_id.str().c_str(),
                       existing.path.c_str());
          }
          else {
            CLOG_DEBUG(&LOG,
                       "multiple definitions of catalog %s with differing paths (%s vs. %s), "
                       "ignoring second one",
                       existing.catalog_id.str().c_str(),
                       existing.path.c_str(),
                       to_be_ignored.path.c_str());
          }
        });
  };

  /* First production directory is this library's own root path. */
  load_catalogs_fn(*this);

  /* Merge in catalogs from every other directory listed in `BLENDER_PRODUCTION_DATAFILES`. */
  for (const std::string &production_path : this->additional_root_paths()) {
    OnDiskAssetLibrary additional_library{
        ASSET_LIBRARY_CUSTOM, {}, utils::normalize_directory_path(production_path), true};
    load_catalogs_fn(additional_library);
  }

  std::lock_guard lock{catalog_service_mutex_};
  catalog_service_ = std::move(new_catalog_service);
}

}  // namespace blender::asset_system
