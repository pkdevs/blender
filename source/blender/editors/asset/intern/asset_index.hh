/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include "AS_file_reader_result.hh"

#include "ED_asset_indexer.hh"

#include <memory>

namespace blender {
struct AssetMetaData;
class StringRefNull;
namespace io::serialize {
class DictionaryValue;
class Value;
}  // namespace io::serialize
}  // namespace blender

namespace blender::ed::asset::index {

struct RemoteListingAssetEntry;

std::unique_ptr<io::serialize::Value> read_contents(StringRefNull filepath);

AssetMetaData *asset_metadata_from_dictionary(const io::serialize::DictionaryValue &entry);

std::optional<bool> file_older_than_timestamp(const char *filepath, Timestamp timestamp);

/**
 * Reading of API schema version 1. See #read_remote_listing() on \a process_fn.
 * \param listing_root_dirpath: Absolute path to the remote listing root directory.
 */
asset_system::ReadingResult<> read_remote_listing_v1(
    StringRefNull listing_root_dirpath,
    RemoteListingEntryProcessFn process_fn,
    RemoteListingWaitForPagesFn wait_fn = nullptr,
    const std::optional<Timestamp> ignore_before_timestamp = std::nullopt);

}  // namespace blender::ed::asset::index
