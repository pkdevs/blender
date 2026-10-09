/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 */

#include <fmt/format.h>

#include "BKE_appdir.hh"
#include "BKE_blender_project.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"

#include "BLT_translation.hh"

#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "project_intern.hh"
#include "wm_files.hh"

namespace blender {

/** Save changes to the active project before replacing or closing it, if enabled. */
static void project_auto_save(Main *bmain)
{
  if (!(U.pref_flag & USER_PREF_FLAG_PROJECT_SAVE)) {
    return;
  }
  const bool is_dirty = BKE_blender_project_read_callback(
      bmain, [](const bke::BlenderProject *project) { return project && project->is_dirty; });
  if (is_dirty) {
    BKE_blender_project_config_file_write(bmain);
  }
}

/* -------------------------------------------------------------------- */
/** \name New Project Operator
 * \{ */

static bool new_project_name_is_valid(const StringRef name)
{
  return !name.is_empty() && name.find_first_of("/\\") == StringRef::not_found;
}

static std::string new_project_name_get(PointerRNA *ptr)
{
  return StringRef(RNA_string_get(ptr, "name")).trim();
}

static std::string new_project_default_parent()
{
  char documents_path[FILE_MAXDIR];
  const char *base_path = BKE_appdir_folder_documents(documents_path) ? documents_path :
                                                                        BLI_dir_home();
  char path[FILE_MAX];
  BLI_path_join(path, sizeof(path), base_path ? base_path : "", "Blender", "Projects");
  return path;
}

/**
 * Folder of the current blend file and its parents, below the home folder and file-system root.
 * For example "~/Projects/Film/shots" and "~/Projects/Film".
 */
static Vector<std::string> new_project_blend_parent_directories(const Main *bmain)
{
  Vector<std::string> directories;
  const char *blendfile_path = BKE_main_blendfile_path(bmain);
  if (blendfile_path[0] == '\0') {
    return directories;
  }

  const char *home_path = BLI_dir_home();
  char directory[FILE_MAX];
  BLI_path_split_dir_part(blendfile_path, directory, sizeof(directory));

  while (!(home_path && BLI_path_contains(directory, home_path))) {
    char parent[FILE_MAX];
    STRNCPY(parent, directory);
    if (!BLI_path_parent_dir(parent)) {
      break;
    }
    BLI_path_slash_rstrip(directory);
    directories.append(directory);
    STRNCPY(directory, parent);
  }
  return directories;
}

static bool new_project_use_existing_parent_directory(const Main *bmain, PointerRNA *ptr)
{
  return RNA_string_length(ptr, "directory") == 0 &&
         !new_project_blend_parent_directories(bmain).is_empty();
}

/** Normalized absolute project root folder, or empty if not absolute. */
static std::string new_project_root_path(const Main *bmain, PointerRNA *ptr)
{
  char path[FILE_MAX];
  if (new_project_use_existing_parent_directory(bmain, ptr)) {
    const Vector<std::string> directories = new_project_blend_parent_directories(bmain);
    const int index = RNA_enum_get(ptr, "existing_parent_directory");
    if (!directories.index_range().contains(index)) {
      return "";
    }
    STRNCPY(path, directories[index].c_str());
  }
  else {
    RNA_string_get(ptr, "directory", path);
    BLI_path_abs(path, BKE_main_blendfile_path(bmain));
  }

  if (!BLI_path_is_abs_from_cwd(path)) {
    return "";
  }
  BLI_path_normalize(path);
  BLI_path_slash_rstrip(path);
  return path;
}

/** Root of the project containing the folder, also when the folder does not exist yet. */
static std::string new_project_find_root(const StringRefNull directory)
{
  char path[FILE_MAX];
  STRNCPY(path, directory.c_str());
  while (!BLI_is_dir(path)) {
    if (!BLI_path_parent_dir(path)) {
      return "";
    }
  }
  return BKE_is_path_inside_blender_project(path);
}

static void new_project_name_from_directory(PointerRNA *ptr, const StringRefNull directory)
{
  /* Propose the folder name as project name. */
  char path[FILE_MAX];
  STRNCPY(path, directory.c_str());
  BLI_path_slash_rstrip(path);
  const char *name = BLI_path_basename(path);
  if (new_project_name_is_valid(name)) {
    RNA_string_set(ptr, "name", name);
  }
}

/**
 * Update the directory from the project name only while its name matches, so a manually chosen
 * directory is kept.
 */
static void new_project_update_directory_from_name_set(PointerRNA *ptr)
{
  char directory[FILE_MAX];
  RNA_string_get(ptr, "directory", directory);
  BLI_path_slash_rstrip(directory);
  RNA_boolean_set(ptr,
                  "update_directory_from_name",
                  BLI_path_basename(directory) == new_project_name_get(ptr));
}

static void new_project_name_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  /* Rename the directory along with the project. */
  const std::string name = new_project_name_get(ptr);
  if (!RNA_boolean_get(ptr, "update_directory_from_name") || !new_project_name_is_valid(name)) {
    return;
  }

  char directory[FILE_MAX];
  RNA_string_get(ptr, "directory", directory);
  BLI_path_slash_rstrip(directory);
  BLI_path_parent_dir(directory);
  BLI_path_append(directory, sizeof(directory), name.c_str());
  RNA_string_set(ptr, "directory", directory);
}

static void new_project_directory_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  new_project_update_directory_from_name_set(ptr);
}

static void new_project_existing_parent_directory_update(Main *bmain,
                                                         Scene * /*scene*/,
                                                         PointerRNA *ptr)
{
  const Vector<std::string> directories = new_project_blend_parent_directories(bmain);
  const int index = RNA_enum_get(ptr, "existing_parent_directory");
  if (directories.index_range().contains(index)) {
    new_project_name_from_directory(ptr, directories[index]);
  }
}

static const EnumPropertyItem *new_project_existing_parent_directory_itemf(bContext *C,
                                                                           PointerRNA * /*ptr*/,
                                                                           PropertyRNA * /*prop*/,
                                                                           bool *r_free)
{
  /* Item strings must remain valid after returning, keep them until they change. */
  static Vector<std::string> directories;
  Vector<std::string> new_directories = new_project_blend_parent_directories(C ? CTX_data_main(C) :
                                                                                 G_MAIN);
  if (new_directories != directories) {
    directories = std::move(new_directories);
  }

  EnumPropertyItem *items = nullptr;
  int totitem = 0;
  for (const int i : directories.index_range()) {
    EnumPropertyItem item = {};
    item.value = i;
    item.identifier = directories[i].c_str();
    item.name = directories[i].c_str();
    RNA_enum_item_add(&items, &totitem, &item);
  }
  RNA_enum_item_end(&items, &totitem);
  *r_free = true;
  return items;
}

static wmOperatorStatus project_new_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  const std::string name = new_project_name_get(op->ptr);
  if (!new_project_name_is_valid(name)) {
    BKE_report(op->reports, RPT_ERROR, "Invalid project name");
    return OPERATOR_CANCELLED;
  }

  const std::string root_path = new_project_root_path(bmain, op->ptr);
  if (root_path.empty()) {
    BKE_report(op->reports, RPT_ERROR, "Invalid project root folder");
    return OPERATOR_CANCELLED;
  }

  /* Nesting inside another project is allowed, but not replacing it. */
  if (BLI_is_dir((root_path + SEP_STR + ".blender_project").c_str())) {
    BKE_report(op->reports, RPT_ERROR, "Folder already contains a project");
    return OPERATOR_CANCELLED;
  }

  if (!BLI_dir_create_recursive(root_path.c_str())) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to create folder \"%s\"", root_path.c_str());
    return OPERATOR_CANCELLED;
  }

  project_auto_save(bmain);

  BKE_blender_project_init(name, root_path);
  if (!BKE_blender_project_config_file_write(bmain)) {
    BKE_blender_project_clear();
    BKE_reportf(
        op->reports, RPT_ERROR, "Failed to write project config in \"%s\"", root_path.c_str());
    return OPERATOR_CANCELLED;
  }
  WM_main_add_notifier(NC_WINDOW, nullptr);

  if (!G.background) {
    BKE_blender_project_recent_update(bmain);
  }

  /* Show the project settings when used from the user interface. */
  if (op->flag & OP_IS_INVOKE) {
    WM_operator_name_call(
        C, "SCREEN_OT_project_setup_show", wm::OpCallContext::ExecDefault, nullptr, nullptr);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus project_new_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  Main *bmain = CTX_data_main(C);

  if (new_project_use_existing_parent_directory(bmain, op->ptr)) {
    /* With a saved file, choose the folder of the file or one of its parents. */
    RNA_enum_set(op->ptr, "existing_parent_directory", 0);
    new_project_name_from_directory(op->ptr, new_project_blend_parent_directories(bmain)[0]);
  }
  else if (RNA_string_length(op->ptr, "directory") == 0) {
    /* Otherwise a new folder named after the project, numbered to avoid existing folders.
     * For example "Untitled Project 2". */
    const std::string projects_dir = new_project_default_parent();
    const std::string base_name = new_project_name_get(op->ptr);
    std::string name = base_name;
    char directory[FILE_MAX];
    BLI_path_join(directory, sizeof(directory), projects_dir.c_str(), name.c_str());
    for (int number = 2; BLI_exists(directory); number++) {
      name = fmt::format("{} {}", base_name, number);
      BLI_path_join(directory, sizeof(directory), projects_dir.c_str(), name.c_str());
    }

    RNA_string_set(op->ptr, "name", name.c_str());
    RNA_string_set(op->ptr, "directory", directory);
  }

  new_project_update_directory_from_name_set(op->ptr);

  return WM_operator_props_dialog_popup(C,
                                        op,
                                        600,
                                        std::nullopt,
                                        IFACE_("Create Project"),
                                        false,
                                        std::nullopt,
                                        false,
                                        WM_POPUP_SIZE_LARGE,
                                        WM_POPUP_POSITION_CENTER);
}

/** Information to help notice when the wrong folder is used by accident. */
static void new_project_draw_folder_info(ui::Layout &layout, const std::string &root_path)
{
  const std::string parent_root = new_project_find_root(root_path);
  if (!parent_root.empty()) {
    if (BLI_path_cmp_normalized(parent_root.c_str(), root_path.c_str()) == 0) {
      layout.label(IFACE_("Folder already contains a project"), ICON_ERROR);
      return;
    }
    layout.label(IFACE_("Folder is inside another project"), ICON_ERROR);
  }
}

static void project_new_ui(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  const Main *bmain = CTX_data_main(C);
  PointerRNA *ptr = op->ptr;

  layout.label_multiline(IFACE_("Blender projects are directories containing blend files, "
                                "assets and other files for one project. For example a film, "
                                "game or architectural visualization."),
                         ICON_NONE);
  layout.separator();

  /* Narrower label column than property split, to leave more space for the folder. */
  constexpr float label_factor = 0.25f;
  auto labeled_row = [&](const StringRef text) -> ui::Layout & {
    ui::Layout &split = layout.split(label_factor, false);
    ui::Layout &col = split.column(false);
    col.alignment_set(ui::LayoutAlign::Right);
    col.label(text, ICON_NONE);
    return split.row(true);
  };

  labeled_row(IFACE_("Name")).prop(ptr, "name", UI_ITEM_NONE, "", ICON_NONE);

  if (new_project_use_existing_parent_directory(bmain, ptr)) {
    labeled_row(IFACE_("Project Root Folder"))
        .prop(ptr, "existing_parent_directory", UI_ITEM_NONE, "", ICON_NONE);
  }
  else {
    /* Parent and folder name fields, with a browse button for both. */
    ui::template_filepath(&labeled_row(IFACE_("Project Root Folder")),
                          C,
                          ptr,
                          "directory",
                          nullptr,
                          nullptr,
                          std::nullopt,
                          std::nullopt,
                          true);
  }

  /* Aligned with the fields, after the label column. */
  const std::string root_path = new_project_root_path(bmain, ptr);
  if (!root_path.empty()) {
    ui::Layout &split = layout.split(label_factor, false);
    split.column(false);
    new_project_draw_folder_info(split.column(false), root_path);
  }
}

void PROJECT_OT_new(wmOperatorType *ot)
{
  ot->name = "New Project";
  ot->idname = "PROJECT_OT_new";
  ot->description = "Create a new project";

  ot->invoke = project_new_invoke;
  ot->exec = project_new_exec;
  ot->ui = project_new_ui;

  PropertyRNA *prop;
  prop = RNA_def_string(
      ot->srna, "name", "Untitled Project", MAX_NAME, "Name", "Name of the project");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_update_runtime(prop, new_project_name_update);

  prop = RNA_def_string_dir_path(ot->srna,
                                 "directory",
                                 nullptr,
                                 FILE_MAX,
                                 "Project Root Folder",
                                 "Project folder, created if missing");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_update_runtime(prop, new_project_directory_update);

  prop = RNA_def_boolean(
      ot->srna, "update_directory_from_name", false, "Update Directory From Name", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  /* Choice of folder when a blend file is open, used if `directory` is not set. */
  prop = RNA_def_enum(ot->srna,
                      "existing_parent_directory",
                      rna_enum_dummy_NULL_items,
                      0,
                      "Project Root Folder",
                      "Folder of the current file or one of its parents, to turn into a project");
  RNA_def_enum_funcs(prop, new_project_existing_parent_directory_itemf);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  RNA_def_property_update_runtime(prop, new_project_existing_parent_directory_update);
}

/** \} */

static wmOperatorStatus project_open_exec(bContext *C, wmOperator *op)
{
  char dirpath[FILE_MAX];
  RNA_string_get(op->ptr, "directory", dirpath);
  std::string project_root_path = BKE_is_path_inside_blender_project(dirpath);

  if (project_root_path == "") {
    BKE_reportf(op->reports, RPT_ERROR, "Folder '%s' does not belong to a project", dirpath);
    return OPERATOR_CANCELLED;
  }

  WM_operator_name_call(
      C, "WM_OT_read_homefile", wm::OpCallContext::ExecDefault, nullptr, nullptr);

  Main *bmain = CTX_data_main(C);
  if (!BKE_blender_project_init_from_root_path(bmain, project_root_path)) {
    BKE_reportf(
        op->reports, RPT_ERROR, "Failed to load project from path: %s", project_root_path.c_str());
    return OPERATOR_CANCELLED;
  }

  if (!G.background) {
    BKE_blender_project_recent_update(bmain);
  }

  return OPERATOR_FINISHED;
}

static void project_open_after_dialog_callback(bContext *C, void *user_data)
{
  WM_operator_name_call_with_properties(C,
                                        "PROJECT_OT_open",
                                        wm::OpCallContext::InvokeDefault,
                                        static_cast<IDProperty *>(user_data),
                                        nullptr);
}

static wmOperatorStatus project_open_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  if (!RNA_boolean_get(op->ptr, "skip_save_check")) {
    RNA_boolean_set(op->ptr, "skip_save_check", true);
    if (wm_operator_close_file_dialog_if_needed(C, op, project_open_after_dialog_callback)) {
      return OPERATOR_INTERFACE;
    }
  }

  /* Open directly when the folder is given, for example for recent projects. */
  if (RNA_struct_property_is_set(op->ptr, "directory")) {
    return project_open_exec(C, op);
  }

  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

void PROJECT_OT_open(wmOperatorType *ot)
{
  ot->name = "Open Project...";
  ot->idname = "PROJECT_OT_open";
  ot->description = "Open a project, starting a new file in it";

  ot->invoke = project_open_invoke;
  ot->exec = project_open_exec;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_DIRECTORY,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  PropertyRNA *prop = RNA_def_boolean(ot->srna,
                                      "skip_save_check",
                                      false,
                                      "Skip check to prevent unsaved changes from being lost",
                                      "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static wmOperatorStatus project_close_exec(bContext *C, wmOperator * /*op*/)
{
  project_auto_save(CTX_data_main(C));
  BKE_blender_project_clear();
  WM_operator_name_call(
      C, "WM_OT_read_homefile", wm::OpCallContext::ExecDefault, nullptr, nullptr);
  return OPERATOR_FINISHED;
}

static void project_close_after_dialog_callback(bContext *C, void *user_data)
{
  WM_operator_name_call_with_properties(C,
                                        "PROJECT_OT_close",
                                        wm::OpCallContext::ExecDefault,
                                        static_cast<IDProperty *>(user_data),
                                        nullptr);
}

static wmOperatorStatus project_close_invoke(bContext *C,
                                             wmOperator *op,
                                             const wmEvent * /*event*/)
{
  if (wm_operator_close_file_dialog_if_needed(C, op, project_close_after_dialog_callback)) {
    return OPERATOR_INTERFACE;
  }
  return project_close_exec(C, op);
}

static bool project_close_poll(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  return BKE_blender_project_get(bmain) != nullptr;
}

void PROJECT_OT_close(wmOperatorType *ot)
{
  ot->name = "Close Project";
  ot->idname = "PROJECT_OT_close";
  ot->description = "Close the active project, starting a new file";

  ot->invoke = project_close_invoke;
  ot->exec = project_close_exec;
  ot->poll = project_close_poll;
}
}  // namespace blender
