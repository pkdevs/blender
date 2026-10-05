/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GHOST_IOSDialog.hh"
#include "GHOST_IOSMessageQueue.hh"
#include "WM_apple_ios.hh"
#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct Link {
  Link *next = nullptr, *prev = nullptr;
};
struct ListBase {
  Link *first = nullptr, *last = nullptr;
};
template<typename T> struct ListBaseT : ListBase {
  struct Iterator {
    T *value;
    T &operator*() const { return *value; }
    void operator++() { value = static_cast<T *>(value->next); }
    bool operator!=(const Iterator &other) const { return value != other.value; }
  };
  Iterator begin() const { return {static_cast<T *>(first)}; }
  Iterator end() const { return {nullptr}; }
};
static void add(ListBase *list, Link *link)
{
  link->prev = list->last;
  if (list->last)
    list->last->next = link;
  else
    list->first = link;
  list->last = link;
}
static void unlink(ListBase *list, Link *link)
{
  if (link->prev)
    link->prev->next = link->next;
  else
    list->first = link->next;
  if (link->next)
    link->next->prev = link->prev;
  else
    list->last = link->prev;
}
static int BLI_findindex(const ListBase *list, const void *pointer)
{
  int index = 0;
  for (Link *item = list->first; item; item = item->next, index++) {
    if (item == pointer)
      return index;
  }
  return -1;
}
#define LISTBASE_FOREACH(type, item, list) \
  for (type item = reinterpret_cast<type>((list)->first); item; \
       item = reinterpret_cast<type>(item->next))
struct ARegion : Link {};
struct ScrArea : Link {
  ListBase regionbase;
};
struct bScreen {
  ListBase areabase;
};
struct wmEventHandler;
struct WindowRuntime {
  void *ghostwin = this;
  ListBaseT<wmEventHandler> modalhandlers;
};
struct wmWindow : Link {
  WindowRuntime storage;
  WindowRuntime *runtime = &storage;
  bScreen screen;
};
struct wmWindowManager {
  ListBase windows;
};
struct bContext {
  wmWindowManager *wm;
  wmWindow *win;
  ScrArea *area;
  ARegion *region;
};
struct wmEvent {};
enum eWM_EventHandlerFlag { DummyFlag };
constexpr int WM_HANDLER_TYPE_UI = 1, WM_UI_HANDLER_BREAK = 1;
struct wmEventHandler : Link {
  int type = WM_HANDLER_TYPE_UI;
};
struct wmEventHandler_UI : wmEventHandler {
  int (*handle_fn)(bContext *, const wmEvent *, void *);
  void (*remove_fn)(bContext *, void *);
  void *user_data;
  struct {
    ScrArea *area;
    ARegion *region;
  } context;
};
static wmWindowManager *CTX_wm_manager(bContext *C)
{
  return C->wm;
}
static wmWindow *CTX_wm_window(bContext *C)
{
  return C->win;
}
static ScrArea *CTX_wm_area(bContext *C)
{
  return C->area;
}
static ARegion *CTX_wm_region(bContext *C)
{
  return C->region;
}
static bScreen *CTX_wm_screen(bContext *C)
{
  return C->win ? &C->win->screen : nullptr;
}
static void CTX_wm_window_set(bContext *C, wmWindow *win)
{
  C->win = win;
  C->area = nullptr;
  C->region = nullptr;
}
static void CTX_wm_area_set(bContext *C, ScrArea *area)
{
  C->area = area;
  C->region = nullptr;
}
static void CTX_wm_region_set(bContext *C, ARegion *region)
{
  C->region = region;
}
static void WM_event_add_ui_handler(bContext *C,
                                    ListBase *list,
                                    int (*handle)(bContext *, const wmEvent *, void *),
                                    void (*remove)(bContext *, void *),
                                    void *data,
                                    eWM_EventHandlerFlag)
{
  auto *handler = new wmEventHandler_UI;
  handler->handle_fn = handle;
  handler->remove_fn = remove;
  handler->user_data = data;
  handler->context = {C->area, C->region};
  add(list, handler);
}
static void WM_event_remove_ui_handler(ListBase *list,
                                       int (*handle)(bContext *, const wmEvent *, void *),
                                       void (*remove)(bContext *, void *),
                                       void *data,
                                       bool)
{
  LISTBASE_FOREACH (wmEventHandler_UI *, handler, list) {
    if (handler->handle_fn == handle && handler->remove_fn == remove && handler->user_data == data)
    {
      unlink(list, handler);
      delete handler;
      return;
    }
  }
}
static GHOST_IOSDialogCompletion native_completion;
static uint64_t native_id;
static bool native_success = true;
static std::vector<uint64_t> dismissed;
bool GHOST_ios_dialog_begin(void *,
                            uint64_t id,
                            const GHOST_IOSDialog &,
                            GHOST_IOSDialogCompletion fn)
{
  native_id = id;
  native_completion = fn;
  return native_success;
}
void GHOST_ios_dialog_cancel(uint64_t id)
{
  dismissed.push_back(id);
}

// BRIDGE_IMPLEMENTATION

int main()
{
  wmWindowManager wm;
  wmWindow win;
  ScrArea area;
  ARegion region;
  add(&wm.windows, &win);
  add(&win.screen.areabase, &area);
  add(&area.regionbase, &region);
  bContext C{&wm, &win, &area, &region};
  GHOST_IOSDialog dialog;
  dialog.actions = {{"Cancel", GHOST_IOSDialogRole::Cancel},
                    {"Delete", GHOST_IOSDialogRole::Destructive}};
  int executed = 0, cancelled = 0;
  const auto engine_thread = std::this_thread::get_id();
  auto done = [&](bContext *context, int action, uint64_t toggles) {
    assert(std::this_thread::get_id() == engine_thread);
    assert(context->win == &win && context->area == &area && context->region == &region);
    assert(!WM_apple_ios_dialog_active(&win) && !win.runtime->modalhandlers.first);
    assert(action == 1 && toggles == 3);
    executed++;
  };
  auto cancel = [&](bContext *) {
    assert(std::this_thread::get_id() == engine_thread);
    cancelled++;
  };
  assert(WM_apple_ios_dialog_begin(&C, dialog, done, cancel, "confirm"));
  assert(!WM_apple_ios_dialog_begin(&C, dialog, done, cancel, "confirm"));
  assert(WM_apple_ios_dialog_active(&win));
  std::thread ui([&] { native_completion(native_id, 1, 3); });
  ui.join();
  assert(executed == 0);
  WM_apple_ios_dialog_flush(&C);
  assert(executed == 1 && cancelled == 0 && requests.empty());
  native_completion(native_id, 1, 3);
  WM_apple_ios_dialog_flush(&C);
  assert(executed == 1);

  assert(WM_apple_ios_dialog_begin(&C, dialog, done, cancel, ""));
  native_completion(native_id, -1, 0);
  WM_apple_ios_dialog_flush(&C);
  assert(cancelled == 1 && requests.empty());

  assert(WM_apple_ios_dialog_begin(&C, dialog, done, cancel, ""));
  auto *handler = static_cast<wmEventHandler_UI *>(win.runtime->modalhandlers.first);
  handler->remove_fn(&C, handler->user_data);  // File/window teardown.
  unlink(&win.runtime->modalhandlers, handler);
  delete handler;
  native_completion(native_id, 1, 3);
  WM_apple_ios_dialog_flush(&C);
  assert(cancelled == 2 && executed == 1);

  assert(WM_apple_ios_dialog_begin(&C, dialog, done, cancel, ""));
  handler = static_cast<wmEventHandler_UI *>(win.runtime->modalhandlers.first);
  unlink(&win.runtime->modalhandlers, handler);
  delete handler;  // Editor removal without callback.
  WM_apple_ios_dialog_flush(&C);
  assert(cancelled == 3 && !WM_apple_ios_dialog_active(&win));

  native_success = false;
  assert(WM_apple_ios_dialog_begin(&C, dialog, done, cancel, ""));
  assert(cancelled == 3);  // Cancellation must not free an operator during invoke.
  WM_apple_ios_dialog_flush(&C);
  assert(cancelled == 4 && requests.empty());
  native_success = true;
  assert(WM_apple_ios_dialog_begin(
      &C,
      dialog,
      [&](bContext *context, int, uint64_t) {
        context->wm = nullptr;
        context->win = nullptr;
      },
      cancel,
      ""));
  native_completion(native_id, 1, 0);
  WM_apple_ios_dialog_flush(&C);
  assert(C.wm == nullptr && C.win == nullptr && requests.empty());
}
