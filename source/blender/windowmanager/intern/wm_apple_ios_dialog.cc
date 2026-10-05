/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS

#  include "WM_api.hh"
#  include "WM_apple_ios.hh"
#  include "wm_event_system.hh"

#  include "BKE_context.hh"
#  include "BKE_report.hh"
#  include "BKE_screen.hh"
#  include "BLI_listbase.h"
#  include "BLT_translation.hh"
#  include "DNA_screen_types.h"
#  include "DNA_windowmanager_types.h"
#  include "GHOST_IOSDialog.hh"
#  include "GHOST_IOSMessageQueue.hh"

#  include <atomic>
#  include <map>
#  include <memory>
#  include <string>

namespace {
struct Request {
  uint64_t id;
  wmWindow *window;
  std::string key;
  std::function<void(bContext *, int, uint64_t)> complete;
  std::function<void(bContext *)> cancel;
};
struct Result {
  uint64_t id;
  int action;
  uint64_t toggles;
};
std::atomic<uint64_t> next_request{0};
std::map<uint64_t, std::unique_ptr<Request>> requests;
GHOST_IOSMessageQueue<Result> results;

void completed(uint64_t id, int action, uint64_t toggles)
{
  results.push({id, action, toggles});
}

int handle(bContext *, const wmEvent *, void *)
{
  return WM_UI_HANDLER_BREAK;
}

void removed(bContext *C, void *data)
{
  const uint64_t id = static_cast<Request *>(data)->id;
  auto it = requests.find(id);
  if (it == requests.end()) {
    return;
  }
  auto request = std::move(it->second);
  requests.erase(it);
  GHOST_ios_dialog_cancel(id);
  if (request->cancel) {
    request->cancel(C);
  }
}

wmEventHandler_UI *find_handler(bContext *C, const Request &request)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm || BLI_findindex(&wm->windows, request.window) == -1) {
    return nullptr;
  }
  LISTBASE_FOREACH (wmEventHandler *, base, &request.window->modalhandlers) {
    if (base->type == WM_HANDLER_TYPE_UI) {
      auto *handler = reinterpret_cast<wmEventHandler_UI *>(base);
      if (handler->handle_fn == handle && handler->user_data == &request) {
        return handler;
      }
    }
  }
  return nullptr;
}

void restore_context(bContext *C, wmWindow *win, ScrArea *area, ARegion *region)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm || !win || BLI_findindex(&wm->windows, win) == -1) {
    return;
  }
  CTX_wm_window_set(C, win);
  bScreen *screen = CTX_wm_screen(C);
  area = screen && BLI_findindex(&screen->areabase, area) != -1 ? area : nullptr;
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, area && BLI_findindex(&area->regionbase, region) != -1 ? region : nullptr);
}
}  // namespace

bool WM_apple_ios_dialog_begin(bContext *C,
                               const GHOST_IOSDialog &dialog,
                               std::function<void(bContext *, int, uint64_t)> complete,
                               std::function<void(bContext *)> cancel,
                               const char *key)
{
  wmWindow *win = CTX_wm_window(C);
  if (!win || !win->ghostwin) {
    return false;
  }
  if (key && key[0]) {
    for (const auto &item : requests) {
      if (item.second->window == win && item.second->key == key) {
        return false;
      }
    }
  }
  const uint64_t id = ++next_request;
  auto request = std::make_unique<Request>(
      Request{id, win, key ? key : "", std::move(complete), std::move(cancel)});
  Request *data = request.get();
  requests.emplace(id, std::move(request));
  WM_event_add_ui_handler(C, &win->modalhandlers, handle, removed, data, eWM_EventHandlerFlag(0));
  if (!GHOST_ios_dialog_begin(win->ghostwin, id, dialog, completed)) {
    /* Deliver cancellation in a later engine iteration, after invoke returns. */
    completed(id, -1, 0);
  }
  return true;
}

bool WM_apple_ios_dialog_active(const wmWindow *win)
{
  for (const auto &item : requests) {
    if (item.second->window == win) {
      return true;
    }
  }
  return false;
}

void WM_apple_ios_dialog_flush(bContext *C)
{
  /* An editor can remove UI handlers without calling their removal callback. */
  std::vector<uint64_t> orphaned;
  for (const auto &item : requests) {
    if (!find_handler(C, *item.second)) {
      orphaned.push_back(item.first);
    }
  }
  for (uint64_t id : orphaned) {
    auto it = requests.find(id);
    if (it != requests.end()) {
      removed(C, it->second.get());
    }
  }
  for (const Result &result : results.take()) {
    auto it = requests.find(result.id);
    if (it == requests.end()) {
      continue;
    }
    wmEventHandler_UI *handler = find_handler(C, *it->second);
    if (!handler) {
      removed(C, it->second.get());
      continue;
    }
    wmWindow *previous_window = CTX_wm_window(C);
    ScrArea *previous_area = CTX_wm_area(C);
    ARegion *previous_region = CTX_wm_region(C);
    restore_context(C, it->second->window, handler->context.area, handler->context.region);
    auto request = std::move(it->second);
    requests.erase(it);
    WM_event_remove_ui_handler(
        &request->window->modalhandlers, handle, removed, request.get(), false);
    GHOST_ios_dialog_cancel(result.id);
    /* Remove ownership before execution: an operator may load a file or close its window. */
    if (result.action < 0) {
      if (request->cancel) {
        request->cancel(C);
      }
    }
    else if (request->complete) {
      request->complete(C, result.action, result.toggles);
    }
    restore_context(C, previous_window, previous_area, previous_region);
  }
}

void WM_apple_ios_reports(wmWindow *win, ReportList *reports, int minimum_level)
{
  if (!win || !win->ghostwin || !reports) {
    return;
  }
  GHOST_IOSDialog dialog;
  dialog.title = RPT_("Report");
  int severity = 0;
  BKE_reports_lock(reports);
  LISTBASE_FOREACH (Report *, report, &reports->list) {
    if (report->type < minimum_level) {
      continue;
    }
    if (!dialog.message.empty()) {
      dialog.message += "\n\n";
    }
    dialog.message += report->message;
    if (report->type > severity) {
      severity = report->type;
      dialog.title = report->typestr;
    }
  }
  BKE_reports_unlock(reports);
  if (!dialog.message.empty()) {
    dialog.actions.push_back({IFACE_("OK")});
    dialog.preferred_action = 0;
    GHOST_ios_dialog_begin(win->ghostwin, ++next_request, dialog, nullptr);
  }
}

#endif
