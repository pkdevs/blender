/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * iPad shell glue for the native UIKit overlay.
 */

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_serialize.hh"
#include "GHOST_IOSMessageQueue.hh"

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "ED_screen.hh"
#include "ED_transform.hh"
#include "ED_view3d.hh"
#include "ED_view3d_pencil_hover.hh"

#include "RNA_access.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#  include "MEM_guardedalloc.h"
#endif

#include "WM_api.hh"
#include "WM_gizmo_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "wm_event_system.hh"
#include "wm_gizmo_wmapi.hh"
#include "wm_window.hh"

#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS
#  include "wm_apple_ios_file.hh"
#  include "WM_apple_ios.hh"
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace blender::ui {
std::string native_menu_query(bContext *C, const std::string &request, bool execute);
std::string native_properties_query(bContext *C, const std::string &request, bool execute);
std::string native_properties_menu_query(bContext *C, const std::string &request, bool execute);
}

namespace blender {
namespace {

enum class IOSUICommand {
  SetModeObject = 0,
  SetModeEdit = 1,
  SetModeSculpt = 2,
  SetModeVertexPaint = 3,
  SetModeWeightPaint = 4,
  SetModeTexturePaint = 5,
  SelectToolSelectBox = 6,
  SelectToolCursor = 7,
  SelectToolMove = 8,
  SelectToolRotate = 9,
  SelectToolScale = 10,
  SelectToolTransform = 11,
  SelectToolAnnotate = 12,
  SelectToolMeasure = 13,
  FrameSelected = 14,
  ResetView = 15,
  OrbitLeft = 16,
  OrbitRight = 17,
  OrbitUp = 18,
  OrbitDown = 19,
  SetViewportWireframe = 20,
  SetViewportSolid = 21,
  SetViewportMaterial = 22,
  SetViewportRendered = 23,
  ViewAll = 24,
  ViewLocal = 25,
  ViewCamera = 26,
  ViewPerspOrtho = 27,
  ViewFront = 28,
  ViewBack = 29,
  ViewRight = 30,
  ViewLeft = 31,
  ViewTop = 32,
  ViewBottom = 33,
};

struct IOSOperatorPropertyCommand {
  std::string op_name;
  std::string prop_name;
  std::string prop_value;
};

struct IOSOperatorCommand {
  std::string op_name;
  bool has_position = false;
  int x = 0, y = 0;
  int retries = 0;
};

struct IOSSettingCommand {
  std::string setting;
  std::string value;
};

struct IOSMenuRequest {
  uint64_t identifier;
  std::string route;
  bool execute;
  bool has_position = false;
  int x = 0, y = 0;
  int retries = 0;
};
using IOSUIMessage = std::variant<IOSUICommand,
                                  IOSOperatorCommand,
                                  IOSOperatorPropertyCommand,
                                  IOSSettingCommand,
                                  std::string,
                                  float,
                                  IOSMenuRequest>;
/* A single inbox preserves ordering across tools, settings, operators and menus. */
GHOST_IOSMessageQueue<IOSUIMessage> ios_ui_messages;
GHOST_IOSMessageQueue<std::pair<uint64_t, std::string>> ios_menu_completed;
/* UIKit-owned result cache. */
std::map<uint64_t, std::string> ios_menu_results;
std::atomic<uint64_t> ios_menu_next_identifier{0};
bool ios_ui_layout_ready = false;
bool ios_ui_gizmo_refresh_pending = true;
int ios_panels_visible = 0;
std::chrono::steady_clock::time_point ios_panels_next_snapshot;
std::string ios_panels_json = "null";
std::string ios_properties_state = "{\"tab\":\"OBJECT\"}";
std::string ios_shading_state = "{\"tab\":\"SHADING\"}";
std::string ios_properties_error;
int64_t ios_panels_sequence = 0;
std::atomic<bool> ios_navigation_hover_active{false};
std::atomic<bool> ios_navigation_redraw_pending{false};
struct IOSPencilHoverRequest {
  bool active = false;
  int x = 0, y = 0;
};
GHOST_IOSLatest<IOSPencilHoverRequest> ios_hover_requests;
GHOST_IOSLatest<std::string> ios_hover_snapshot;
IOSPencilHoverRequest ios_hover_request;

struct IOSActionItem {
  const char *title;
  const char *op_name;
  const char *symbol;
  const char *prop_name = "";
  const char *prop_value = "";
  const char *key = "";
  bool selected = false;
};

GHOST_IOSLatest<std::string> ios_toolbar_snapshot;
GHOST_IOSLatest<std::string> ios_panels_snapshot;
std::string ios_toolbar_json =
    "{\"mode\":\"objectmode\",\"active\":\"builtin.select_box\",\"tools\":[]}";

std::string ios_json_escape(const std::string &value);


bool ios_tool_id_starts_with(const char *tool_id, const char *prefix)
{
  return tool_id != nullptr && std::strncmp(tool_id, prefix, std::strlen(prefix)) == 0;
}

bool ios_tool_id_equals(const char *tool_id, const char *value)
{
  return tool_id != nullptr && std::strcmp(tool_id, value) == 0;
}

void ios_action_add_operator(bContext *C,
                             std::vector<IOSActionItem> &items,
                             const char *title,
                             const char *op_name,
                             const char *symbol,
                             const char *prop_name = "",
                             const char *prop_value = "")
{
  if (WM_operator_name_poll(C, op_name)) {
    items.push_back({title, op_name, symbol, prop_name, prop_value, ""});
  }
}

void ios_action_add_key(std::vector<IOSActionItem> &items,
                        const char *title,
                        const char *symbol,
                        const char *key)
{
  items.push_back({title, "", symbol, "", "", key});
}

wmOperator *ios_constraint_operator(wmWindow *win)
{
  if (win == nullptr) {
    return nullptr;
  }

  for (wmEventHandler &handler_base_item : win->runtime->modalhandlers) {
    wmEventHandler *handler_base = &handler_base_item;
    if (handler_base->type != WM_HANDLER_TYPE_OP) {
      continue;
    }
    wmEventHandler_Op *handler = reinterpret_cast<wmEventHandler_Op *>(handler_base);
    int axes;
    if (blender::ed::transform::modal_constraint_get(handler->op, axes)) {
      return handler->op;
    }
  }
  return nullptr;
}

void ios_action_add_select_all(bContext *C,
                               std::vector<IOSActionItem> &items,
                               const eContextObjectMode mode,
                               const char *title,
                               const char *action,
                               const char *symbol)
{
  const char *op_name = mode == CTX_MODE_EDIT_MESH ? "MESH_OT_select_all" :
                                                   "OBJECT_OT_select_all";
  ios_action_add_operator(C, items, title, op_name, symbol, "action", action);
}

std::vector<IOSActionItem> ios_action_items_for_tool(bContext *C,
                                                     const eContextObjectMode mode,
                                                     const char *active_tool_id)
{
  std::vector<IOSActionItem> items;

  if (wmOperator *op = ios_constraint_operator(CTX_wm_window(C))) {
    int axes;
    blender::ed::transform::modal_constraint_get(op, axes);
    for (int axis = 0; axis < 3; axis++) {
      const char *name = axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
      ios_action_add_key(items, name, "", name);
      items.back().selected = (axes & (1 << axis)) != 0;
    }
    return items;
  }

  if (ios_tool_id_starts_with(active_tool_id, "builtin.select")) {
    ios_action_add_select_all(C, items, mode, "All", "SELECT", "checkmark.circle");
    ios_action_add_select_all(C, items, mode, "None", "DESELECT", "circle");
    ios_action_add_select_all(C, items, mode, "Invert", "INVERT", "arrow.up.arrow.down.circle");
    ios_action_add_operator(C, items, "X-Ray", "VIEW3D_OT_toggle_xray", "square.2.layers.3d");

    if (mode == CTX_MODE_EDIT_MESH) {
      ios_action_add_operator(C, items, "More", "MESH_OT_select_more", "plus.circle");
      ios_action_add_operator(C, items, "Less", "MESH_OT_select_less", "minus.circle");
      ios_action_add_operator(C, items, "Linked", "MESH_OT_select_linked", "link.circle");
      ios_action_add_operator(C, items, "Select Ring", "MESH_OT_edgering_select", "circle.hexagongrid");
    }
    return items;
  }

  if (ios_tool_id_equals(active_tool_id, "builtin.move") ||
      ios_tool_id_equals(active_tool_id, "builtin.rotate") ||
      ios_tool_id_equals(active_tool_id, "builtin.scale") ||
      ios_tool_id_equals(active_tool_id, "builtin.transform"))
  {
    ios_action_add_operator(C, items, "Snap", "VIEW3D_OT_snap_selected_to_grid", "grid");
    ios_action_add_operator(C, items, "Origin", "OBJECT_OT_origin_set", "scope");
    ios_action_add_operator(C, items, "Apply", "OBJECT_OT_transform_apply", "checkmark");
    ios_action_add_operator(C, items, "Clear", "OBJECT_OT_transforms_to_deltas", "arrow.counterclockwise");
    return items;
  }

  if (ios_tool_id_equals(active_tool_id, "builtin.measure")) {
    ios_action_add_operator(C, items, "Frame", "VIEW3D_OT_view_selected", "viewfinder");
    ios_action_add_operator(C, items, "X-Ray", "VIEW3D_OT_toggle_xray", "square.2.layers.3d");
    return items;
  }

  if (mode == CTX_MODE_EDIT_MESH &&
      (ios_tool_id_equals(active_tool_id, "builtin.extrude") ||
       ios_tool_id_equals(active_tool_id, "builtin.extrude_manifold") ||
       ios_tool_id_equals(active_tool_id, "builtin.extrude_along_normals") ||
       ios_tool_id_equals(active_tool_id, "builtin.extrude_individual")))
  {
    ios_action_add_operator(C, items, "Extrude", "VIEW3D_OT_edit_mesh_extrude_move_normal", "arrow.up.square");
    ios_action_add_operator(C, items, "Inset", "MESH_OT_inset", "square.inset.filled");
    ios_action_add_operator(C, items, "Bevel", "MESH_OT_bevel", "square.on.circle");
    ios_action_add_operator(C, items, "Merge", "MESH_OT_merge", "arrow.triangle.merge");
    return items;
  }

  if (mode == CTX_MODE_SCULPT || mode == CTX_MODE_PAINT_TEXTURE || mode == CTX_MODE_PAINT_VERTEX ||
      mode == CTX_MODE_PAINT_WEIGHT)
  {
    ios_action_add_operator(C, items, "Undo", "ED_OT_undo", "arrow.uturn.backward");
    ios_action_add_operator(C, items, "Redo", "ED_OT_redo", "arrow.uturn.forward");
    ios_action_add_operator(C, items, "Frame", "VIEW3D_OT_view_selected", "viewfinder");
    return items;
  }

  return items;
}


ScrArea *ios_find_view3d_area(bScreen *screen)
{
  if (screen == nullptr) {
    return nullptr;
  }

  ScrArea *best_area = nullptr;
  int best_area_size = -1;
  for (ScrArea &area_item : screen->areabase) {
    ScrArea *area = &area_item;
    if (area->spacetype != SPACE_VIEW3D) {
      continue;
    }
    const int area_size = int(area->winx) * int(area->winy);
    if (area_size > best_area_size) {
      best_area = area;
      best_area_size = area_size;
    }
  }
  return best_area;
}

ARegion *ios_find_window_region(ScrArea *area)
{
  if (area == nullptr) {
    return nullptr;
  }

  for (ARegion &region_item : area->regionbase) {
    ARegion *region = &region_item;
    if (region->regiontype == RGN_TYPE_WINDOW) {
      return region;
    }
  }
  return nullptr;
}

wmWindow *ios_active_window(bContext *C)
{
  wmWindow *win = CTX_wm_window(C);
  if (win != nullptr) {
    return win;
  }

  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return nullptr;
  }
  return static_cast<wmWindow *>(wm->windows.first);
}

struct IOSContextStore {
  wmWindow *win;
  bScreen *screen;
  ScrArea *area;
  ARegion *region;
};

IOSContextStore ios_context_push_view3d(bContext *C)
{
  IOSContextStore store = {
      CTX_wm_window(C), CTX_wm_screen(C), CTX_wm_area(C), CTX_wm_region(C)};

  wmWindow *win = ios_active_window(C);
  bScreen *screen = win ? WM_window_get_active_screen(win) : CTX_wm_screen(C);
  ScrArea *area = ios_find_view3d_area(screen);
  ARegion *region = ios_find_window_region(area);

  CTX_wm_window_set(C, win);
  CTX_wm_screen_set(C, screen);
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, region);

  return store;
}

void ios_context_pop(bContext *C, const IOSContextStore &store)
{
  if (ios_ui_layout_ready) {
    wmWindow *win = ios_active_window(C);
    bScreen *screen = win ? WM_window_get_active_screen(win) : store.screen;
    ScrArea *area = ios_find_view3d_area(screen);
    ARegion *region = ios_find_window_region(area);

    CTX_wm_window_set(C, win);
    CTX_wm_screen_set(C, screen);
    CTX_wm_area_set(C, area);
    CTX_wm_region_set(C, region);
    return;
  }

  CTX_wm_window_set(C, store.win);
  CTX_wm_screen_set(C, store.screen);
  CTX_wm_area_set(C, store.area);
  CTX_wm_region_set(C, store.region);
}

void ios_operator_call(bContext *C, const char *op_name, PointerRNA *props = nullptr)
{
  if (WM_operator_name_poll(C, op_name)) {
    /* Smooth view uses OP_IS_INVOKE, including for operators with only an exec callback.
     * Keep non-navigation quick actions on their existing execution path. */
    const bool smooth_view = ELEM(std::string_view(op_name),
                                  "VIEW3D_OT_view_selected",
                                  "VIEW3D_OT_view_all",
                                  "VIEW3D_OT_localview",
                                  "VIEW3D_OT_view_camera",
                                  "VIEW3D_OT_view_persportho",
                                  "VIEW3D_OT_view_axis",
                                  "VIEW3D_OT_view_orbit");
    WM_operator_name_call(C,
                          op_name,
                          smooth_view ? blender::wm::OpCallContext::InvokeRegionWin :
                                        blender::wm::OpCallContext::ExecRegionWin,
                          props,
                          nullptr);
  }
}

bool ios_undo_available(bContext *C)
{
  wmWindow *win = CTX_wm_window(C);
  if (win == nullptr) {
    return false;
  }
  /* Undo must not invalidate data owned by an in-progress tool or navigation operation. */
  for (wmEventHandler &handler_item : win->runtime->modalhandlers) {
    wmEventHandler *handler = &handler_item;
    if (handler->type == WM_HANDLER_TYPE_OP) {
      return false;
    }
  }
  return WM_operator_name_poll(C, "ED_OT_undo");
}

void ios_operator_invoke(bContext *C, const char *op_name)
{
  if (STREQ(op_name, "ED_OT_undo") && !ios_undo_available(C)) {
    return;
  }
  if (WM_operator_name_poll(C, op_name)) {
    WM_operator_name_call(C, op_name, blender::wm::OpCallContext::InvokeRegionWin, nullptr, nullptr);
  }
}

void ios_operator_call_with_enum_property(bContext *C,
                                          const char *op_name,
                                          const char *prop_name,
                                          const char *prop_value)
{
  if (op_name == nullptr || prop_name == nullptr || prop_value == nullptr || prop_name[0] == '\0' ||
      prop_value[0] == '\0')
  {
    ios_operator_call(C, op_name);
    return;
  }

  wmOperatorType *ot = WM_operatortype_find(op_name, false);
  if (ot == nullptr) {
    return;
  }

  PointerRNA props = WM_operator_properties_create_ptr(ot);
  RNA_enum_set_identifier(C, &props, prop_name, prop_value);
  ios_operator_call(C, op_name, &props);
  WM_operator_properties_free(&props);
}

void ios_mode_set(bContext *C, const char *mode)
{
  wmOperatorType *ot = WM_operatortype_find("OBJECT_OT_mode_set", false);
  if (ot == nullptr) {
    return;
  }

  PointerRNA props = WM_operator_properties_create_ptr(ot);
  RNA_enum_set_identifier(C, &props, "mode", mode);
  RNA_boolean_set(&props, "toggle", false);
  if (WM_operator_name_poll(C, "OBJECT_OT_mode_set")) {
    WM_operator_name_call_ptr(C, ot, blender::wm::OpCallContext::ExecRegionWin, &props, nullptr);
  }
  WM_operator_properties_free(&props);
}

void ios_view_axis(bContext *C, const char *axis_type)
{
  wmOperatorType *ot = WM_operatortype_find("VIEW3D_OT_view_axis", false);
  if (ot == nullptr) {
    return;
  }

  PointerRNA props = WM_operator_properties_create_ptr(ot);
  RNA_enum_set_identifier(C, &props, "type", axis_type);
  RNA_boolean_set(&props, "align_active", false);
  RNA_boolean_set(&props, "relative", false);
  ios_operator_call(C, "VIEW3D_OT_view_axis", &props);
  WM_operator_properties_free(&props);
}

void ios_view_orbit(bContext *C, const char *direction)
{
  wmOperatorType *ot = WM_operatortype_find("VIEW3D_OT_view_orbit", false);
  if (ot == nullptr) {
    return;
  }

  PointerRNA props = WM_operator_properties_create_ptr(ot);
  RNA_enum_set_identifier(C, &props, "type", direction);
  ios_operator_call(C, "VIEW3D_OT_view_orbit", &props);
  WM_operator_properties_free(&props);
}

void ios_view_roll_angle(bContext *C, const float angle)
{
  wmOperatorType *ot = WM_operatortype_find("VIEW3D_OT_view_roll", false);
  if (ot == nullptr) {
    return;
  }

  PointerRNA props = WM_operator_properties_create_ptr(ot);
  RNA_enum_set_identifier(C, &props, "type", "ANGLE");
  RNA_float_set(&props, "angle", angle);
  if (WM_operator_name_poll(C, "VIEW3D_OT_view_roll")) {
    WM_operator_name_call_ptr(C, ot, blender::wm::OpCallContext::ExecRegionWin, &props, nullptr);
  }
  WM_operator_properties_free(&props);
}

void ios_viewport_shading_set(bContext *C, const int shading_type)
{
  Main *bmain = CTX_data_main(C);
  ScrArea *area = CTX_wm_area(C);
  View3D *v3d = area ? static_cast<View3D *>(area->spacedata.first) : nullptr;
  if (bmain == nullptr || area == nullptr || v3d == nullptr || area->spacetype != SPACE_VIEW3D) {
    return;
  }

  if (v3d->shading.type == shading_type) {
    return;
  }

  if (shading_type == OB_WIRE) {
    v3d->shading.prev_type_wire = v3d->shading.type;
  }
  else {
    v3d->shading.prev_type = v3d->shading.type;
  }
  v3d->shading.type = shading_type;
  ED_view3d_shade_update(bmain, v3d, area);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D | NS_VIEW3D_SHADING, v3d);
}

int ios_orientation_value_for_id(const std::string &value)
{
  if (value == "local") {
    return V3D_ORIENT_LOCAL;
  }
  if (value == "normal") {
    return V3D_ORIENT_NORMAL;
  }
  if (value == "gimbal") {
    return V3D_ORIENT_GIMBAL;
  }
  if (value == "view") {
    return V3D_ORIENT_VIEW;
  }
  if (value == "cursor") {
    return V3D_ORIENT_CURSOR;
  }
  if (value == "parent") {
    return V3D_ORIENT_PARENT;
  }
  return V3D_ORIENT_GLOBAL;
}

int ios_pivot_value_for_id(const std::string &value)
{
  if (value == "bounds") {
    return V3D_AROUND_CENTER_BOUNDS;
  }
  if (value == "cursor") {
    return V3D_AROUND_CURSOR;
  }
  if (value == "individual") {
    return V3D_AROUND_LOCAL_ORIGINS;
  }
  if (value == "active") {
    return V3D_AROUND_ACTIVE;
  }
  return V3D_AROUND_CENTER_MEDIAN;
}

eSnapMode ios_snap_mode_for_id(const std::string &value)
{
  if (value == "grid") {
    return SCE_SNAP_TO_GRID;
  }
  if (value == "vertex") {
    return eSnapMode(SCE_SNAP_TO_POINT | SCE_SNAP_TO_EDGE_ENDPOINT);
  }
  if (value == "edge") {
    return SCE_SNAP_TO_EDGE;
  }
  if (value == "face") {
    return SCE_SNAP_TO_FACE;
  }
  if (value == "volume") {
    return SCE_SNAP_TO_VOLUME;
  }
  return SCE_SNAP_TO_INCREMENT;
}

const char *ios_orientation_id(const int orientation)
{
  switch (orientation) {
    case V3D_ORIENT_GLOBAL: return "global";
    case V3D_ORIENT_LOCAL: return "local";
    case V3D_ORIENT_NORMAL: return "normal";
    case V3D_ORIENT_GIMBAL: return "gimbal";
    case V3D_ORIENT_VIEW: return "view";
    case V3D_ORIENT_CURSOR: return "cursor";
    case V3D_ORIENT_PARENT: return "parent";
    default: return "custom";
  }
}

const char *ios_pivot_id(const int pivot)
{
  switch (pivot) {
    case V3D_AROUND_CENTER_BOUNDS: return "bounds";
    case V3D_AROUND_CURSOR: return "cursor";
    case V3D_AROUND_LOCAL_ORIGINS: return "individual";
    case V3D_AROUND_ACTIVE: return "active";
    default: return "median";
  }
}

const char *ios_snap_id(const ToolSettings *settings)
{
  if (!(settings->snap_flag & SCE_SNAP)) {
    return "off";
  }
  for (const char *id : {"increment", "grid", "vertex", "edge", "face", "volume"}) {
    if (settings->snap_mode == ios_snap_mode_for_id(id)) {
      return id;
    }
  }
  return "mixed";
}

const char *ios_proportional_id(const ToolSettings *settings, const eContextObjectMode mode)
{
  const bool enabled = mode == CTX_MODE_OBJECT ? settings->proportional_objects :
                                               (settings->proportional_edit & PROP_EDIT_USE) != 0;
  if (!enabled) {
    return "off";
  }
  if (settings->proportional_edit & PROP_EDIT_CONNECTED) {
    return "connected";
  }
  if (settings->proportional_edit & PROP_EDIT_PROJECTED) {
    return "projected";
  }
  return "on";
}

void ios_proportional_set(ToolSettings *settings,
                          const eContextObjectMode mode,
                          const std::string &value)
{
  if (mode == CTX_MODE_OBJECT) {
    settings->proportional_objects = value != "off";
  }
  else {
    SET_FLAG_FROM_TEST(settings->proportional_edit, value != "off", PROP_EDIT_USE);
  }
  SET_FLAG_FROM_TEST(settings->proportional_edit, value == "connected", PROP_EDIT_CONNECTED);
  SET_FLAG_FROM_TEST(settings->proportional_edit, value == "projected", PROP_EDIT_PROJECTED);
}

std::string ios_transform_settings_json(const Scene *scene, const eContextObjectMode mode)
{
  if (!scene || !scene->toolsettings) {
    return "{}";
  }
  const ToolSettings *settings = scene->toolsettings;
  const int orientation = BKE_scene_orientation_slot_get_index(
      &scene->orientation_slots[SCE_ORIENT_DEFAULT]);
  std::string json = "{\"orientation\":\"";
  json += ios_orientation_id(orientation);
  json += "\",\"pivot\":\"";
  json += ios_pivot_id(settings->transform_pivot_point);
  json += "\",\"snap\":\"";
  json += ios_snap_id(settings);
  json += "\",\"proportional\":\"";
  json += ios_proportional_id(settings, mode);
  json += "\"}";
  return json;
}

std::string ios_mesh_select_mode_string(const ToolSettings *tool_settings)
{
  if (tool_settings == nullptr) {
    return "VERT";
  }
  if (tool_settings->selectmode & SCE_SELECT_FACE) {
    return "FACE";
  }
  if (tool_settings->selectmode & SCE_SELECT_EDGE) {
    return "EDGE";
  }
  return "VERT";
}

void ios_mesh_select_mode_set(bContext *C, const std::string &value)
{
  wmOperatorType *ot = WM_operatortype_find("MESH_OT_select_mode", false);
  if (ot == nullptr) {
    return;
  }

  PointerRNA props = WM_operator_properties_create_ptr(ot);
  RNA_enum_set_identifier(C, &props, "type", value.c_str());
  RNA_enum_set_identifier(C, &props, "action", "ENABLE");
  RNA_boolean_set(&props, "use_extend", false);
  RNA_boolean_set(&props, "use_expand", false);
  if (WM_operator_name_poll(C, "MESH_OT_select_mode")) {
    WM_operator_name_call_ptr(C, ot, blender::wm::OpCallContext::ExecRegionWin, &props, nullptr);
  }
  WM_operator_properties_free(&props);
}

void ios_setting_execute(bContext *C, const IOSSettingCommand &command)
{
  if (command.setting == "native_shading_state") {
    ios_shading_state = command.value;
    ios_panels_next_snapshot = {};
    return;
  }
  if (command.setting == "native_shading_visible") {
    ios_panels_visible = (ios_panels_visible & ~4) | (command.value == "1" ? 4 : 0);
    ios_panels_next_snapshot = {};
    return;
  }
  if (command.setting == "native_properties_state") {
    ios_properties_state = command.value;
    ios_panels_next_snapshot = {};
    return;
  }
  if (command.setting == "native_properties_edit" || command.setting == "native_panel_edit") {
    std::istringstream stream(command.value);
    auto value = io::serialize::JsonFormatter().deserialize(stream);
    if (const auto *request = value ? value->as_dictionary_value() : nullptr) {
      ios_panels_sequence = request->lookup_int("sequence").value_or(ios_panels_sequence);
    }
    ios_properties_error.clear();
  }
  if (command.setting == "native_properties_edit") {
    ios_panels_next_snapshot = {};
    if (wmWindow *window = CTX_wm_window(C)) {
      for (wmEventHandler &handler : window->runtime->modalhandlers) {
        if (handler.type == WM_HANDLER_TYPE_OP) {
          ios_properties_error = "Finish the current operation before editing properties.";
          return;
        }
      }
    }
#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS
    if (WM_apple_ios_dialog_active(CTX_wm_window(C))) {
      ios_properties_error = "Finish the current dialog before editing properties.";
      return;
    }
#endif
    ios_properties_error = ui::native_properties_query(C, command.value, true);
    return;
  }
  if (command.setting == "native_panels") {
    ios_panels_next_snapshot = {};
    ios_panels_visible = (ios_panels_visible & 4) | (command.value == "3" ? 3 : command.value == "2" ? 2 :
                         command.value == "1" ? 1 : 0);
    return;
  }
#ifdef WITH_PYTHON
  if (command.setting == "native_panel_edit") {
    ios_panels_next_snapshot = {};
    const char *imports[] = {"bpy", "bl_ui", nullptr};
    bool blocked = false;
#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS
    blocked = WM_apple_ios_dialog_active(CTX_wm_window(C));
#endif
    const std::string expression = "bl_ui.native_panels.apply_json(bpy.context, \"" +
                                   ios_json_escape(command.value) + "\", " +
                                   (blocked ? "True" : "False") + ")";
    BPY_run_string_eval(C, imports, expression.c_str());
    return;
  }
#endif
  if (command.setting == "constraint_axis") {
    const int axis = command.value == "X" ? 0 : command.value == "Y" ? 1 :
                    command.value == "Z" ? 2 : -1;
    blender::ed::transform::modal_constraint_toggle(
        C, ios_constraint_operator(CTX_wm_window(C)), axis);
    return;
  }
  Scene *scene = CTX_data_scene(C);
  ToolSettings *tool_settings = scene ? scene->toolsettings : nullptr;
  if (scene == nullptr || tool_settings == nullptr) {
    return;
  }

  if (command.setting == "orientation") {
    BKE_scene_orientation_slot_set_index(&scene->orientation_slots[SCE_ORIENT_DEFAULT],
                                         ios_orientation_value_for_id(command.value));
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
    return;
  }

  if (command.setting == "pivot") {
    tool_settings->transform_pivot_point = ios_pivot_value_for_id(command.value);
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
    return;
  }

  if (command.setting == "snap") {
    if (command.value == "off") {
      tool_settings->snap_flag &= ~SCE_SNAP;
    }
    else {
      tool_settings->snap_flag |= SCE_SNAP;
      tool_settings->snap_mode = ios_snap_mode_for_id(command.value);
    }
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
    return;
  }

  if (command.setting == "proportional") {
    ios_proportional_set(tool_settings, CTX_data_mode_enum(C), command.value);
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
    return;
  }

  if (command.setting == "mesh_select_mode") {
    ios_mesh_select_mode_set(C, command.value);
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
  }
}

void ios_command_execute(bContext *C, IOSUICommand command)
{
  const IOSContextStore context_store = ios_context_push_view3d(C);
  if (CTX_wm_window(C) == nullptr || CTX_wm_area(C) == nullptr || CTX_wm_region(C) == nullptr) {
    ios_context_pop(C, context_store);
    return;
  }

  switch (command) {
    case IOSUICommand::SetModeObject:
      ios_mode_set(C, "OBJECT");
      break;
    case IOSUICommand::SetModeEdit:
      ios_mode_set(C, "EDIT");
      break;
    case IOSUICommand::SetModeSculpt:
      ios_mode_set(C, "SCULPT");
      break;
    case IOSUICommand::SetModeVertexPaint:
      ios_mode_set(C, "VERTEX_PAINT");
      break;
    case IOSUICommand::SetModeWeightPaint:
      ios_mode_set(C, "WEIGHT_PAINT");
      break;
    case IOSUICommand::SetModeTexturePaint:
      ios_mode_set(C, "TEXTURE_PAINT");
      break;
    case IOSUICommand::SelectToolSelectBox:
      WM_toolsystem_ref_set_by_id(C, "builtin.select_box");
      break;
    case IOSUICommand::SelectToolCursor:
      WM_toolsystem_ref_set_by_id(C, "builtin.cursor");
      break;
    case IOSUICommand::SelectToolMove:
      WM_toolsystem_ref_set_by_id(C, "builtin.move");
      break;
    case IOSUICommand::SelectToolRotate:
      WM_toolsystem_ref_set_by_id(C, "builtin.rotate");
      break;
    case IOSUICommand::SelectToolScale:
      WM_toolsystem_ref_set_by_id(C, "builtin.scale");
      break;
    case IOSUICommand::SelectToolTransform:
      WM_toolsystem_ref_set_by_id(C, "builtin.transform");
      break;
    case IOSUICommand::SelectToolAnnotate:
      WM_toolsystem_ref_set_by_id(C, "builtin.annotate");
      break;
    case IOSUICommand::SelectToolMeasure:
      WM_toolsystem_ref_set_by_id(C, "builtin.measure");
      break;
    case IOSUICommand::FrameSelected:
      ios_operator_call(C, "VIEW3D_OT_view_selected");
      break;
    case IOSUICommand::ResetView:
      ios_view_axis(C, "FRONT");
      break;
    case IOSUICommand::OrbitLeft:
      ios_view_orbit(C, "ORBITLEFT");
      break;
    case IOSUICommand::OrbitRight:
      ios_view_orbit(C, "ORBITRIGHT");
      break;
    case IOSUICommand::OrbitUp:
      ios_view_orbit(C, "ORBITUP");
      break;
    case IOSUICommand::OrbitDown:
      ios_view_orbit(C, "ORBITDOWN");
      break;
    case IOSUICommand::SetViewportWireframe:
      ios_viewport_shading_set(C, OB_WIRE);
      break;
    case IOSUICommand::SetViewportSolid:
      ios_viewport_shading_set(C, OB_SOLID);
      break;
    case IOSUICommand::SetViewportMaterial:
      ios_viewport_shading_set(C, OB_MATERIAL);
      break;
    case IOSUICommand::SetViewportRendered:
      ios_viewport_shading_set(C, OB_RENDER);
      break;
    case IOSUICommand::ViewAll:
      ios_operator_call(C, "VIEW3D_OT_view_all");
      break;
    case IOSUICommand::ViewLocal:
      ios_operator_call(C, "VIEW3D_OT_localview");
      break;
    case IOSUICommand::ViewCamera:
      ios_operator_call(C, "VIEW3D_OT_view_camera");
      break;
    case IOSUICommand::ViewPerspOrtho:
      ios_operator_call(C, "VIEW3D_OT_view_persportho");
      break;
    case IOSUICommand::ViewFront:
      ios_view_axis(C, "FRONT");
      break;
    case IOSUICommand::ViewBack:
      ios_view_axis(C, "BACK");
      break;
    case IOSUICommand::ViewRight:
      ios_view_axis(C, "RIGHT");
      break;
    case IOSUICommand::ViewLeft:
      ios_view_axis(C, "LEFT");
      break;
    case IOSUICommand::ViewTop:
      ios_view_axis(C, "TOP");
      break;
    case IOSUICommand::ViewBottom:
      ios_view_axis(C, "BOTTOM");
      break;
  }

  ios_context_pop(C, context_store);
}

void ios_tool_set_by_id(bContext *C, const std::string &tool_id)
{
  if (tool_id.empty()) {
    return;
  }

  const IOSContextStore context_store = ios_context_push_view3d(C);
  if (CTX_wm_window(C) != nullptr && CTX_wm_area(C) != nullptr && CTX_wm_region(C) != nullptr) {
    WM_toolsystem_ref_set_by_id(C, tool_id.c_str());
  }
  ios_context_pop(C, context_store);
}

std::string ios_toolbar_build_json_for_context(bContext *C)
{
  const IOSContextStore context_store = ios_context_push_view3d(C);
  const bool has_view3d_context = CTX_wm_window(C) != nullptr && CTX_wm_area(C) != nullptr &&
                                  CTX_wm_region(C) != nullptr;
  Scene *scene = has_view3d_context ? CTX_data_scene(C) : nullptr;
  ToolSettings *tool_settings = scene ? scene->toolsettings : nullptr;
  const eContextObjectMode mode = has_view3d_context ? CTX_data_mode_enum(C) : CTX_MODE_OBJECT;
  const char *mode_string = has_view3d_context ? CTX_data_mode_string(C) : "objectmode";
  const View3D *view3d = has_view3d_context ? CTX_wm_view3d(C) : nullptr;
  const char *shading = "solid";
  if (view3d) {
    switch (view3d->shading.type) {
      case OB_WIRE:
        shading = "wireframe";
        break;
      case OB_MATERIAL:
        shading = "material";
        break;
      case OB_RENDER:
        shading = "rendered";
        break;
      default:
        break;
    }
  }
  const std::string mesh_select_mode = mode == CTX_MODE_EDIT_MESH ?
                                           ios_mesh_select_mode_string(tool_settings) :
                                           "";
  bToolRef *active_tool = has_view3d_context ? WM_toolsystem_ref_from_context(C) : nullptr;
  const char *active_tool_id = active_tool ? active_tool->idname : "builtin.select_box";
  std::string tools_json = "[]";
  std::string panels_json = "null";
#ifdef WITH_PYTHON
  if (has_view3d_context) {
    const char *imports[] = {"bpy", "bl_ui", nullptr};
    char *result = nullptr;
    if (BPY_run_string_as_string(C,
                                imports,
                                "bl_ui.space_toolsystem_common.native_toolbar_json(bpy.context)",
                                nullptr,
                                &result))
    {
      tools_json = result;
      MEM_delete(result);
    }
    const auto now = std::chrono::steady_clock::now();
    if (ios_panels_visible && now >= ios_panels_next_snapshot) {
      /* Scene projection follows the native refresh cadence, not the viewport frame rate. */
      ios_panels_next_snapshot = now + std::chrono::milliseconds(250);
      const std::string expression = "bl_ui.native_panels.snapshot_json(bpy.context, " +
                                     std::to_string(ios_panels_visible & 2) + ")";
      result = nullptr;
      if (BPY_run_string_as_string(C, imports, expression.c_str(), nullptr, &result)) {
        ios_panels_json = result;
        MEM_delete(result);
      }
      using namespace io::serialize;
      std::istringstream base_stream(ios_panels_json);
      auto base = JsonFormatter().deserialize(base_stream);
      DictionaryValue merged;
      if (const auto *dictionary = base ? base->as_dictionary_value() : nullptr) {
        for (const auto &item : dictionary->elements()) {
          if (item.first != "ack" && (item.first != "error" || ios_properties_error.empty())) {
            merged.append(item.first, item.second);
          }
        }
      }
      merged.append_int("ack", ios_panels_sequence);
      if (!ios_properties_error.empty()) {
        merged.append_str("error", ios_properties_error);
      }
      if (ios_panels_visible & 1) {
        std::istringstream properties_stream(ui::native_properties_query(C, ios_properties_state, false));
        auto properties = JsonFormatter().deserialize(properties_stream);
        if (properties) {
          merged.append("inspector", std::move(properties));
        }
      }
      if (ios_panels_visible & 4) {
        std::istringstream shading_stream(ui::native_properties_query(C, ios_shading_state, false));
        auto shading = JsonFormatter().deserialize(shading_stream);
        if (shading) { merged.append("shading", std::move(shading)); }
      }
      std::ostringstream merged_stream;
      JsonFormatter().serialize(merged_stream, merged);
      ios_panels_json = merged_stream.str();
      ios_panels_snapshot.publish(ios_panels_json);
    }
  }
#endif
  const std::vector<IOSActionItem> actions = has_view3d_context ?
                                                ios_action_items_for_tool(C, mode, active_tool_id) :
                                                std::vector<IOSActionItem>();
  const bool can_undo = has_view3d_context && ios_undo_available(C);
  const std::string settings_json = ios_transform_settings_json(scene, mode);
  std::string navigation_json = "null";
  if (has_view3d_context) {
    const wmWindow *win = CTX_wm_window(C);
    ARegion *region = CTX_wm_region(C);
    const rcti *visible = ED_region_visible_rect(region);
    const float width = WM_window_native_pixel_x(win);
    const float height = WM_window_native_pixel_y(win);
    const float radius = U.gizmo_size_navigate_v3d * 0.5f * U.scale_factor;
    const float inset = radius + 10.0f * U.scale_factor;
    if (width > 0 && height > 0) {
      /* Match view3d_gizmo_navigate.cc, in normalized UIKit window coordinates.
       * The native shell can follow the last frame during asynchronous resizing. */
      navigation_json = "[" +
                        std::to_string((region->winrct.xmin + visible->xmax - inset) / width) +
                        "," +
                        std::to_string(1.0f - (region->winrct.ymin + visible->ymin + inset) / height) +
                        "," + std::to_string(radius / width) + "]";
    }
  }
  ios_context_pop(C, context_store);

  std::string json = "{\"mode\":\"";
  json += ios_json_escape(mode_string ? mode_string : "objectmode");
  json += "\",\"shading\":\"";
  json += shading;
  json += "\",\"meshSelectMode\":\"";
  json += ios_json_escape(mesh_select_mode);
  json += "\",\"active\":\"";
  json += ios_json_escape(active_tool_id ? active_tool_id : "");
  json += "\",\"canUndo\":";
  json += can_undo ? "true" : "false";
  json += ",\"settings\":";
  json += settings_json;
  json += ",\"navigationGizmo\":";
  json += navigation_json;
  json += ",\"tools\":";
  json += tools_json;
  json += ",\"panels\":";
  json += panels_json;
  json += ",\"actions\":[";

  bool needs_comma = false;
  for (const IOSActionItem &action : actions) {
    if (needs_comma) {
      json += ",";
    }
    needs_comma = true;
    json += "{\"title\":\"";
    json += ios_json_escape(action.title);
    json += "\",\"operator\":\"";
    json += ios_json_escape(action.op_name);
    json += "\",\"symbol\":\"";
    json += ios_json_escape(action.symbol);
    json += "\",\"property\":\"";
    json += ios_json_escape(action.prop_name ? action.prop_name : "");
    json += "\",\"value\":\"";
    json += ios_json_escape(action.prop_value ? action.prop_value : "");
    json += "\",\"key\":\"";
    json += ios_json_escape(action.key ? action.key : "");
    json += "\",\"selected\":";
    json += action.selected ? "true" : "false";
    json += "}";
  }
  json += "]}";
  return json;
}

void ios_toolbar_build(bContext *C)
{
  std::string json = ios_toolbar_build_json_for_context(C);
  ios_toolbar_snapshot.publish(std::move(json));
}

std::string ios_json_escape(const std::string &value)
{
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

}  // namespace

extern "C" void WM_apple_ios_ui_command_enqueue(int command)
{
  if (command < int(IOSUICommand::SetModeObject) ||
      command > int(IOSUICommand::ViewBottom))
  {
    return;
  }

  ios_ui_messages.push(IOSUICommand(command));
}

extern "C" void WM_apple_ios_ui_pencil_hover_set(bool active, int x, int y)
{
  ios_hover_requests.publish({active, x, y});
}

extern "C" char *WM_apple_ios_ui_pencil_hover_copy_json()
{
  static std::string snapshot = "{}";
  ios_hover_snapshot.take(snapshot);
  return strdup(snapshot.c_str());
}

extern "C" void WM_apple_ios_ui_operator_enqueue(const char *op_name)
{
  if (op_name == nullptr || op_name[0] == '\0') {
    return;
  }

  ios_ui_messages.push(IOSOperatorCommand{op_name});
}

extern "C" void WM_apple_ios_ui_operator_at_position_enqueue(const char *op_name, int x, int y)
{
  if (op_name == nullptr || op_name[0] == '\0') {
    return;
  }
  ios_ui_messages.push(IOSOperatorCommand{op_name, true, x, y});
}

extern "C" void WM_apple_ios_ui_operator_enum_enqueue(const char *op_name,
                                                       const char *prop_name,
                                                       const char *prop_value)
{
  if (op_name == nullptr || op_name[0] == '\0') {
    return;
  }

  ios_ui_messages.push(IOSOperatorPropertyCommand{
      op_name, prop_name ? prop_name : "", prop_value ? prop_value : ""});
}

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value)
{
  if (setting == nullptr || setting[0] == '\0' || value == nullptr || value[0] == '\0') {
    return;
  }

  ios_ui_messages.push(IOSSettingCommand{setting, value});
}

extern "C" void WM_apple_ios_ui_tool_set_by_id(const char *tool_id)
{
  if (tool_id == nullptr || tool_id[0] == '\0') {
    return;
  }

  ios_ui_messages.push(std::string(tool_id));
}

extern "C" void WM_apple_ios_ui_view_roll_enqueue(float angle)
{
  if (angle == 0.0f) {
    return;
  }

  ios_ui_messages.push(angle);
}

extern "C" char *WM_apple_ios_ui_toolbar_copy_json()
{
  ios_toolbar_snapshot.take(ios_toolbar_json);

  char *json_copy = static_cast<char *>(std::malloc(ios_toolbar_json.size() + 1));
  if (json_copy == nullptr) {
    return nullptr;
  }
  std::memcpy(json_copy, ios_toolbar_json.c_str(), ios_toolbar_json.size() + 1);
  return json_copy;
}

extern "C" char *WM_apple_ios_ui_panels_take_json()
{
  std::string json;
  if (!ios_panels_snapshot.take(json)) {
    return nullptr;
  }
  char *copy = static_cast<char *>(std::malloc(json.size() + 1));
  if (copy) {
    std::memcpy(copy, json.c_str(), json.size() + 1);
  }
  return copy;
}

extern "C" void WM_apple_ios_ui_toolbar_free_json(char *json)
{
  std::free(json);
}

void ios_ui_apply_viewport_preferences(bContext *C)
{
  if (C == nullptr) {
    return;
  }

  bool refresh_gizmo_map = ios_ui_gizmo_refresh_pending;

  /* Keep taps (including Pencil jitter) on Blender's click-selection path, which
   * also sets the active object. Real tool drags start after a small movement,
   * with no hold delay; pressure strokes still receive every motion event. */
  U.drag_threshold_mouse = 3;
  U.drag_threshold_tablet = 3;
  U.drag_threshold = 0;
  U.move_threshold = 0;
  U.flag &= ~USER_TOOLTIPS;

  if ((U.uiflag & USER_SHOW_GIZMO_NAVIGATE) == 0) {
    U.uiflag |= USER_SHOW_GIZMO_NAVIGATE;
    refresh_gizmo_map = true;
  }
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    U.gizmo_flag |= USER_GIZMO_DRAW;
    refresh_gizmo_map = true;
  }
  if (U.mini_axis_type != USER_MINI_AXIS_TYPE_GIZMO) {
    U.mini_axis_type = USER_MINI_AXIS_TYPE_GIZMO;
    refresh_gizmo_map = true;
  }
  if (U.gizmo_size_navigate_v3d == 0) {
    U.gizmo_size_navigate_v3d = 80;
    refresh_gizmo_map = true;
  }

  const IOSContextStore context_store = ios_context_push_view3d(C);
  ScrArea *area = CTX_wm_area(C);

  if (area != nullptr && area->spacetype == SPACE_VIEW3D) {
    View3D *v3d = static_cast<View3D *>(area->spacedata.first);
    if (v3d != nullptr) {
      if (v3d->gizmo_flag & (V3D_GIZMO_HIDE | V3D_GIZMO_HIDE_NAVIGATE)) {
        refresh_gizmo_map = true;
      }
      v3d->gizmo_flag &= ~(V3D_GIZMO_HIDE | V3D_GIZMO_HIDE_NAVIGATE);
      v3d->overlay.flag |= V3D_OVERLAY_HIDE_CURSOR;
    }
    ARegion *region = ios_find_window_region(area);
    if (refresh_gizmo_map && region != nullptr && region->runtime->gizmo_map != nullptr) {
      WM_gizmomap_reinit(region->runtime->gizmo_map);
      WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
      ED_region_tag_redraw(region);
    }
  }

  ios_ui_gizmo_refresh_pending = false;
  ios_context_pop(C, context_store);
}

extern "C" void WM_apple_ios_ui_ensure_layout(bContext *C)
{
  if (C == nullptr) {
    return;
  }

  if (!ios_ui_layout_ready) {
    const IOSContextStore context_store = ios_context_push_view3d(C);
    wmWindow *win = CTX_wm_window(C);
    bScreen *screen = CTX_wm_screen(C);
    ScrArea *area = CTX_wm_area(C);

    if (win != nullptr && screen != nullptr && area != nullptr && screen->state == SCREENNORMAL) {
      ED_screen_state_toggle(C, win, area, SCREENFULL);
    }

    ios_ui_layout_ready = true;
    ios_ui_gizmo_refresh_pending = true;
    ios_context_pop(C, context_store);
  }

  ios_ui_apply_viewport_preferences(C);
}

extern "C" uint64_t WM_apple_ios_ui_menu_request(const char *route, bool execute)
{
  const uint64_t identifier = ++ios_menu_next_identifier;
  ios_ui_messages.push(IOSMenuRequest{identifier, route ? route : "{}", execute});
  return identifier;
}

extern "C" uint64_t WM_apple_ios_ui_menu_at_position_request(const char *route,
                                                            bool execute,
                                                            int x,
                                                            int y)
{
  const uint64_t identifier = ++ios_menu_next_identifier;
  ios_ui_messages.push(IOSMenuRequest{identifier, route ? route : "{}", execute, true, x, y});
  return identifier;
}

/* UIKit only exchanges owned JSON with the engine, never live Blender UI pointers. */
extern "C" char *WM_apple_ios_ui_menu_take_result(uint64_t identifier)
{
  for (auto &result : ios_menu_completed.take()) {
    ios_menu_results.emplace(result.first, std::move(result.second));
  }
  while (ios_menu_results.size() > 128) {
    ios_menu_results.erase(ios_menu_results.begin());
  }
  const auto found = ios_menu_results.find(identifier);
  if (found == ios_menu_results.end()) {
    return nullptr;
  }
  char *result = strdup(found->second.c_str());
  ios_menu_results.erase(found);
  return result;
}

extern "C" void WM_apple_ios_ui_navigation_hover_set(bool active)
{
  if (ios_navigation_hover_active.exchange(active) != active) {
    ios_navigation_redraw_pending.store(true);
  }
}

extern "C" bool WM_apple_ios_ui_navigation_hover_active()
{
  return ios_navigation_hover_active.load();
}

extern "C" void WM_apple_ios_ui_navigation_redraw_request()
{
  ios_navigation_redraw_pending.store(true);
}

static void ios_ui_dispatch_operator(bContext *C, IOSOperatorCommand &command)
{
  const IOSContextStore context_store = ios_context_push_view3d(C);
  wmWindow *win = CTX_wm_window(C);
  bool ready = win && CTX_wm_area(C) && CTX_wm_region(C);
  if (ready && command.has_position) {
    for (wmEventHandler &handler_item : win->runtime->modalhandlers) {
      wmEventHandler *handler = &handler_item;
      if (handler->type == WM_HANDLER_TYPE_OP) {
        ready = false;
        break;
      }
    }
    if (!ready && command.retries++ < 2) {
      /* Allow the queued Pencil cancellation/release to reach the modal handler. */
      ios_ui_messages.push(command);
    }
    if (ready && win->runtime->eventstate) {
      int x = command.x, y = command.y;
      wm_cursor_position_from_ghost_client_coords(win, &x, &y);
      win->runtime->eventstate->xy[0] = x;
      win->runtime->eventstate->xy[1] = y;
    }
  }
  if (ready) {
    ios_operator_invoke(C, command.op_name.c_str());
  }
  ios_context_pop(C, context_store);
}

static void ios_ui_dispatch_menu(bContext *C, IOSMenuRequest &request)
{
  const IOSContextStore context_store = ios_context_push_view3d(C);
  std::string result = "[]";
  wmWindow *win = CTX_wm_window(C);
  bool ready = true;
  if (win && request.has_position) {
    for (wmEventHandler &handler_item : win->runtime->modalhandlers) {
      wmEventHandler *handler = &handler_item;
      if (handler->type == WM_HANDLER_TYPE_OP) {
        ready = false;
        break;
      }
    }
    if (!ready && request.retries++ < 2) {
      ios_ui_messages.push(request);
      ios_context_pop(C, context_store);
      return;
    }
    if (ready && win->runtime->eventstate) {
      int x = request.x, y = request.y;
      wm_cursor_position_from_ghost_client_coords(win, &x, &y);
      win->runtime->eventstate->xy[0] = x;
      win->runtime->eventstate->xy[1] = y;
    }
  }
  if (ready && win && CTX_wm_area(C) && CTX_wm_region(C)) {
    result = blender::ui::native_properties_menu_query(C, request.route, request.execute);
  }
  ios_context_pop(C, context_store);
  if (!request.execute) {
    ios_menu_completed.push({request.identifier, std::move(result)});
  }
}

static void ios_ui_dispatch(bContext *C, IOSUIMessage &message)
{
#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS
  const auto *setting = std::get_if<IOSSettingCommand>(&message);
  const bool panel_message = setting && (setting->setting == "native_panels" ||
                                         setting->setting == "native_shading_state" ||
                                         setting->setting == "native_shading_visible" ||
                                         setting->setting == "native_panel_edit" ||
                                         setting->setting == "native_properties_state" ||
                                         setting->setting == "native_properties_edit");
  if (WM_apple_ios_dialog_active(CTX_wm_window(C)) && !panel_message) {
    if (const auto *request = std::get_if<IOSMenuRequest>(&message)) {
      if (!request->execute) {
        ios_menu_completed.push({request->identifier, "[]"});
      }
    }
    return;
  }
#endif
  std::visit(
      [&](auto &command) {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, IOSUICommand>) {
          ios_command_execute(C, command);
        }
        else if constexpr (std::is_same_v<T, std::string>) {
          ios_tool_set_by_id(C, command);
        }
        else if constexpr (std::is_same_v<T, IOSOperatorCommand>) {
          ios_ui_dispatch_operator(C, command);
        }
        else if constexpr (std::is_same_v<T, IOSMenuRequest>) {
          ios_ui_dispatch_menu(C, command);
        }
        else {
          const IOSContextStore context_store = ios_context_push_view3d(C);
          if (CTX_wm_window(C) && CTX_wm_area(C) && CTX_wm_region(C)) {
            if constexpr (std::is_same_v<T, IOSSettingCommand>) {
              ios_setting_execute(C, command);
            }
            else if constexpr (std::is_same_v<T, float>) {
              ios_view_roll_angle(C, command);
            }
            else if constexpr (std::is_same_v<T, IOSOperatorPropertyCommand>) {
              ios_operator_call_with_enum_property(C,
                                                   command.op_name.c_str(),
                                                   command.prop_name.c_str(),
                                                   command.prop_value.c_str());
            }
          }
          ios_context_pop(C, context_store);
        }
      },
      message);
}

extern "C" void WM_apple_ios_ui_flush_commands(bContext *C)
{
  if (C == nullptr) {
    return;
  }

#if defined(WITH_APPLE_CROSSPLATFORM) && APPLE_TARGET_IOS
  WM_apple_ios_dialog_flush(C);
  WM_apple_ios_fileselect_flush(C);
#endif
  WM_apple_ios_ui_ensure_layout(C);

  if (ios_navigation_redraw_pending.exchange(false)) {
    const IOSContextStore context_store = ios_context_push_view3d(C);
    if (ARegion *region = CTX_wm_region(C)) {
      ED_region_tag_redraw_editor_overlays(region);
    }
    ios_context_pop(C, context_store);
  }

  for (IOSUIMessage &message : ios_ui_messages.take()) {
    ios_ui_dispatch(C, message);
  }

  ios_hover_requests.take(ios_hover_request);
  {
    const IOSContextStore context_store = ios_context_push_view3d(C);
    ARegion *region = CTX_wm_region(C);
    wmWindow *window = CTX_wm_window(C);
    bool active = ios_hover_request.active && window && region;
    if (active) {
      for (wmEventHandler &handler : window->runtime->modalhandlers) {
        if (handler.type == WM_HANDLER_TYPE_OP || handler.type == WM_HANDLER_TYPE_UI) {
          active = false;
          break;
        }
      }
      if (region->runtime->gizmo_map && wm_gizmomap_highlight_get(region->runtime->gizmo_map)) {
        active = false;
      }
    }
    int mval[2] = {ios_hover_request.x, ios_hover_request.y};
    if (window && region) {
      wm_cursor_position_from_ghost_client_coords(window, &mval[0], &mval[1]);
      mval[0] -= region->winrct.xmin;
      mval[1] -= region->winrct.ymin;
    }
    ED_view3d_pencil_hover_update(C, active, mval);
    const View3DPencilHover hit = ED_view3d_pencil_hover_get(region);
    ios_hover_snapshot.publish("{\"object\":" + std::to_string(hit.object_uid) +
                               ",\"kind\":" + std::to_string(hit.kind) +
                               ",\"index\":" + std::to_string(hit.index) + "}");
    ios_context_pop(C, context_store);
  }

  ios_toolbar_build(C);
}

}  // namespace blender
