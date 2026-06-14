/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#ifdef WITH_OTIO

#  include <cerrno>
#  include <cstring>
#  include <map>

#  include "DNA_modifier_types.h"
#  include "DNA_object_types.h"
#  include "DNA_scene_types.h"
#  include "DNA_space_enums.h"

#  include "BKE_context.hh"
#  include "BKE_file_handler.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"

#  include "BLI_path_utils.hh"
#  include "BLI_string_utf8.h"
#  include "BLI_utildefines.h"
#  include "BLI_vector.hh"

#  include "BLT_translation.hh"

#  include "RNA_access.hh"
#  include "RNA_define.hh"
#  include "RNA_enum_types.hh"

#  include "ED_fileselect.hh"
#  include "ED_object.hh"

#  include "UI_interface.hh"
#  include "UI_resources.hh"

#  include "WM_api.hh"
#  include "WM_types.hh"

#  include "DEG_depsgraph.hh"

#  include "SEQ_sequencer.hh"

#  include "IO_otio.hh"
#  include "io_otio_ops.hh"
#  include "io_utils.hh"

#  include "UI_interface_layout.hh"

#  include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"io.otio"};

static const EnumPropertyItem io_otio_scene_strip_resolution[] = {
    {static_cast<int>(io::otio::SceneStripRes::PERCENT_100),
     "SCENE_STRIP_100",
     ICON_NONE,
     "100%",
     "Bake Scene Strips at 100% Resolution"},
    {static_cast<int>(io::otio::SceneStripRes::PERCENT_75),
     "SCENE_STRIP_75",
     ICON_NONE,
     "75%",
     "Bake Scene Strips at 75% Resolution"},
    {static_cast<int>(io::otio::SceneStripRes::PERCENT_50),
     "SCENE_STRIP_50",
     ICON_NONE,
     "50%",
     "Bake Scene Strips at 50% Resolution"},
    {static_cast<int>(io::otio::SceneStripRes::PERCENT_25),
     "SCENE_STRIP_25",
     ICON_NONE,
     "25%",
     "Bake Scene Strips at 25% Resolution"},
    {0, nullptr, 0, nullptr, nullptr}};

static const EnumPropertyItem io_otio_image_sequence_export_option[] = {
    {static_cast<int>(io::otio::ExportOption::DEFAULT),
     "IMG_SEQUENCE_DEFAULT",
     ICON_NONE,
     "Default",
     "Export as a Clip with ImageSequenceReference"},
    {static_cast<int>(io::otio::ExportOption::RENDER_MOVIE),
     "IMG_SEQUENCE_RENDER_MOVIE",
     ICON_NONE,
     "Render Movie",
     "Render and Link the Image Sequence as a Movie"},
    {0, nullptr, 0, nullptr, nullptr}};

static const EnumPropertyItem io_otio_image_sequence_export_fallback[] = {
#  ifndef WIN32
    {static_cast<int>(io::otio::ImgSeqFallback::SYMLINK),
     "IMG_SEQUENCE_SYMLINK",
     ICON_NONE,
     "Create Symlinks",
     "Create Sequenced Symbolic Links that Point to Original Images"},
#  endif
    {static_cast<int>(io::otio::ImgSeqFallback::RENDER_MOVIE),
     "IMG_SEQUENCE_RENDER_MOVIE",
     ICON_NONE,
     "Render Movie",
     "Render and Link the Image Sequence as a Movie"},
    {static_cast<int>(io::otio::ImgSeqFallback::RENAME),
     "IMG_SEQUENCE_RENAME",
     ICON_NONE,
     "Rename Images",
     "Append Sequence Numbers to Image Name"},
    {0, nullptr, 0, nullptr, nullptr}};

static const EnumPropertyItem io_otio_meta_strip_export_option[] = {
    {static_cast<int>(io::otio::ExportOption::DEFAULT),
     "META_STRIP_DEFAULT",
     ICON_NONE,
     "Default",
     "Export as a Native OTIO Stack Object"},
    {static_cast<int>(io::otio::ExportOption::RENDER_MOVIE),
     "META_STRIP_RENDER_MOVIE",
     ICON_NONE,
     "Render Movie",
     "Render and Link the Meta Strips and Seqeuncer Scene Strips as Movie"},
    {0, nullptr, 0, nullptr, nullptr}};

static wmOperatorStatus wm_otio_export_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent * /*event*/)
{
  ED_fileselect_ensure_default_filepath(C, op, ".otio");

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus wm_otio_export_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  OTIOExportParams export_params;

  export_params.bake_scene_strips = RNA_boolean_get(op->ptr, "bake_scene_strips");
  export_params.scene_strip_res = io::otio::SceneStripRes(
      RNA_enum_get(op->ptr, "scene_strip_resolution"));

  export_params.img_sequence_export = io::otio::ExportOption(
      RNA_enum_get(op->ptr, "img_sequence_export_option"));

  export_params.img_sequence_fallback = io::otio::ImgSeqFallback(
      RNA_enum_get(op->ptr, "img_sequence_fallback"));

  export_params.meta_strip_export = io::otio::ExportOption(
      RNA_enum_get(op->ptr, "meta_strip_export_option"));

  wmOperatorStatus op_stat = OTIO_export(C, filepath, &export_params);
  return op_stat;
}

static void ui_otio_export_settings(const bContext *C, ui::Layout &layout, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  /* Scene Strip Options. */
  if (ui::Layout *panel = layout.panel(C, "OTIO_export_scene", false, IFACE_("Scene Strips"))) {
    ui::Layout *col = &panel->column(false);
    col->prop(ptr, "bake_scene_strips", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    ui::Layout *sub = &col->column(false);
    sub->enabled_set(RNA_boolean_get(ptr, "bake_scene_strips"));
    sub->prop(ptr, "scene_strip_resolution", UI_ITEM_NONE, IFACE_("Resolution"), ICON_NONE);
  }

  /* Meta Strips and Sequencer Strips Options. */
  if (ui::Layout *panel = layout.panel(
          C, "OTIO_export_meta_strip", false, IFACE_("Meta Strips and Sequencer Scene Strips")))
  {
    ui::Layout *col = &panel->column(false);
    col->prop(ptr, "meta_strip_export_option", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  /* Image Sequence Options. */
  if (ui::Layout *panel = layout.panel(C, "OTIO_export_img_seq", false, IFACE_("Image Sequences")))
  {
    ui::Layout *col = &panel->column(false);
    col->prop(ptr, "img_sequence_export_option", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    ui::Layout *sub = &col->column(false);
    sub->enabled_set(io::otio::ExportOption(RNA_enum_get(ptr, "img_sequence_export_option")) ==
                     io::otio::ExportOption::DEFAULT);
    sub->prop(ptr, "img_sequence_fallback", UI_ITEM_NONE, IFACE_("Fallback Method"), ICON_NONE);
  }
}

static void wm_otio_export_draw(bContext *C, wmOperator *op)
{
  ui_otio_export_settings(C, *op->layout, op->ptr);
}

static bool wm_otio_export_check(bContext * /*C*/, wmOperator *op)
{
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  if (!BLI_path_extension_check(filepath, ".otio")) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ".otio");
    RNA_string_set(op->ptr, "filepath", filepath);
    return true;
  }

  return false;
}

void WM_OT_otio_export(wmOperatorType *ot)
{
  ot->name = "Export OTIO";
  ot->description = "Export Sequencer Scene as OpenTimelineIO Timeline";
  ot->idname = "WM_OT_otio_export";

  ot->invoke = wm_otio_export_invoke;
  ot->exec = wm_otio_export_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_otio_export_draw;
  ot->check = wm_otio_export_check;
  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  PropertyRNA *prop = RNA_def_string(ot->srna, "filter_glob", "*.otio", 0, "", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  RNA_def_boolean(
      ot->srna, "bake_scene_strips", true, "Bake Scene Strips", "Export Scene Strips as Video");

  RNA_def_enum(ot->srna,
               "scene_strip_resolution",
               io_otio_scene_strip_resolution,
               static_cast<int>(io::otio::SceneStripRes::PERCENT_100),
               "Scene Strip Resolution",
               "Resolution at which to Export the Scene Strips");

  RNA_def_enum(ot->srna,
               "img_sequence_export_option",
               io_otio_image_sequence_export_option,
               static_cast<int>(io::otio::ExportOption::DEFAULT),
               "Export Method",
               "Method to Use to Export Image Sequences");

  RNA_def_enum(ot->srna,
               "img_sequence_fallback",
               io_otio_image_sequence_export_fallback,
#  ifndef WIN32
               static_cast<int>(io::otio::ImgSeqFallback::SYMLINK),
#  else
               static_cast<int>(io::otio::ImgSeqFallback::RENDER_MOVIE),
#  endif
               "Fallback Method",
               "Method to Use to Export Non-Sequenced Image Sequences");

  RNA_def_enum(ot->srna,
               "meta_strip_export_option",
               io_otio_meta_strip_export_option,
               static_cast<int>(io::otio::ExportOption::DEFAULT),
               "Export Method",
               "Method to Use to Export Meta Strips or Sequencer Scene Strips");
}

namespace ed::io {
void otio_file_handler_add()
{
  auto fh = std::make_unique<bke::FileHandlerType>();
  STRNCPY_UTF8(fh->idname, "IO_FH_otio");
  STRNCPY_UTF8(fh->export_operator, "WM_OT_otio_export");
  STRNCPY_UTF8(fh->label, "OpenTimelineIO");
  STRNCPY_UTF8(fh->file_extensions_str, ".otio");
  fh->poll_drop = poll_file_object_drop;
  bke::file_handler_add(std::move(fh));
}
}  // namespace ed::io
}  // namespace blender

#endif
