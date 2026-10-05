/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cassert>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "GHOST_IOSDocumentPicker.hh"
#include "GHOST_IOSMessageQueue.hh"

constexpr int FILE_MAX = 4096, FILE_LOADLIB = 1, RPT_ERROR = 1;
constexpr int EVT_FILESELECT_CANCEL = 2, EVT_FILESELECT_EXEC = 3, WM_HANDLER_TYPE_OP = 4;
enum wmOperatorStatus { OPERATOR_CANCELLED, OPERATOR_FINISHED };
#define STRNCPY(dst, src) snprintf(dst, sizeof(dst), "%s", src)
#define STREQ(a, b) (strcmp(a, b) == 0)
#define LISTBASE_FOREACH(type, var, list) \
  for (type var = (type)(list)->first; var; var = (type)var->next)
struct ListBase { void *first = nullptr; };
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
struct PropertyRNA {
  std::string text;
  bool value = false, default_value = false;
  int number = 0;
  std::vector<std::shared_ptr<struct PointerRNA>> collection;
};
struct PointerRNA { std::map<std::string, PropertyRNA> props; PointerRNA *target = nullptr; };
struct bContext;
struct wmOperator;
struct wmOperatorType {
  const char *name = "Open", *idname = "WM_OT_open_mainfile";
  wmOperatorStatus (*exec)(bContext *, wmOperator *);
  bool (*check)(bContext *, wmOperator *);
};
struct wmOperator { PointerRNA *ptr; wmOperatorType *type; void *reports = nullptr; };
struct wmEventHandler { void *next = nullptr; int type = WM_HANDLER_TYPE_OP; };
struct wmEventHandler_Op { wmEventHandler head; bool is_fileselect = true; wmOperator *op; };
struct WindowRuntime { ListBaseT<wmEventHandler> modalhandlers; void *ghostwin = nullptr; };
struct wmWindow {
  void *next = nullptr;
  WindowRuntime storage;
  WindowRuntime *runtime = &storage;
};
struct wmWindowManager { ListBaseT<wmWindow> windows; };
struct Main { const char *filepath = "/project/scene.blend"; };
struct bContext { wmWindow *win; wmWindowManager *wm; Main main; };
static wmWindowManager *CTX_wm_manager(bContext *C) { return C->wm; }
static wmWindow *CTX_wm_window(bContext *C) { return C->win; }
static Main *CTX_data_main(bContext *C) { return &C->main; }
static const char *BKE_main_blendfile_path(Main *main) { return main->filepath; }
static std::string reported;
static void BKE_report(void *, int, const char *message) { reported = message; }
static void WM_global_report(int, const char *message) { reported = message; }
static PropertyRNA *RNA_struct_find_property(PointerRNA *ptr, const char *name) {
  if (ptr->target) ptr = ptr->target;
  auto it = ptr->props.find(name);
  return it == ptr->props.end() ? nullptr : &it->second;
}
static bool RNA_boolean_get(PointerRNA *ptr, const char *name) { return ptr->props.at(name).value; }
static bool RNA_property_boolean_get_default(PointerRNA *, PropertyRNA *p) { return p->default_value; }
static int RNA_int_get(PointerRNA *ptr, const char *name) { return ptr->props.at(name).number; }
static std::string RNA_property_string_get(PointerRNA *, PropertyRNA *prop) { return prop->text; }
static void RNA_string_get(PointerRNA *ptr, const char *name, char *path) {
  snprintf(path, FILE_MAX, "%s", ptr->props.at(name).text.c_str());
}
static void RNA_string_set(PointerRNA *ptr, const char *name, const char *path) {
  (ptr->target ?: ptr)->props[name].text = path;
}
static void RNA_property_collection_clear(PointerRNA *, PropertyRNA *p) { p->collection.clear(); }
static void RNA_property_collection_add(PointerRNA *, PropertyRNA *p, PointerRNA *entry) {
  p->collection.push_back(std::make_shared<PointerRNA>());
  entry->target = p->collection.back().get();
}
static void BLI_path_join(char *path, size_t size, const char *dir, const char *name) {
  snprintf(path, size, "%s", (std::filesystem::path(dir) / name).c_str());
}
static void BLI_path_abs(char *path, const char *base) {
  if (strncmp(path, "//", 2) == 0) {
    std::string value = (std::filesystem::path(base).parent_path() / (path + 2)).string();
    snprintf(path, FILE_MAX, "%s", value.c_str());
  }
}
static void BLI_path_rel(char *path, const char *base) {
  std::string value = "//" + std::filesystem::path(path).lexically_relative(
      std::filesystem::path(base).parent_path()).string();
  snprintf(path, FILE_MAX, "%s", value.c_str());
}
static void BLI_path_parent_dir(char *path) {
  std::string parent = std::filesystem::path(path).parent_path().string();
  snprintf(path, FILE_MAX, "%s", parent.c_str());
}
static void BLI_path_slash_ensure(char *path, size_t) {
  size_t len = strlen(path);
  if (len && path[len - 1] != '/') { path[len] = '/'; path[len + 1] = 0; }
}
static const char *BLI_path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}
static std::vector<int> events;
static void WM_event_fileselect_event(wmWindowManager *, void *, int event) { events.push_back(event); }
static uint64_t shown;
static GHOST_IOSDocumentMode shown_mode;
static bool present_ok = true, coordinated_writing, coordinated_ok = true, multiple;
static int cancelled, executions, checks;
static std::string initial_path;
static GHOST_IOSDocumentCompletion done;
extern "C" bool GHOST_ios_document_picker_begin(void *, uint64_t id, GHOST_IOSDocumentMode mode,
    const char *, const char *path, const char *, const char *, bool multi, GHOST_IOSDocumentCompletion cb) {
  shown = id; shown_mode = mode; initial_path = path; multiple = multi; done = cb; return present_ok;
}
extern "C" void GHOST_ios_document_picker_cancel(uint64_t) { cancelled++; }
extern "C" bool GHOST_ios_document_coordinate(const char *path, bool writing,
    void (*execute)(void *, const char *), void *context, char *error, size_t size) {
  coordinated_writing = writing;
  if (coordinated_ok) execute(context, path);
  else snprintf(error, size, "Provider unavailable");
  return coordinated_ok;
}

// BRIDGE_IMPLEMENTATION

int main()
{
  PointerRNA props;
  wmOperatorType type;
  type.exec = [](bContext *, wmOperator *) { executions++; return OPERATOR_FINISHED; };
  type.check = [](bContext *, wmOperator *) { checks++; return false; };
  wmOperator op{&props, &type};
  wmEventHandler_Op handler{{}, true, &op};
  wmWindow win;
  win.runtime->modalhandlers.first = &handler;
  wmWindowManager wm;
  wm.windows.first = &win;
  bContext C{&win, &wm};
  props.props["filepath"].text = "//old.blend";
  props.props["filter_blender"].value = true;
  props.props["check_existing"].default_value = false;
  assert(WM_apple_ios_fileselect_begin(&C, &op));
  assert(shown_mode == GHOST_IOSDocumentMode::Open && initial_path == "/project/old.blend");
  const char *paths[] = {"/files/new.blend"};
  done(shown, paths, 1, nullptr);
  assert(events.empty() && props.props["filepath"].text == "//old.blend");
  WM_apple_ios_fileselect_flush(&C);
  assert(events.back() == EVT_FILESELECT_EXEC && props.props["filepath"].text == paths[0]);
  assert(WM_apple_ios_fileselect_exec(&C, &op) == OPERATOR_FINISHED && executions == 1);
  assert(!coordinated_writing && checks == 1);
  WM_apple_ios_fileselect_cancel(&op);
  done(shown, paths, 1, nullptr);
  events.clear();
  WM_apple_ios_fileselect_flush(&C);
  assert(events.empty() && !WM_apple_ios_fileselect_active(&op));

  // Save mode is a property of the operator, not the user's overwrite-check toggle.
  props.props["check_existing"].default_value = true;
  props.props["check_existing"].value = false;
  assert(WM_apple_ios_fileselect_begin(&C, &op) && shown_mode == GHOST_IOSDocumentMode::Save);
  done(shown, nullptr, 0, nullptr);
  WM_apple_ios_fileselect_flush(&C);
  assert(events.back() == EVT_FILESELECT_CANCEL && executions == 1);
  WM_apple_ios_fileselect_cancel(&op);

  // Multi-file imports retain folder, filenames, relative-path policy and custom options.
  props.props["check_existing"].default_value = false;
  props.props["files"];
  props.props["directory"];
  props.props["filename"];
  props.props["relative_path"].value = true;
  props.props["custom_import_option"].value = true;
  assert(WM_apple_ios_fileselect_begin(&C, &op) && multiple);
  const char *images[] = {"/project/textures/one.png", "/project/textures/two.png"};
  done(shown, images, 2, nullptr);
  WM_apple_ios_fileselect_flush(&C);
  assert(props.props["filepath"].text == "//textures/one.png");
  assert(props.props["directory"].text == "/project/textures/");
  assert(props.props["filename"].text == "one.png");
  assert(props.props["files"].collection.size() == 2);
  assert(props.props["files"].collection[1]->props["name"].text == "two.png");
  assert(props.props["custom_import_option"].value);
  coordinated_ok = false;
  assert(WM_apple_ios_fileselect_exec(&C, &op) == OPERATOR_CANCELLED);
  assert(reported == "Provider unavailable" && executions == 1);
  WM_apple_ios_fileselect_cancel(&op);

  // Stale callbacks must not dereference an operator whose modal handler disappeared.
  assert(WM_apple_ios_fileselect_begin(&C, &op));
  win.runtime->modalhandlers.first = nullptr;
  done(shown, images, 2, nullptr);
  events.clear();
  WM_apple_ios_fileselect_flush(&C);
  assert(events.empty() && !WM_apple_ios_fileselect_active(&op));
  win.runtime->modalhandlers.first = &handler;
  props.props.clear();
  props.props["directory"];
  assert(WM_apple_ios_fileselect_begin(&C, &op) && shown_mode == GHOST_IOSDocumentMode::Directory);
  WM_apple_ios_fileselect_cancel(&op);
  props.props["filemode"].number = FILE_LOADLIB;
  assert(!WM_apple_ios_fileselect_begin(&C, &op));
  props.props.erase("filemode");
  present_ok = false;
  assert(WM_apple_ios_fileselect_begin(&C, &op));
  assert(WM_apple_ios_fileselect_active(&op) && events.back() == EVT_FILESELECT_CANCEL);
  WM_apple_ios_fileselect_cancel(&op);
  assert(requests.empty());
}
