/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS

#  include "wm_apple_ios_file.hh"

#  include "BKE_context.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"
#  include "BLI_listbase.h"
#  include "BLI_path_utils.hh"
#  include "BLI_string.h"
#  include "DNA_space_types.h"
#  include "DNA_windowmanager_types.h"
#  include "GHOST_IOSDocumentPicker.hh"
#  include "GHOST_IOSMessageQueue.hh"
#  include "RNA_access.hh"
#  include "WM_api.hh"
#  include "wm_event_system.hh"
#  include "wm_event_types.hh"

#  include <map>
#  include <string>
#  include <vector>

namespace blender {
namespace {
struct Request {
  wmOperator *op;
  GHOST_IOSDocumentMode mode;
  std::string path;
};
struct Result {
  uint64_t id;
  std::vector<std::string> paths;
  std::string error;
};
std::map<uint64_t, Request> requests;
uint64_t next_request = 0;
GHOST_IOSMessageQueue<Result> results;

bool boolean(wmOperator *op, const char *name)
{
  return RNA_struct_find_property(op->ptr, name) && RNA_boolean_get(op->ptr, name);
}

std::string string(wmOperator *op, const char *name)
{
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, name);
  return prop ? RNA_property_string_get(op->ptr, prop) : "";
}

void completed(uint64_t id, const char *const *paths, int count, const char *error)
{
  Result result{id, {}, error ? error : ""};
  for (int i = 0; i < count; i++) {
    result.paths.emplace_back(paths[i]);
  }
  results.push(std::move(result));
}

bool operator_is_live(bContext *C, const wmOperator *op)
{
  for (wmWindow &win_item : CTX_wm_manager(C)->windows) {
    wmWindow *win = &win_item;
    for (wmEventHandler &base_item : win->runtime->modalhandlers) {
      wmEventHandler *base = &base_item;
      if (base->type == WM_HANDLER_TYPE_OP) {
        auto *handler = reinterpret_cast<wmEventHandler_Op *>(base);
        if (handler->is_fileselect && handler->op == op) {
          return true;
        }
      }
    }
  }
  return false;
}
}  // namespace

bool WM_apple_ios_fileselect_begin(bContext *C, wmOperator *op)
{
  /* Linking/appending browses datablocks inside .blend files, not ordinary files. */
  if (boolean(op, "filter_blenlib") || (RNA_struct_find_property(op->ptr, "filemode") &&
                                        RNA_int_get(op->ptr, "filemode") == FILE_LOADLIB))
  {
    return false;
  }
  const bool filepath = RNA_struct_find_property(op->ptr, "filepath");
  const bool directory = RNA_struct_find_property(op->ptr, "directory");
  if (!filepath && !directory) {
    return false;
  }
  const bool folder = !filepath && !RNA_struct_find_property(op->ptr, "filename") &&
                      !RNA_struct_find_property(op->ptr, "files");
  PropertyRNA *check_existing = RNA_struct_find_property(op->ptr, "check_existing");
  const bool save = check_existing && RNA_property_boolean_get_default(op->ptr, check_existing);
  const GHOST_IOSDocumentMode mode = folder ? GHOST_IOSDocumentMode::Directory :
                                     save   ? GHOST_IOSDocumentMode::Save :
                                              GHOST_IOSDocumentMode::Open;
  std::string initial = string(op, "filepath");
  if (initial.empty()) {
    char path[FILE_MAX];
    BLI_path_join(
        path, sizeof(path), string(op, "directory").c_str(), string(op, "filename").c_str());
    initial = path;
  }
  if (!initial.empty()) {
    char path[FILE_MAX];
    STRNCPY(path, initial.c_str());
    BLI_path_abs(path, BKE_main_blendfile_path(CTX_data_main(C)));
    initial = path;
  }
  std::string types;
  const struct {
    const char *property;
    const char *type;
  } filters[] = {
      {"filter_blender", "org.blenderfoundation.blender.file"},
      {"filter_backup", "public.data"},
      {"filter_image", "public.image"},
      {"filter_movie", "public.movie"},
      {"filter_sound", "public.audio"},
      {"filter_font", "public.font"},
      {"filter_text", "public.text"},
      {"filter_python", "public.python-script"},
      {"filter_archive", "public.archive"},
  };
  for (const auto &filter : filters) {
    if (boolean(op, filter.property)) {
      types += std::string(filter.type) + ";";
    }
  }
  const uint64_t id = ++next_request;
  requests.emplace(id, Request{op, mode, ""});
  if (!GHOST_ios_document_picker_begin(CTX_wm_window(C)->runtime->ghostwin,
                                       id,
                                       mode,
                                       op->type->name,
                                       initial.c_str(),
                                       types.c_str(),
                                       string(op, "filter_glob").c_str(),
                                       mode == GHOST_IOSDocumentMode::Open &&
                                           RNA_struct_find_property(op->ptr, "files"),
                                       completed))
  {
    WM_global_report(RPT_ERROR, "Unable to present the iOS document picker");
    WM_event_fileselect_event(CTX_wm_manager(C), op, EVT_FILESELECT_CANCEL);
  }
  return true;
}

bool WM_apple_ios_fileselect_active(wmOperator *op)
{
  for (const auto &item : requests) {
    if (item.second.op == op) {
      return true;
    }
  }
  return false;
}

void WM_apple_ios_fileselect_cancel(wmOperator *op)
{
  for (auto it = requests.begin(); it != requests.end();) {
    if (it->second.op == op) {
      GHOST_ios_document_picker_cancel(it->first);
      it = requests.erase(it);
    }
    else {
      ++it;
    }
  }
}

void WM_apple_ios_fileselect_flush(bContext *C)
{
  auto ready = results.take();
  for (const Result &result : ready) {
    auto it = requests.find(result.id);
    if (it == requests.end()) {
      continue;
    }
    wmOperator *op = it->second.op;
    if (!operator_is_live(C, op)) {
      WM_apple_ios_fileselect_cancel(op);
      continue;
    }
    if (!result.error.empty()) {
      WM_global_report(RPT_ERROR, result.error.c_str());
    }
    if (result.paths.empty() || !result.error.empty()) {
      WM_event_fileselect_event(CTX_wm_manager(C), op, EVT_FILESELECT_CANCEL);
      continue;
    }
    const std::string &path = result.paths.front();
    if (path.size() >= FILE_MAX) {
      WM_global_report(RPT_ERROR, "The selected path is too long");
      WM_event_fileselect_event(CTX_wm_manager(C), op, EVT_FILESELECT_CANCEL);
      continue;
    }
    it->second.path = path;
    const bool folder = it->second.mode == GHOST_IOSDocumentMode::Directory;
    char directory[FILE_MAX];
    STRNCPY(directory, path.c_str());
    if (!folder) {
      BLI_path_parent_dir(directory);
    }
    BLI_path_slash_ensure(directory, sizeof(directory));
    if (RNA_struct_find_property(op->ptr, "filepath")) {
      char filepath[FILE_MAX];
      STRNCPY(filepath, path.c_str());
      if (boolean(op, "relative_path")) {
        BLI_path_rel(filepath, BKE_main_blendfile_path(CTX_data_main(C)));
      }
      RNA_string_set(op->ptr, "filepath", filepath);
    }
    if (RNA_struct_find_property(op->ptr, "directory")) {
      RNA_string_set(op->ptr, "directory", directory);
    }
    if (RNA_struct_find_property(op->ptr, "filename")) {
      RNA_string_set(op->ptr, "filename", BLI_path_basename(path.c_str()));
    }
    if (PropertyRNA *files = RNA_struct_find_property(op->ptr, "files")) {
      RNA_property_collection_clear(op->ptr, files);
      for (const std::string &selected : result.paths) {
        PointerRNA entry;
        RNA_property_collection_add(op->ptr, files, &entry);
        RNA_string_set(&entry, "name", BLI_path_basename(selected.c_str()));
      }
    }
    WM_event_fileselect_event(CTX_wm_manager(C), op, EVT_FILESELECT_EXEC);
  }
}

wmOperatorStatus WM_apple_ios_fileselect_exec(bContext *C, wmOperator *op)
{
  for (const auto &item : requests) {
    if (item.second.op != op || item.second.path.empty()) {
      continue;
    }
    Request request = item.second; /* Loading a file can invalidate the request map. */
    /* The file-select handler has restored the invoking editor's context here. */
    if (op->type->check) {
      op->type->check(C, op);
    }
    if (RNA_struct_find_property(op->ptr, "filepath")) {
      char checked[FILE_MAX];
      RNA_string_get(op->ptr, "filepath", checked);
      BLI_path_abs(checked, BKE_main_blendfile_path(CTX_data_main(C)));
      request.path = checked;
    }
    /* .blend writes are coordinated in wm_files, including subsequent ordinary Saves. */
    if (request.mode == GHOST_IOSDocumentMode::Save &&
        (STREQ(op->type->idname, "WM_OT_save_mainfile") ||
         STREQ(op->type->idname, "WM_OT_save_as_mainfile")))
    {
      return op->type->exec(C, op);
    }
    struct Execution {
      bContext *C;
      wmOperator *op;
      bool folder;
      wmOperatorStatus status = OPERATOR_CANCELLED;
    } execution{C, op, request.mode == GHOST_IOSDocumentMode::Directory};
    char error[1024] = {};
    const bool ok = GHOST_ios_document_coordinate(
        request.path.c_str(),
        request.mode == GHOST_IOSDocumentMode::Save,
        [](void *data, const char *path) {
          auto &execution = *static_cast<Execution *>(data);
          wmOperator *op = execution.op;
          if (RNA_struct_find_property(op->ptr, "directory")) {
            char directory[FILE_MAX];
            STRNCPY(directory, path);
            if (!execution.folder) {
              BLI_path_parent_dir(directory);
            }
            BLI_path_slash_ensure(directory, sizeof(directory));
            RNA_string_set(op->ptr, "directory", directory);
          }
          if (RNA_struct_find_property(op->ptr, "filename")) {
            RNA_string_set(op->ptr, "filename", BLI_path_basename(path));
          }
          if (RNA_struct_find_property(op->ptr, "filepath")) {
            char filepath[FILE_MAX];
            STRNCPY(filepath, path);
            if (boolean(op, "relative_path")) {
              BLI_path_rel(filepath, BKE_main_blendfile_path(CTX_data_main(execution.C)));
            }
            RNA_string_set(op->ptr, "filepath", filepath);
          }
          execution.status = execution.op->type->exec(execution.C, execution.op);
        },
        &execution,
        error,
        sizeof(error));
    if (!ok) {
      BKE_report(op->reports, RPT_ERROR, error);
    }
    return execution.status;
  }
  return op->type->exec(C, op);
}

}  // namespace blender

#endif
