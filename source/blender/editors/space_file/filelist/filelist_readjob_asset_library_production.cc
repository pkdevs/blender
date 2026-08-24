/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 */

#include "AS_asset_library.hh"
#include "AS_production_library.hh"

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_asset_types.h"

#include "filelist_intern.hh"
#include "filelist_readjob.hh"

namespace blender {

static void filelist_readjob_production_asset_library(FileListReadJob *job_params,
                                                       bool *stop,
                                                       bool *do_update,
                                                       float *progress)
{
  FileList *filelist = job_params->tmp_filelist; /* Use the thread-safe filelist queue. */
  BLI_assert(filelist->filelist.entries.is_empty() &&
             (filelist->filelist.entries_num == FILEDIR_NBR_ENTRIES_UNSET));

  filelist_readjob_load_asset_library_data(job_params, do_update);

  /* A valid, but empty file-list from now. */
  filelist->filelist.entries_num = 0;

  BLI_assert(job_params->filelist->asset_library_ref->type == ASSET_LIBRARY_PRODUCTION);
  BLI_assert(job_params->load_asset_library != nullptr);

  /* Read the first directory listed in `BLENDER_PRODUCTION_DATAFILES`, which is this library's
   * main root path. */
  filelist_readjob_recursive_dir_add_items(true, job_params, stop, do_update, progress);

  /* Also read every other directory listed in `BLENDER_PRODUCTION_DATAFILES`. Their catalogs
   * were already merged into this library's catalog service (see
   * `ProductionAssetLibrary::refresh_catalogs()`). */
  for (const std::string &additional_path : job_params->load_asset_library->additional_root_paths())
  {
    STRNCPY(filelist->filelist.root, additional_path.c_str());
    BLI_path_slash_ensure(filelist->filelist.root, sizeof(filelist->filelist.root));

    float progress_additional = 0.0f;
    filelist_readjob_recursive_dir_add_items(
        true, job_params, stop, do_update, &progress_additional);
  }
}

void filelist_set_readjob_production_asset_library(FileList *filelist)
{
  filelist->check_dir_fn = filelist_checkdir_return_always_valid;
  filelist->read_job_fn = filelist_readjob_production_asset_library;
  filelist->prepare_filter_fn = prepare_filter_asset_library;
  filelist->filter_fn = is_filtered_asset_library;
  filelist->tags |= FILELIST_TAGS_APPLY_FUZZY_SEARCH;
}

}  // namespace blender
