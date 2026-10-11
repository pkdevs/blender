/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * iOS documents: where auto-saves are kept, writing through file coordination, opening and
 * saving documents for the document browser, and the previews they show there.
 */

#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS

#  include <cstring>
#  include <ctime>
#  include <optional>
#  include <string>

#  include "BKE_appdir.hh"
#  include "BKE_context.hh"
#  include "BKE_global.hh"
#  include "BKE_image.hh"
#  include "BKE_layer.hh"
#  include "BKE_main.hh"
#  include "BKE_object.hh"
#  include "BKE_report.hh"
#  include "BLI_fileops.h"
#  include "BLI_fileops_types.h"
#  include "BLI_listbase.h"
#  include "BLI_listbase_iterator.hh"
#  include "BLI_math_matrix.hh"
#  include "BLI_math_vector.h"
#  include "BLI_math_vector.hh"
#  include "BLI_path_utils.hh"
#  include "BLI_string.h"
#  include "DEG_depsgraph_query.hh"
#  include "DNA_object_types.h"
#  include "DNA_scene_types.h"
#  include "DNA_space_types.h"
#  include "DNA_userdef_types.h"
#  include "DNA_view3d_types.h"
#  include "DNA_windowmanager_types.h"
#  include "ED_view3d.hh"
#  include "ED_view3d_offscreen.hh"
#  include "GHOST_IOSDocumentPicker.hh"
#  include "IMB_imbuf.hh"
#  include "IMB_imbuf_types.hh"
#  include "IMB_thumbs.hh"
#  include "MEM_guardedalloc.h"
#  include "RNA_access.hh"
#  include "WM_api.hh"
#  include "WM_apple_ios.hh"
#  include "WM_types.hh"

#  ifdef WITH_PYTHON
#    include "BPY_extern_run.hh"
#  endif

#  include "wm.hh"
#  include "wm_apple_ios_document.hh"
#  include "wm_files.hh"
#  include "wm_window.hh"

namespace blender {

/** Documents on iOS preview their scene framed whole, as the 3D View's Frame All shows it. */
static bool preview_frame_all = false;

/**
 * Render the 3D View's angle onto everything visible, framed as Frame All does. Lights, cameras
 * and empties are left out of the framing, so a far light does not shrink the scene.
 */
ImBuf *wm_apple_ios_thumb_frame_all(const bContext *C,
                                    Depsgraph *depsgraph,
                                    Scene *scene,
                                    View3D *v3d,
                                    ARegion *region,
                                    char err_out[256])
{
  if (!preview_frame_all || v3d == nullptr || region == nullptr) {
    return nullptr;
  }
  ViewLayer *view_layer = CTX_data_view_layer(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
  if (view_layer == nullptr || rv3d == nullptr) {
    return nullptr;
  }
  float3 min(FLT_MAX), max(-FLT_MAX);
  bool found = false;
  BKE_view_layer_synced_ensure(scene, view_layer);
  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    Object *ob = base.object;
    if (!BKE_base_is_visible(v3d, &base) ||
        ELEM(ob->type, OB_CAMERA, OB_LAMP, OB_LIGHTPROBE, OB_EMPTY, OB_SPEAKER))
    {
      continue;
    }
    const Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
    const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(ob_eval);
    if (!bounds) {
      continue;
    }
    const float4x4 &matrix = ob_eval->object_to_world();
    for (int i = 0; i < 8; i++) {
      const float3 corner((i & 1) ? bounds->max.x : bounds->min.x,
                          (i & 2) ? bounds->max.y : bounds->min.y,
                          (i & 4) ? bounds->max.z : bounds->min.z);
      const float3 world = math::transform_point(matrix, corner);
      min = math::min(min, world);
      max = math::max(max, world);
    }
    found = true;
  }
  if (!found) {
    return nullptr;
  }

  /* Frame the bounds from the view's angle; restore the view afterwards. */
  const float3 ofs_old(rv3d->ofs);
  const float dist_old = rv3d->dist;
  const char persp_old = rv3d->persp;
  const short flag2_old = v3d->flag2;
  if (rv3d->persp == RV3D_CAMOB) {
    rv3d->persp = RV3D_PERSP;
  }
  const float radius = math::max(math::distance(min, max) / 2.0f, 0.01f);
  copy_v3_v3(rv3d->ofs, -math::midpoint(min, max));
  rv3d->dist = ED_view3d_radius_to_dist(v3d, region, depsgraph, rv3d->persp, false, radius * 1.1f);
  /* Just the scene, without the grid, outlines and other overlays. */
  v3d->flag2 |= V3D_HIDE_OVERLAYS;
  const eDrawType drawtype = v3d->shading.type == OB_RENDER ? OB_MATERIAL :
                                                              eDrawType(v3d->shading.type);
  ImBuf *ibuf = ED_view3d_draw_offscreen_imbuf(depsgraph,
                                               scene,
                                               drawtype,
                                               v3d,
                                               region,
                                               PREVIEW_RENDER_LARGE_HEIGHT * 2,
                                               PREVIEW_RENDER_LARGE_HEIGHT * 2,
                                               IB_byte_data,
                                               R_ALPHAPREMUL,
                                               nullptr,
                                               true,
                                               nullptr,
                                               nullptr,
                                               false,
                                               err_out);
  v3d->flag2 = flag2_old;
  copy_v3_v3(rv3d->ofs, ofs_old);
  rv3d->dist = dist_old;
  rv3d->persp = persp_old;
  return ibuf;
}

/**
 * iOS may purge temporary files and ends apps in the background without warning. Keep auto-saves
 * where they survive, one per document rather than per process, so work continues on the next
 * launch. The name carries a hash of the document's full path, as two documents in different
 * folders can share a name.
 */
bool wm_apple_ios_autosave_location(const char *blendfile_path, char filepath[FILE_MAX])
{
  const std::optional<std::string> folder = BKE_appdir_folder_id_create(BLENDER_USER_DATAFILES,
                                                                        "autosave");
  if (!folder) {
    return false;
  }
  char filename[FILE_MAX];
  if (blendfile_path && (blendfile_path[0] != '\0')) {
    uint32_t hash = 2166136261u; /* FNV-1a: stable across launches and app versions. */
    for (const char *c = blendfile_path; *c; c++) {
      hash = (hash ^ uint8_t(*c)) * 16777619u;
    }
    const char *basename = BLI_path_basename(blendfile_path);
    SNPRINTF(filename, "%.*s_%08x_autosave.blend", int(strlen(basename)) - 6, basename, hash);
  }
  else {
    STRNCPY(filename, "untitled_autosave.blend");
  }
  BLI_path_join(filepath, FILE_MAX, folder->c_str(), filename);
  return true;
}

void wm_apple_ios_autosave_remove(const char *blendfile_path)
{
  char filepath[FILE_MAX];
  if (wm_apple_ios_autosave_location(blendfile_path, filepath) && BLI_exists(filepath)) {
    BLI_delete(filepath, false, false);
  }
}

bool wm_apple_ios_autosave_newer(const char *filepath, char r_autosave[FILE_MAX])
{
  BLI_stat_t autosave_stat, file_stat;
  return wm_apple_ios_autosave_location(filepath, r_autosave) &&
         BLI_stat(r_autosave, &autosave_stat) == 0 && BLI_stat(filepath, &file_stat) == 0 &&
         autosave_stat.st_mtime > file_stat.st_mtime;
}

bool wm_apple_ios_file_write(bContext *C,
                             char *filepath,
                             const int fileflags,
                             const eBLO_WritePathRemap remap_mode,
                             const bool use_save_as_copy,
                             ReportList *reports)
{
  /* Saving where iOS may be syncing or providing the file, such as iCloud Drive, goes through
   * file coordination, which can hand back another path to write to. */
  struct Write {
    bContext *C;
    int flags;
    eBLO_WritePathRemap remap;
    bool copy;
    ReportList *reports;
    char *filepath;
    bool success = false;
  } write{C, fileflags, remap_mode, use_save_as_copy, reports, filepath};
  char coordination_error[1024] = {};
  const bool coordinated = GHOST_ios_document_coordinate(
      filepath,
      true,
      [](void *data, const char *path) {
        auto &write = *static_cast<Write *>(data);
        BLI_strncpy(write.filepath, path, FILE_MAX);
        write.success = wm_file_write(
            write.C, path, write.flags, write.remap, write.copy, write.reports);
      },
      &write,
      coordination_error,
      sizeof(coordination_error));
  if (!coordinated) {
    BKE_report(reports, RPT_ERROR, coordination_error);
  }
  return write.success;
}

/** Auto-saves of documents left alone for a month are not worth keeping. */
static void wm_autosave_prune()
{
  const std::optional<std::string> folder = BKE_appdir_folder_id(BLENDER_USER_DATAFILES,
                                                                 "autosave");
  if (!folder) {
    return;
  }
  direntry *entries = nullptr;
  const uint count = BLI_filelist_dir_contents(folder->c_str(), &entries);
  const int64_t month_ago = int64_t(time(nullptr)) - 30 * 24 * 60 * 60;
  for (uint i = 0; i < count; i++) {
    if (!S_ISDIR(entries[i].type) && StringRef(entries[i].relname).endswith("_autosave.blend") &&
        entries[i].s.st_mtime < month_ago)
    {
      BLI_delete(entries[i].path, false, false);
    }
  }
  BLI_filelist_free(entries, count);
}

void WM_apple_ios_autosave_ensure()
{
  /* iOS ends apps without warning, so auto-save is always on. */
  U.flag |= USER_AUTOSAVE;
  if (U.savetime <= 0) {
    U.savetime = 2;
  }
  wm_autosave_prune();
}

void WM_apple_ios_autosave_now(bContext *C)
{
  /* Unlike the timer, write in any mode: this may be the last chance before iOS ends the app. */
  wmWindowManager *wm = CTX_wm_manager(C);
  Main *bmain = CTX_data_main(C);
  /* A recovered document has unsaved work even before it is edited again. */
  if (wm == nullptr || bmain == nullptr || (wm->file_saved && !bmain->recovered)) {
    return;
  }
  WM_autosave_write(wm, bmain);
}

/**
 * Run an operator on the first window, as when the system opens a file, see
 * #GHOST_kEventOpenMainFile. Documents are opened and created from outside Blender's event loop.
 */
static bool wm_document_operator(bContext *C, const char *idname, PointerRNA *props)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = wm ? static_cast<wmWindow *>(wm->windows.first) : nullptr;
  if (win == nullptr) {
    return false;
  }
  wmWindow *win_prev = CTX_wm_window(C);
  CTX_wm_window_set(C, win);
  const wmOperatorStatus status = WM_operator_name_call_ptr(
      C, WM_operatortype_find(idname, false), wm::OpCallContext::ExecDefault, props, nullptr);
  CTX_wm_window_set(C, win_prev);
  return status & OPERATOR_FINISHED;
}

bool WM_apple_ios_document_open(bContext *C, const char *filepath, const bool template_document)
{
  PointerRNA props = WM_operator_properties_create("WM_OT_open_mainfile");
  RNA_string_set(&props, "filepath", filepath);
  RNA_boolean_set(&props, "display_file_selector", false);
  if (template_document) {
    RNA_boolean_set(&props, "use_scripts", false);
  }
  const bool success = wm_document_operator(C, "WM_OT_open_mainfile", &props);
  WM_operator_properties_free(&props);
  return success;
}

/**
 * Write the open file for a document, with its preview: in the window, as saving from Blender
 * does, which a preview of the 3D View or the scene's camera needs.
 */
static bool wm_document_write(bContext *C, const char *filepath, const bool preview)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = wm ? static_cast<wmWindow *>(wm->windows.first) : nullptr;
  wmWindow *win_prev = CTX_wm_window(C);
  if (win) {
    CTX_wm_window_set(C, win);
    /* Saving from Blender happens while its window is the drawing target, which the preview's
     * drawing relies on. iOS asks between frames, when something else may draw, such as the
     * native interface's previews: make the window the target again. */
    if (preview && win->runtime->ghostwin) {
      wm_window_clear_drawable(wm);
      wm_window_make_drawable(wm, win);
    }
  }
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  const char preview_type = U.file_preview_type;
  /* A Frame All view of the scene, see #wm_apple_ios_thumb_frame_all. */
  U.file_preview_type = preview ? USER_FILE_PREVIEW_CAMERA : USER_FILE_PREVIEW_NONE;
  preview_frame_all = true;
  const bool success = wm_file_write(
      C, filepath, G.fileflags, BLO_WRITE_PATH_REMAP_NONE, true, &reports);
  U.file_preview_type = preview_type;
  preview_frame_all = false;
  BKE_reports_free(&reports);
  CTX_wm_window_set(C, win_prev);
  return success;
}

bool WM_apple_ios_document_write(bContext *C, const char *filepath, const bool preview)
{
  /* A copy for iOS to move into place: paths stay relative to the document, and the file being
   * replaced keeps no `.blend1` backup, iOS keeps versions of documents itself. */
  const bool success = wm_document_write(C, filepath, preview);
  if (!success) {
    return false;
  }
  /* The document is saved, as when saving it in Blender. */
  wm_apple_ios_autosave_remove(BKE_main_blendfile_path_from_global());
  WM_event_add_notifier(C, NC_WM | ND_FILESAVE, nullptr);
  if (wmWindowManager *wm = CTX_wm_manager(C)) {
    wm_autosave_timer_end(wm);
    wm_autosave_timer_begin(wm);
    wm->autosave_scheduled = false;
  }
  return true;
}

std::string WM_apple_ios_document_template_write(bContext *C,
                                                 const char *filepath,
                                                 const char *preview,
                                                 std::string &attribution)
{
#  ifdef WITH_PYTHON
  const char *imports[] = {"bpy", "bl_ui", "bl_ui.native_templates", nullptr};
  char *error = nullptr;
  if (!BPY_run_string_as_string(
          C, imports, "bl_ui.native_templates.export_error()", nullptr, &error))
  {
    return "Could not validate the template's resources.";
  }
  const std::string message = error ? error : "";
  if (error) {
    MEM_delete(error);
  }
  if (!message.empty()) {
    return message;
  }
  char *credit = nullptr;
  if (!BPY_run_string_as_string(
          C, imports, "bl_ui.native_templates.attribution_json()", nullptr, &credit))
  {
    return "Could not preserve the template's asset attribution.";
  }
  attribution = credit ? credit : "{}";
  if (credit) {
    MEM_delete(credit);
  }
  /* Save a copy without acknowledging or clearing the open document's unsaved work. */
  if (!wm_document_write(C, filepath, true)) {
    return "Could not save the template.";
  }
  ImBuf *image = IMB_thumb_manage(filepath, THB_LARGE, THB_SOURCE_BLEND);
  if (!image) {
    return "Could not create the template preview.";
  }
  image->ftype = IMB_FTYPE_PNG;
  const bool saved = IMB_save_image(image, preview, IB_byte_data);
  IMB_freeImBuf(image);
  return saved ? "" : "Could not save the template preview.";
#  else
  return "Template export requires Python support.";
#  endif
}

}  // namespace blender

#endif
