/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * The native iPad and Apple Vision Pro interface, as Blender's window manager sees it: what it
 * shows in place of Blender's own, see #wmNativeInterface.
 */

#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS

#  include <cstring>

#  include "BLI_listbase.h"
#  include "BLI_string.h"

#  include "BLT_translation.hh"

#  include "DNA_space_types.h"
#  include "DNA_windowmanager_types.h"

#  include "GHOST_IOSNativeInterface.hh"
#  include "wm_apple_ios_alerts.hh"

#  include "UI_interface_icons.hh"

#  include "WM_api.hh"
#  include "WM_apple_ios.hh"
#  include "WM_keymap.hh"
#  include "WM_native_interface.hh"
#  include "WM_types.hh"

#  include "wm_apple_ios_file.hh"
#  include "wm_draw.hh"
#  include "wm_event_system.hh"
#  include "wm_window.hh"

namespace blender {

extern "C" void WM_apple_ios_ui_flush_commands(bContext *C);
extern "C" double WM_apple_ios_engine_idle_seconds(bContext *C);
extern "C" void WM_apple_ios_ui_pencil_hover_set(bool active, int x, int y);
extern "C" void WM_apple_ios_ui_navigation_hover_set(bool active);
extern "C" void WM_apple_ios_ui_view_roll_enqueue(float angle);

namespace ui {
void native_redo_operator_register(bContext *C, wmOperator *op);
void native_redo_operator_free(wmOperator *op);
void native_redo_operator_clear();
}  // namespace ui

namespace {

bool alert(bContext *C,
           const wmNativeAlert &alert,
           wmNativeAlertComplete complete,
           wmNativeAlertCancel cancel,
           const char *key)
{
  wmIOSDialog dialog;
  dialog.title = alert.title;
  dialog.message = alert.message;
  for (const wmNativeAlert::Action &action : alert.actions) {
    dialog.actions.push_back(
        {action.title,
         action.role == wmNativeAlert::Role::Cancel      ? wmIOSDialogRole::Cancel :
         action.role == wmNativeAlert::Role::Destructive ? wmIOSDialogRole::Destructive :
                                                           wmIOSDialogRole::Default});
  }
  for (const wmNativeAlert::Toggle &toggle : alert.toggles) {
    dialog.toggles.push_back({toggle.title, toggle.selected});
  }
  dialog.preferred_action = alert.preferred_action;
  return WM_apple_ios_dialog_begin(C, dialog, std::move(complete), std::move(cancel), key);
}

/**
 * A native alert for #WM_operator_confirm_ex. Its confirm button is destructive, red, when it
 * says it deletes, removes or reverts, or Blender warns about it, as iOS marks such actions.
 */
std::optional<wmOperatorStatus> confirm(bContext *C,
                                        wmOperator *op,
                                        const char *title,
                                        const char *message,
                                        const char *confirm_text,
                                        const ui::AlertIcon icon,
                                        const bool cancel_default)
{
  wmNativeAlert dialog;
  dialog.title = title ? title : WM_operatortype_name(op->type, op->ptr);
  dialog.message = message ? message : "";
  const char *confirm_label = confirm_text ? confirm_text : IFACE_("OK");
  const bool destructive = ELEM(icon, ui::AlertIcon::Warning, ui::AlertIcon::Error) ||
                           STREQ(confirm_label, IFACE_("Delete")) ||
                           STREQ(confirm_label, IFACE_("Remove")) ||
                           STREQ(confirm_label, IFACE_("Revert"));
  dialog.actions = {
      {IFACE_("Cancel"), wmNativeAlert::Role::Cancel},
      {confirm_label,
       destructive ? wmNativeAlert::Role::Destructive : wmNativeAlert::Role::Default}};
  dialog.preferred_action = cancel_default || destructive ? 0 : 1;
  auto cancel = [op](bContext *context) {
    if (op->type->cancel) {
      op->type->cancel(context, op);
    }
    WM_operator_free(op);
  };
  if (alert(
          C,
          dialog,
          [op, cancel](bContext *context, int action, uint64_t) {
            if (action == 1) {
              WM_operator_call_ex(context, op, true);
            }
            else {
              cancel(context);
            }
          },
          cancel,
          ""))
  {
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_CANCELLED;
}

/** Two fingers move the view; Blender's own keymaps have no touch gestures. */
void keyconfig_default_ensure(wmKeyConfig *keyconf)
{
  wmKeyMap *keymap = WM_keymap_ensure(
      keyconf, "3D View iPad Gestures", SPACE_VIEW3D, RGN_TYPE_WINDOW);
  if (BLI_listbase_is_empty(&keymap->items)) {
    const KeyMapItem_Params params = {MOUSEPAN, KM_ANY, KM_ANY, 0, KM_ANY};
    WM_keymap_add_item(keymap, "VIEW3D_OT_move", &params);
  }
}

wmNativeInterface native_interface_create()
{
  wmNativeInterface native;
  native.state_changed = WM_apple_ios_ui_tag_changed;
  native.reports_added = WM_apple_ios_reports;
  native.replaces_redo_panel = true;
  native.operator_registered = ui::native_redo_operator_register;
  native.operator_freed = ui::native_redo_operator_free;
  native.redo_cleared = ui::native_redo_operator_clear;
  native.fileselect_begin = WM_apple_ios_fileselect_begin;
  native.fileselect_active = WM_apple_ios_fileselect_active;
  native.fileselect_exec = WM_apple_ios_fileselect_exec;
  native.fileselect_cancel = WM_apple_ios_fileselect_cancel;
  native.alert = alert;
  native.confirm = confirm;
  native.pointer_hovering = WM_apple_ios_pencil_hover_active;
  native.direct_gizmo_press = true;
  native.hide_gizmo_tooltips = true;
  native.keyconfig_default_ensure = keyconfig_default_ensure;
  return native;
}

/** One pass of Blender's loop, run by GHOST on its engine thread, see #WM_main_loop_body. */
void engine_step(bContext *C)
{
  WM_apple_ios_ui_flush_commands(C);
  wm_window_events_process(C);
  wm_event_do_handlers(C);
  wm_event_do_notifiers(C);
  /* Input is always taken, but drawing waits for the GPU to catch up: the next frame then shows
   * all input since, instead of queuing behind frames still to be rendered. GHOST wakes this loop
   * as a frame completes. */
  if (GHOST_ios_gpu_ready()) {
    wm_draw_update(C);
  }
}

GHOST_IOSNativeInterface ghost_interface_create()
{
  GHOST_IOSNativeInterface native;
  native.engine_step = engine_step;
  native.engine_idle_seconds = WM_apple_ios_engine_idle_seconds;
  native.autosave_now = WM_apple_ios_autosave_now;
  native.pencil_hover = WM_apple_ios_ui_pencil_hover_set;
  native.navigation_hover = WM_apple_ios_ui_navigation_hover_set;
  native.view_roll = WM_apple_ios_ui_view_roll_enqueue;
  native.launcher_controller = WM_apple_ios_launcher_controller;
  native.window_controller = WM_apple_ios_window_controller;
  native.document_window_changed = WM_apple_ios_document_window_changed;
  native.document_modified = WM_apple_ios_document_set_modified;
  native.open_url = WM_apple_ios_open_url;
  return native;
}

}  // namespace

void WM_apple_ios_native_interface_register()
{
  static const GHOST_IOSNativeInterface ghost = ghost_interface_create();
  GHOST_ios_native_interface_set(&ghost);
  static const wmNativeInterface native = native_interface_create();
  WM_native_interface_set(&native);
}

}  // namespace blender

#endif
