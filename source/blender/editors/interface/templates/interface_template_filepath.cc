/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "MEM_guardedalloc.h"

#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_string_ref.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "interface_intern.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

struct TemplatePathData {
  PointerRNA ptr;
  PropertyRNA *path_prop;

  PropertySubType path_prop_subtype;

  std::optional<std::string> filter_glob;
};

/** Data owned by a text field editing the parent directory or the name of a path property. */
struct PathPartEdit {
  PointerRNA ptr;
  PropertyRNA *prop;
  bool is_name;
  /** Edited by the button. */
  char text[FILE_MAX];
};

static void path_part_edit_apply(bContext *C, void *argN, void * /*arg2*/)
{
  PathPartEdit *edit = static_cast<PathPartEdit *>(argN);

  /* An empty name would turn the path into its parent directory. */
  if (edit->is_name && (edit->text[0] == '\0' || BLI_path_slash_find(edit->text))) {
    return;
  }

  char path[FILE_MAX];
  RNA_property_string_get(&edit->ptr, edit->prop, path);
  BLI_path_slash_rstrip(path);
  char dir[FILE_MAX] = "";
  char name[FILE_MAXFILE] = "";
  BLI_path_split_dir_file(path, dir, sizeof(dir), name, sizeof(name));

  char new_path[FILE_MAX];
  if (edit->is_name) {
    BLI_path_join(new_path, sizeof(new_path), dir, edit->text);
  }
  else {
    BLI_path_join(new_path, sizeof(new_path), edit->text, name);
  }
  RNA_property_string_set(&edit->ptr, edit->prop, new_path);
  RNA_property_update(C, &edit->ptr, edit->prop);
}

static void path_part_edit_free(void *argN)
{
  MEM_delete(static_cast<PathPartEdit *>(argN));
}

static void *path_part_edit_copy(const void *argN)
{
  return MEM_new<PathPartEdit>(__func__, *static_cast<const PathPartEdit *>(argN));
}

/**
 * The buffer is allocated again on every redraw, so compare the edited path part instead, to
 * keep the button active while editing.
 */
static bool path_part_edit_identity_cmp(const Button *a, const Button *b)
{
  const PathPartEdit *edit_a = static_cast<const PathPartEdit *>(a->func_argN);
  const PathPartEdit *edit_b = static_cast<const PathPartEdit *>(b->func_argN);
  return edit_a->ptr.data == edit_b->ptr.data && edit_a->prop == edit_b->prop &&
         edit_a->is_name == edit_b->is_name;
}

/** Text field editing the parent directory or the name of a path property. */
static void template_filepath_part_button(Layout &layout,
                                          const PointerRNA &ptr,
                                          PropertyRNA *prop,
                                          const bool is_name)
{
  PathPartEdit *edit = MEM_new<PathPartEdit>(__func__);
  edit->ptr = ptr;
  edit->prop = prop;
  edit->is_name = is_name;

  char path[FILE_MAX];
  RNA_property_string_get(&edit->ptr, prop, path);
  BLI_path_slash_rstrip(path);
  char dir[FILE_MAX] = "";
  char name[FILE_MAXFILE] = "";
  BLI_path_split_dir_file(path, dir, sizeof(dir), name, sizeof(name));
  if (is_name) {
    STRNCPY(edit->text, name);
  }
  else {
    /* Without trailing slash, except for the file-system root. */
    STRNCPY(edit->text, dir);
    BLI_path_slash_rstrip(edit->text);
    if (edit->text[0] == '\0') {
      STRNCPY(edit->text, dir);
    }
  }

  Block *block = layout.block();
  block_layout_set_current(block, &layout);
  Button *but = uiDefBut(block,
                         ButtonType::Text,
                         "",
                         0,
                         0,
                         UI_UNIT_X * 5,
                         UI_UNIT_Y,
                         edit->text,
                         0,
                         is_name ? FILE_MAXFILE : FILE_MAX,
                         RNA_property_ui_description(prop));
  if (!RNA_property_editable(&edit->ptr, prop)) {
    button_flag_enable(but, BUT_DISABLED);
  }

  /* The button owns the data, including the edited text buffer. */
  button_funcN_set(
      but, path_part_edit_apply, edit, nullptr, path_part_edit_free, path_part_edit_copy);
  button_func_identity_compare_set(but, path_part_edit_identity_cmp);
}

static void template_filepath_buttons(const bContext * /* C */,
                                      Layout &layout,
                                      TemplatePathData &path_data,
                                      const char *pathselect_op,
                                      std::optional<StringRef> text,
                                      const std::optional<StringRef> placeholder,
                                      const bool split_name)
{
  /* With split name, the browse button is not attached to the name, to show it affects both. */
  Layout &row = layout.row(!split_name);

  if (split_name) {
    /* Parent directory and name fields, separated by a slash. */
    Layout &parts_row = row.row(true);
    template_filepath_part_button(parts_row, path_data.ptr, path_data.path_prop, false);

    /* Fixed size slash and name, so the parent directory gets the remaining space. */
    Layout &separator = parts_row.row(true);
    separator.ui_units_x_set(0.5f);
    separator.fixed_size_set(true);
    /* Unlike label(), centers the text itself and not only the button. */
    separator.label_multiline(SEP_STR, ICON_NONE, UI_STYLE_TEXT_CENTER);

    Layout &name_row = parts_row.row(true);
    name_row.ui_units_x_set(7.0f);
    name_row.fixed_size_set(true);
    template_filepath_part_button(name_row, path_data.ptr, path_data.path_prop, true);

    /* Hidden button for the property, used by the browse operator to find it. Without width, so
     * it does not add spacing or affect alignment. */
    block_layout_set_current(parts_row.block(), &parts_row);
    Button *but = uiDefAutoButR(parts_row.block(),
                                &path_data.ptr,
                                path_data.path_prop,
                                -1,
                                "",
                                ICON_NONE,
                                0,
                                0,
                                0,
                                UI_UNIT_Y);
    but->flag |= UI_HIDDEN;
  }
  else {
    row.prop(&path_data.ptr,
             path_data.path_prop,
             RNA_NO_INDEX,
             0,
             ITEM_R_PATH_NO_OPEN_BUTTON,
             text,
             ICON_NONE,
             placeholder);
  }

  const int icon = RNA_property_editable(&path_data.ptr, path_data.path_prop) ?
                       ICON_FILEBROWSER :
                       ICON_FOLDER_REDIRECT;
  if (pathselect_op) {
    PointerRNA op_ptr = row.op(
        pathselect_op, std::nullopt, icon, wm::OpCallContext::InvokeDefault, ITEM_R_ICON_ONLY);
    /* Unknown operators return an empty pointer. */
    if (op_ptr.data && path_data.path_prop_subtype == PROP_FILEPATH && path_data.filter_glob) {
      PropertyRNA *filter_glob_prop = RNA_struct_find_property(&op_ptr, "filter_glob");
      if (filter_glob_prop) {
        RNA_property_string_set(&op_ptr, filter_glob_prop, path_data.filter_glob->c_str());
      }
    }
  }
  else if (path_data.path_prop_subtype == PROP_DIRPATH) {
    /* #BUTTONS_OT_directory_browse calls #context_active_but_prop_get_filebrowser. */
    row.op("BUTTONS_OT_directory_browse",
           std::nullopt,
           icon,
           wm::OpCallContext::InvokeDefault,
           ITEM_R_ICON_ONLY);
  }
  else {
    /* #BUTTONS_OT_file_browse calls #context_active_but_prop_get_filebrowser. */
    PointerRNA op_ptr = row.op("BUTTONS_OT_file_browse",
                               std::nullopt,
                               icon,
                               wm::OpCallContext::InvokeDefault,
                               ITEM_R_ICON_ONLY);
    if (path_data.filter_glob) {
      RNA_string_set(&op_ptr, "filter_glob", path_data.filter_glob->c_str());
    }
  }
}

void template_filepath(Layout *layout,
                       const bContext *C,
                       PointerRNA *ptr,
                       const StringRefNull propname,
                       const char *pathselect_op,
                       const char *filter_glob,
                       const std::optional<StringRef> text,
                       const std::optional<StringRef> placeholder,
                       const bool split_name)
{
  TemplatePathData template_path_data;
  template_path_data.ptr = *ptr;

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (!prop || RNA_property_type(prop) != PROP_STRING) {
    RNA_warning(
        "String property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }
  template_path_data.path_prop = prop;

  template_path_data.path_prop_subtype = RNA_property_subtype(prop);
  if (!ELEM(template_path_data.path_prop_subtype, PROP_FILEPATH, PROP_DIRPATH)) {
    RNA_warning("String property is not a dirpath or filepath: %s.%s",
                RNA_struct_identifier(ptr->type),
                propname.c_str());
    return;
  }

  if (filter_glob) {
    template_path_data.filter_glob = filter_glob;
  }

  template_filepath_buttons(
      C, *layout, template_path_data, pathselect_op, text, placeholder, split_name);
}

}  // namespace blender::ui
