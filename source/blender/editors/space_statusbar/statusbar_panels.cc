/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_BLENDER_UPDATES_NOTIFICATIONS

#  include "BKE_blender_updates.hh"
#  include "BKE_blender_version.h"
#  include "BKE_global.hh"
#  include "BKE_idprop.hh"
#  include "BKE_screen.hh"

#  include "BLI_listbase.hh"
#  include "BLI_string.hh"
#  include "BLI_string_utf8.hh"
#  include "BLI_vector.hh"

#  include "BLT_translation.hh"

#  include "UI_interface.hh"
#  include "UI_interface_layout.hh"

#  include "WM_api.hh"

#  include "RNA_access.hh"

#  include <fmt/format.h>
#  include <string>

#  include "statusbar_intern.hh"

namespace blender {

static void version_update_draw_body(const bke::VersionUpdate &update,
                                     ui::Layout &layout,
                                     bool is_unique_update = false)
{
  ui::Layout &row = layout.row(true);
  row.emboss_set(ui::EmbossType::None);
  row.alignment_set(ui::LayoutAlign::Expand);
  if (update.version.version == BLENDER_VERSION) {
    row.label(IFACE_("Updating to bugfix releases is highly recommended."), ICON_NONE);
  }
  else {
    row.label(update.description, ICON_NONE);
  }

  layout.separator(0.0f);

  ui::Layout &buttons_row = layout.row(true);

  ui::Layout &left_row = buttons_row.row(false);
  left_row.alignment_set(ui::LayoutAlign::Left);
  ui::Button *button = uiDefBut(
      layout.block(),
      ui::ButtonType::But,
      is_unique_update ? IFACE_("Skip This Version") :
                         fmt::format(fmt::runtime(IFACE_("Skip {}")), update.version_str).c_str(),
      0,
      0,
      5.0f * UI_UNIT_X,
      UI_UNIT_Y,
      nullptr,
      0,
      0,
      "");
  ui::button_func_set(button, [update_info = &update, block = layout.block()](bContext &C) {
    bke::ignore_update(update_info);
    WM_event_add_notifier(&C, NC_WINDOW, nullptr);
    if (!bke::have_available_updates(C) && ui::block_is_popup(block)) {
      ui::popup_block_close(&C, CTX_wm_window(&C), block);
    }
  });
  ui::Layout &right_row = buttons_row.row(false);
  right_row.alignment_set(ui::LayoutAlign::Right);
  right_row.active_default_set(true);
  PointerRNA op_ptr = right_row.op("WM_OT_url_open", IFACE_("Download"), ICON_IMPORT);
  RNA_string_set(&op_ptr, "url", update.download_url.c_str());
};

static void panel_blender_updates_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  Vector<const bke::VersionUpdate *> available_updates = bke::available_updates();
  if (available_updates.is_empty()) {
    ui::Layout &header = layout.row(true);
    header.label(IFACE_("No updates available"), ICON_NONE);
    return;
  }
  if (available_updates.size() == 1) {
    const bke::VersionUpdate &update = *available_updates[0];
    ui::Layout &header = layout.row(true);
    const char *release_text = update.version.version != BLENDER_VERSION ?
                                   "New release available {} - {}" :
                                   "New bugfix release available: {} - {}";
    header.label(fmt::format(fmt::runtime(IFACE_(release_text)),
                             update.version_str + (update.is_lts ? " LTS" : ""),
                             update.date()),
                 ICON_NONE);
    ui::Layout &sub = header.row(false);
    sub.alignment_set(ui::LayoutAlign::Right);
    sub.link(update.release_notes_url, IFACE_("What's New"), ICON_NONE);
    version_update_draw_body(update, layout.column(false), true);
    return;
  }

  ui::Layout &header = layout.row(true);
  header.label(IFACE_("New Releases Available"), ICON_NONE);

  ui::Layout &skip_all_row = header.row(true);
  skip_all_row.alignment_set(ui::LayoutAlign::Right);
  ui::Button *button = uiDefBut(layout.block(),
                                ui::ButtonType::But,
                                IFACE_("Skip All"),
                                0,
                                0,
                                5 * UI_UNIT_X,
                                UI_UNIT_Y,
                                nullptr,
                                0,
                                0,
                                "");
  ui::button_func_set(button, [block = layout.block()](bContext &C) {
    bke::ignore_all_updates();
    WM_event_add_notifier(&C, NC_WINDOW, nullptr);
    if (ui::block_is_popup(block)) {
      ui::popup_block_close(&C, CTX_wm_window(&C), block);
    }
  });
  ui::button_drawflag_disable(button, ui::BUT_TEXT_RIGHT);
  for (const bke::VersionUpdate *update : available_updates) {
    ui::PanelLayout panel_layout = layout.panel(C, "Update_" + update->version_str, false);
    std::string version_type = update->track_type == bke::TrackType::CurrentRelease ?
                                   "current release" :
                                   (update->track_type == bke::TrackType::LatestLTS ?
                                        "latest LTS" :
                                        "latest");
    panel_layout.header->label(
        fmt::format(
            "Blender {} ({}) - {}", update->version_str, IFACE_(version_type), update->date()),
        ICON_NONE);
    /* Avoid default layout panels spacing, to prevent last panel not matching popover bounds. */
    if (update != available_updates.last()) {
      layout.separator(0.25f);
    }
    ui::Layout *body = panel_layout.body;
    if (!body) {
      continue;
    }
    ui::Layout &sub = panel_layout.header->row(false);
    sub.alignment_set(ui::LayoutAlign::Right);
    sub.link(update->release_notes_url, IFACE_("What's New"), ICON_NONE);
    version_update_draw_body(*update, body->column(false));
  }
}

static bool panel_blender_updates_poll(const bContext * /*C*/, PanelType * /*pt*/)
{
  return !bke::available_updates().is_empty();
}

void panel_blender_updates_register(ARegionType *region_type)
{

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "STATUS_PT_blender_updates");
  STRNCPY_UTF8(pt->label, N_("Updates Available"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Display available Blender release updates in a popover panel");
  pt->draw = panel_blender_updates_draw;
  pt->poll = panel_blender_updates_poll;
  pt->region_type = RGN_TYPE_HEADER;
  pt->space_type = SPACE_INFO;
  pt->ui_units_x = 20;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

}  // namespace blender

#endif /* WITH_BLENDER_UPDATES_NOTIFICATIONS */
