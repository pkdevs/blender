/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "on_disk_library.hh"

namespace blender::asset_system {

/**
 * Asset library merging every directory listed in the `BLENDER_PRODUCTION_DATAFILES` environment
 * variable into a single, independent, first-class library, shown in the UI as "Production".
 */
class ProductionAssetLibrary : public OnDiskAssetLibrary {
 public:
  ProductionAssetLibrary();

  std::optional<AssetLibraryReference> library_reference() const override;
  std::optional<eAssetImportMethod> import_method() const override;

  Vector<std::string> additional_root_paths() const override;

  void refresh_catalogs() override;
};

}  // namespace blender::asset_system
