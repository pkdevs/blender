/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Hooks for `wm_files.cc` into iOS document handling, see `wm_apple_ios_document.cc`.
 */

#pragma once

#include "BLI_path_utils.hh"

#include "BLO_writefile.hh"

namespace blender {

struct ARegion;
struct Depsgraph;
struct ImBuf;
struct ReportList;
struct Scene;
struct View3D;
struct bContext;

/**
 * The document's preview while iOS saves it: everything visible, framed from the 3D View's
 * angle as Frame All does. Null when saving for anything else, or with nothing to frame.
 */
ImBuf *wm_apple_ios_thumb_frame_all(const bContext *C,
                                    Depsgraph *depsgraph,
                                    Scene *scene,
                                    View3D *v3d,
                                    ARegion *region,
                                    char err_out[256]);

/**
 * iOS may purge temporary files and ends apps in the background without warning. Keep auto-saves
 * where they survive, one per document rather than per process, so work continues on the next
 * launch.
 */
bool wm_apple_ios_autosave_location(const char *blendfile_path, char filepath[FILE_MAX]);
/** Remove the auto-save of a document, once its work is saved. */
void wm_apple_ios_autosave_remove(const char *blendfile_path);
/**
 * An auto-save newer than the document holds changes that were never saved, because Blender was
 * closed or another file was opened: opening the document continues from it.
 */
bool wm_apple_ios_autosave_newer(const char *filepath, char r_autosave[FILE_MAX]);

/** #wm_file_write through iOS file coordination, which may change `filepath`. */
bool wm_apple_ios_file_write(bContext *C,
                             char *filepath,
                             int fileflags,
                             eBLO_WritePathRemap remap_mode,
                             bool use_save_as_copy,
                             ReportList *reports);

}  // namespace blender
