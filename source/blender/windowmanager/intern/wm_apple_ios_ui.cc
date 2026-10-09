/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * iPad shell glue for the native UIKit overlay.
 */

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_appdir.hh"
#include "BLT_translation.hh"
#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_paint.hh"
#include "BKE_preferences.h"
#include "BKE_preview_image.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "GHOST_IOSMessageQueue.hh"

#include "DEG_depsgraph.hh"

#include "DNA_asset_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "ED_asset.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_object.hh"
#include "../asset/ED_asset_shelf.hh"
#include "ED_screen.hh"
#include "ED_transform.hh"
#include "ED_view3d.hh"
#include "ED_view3d_pencil_hover.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_resources.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
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
std::string native_operator_redo_query(bContext *C, const std::string &request, bool execute);
void native_properties_interaction_finish(bContext *C);
std::string native_properties_menu_query(bContext *C, const std::string &request, bool execute);
}

namespace blender {
void ios_interface_set(bContext *C, bool desktop);

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

struct IOSBrushCommand {
  std::string key;
};

/** Native asset pane drags; see #ios_asset_drag. */
enum IOSAssetDragPhase {
  IOS_ASSET_DRAG_BEGIN,
  IOS_ASSET_DRAG_MOVE,
  IOS_ASSET_DRAG_DROP,
  IOS_ASSET_DRAG_CANCEL,
  /** Drop at the 3D cursor, or onto the active object for materials and node groups. */
  IOS_ASSET_ADD,
  /** Internal second step of #IOS_ASSET_ADD: release at window coordinates. */
  IOS_ASSET_ADD_RELEASE,
};

struct IOSAssetDragCommand {
  int phase = IOS_ASSET_DRAG_MOVE;
  std::string key;
  int x = 0, y = 0;
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
                                  IOSMenuRequest,
                                  IOSBrushCommand,
                                  IOSAssetDragCommand>;
/* A single inbox preserves ordering across tools, settings, operators and menus. */
GHOST_IOSMessageQueue<IOSUIMessage> ios_ui_messages;
GHOST_IOSMessageQueue<std::pair<uint64_t, std::string>> ios_menu_completed;
/* UIKit-owned result cache. */
std::map<uint64_t, std::string> ios_menu_results;
std::atomic<uint64_t> ios_menu_next_identifier{0};
bool ios_ui_layout_ready = false;
bool ios_ui_gizmo_refresh_pending = true;
/** Blender's own interface instead of the native one: normal screens, no native controls. */
std::atomic<bool> ios_desktop_ui{false};
int ios_panels_visible = 0;
std::chrono::steady_clock::time_point ios_panels_next_snapshot;
std::string ios_panels_json = "null";
std::string ios_properties_state = "{\"tab\":\"OBJECT\"}";
std::string ios_shading_state = "{\"tab\":\"SHADING\"}";
std::string ios_properties_error;
int64_t ios_panels_sequence = 0;
std::atomic<bool> ios_navigation_hover_active{false};
std::atomic<bool> ios_navigation_redraw_pending{false};
/** Height of the native bottom tray (the asset pane) as a fraction of the window height. */
std::atomic<float> ios_bottom_tray{0.0f};
extern "C" int WM_apple_ios_ui_bottom_tray_ymin(const wmWindow *win, const ARegion *region);
struct IOSPencilHoverRequest {
  bool active = false;
  int x = 0, y = 0;
};
GHOST_IOSLatest<IOSPencilHoverRequest> ios_hover_requests;
GHOST_IOSLatest<std::string> ios_hover_snapshot;
/* Window coordinates of the asset drag in progress. */
int2 ios_asset_drag_xy(0);
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
GHOST_IOSLatest<std::string> ios_timeline_snapshot;
std::string ios_timeline_json;
/* Keyframes shown on the timeline, refreshed on the native cadence while it is open. */
bool ios_timeline_visible = false;
std::string ios_timeline_keys_json = "[]";
std::chrono::steady_clock::time_point ios_timeline_next_keys;
/* Native asset pane: listed only while shown. */
bool ios_assets_visible = false;
std::string ios_assets_json;
std::chrono::steady_clock::time_point ios_assets_next_snapshot;
GHOST_IOSLatest<std::string> ios_assets_snapshot;
std::string ios_toolbar_json =
    "{\"mode\":\"objectmode\",\"active\":\"builtin.select_box\",\"tools\":[]}";

std::string ios_json_escape(const std::string &value);


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

  /* Everything else comes from `bl_ui.native_actions`, queried with the toolbar snapshot. */
  UNUSED_VARS(mode, active_tool_id);
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

/** Modes the active object supports, as listed by Blender's own header mode menu.
 * Filter directly: `OBJECT_OT_mode_set` items read through owner-less operator properties
 * are resolved without context, which lists every mode for every object type. */
std::string ios_object_modes_json(bContext *C)
{
  const Object *active = CTX_data_active_object(C);
  std::string json = "[";
  for (const EnumPropertyItem *item = rna_enum_object_mode_items; item->identifier; item++) {
    if (!item->identifier[0] ||
        !(active ? ed::object::mode_compat_test(active, eObjectMode(item->value)) :
                   item->value == OB_MODE_OBJECT))
    {
      continue;
    }
    const char *icon = nullptr;
    RNA_enum_identifier(rna_enum_icon_items, item->icon, &icon);
    json += json.size() > 1 ? "," : "";
    json += "{\"id\":\"" + ios_json_escape(item->identifier) + "\",\"label\":\"" +
            ios_json_escape(item->name ? item->name : item->identifier) + "\",\"icon\":\"" +
            ios_json_escape(icon ? icon : "") + "\"}";
  }
  return json + "]";
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
  bool preview = false;
  if (command.setting == "native_redo_edit") {
    ui::native_operator_redo_query(C, command.value, true);
    return;
  }
  if (command.setting == "interface") {
    ios_interface_set(C, command.value == "desktop");
    return;
  }
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
      preview = request->lookup_str("phase").value_or("") == "update";
    }
    ios_properties_error.clear();
  }
  if (command.setting == "native_properties_edit") {
    /* Local controls already show the drag value. Keep expensive full-panel snapshots
     * on their normal cadence while evaluation and viewport redraw run live. */
    if (!preview) { ios_panels_next_snapshot = {}; }
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
  if (command.setting == "render" || command.setting == "viewport_toggle") {
    /* Rendering and viewport options from the iPad's Share and More menus, see
     * `bl_ui.native_render`. */
    const IOSContextStore context_store = ios_context_push_view3d(C);
    const char *imports[] = {"bpy", "bl_ui", nullptr};
    std::string expression;
    if (command.setting == "viewport_toggle") {
      expression = "bl_ui.native_render.viewport_toggle(bpy.context, \"" +
                   ios_json_escape(command.value) + "\")";
    }
    else if (command.value == "cancel") {
      WM_jobs_kill_type(CTX_wm_manager(C), nullptr, WM_JOB_TYPE_RENDER);
      expression = "bl_ui.native_render.cancel(bpy.context)";
    }
    else if (command.value == "clear") {
      expression = "bl_ui.native_render.clear(bpy.context)";
    }
    else {
      expression = std::string("bl_ui.native_render.start(bpy.context, ") +
                   (command.value == "animation" ? "True" : "False") + ")";
    }
    BPY_run_string_eval(C, imports, expression.c_str());
    ios_context_pop(C, context_store);
    return;
  }
  if (command.setting == "native_action") {
    /* The button may predate a context change; `run` checks the action still applies. */
    const bToolRef *tool = WM_toolsystem_ref_from_context(C);
    const char *imports[] = {"bpy", "bl_ui", nullptr};
    const std::string expression = "bl_ui.native_actions.run(bpy.context, \"" +
                                   ios_json_escape(command.value) + "\", \"" +
                                   ios_json_escape(tool ? tool->idname : "") + "\")";
    BPY_run_string_eval(C, imports, expression.c_str());
    return;
  }
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
  if (command.setting == "object_mode") {
    ios_mode_set(C, command.value.c_str());
    return;
  }
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

  if (command.setting == "assets") {
    /* The pane is listed only while shown; showing it publishes a fresh list. */
    ios_assets_visible = command.value == "show";
    ios_assets_json.clear();
    ios_assets_next_snapshot = {};
    return;
  }

  if (command.setting == "timeline_visible") {
    ios_timeline_visible = command.value == "1";
    ios_timeline_next_keys = {};
    return;
  }

#ifdef WITH_PYTHON
  if (command.setting == "timeline_key_select" || command.setting == "timeline_key_move" ||
      command.setting == "timeline_key_delete")
  {
    /* Select "frames|extend" (frames comma separated), move the selected keys by a delta, or delete
     * the selected keys. Values are parsed here so nothing but numbers reaches Python. */
    std::string expression;
    if (command.setting == "timeline_key_select") {
      const size_t bar = command.value.find('|');
      std::string frames;
      const std::string list = command.value.substr(0, bar);
      for (size_t begin = 0; begin < list.size();) {
        const size_t comma = std::min(list.find(',', begin), list.size());
        if (comma > begin) {
          frames += (frames.empty() ? "" : ",") +
                    std::to_string(std::atoi(list.substr(begin, comma - begin).c_str()));
        }
        begin = comma + 1;
      }
      const bool extend = bar != std::string::npos && command.value.substr(bar + 1) == "1";
      expression = "bpy.ops.anim.native_key_select(frames=\"" + frames + "\", extend=" +
                   (extend ? "True" : "False") + ")";
    }
    else if (command.setting == "timeline_key_move") {
      expression = "bpy.ops.anim.native_key_move(delta=" +
                   std::to_string(std::atoi(command.value.c_str())) + ")";
    }
    else {
      expression = "bpy.ops.anim.native_key_delete()";
    }
    const char *imports[] = {"bpy", nullptr};
    BPY_run_string_eval(C, imports, expression.c_str());
    ios_timeline_next_keys = {};
    return;
  }
#endif

  if (command.setting == "timeline_frame") {
    /* Match SCREEN_OT_frame_offset: scrubbing lands on whole frames. */
    scene->r.cfra = std::min(std::atoi(command.value.c_str()), int(MAXFRAME));
    FRAMENUMBER_MIN_CLAMP(scene->r.cfra);
    scene->r.subframe = 0.0f;
    ED_areas_do_frame_follow(C, false);
    DEG_id_tag_update(&scene->id, ID_RECALC_FRAME_CHANGE);
    WM_event_add_notifier(C, NC_SCENE | ND_FRAME, scene);
    return;
  }

  if (command.setting == "timeline") {
    if (command.value == "autokey") {
      /* The timeline's record button: Blender's Auto Keying toggle. */
      Scene *scene = CTX_data_scene(C);
      if (scene == nullptr) {
        return;
      }
      PointerRNA ptr = RNA_pointer_create_discrete(&scene->id, RNA_ToolSettings, scene->toolsettings);
      PropertyRNA *prop = RNA_struct_find_property(&ptr, "use_keyframe_insert_auto");
      RNA_property_boolean_set(&ptr, prop, !RNA_property_boolean_get(&ptr, prop));
      RNA_property_update(C, &ptr, prop);
      WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
      return;
    }
    if (command.value == "play") {
      ios_operator_call(C, "SCREEN_OT_animation_play");
      return;
    }
    const bool jump = command.value == "start" || command.value == "end";
    const char *op_name = jump ? "SCREEN_OT_frame_jump" : "SCREEN_OT_keyframe_jump";
    wmOperatorType *ot = WM_operatortype_find(op_name, false);
    if (ot == nullptr || (!jump && command.value != "prev_key" && command.value != "next_key")) {
      return;
    }
    PointerRNA props = WM_operator_properties_create_ptr(ot);
    RNA_boolean_set(&props, jump ? "end" : "next", command.value == "end" || command.value == "next_key");
    ios_operator_call(C, op_name, &props);
    WM_operator_properties_free(&props);
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

/* -------------------------------------------------------------------- */
/** \name Native Brush Rail
 *
 * Projects the brush asset shelf of the current paint mode. Filtering, ordering of the asset
 * list, the active brush and activation all go through the shelf type, so the native rail lists
 * the same brushes as Blender's own asset shelf popup (without its per-tool filter).
 * \{ */

std::string ios_brushes_json = "null";
std::chrono::steady_clock::time_point ios_brushes_next_snapshot;
/* Session-lifetime PNG copies of brush previews, keyed like #ios_brush_key(). An empty path means
 * the asset has no usable preview. */
std::map<std::string, std::string> ios_brush_preview_paths;

const AssetShelfType *ios_brush_shelf_type(const eContextObjectMode mode)
{
  /* The brush shelves of `space_view3d.py`, one for each mode that paints with brushes. */
  const char *idname = nullptr;
  switch (mode) {
    case CTX_MODE_SCULPT:
      idname = "VIEW3D_AST_brush_sculpt";
      break;
    case CTX_MODE_SCULPT_CURVES:
      idname = "VIEW3D_AST_brush_sculpt_curves";
      break;
    case CTX_MODE_PAINT_VERTEX:
      idname = "VIEW3D_AST_brush_vertex_paint";
      break;
    case CTX_MODE_PAINT_WEIGHT:
      idname = "VIEW3D_AST_brush_weight_paint";
      break;
    case CTX_MODE_PAINT_TEXTURE:
      idname = "VIEW3D_AST_brush_texture_paint";
      break;
    case CTX_MODE_PAINT_GREASE_PENCIL:
      idname = "VIEW3D_AST_brush_gpencil_paint";
      break;
    case CTX_MODE_SCULPT_GREASE_PENCIL:
      idname = "VIEW3D_AST_brush_gpencil_sculpt";
      break;
    case CTX_MODE_VERTEX_GREASE_PENCIL:
      idname = "VIEW3D_AST_brush_gpencil_vertex";
      break;
    case CTX_MODE_WEIGHT_GREASE_PENCIL:
      idname = "VIEW3D_AST_brush_gpencil_weight";
      break;
    default:
      return nullptr;
  }
  return ed::asset::shelf::type_find_from_idname(idname);
}

std::string ios_brush_key(const AssetWeakReference &reference)
{
  std::string key = std::to_string(reference.asset_library_type);
  key += ":";
  key += reference.asset_library_identifier ? reference.asset_library_identifier : "";
  key += ":";
  key += reference.relative_asset_identifier ? reference.relative_asset_identifier : "";
  return key;
}

/** Write the asset's preview to a PNG UIKit can load. Reading a deferred preview is blocking. */
std::string ios_brush_preview_write(asset_system::AssetRepresentation &asset,
                                    const std::string &key)
{
  asset.ensure_previewable();
  PreviewImage *preview = asset.get_preview();
  if (preview == nullptr) {
    return "";
  }
  BKE_previewimg_ensure(preview, ICON_SIZE_PREVIEW);
  const uint8_t *pixels = reinterpret_cast<const uint8_t *>(preview->rect[ICON_SIZE_PREVIEW]);
  const unsigned int width = preview->w[ICON_SIZE_PREVIEW];
  const unsigned int height = preview->h[ICON_SIZE_PREVIEW];
  if (pixels == nullptr || width == 0 || height == 0) {
    return "";
  }

  char directory[FILE_MAX];
  BLI_path_join(directory, sizeof(directory), BKE_tempdir_session(), "brush_previews");
  if (!BLI_dir_create_recursive(directory)) {
    return "";
  }
  char filepath[FILE_MAX];
  const std::string filename = std::to_string(std::hash<std::string>{}(key)) + ".png";
  BLI_path_join(filepath, sizeof(filepath), directory, filename.c_str());

  ImBuf *ibuf = IMB_allocFromBuffer(pixels, nullptr, width, height, 4);
  if (ibuf == nullptr) {
    return "";
  }
  /* Previews are premultiplied, PNG is not. */
  IMB_unpremultiply_alpha(ibuf);
  ibuf->ftype = IMB_FTYPE_PNG;
  ibuf->planes = 32;
  const bool saved = IMB_save_image(ibuf, filepath, IB_byte_data);
  IMB_freeImBuf(ibuf);
  return saved ? filepath : "";
}

std::string ios_brushes_build_json(bContext *C, const eContextObjectMode mode)
{
  const AssetShelfType *shelf_type = ios_brush_shelf_type(mode);
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (shelf_type == nullptr || paint == nullptr) {
    return "null";
  }

  const AssetLibraryReference library_ref = asset_system::all_library_reference();
  ed::asset::list::storage_fetch(&library_ref, C);
  if (!ed::asset::list::is_loaded(&library_ref)) {
    /* Keep the previous list while the asset libraries are (re)loading. */
    return ios_brushes_json == "null" ? "{\"loading\":true,\"items\":[]}" : ios_brushes_json;
  }

  struct BrushItem {
    std::string key;
    std::string name;
    std::string catalog;
    std::string preview;
  };
  std::vector<BrushItem> items;
  /* Blocking preview reads are spread over several snapshots. */
  int preview_budget = 4;
  ed::asset::list::iterate(library_ref, [&](asset_system::AssetRepresentation &asset) {
    if (!ed::asset::shelf::type_asset_poll(*shelf_type, asset)) {
      return true;
    }
    BrushItem item;
    item.key = ios_brush_key(asset.make_weak_reference());
    item.name = asset.get_name();
    if (const asset_system::AssetCatalog *catalog =
            asset.owner_asset_library().catalog_service().find_catalog(
                asset.get_metadata().catalog_id))
    {
      item.catalog = catalog->path.str();
    }
    auto preview = ios_brush_preview_paths.find(item.key);
    if (preview == ios_brush_preview_paths.end() && preview_budget > 0) {
      preview_budget--;
      preview = ios_brush_preview_paths.emplace(item.key, ios_brush_preview_write(asset, item.key))
                    .first;
    }
    if (preview != ios_brush_preview_paths.end()) {
      item.preview = preview->second;
    }
    items.push_back(std::move(item));
    return true;
  });
  /* Group by catalog like the shelf's catalog tabs; uncatalogued brushes go last. */
  std::stable_sort(items.begin(), items.end(), [](const BrushItem &a, const BrushItem &b) {
    if (a.catalog.empty() != b.catalog.empty()) {
      return b.catalog.empty();
    }
    return a.catalog < b.catalog;
  });

  const std::string active = paint->brush_asset_reference ?
                                 ios_brush_key(*paint->brush_asset_reference) :
                                 "";
  std::string json = "{\"shelf\":\"";
  json += ios_json_escape(shelf_type->idname);
  json += "\",\"active\":\"";
  json += ios_json_escape(active);
  json += "\",\"pending\":";
  json += preview_budget == 0 ? "true" : "false";
  json += ",\"items\":[";
  for (size_t i = 0; i < items.size(); i++) {
    const BrushItem &item = items[i];
    if (i > 0) {
      json += ",";
    }
    const std::string catalog_name = item.catalog.substr(item.catalog.rfind('/') + 1);
    json += "{\"id\":\"" + ios_json_escape(item.key) + "\",\"title\":\"" +
            ios_json_escape(item.name) + "\",\"catalog\":\"" +
            ios_json_escape(catalog_name) + "\",\"preview\":\"" +
            ios_json_escape(item.preview) + "\"}";
  }
  json += "]}";
  return json;
}

void ios_brush_activate(bContext *C, const std::string &key)
{
  const IOSContextStore context_store = ios_context_push_view3d(C);
  const AssetShelfType *shelf_type = CTX_wm_area(C) && CTX_wm_region(C) ?
                                         ios_brush_shelf_type(CTX_data_mode_enum(C)) :
                                         nullptr;
  wmOperatorType *ot = shelf_type ?
                           WM_operatortype_find(shelf_type->activate_operator.c_str(), true) :
                           nullptr;
  if (ot) {
    const AssetLibraryReference library_ref = asset_system::all_library_reference();
    ed::asset::list::iterate(library_ref, [&](asset_system::AssetRepresentation &asset) {
      if (ios_brush_key(asset.make_weak_reference()) != key) {
        return true;
      }
      /* Same call as activating an item of the asset shelf. */
      PointerRNA props = WM_operator_properties_create_ptr(ot);
      ed::asset::operator_asset_reference_props_set(asset, props);
      WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeRegionWin, &props, nullptr);
      WM_operator_properties_free(&props);
      return false;
    });
  }
  ios_context_pop(C, context_store);
  /* Publish the new active brush with the next snapshot. */
  ios_brushes_next_snapshot = {};
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Native Asset Pane
 *
 * Projects the assets of every library for the native asset pane. Dropping an asset into the
 * viewport goes through Blender's own drag and drop: a native drag starts a regular asset drag
 * and feeds it pointer moves and the release, so the viewport's drop boxes preview, snap, import
 * and add the asset exactly as dragging it from Blender's Asset Browser does.
 * \{ */

/**
 * Assets the viewport accepts as drops. Brushes have their own rail. Node groups are listed only
 * when they are Geometry Nodes modifiers, the same check as Blender's Add Modifier menu.
 */
bool ios_asset_listed(const asset_system::AssetRepresentation &asset)
{
  const ID_Type type = asset.get_id_type();
  if (ELEM(type, ID_OB, ID_GR, ID_MA, ID_WO)) {
    return true;
  }
  if (type != ID_NT) {
    return false;
  }
  const AssetMetaData &metadata = asset.get_metadata();
  const IDProperty *tree_type = BKE_asset_metadata_idprop_find(&metadata, "type");
  const IDProperty *traits = BKE_asset_metadata_idprop_find(&metadata,
                                                           "geometry_node_asset_traits_flag");
  return tree_type && IDP_int_get(tree_type) == NTREE_GEOMETRY && traits &&
         (IDP_int_get(traits) & GEO_NODE_ASSET_MODIFIER);
}

/** The pane's type name: listed node groups are modifiers. */
const char *ios_asset_type_name(const ID_Type type)
{
  return type == ID_NT ? "Modifier" : BKE_idtype_idcode_to_name(type);
}

std::string ios_asset_library_name(asset_system::AssetLibrary &library)
{
  switch (library.library_type()) {
    case ASSET_LIBRARY_LOCAL:
      return "Current File";
    case ASSET_LIBRARY_ESSENTIALS:
      return "Essentials";
    default:
      return library.name();
  }
}

std::string ios_assets_build_json(bContext *C)
{
  const AssetLibraryReference library_ref = asset_system::all_library_reference();
  ed::asset::list::storage_fetch(&library_ref, C);
  if (!ed::asset::list::is_loaded(&library_ref)) {
    /* Keep the previous list while the asset libraries are (re)loading. */
    return ios_assets_json.empty() ? "{\"loading\":true,\"items\":[]}" : ios_assets_json;
  }

  std::string items;
  /* Blocking preview reads are spread over several snapshots. */
  int preview_budget = 6;
  ed::asset::list::iterate(library_ref, [&](asset_system::AssetRepresentation &asset) {
    if (!ios_asset_listed(asset)) {
      return true;
    }
    const ID_Type type = asset.get_id_type();
    const std::string key = ios_brush_key(asset.make_weak_reference());
    const AssetMetaData &metadata = asset.get_metadata();
    asset_system::AssetLibrary &library = asset.owner_asset_library();
    std::string catalog;
    if (const asset_system::AssetCatalog *found = library.catalog_service().find_catalog(
            metadata.catalog_id))
    {
      catalog = found->path.str();
    }
    auto preview = ios_brush_preview_paths.find(key);
    if (preview == ios_brush_preview_paths.end() && preview_budget > 0) {
      preview_budget--;
      preview = ios_brush_preview_paths.emplace(key, ios_brush_preview_write(asset, key)).first;
    }
    std::string tags;
    for (const AssetTag &tag : metadata.tags) {
      tags += tags.empty() ? "\"" : ",\"";
      tags += ios_json_escape(tag.name) + "\"";
    }
    items += items.empty() ? "{" : ",{";
    items += "\"id\":\"" + ios_json_escape(key) + "\",\"name\":\"" +
             ios_json_escape(asset.get_name()) + "\",\"type\":\"" +
             ios_json_escape(ios_asset_type_name(type)) + "\",\"library\":\"" +
             ios_json_escape(ios_asset_library_name(library)) + "\",\"catalog\":\"" +
             ios_json_escape(catalog) + "\",\"description\":\"" +
             ios_json_escape(metadata.description ? metadata.description : "") +
             "\",\"author\":\"" + ios_json_escape(metadata.author ? metadata.author : "") +
             "\",\"tags\":[" + tags + "],\"preview\":\"" +
             ios_json_escape(preview != ios_brush_preview_paths.end() ? preview->second : "") +
             "\"}";
    return true;
  });
  return std::string("{\"pending\":") + (preview_budget == 0 ? "true" : "false") +
         ",\"items\":[" + items + "]}";
}

void ios_assets_build(bContext *C)
{
  if (!ios_assets_visible) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < ios_assets_next_snapshot) {
    return;
  }
  /* Asset lists change rarely; keep loading previews quickly while some are missing. */
  std::string json = ios_assets_build_json(C);
  const bool busy = json.find("\"pending\":true") != std::string::npos ||
                    json.find("\"loading\":true") != std::string::npos;
  ios_assets_next_snapshot = now + std::chrono::milliseconds(busy ? 40 : 500);
  if (json != ios_assets_json) {
    ios_assets_json = json;
    ios_assets_snapshot.publish(std::move(json));
  }
}

asset_system::AssetRepresentation *ios_asset_find(const std::string &key)
{
  asset_system::AssetRepresentation *result = nullptr;
  ed::asset::list::iterate(asset_system::all_library_reference(),
                           [&](asset_system::AssetRepresentation &asset) {
                             if (ios_brush_key(asset.make_weak_reference()) != key) {
                               return true;
                             }
                             result = &asset;
                             return false;
                           });
  return result;
}

/** Start a drag of \a asset, as the Asset Browser and asset shelf do. */
void ios_asset_drag_start(bContext *C, asset_system::AssetRepresentation &asset)
{
  if (ID *local_id = asset.local_id()) {
    WM_event_start_drag(C, ICON_NONE, WM_DRAG_ID, local_id, WM_DRAG_NOP);
    return;
  }
  eAssetImportMethod import_method = asset.get_import_method().value_or(ASSET_IMPORT_PACK);
  if (U.experimental.no_data_block_packing && import_method == ASSET_IMPORT_PACK) {
    import_method = ASSET_IMPORT_APPEND_REUSE;
  }
  AssetImportSettings import_settings{};
  import_settings.method = import_method;
  import_settings.use_instance_collections = false;
  WM_event_start_drag(C,
                      ICON_NONE,
                      WM_DRAG_ASSET,
                      WM_drag_create_asset_data(&asset, import_settings),
                      WM_DRAG_FREE_DATA);
}

/** Queue a pointer event at window coordinates \a xy, after the events already queued. */
void ios_asset_drag_event(wmWindow *win, const int xy[2], const wmEventType type, const short val)
{
  wmEvent event = *win->runtime->eventstate;
  event.type = type;
  event.val = val;
  event.flag = eWM_EventFlag(0);
  event.custom = 0;
  event.customdata = nullptr;
  event.customdata_free = false;
  copy_v2_v2_int(event.prev_xy, win->runtime->eventstate->xy);
  copy_v2_v2_int(event.xy, xy);
  copy_v2_v2_int(win->runtime->eventstate->xy, xy);
  WM_event_add(win, &event);
}

/** Where tapping an asset adds it: the 3D cursor, or the active object for data it takes. */
bool ios_asset_add_location(bContext *C, const ID_Type type, int r_xy[2])
{
  ARegion *region = CTX_wm_region(C);
  Scene *scene = CTX_data_scene(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW || !scene) {
    return false;
  }
  float3 location = scene->cursor.location;
  if (ELEM(type, ID_MA, ID_NT)) {
    Object *object = CTX_data_active_object(C);
    if (!object) {
      WM_global_report(RPT_WARNING,
                       type == ID_MA ? "Select an object to apply the material to" :
                                       "Select an object to add the modifier to");
      return false;
    }
    location = object->object_to_world().location();
  }
  float2 region_xy;
  if (ED_view3d_project_float_global(region, location, region_xy, V3D_PROJ_TEST_NOP) !=
      V3D_PROJ_RET_OK)
  {
    region_xy = float2(region->winx, region->winy) * 0.5f;
  }
  r_xy[0] = region->winrct.xmin + int(region_xy.x);
  r_xy[1] = region->winrct.ymin + int(region_xy.y);
  return true;
}

/** Name of the bundled library of starter assets, in `datafiles/starter_assets`. */
constexpr const char *IOS_STARTER_LIBRARY_NAME = "Starter";

/**
 * Bring the drop's snap target up to date at window coordinates \a xy. Drops place objects with
 * the snap data the viewport computes while drawing; a quick drag or a tap can be released before
 * the viewport has drawn at that point.
 */
void ios_asset_drop_snap_update(bContext *C, const int xy[2])
{
  ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW || !CTX_wm_view3d(C)) {
    return;
  }
  V3DSnapCursorState *state = ED_view3d_cursor_snap_state_active_get();
  if (!state) {
    return;
  }
  const int2 mval(xy[0] - region->winrct.xmin, xy[1] - region->winrct.ymin);
  ED_view3d_cursor_snap_data_update(state, C, region, mval);
}

void ios_asset_drag(bContext *C, const IOSAssetDragCommand &command)
{
  const IOSContextStore context_store = ios_context_push_view3d(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  if (!win || !win->runtime->eventstate) {
    ios_context_pop(C, context_store);
    return;
  }
  const bool dragging = !BLI_listbase_is_empty(&wm->runtime->drags);
  int xy[2] = {command.x, command.y};
  if (!ELEM(command.phase, IOS_ASSET_ADD, IOS_ASSET_ADD_RELEASE)) {
    wm_cursor_position_from_ghost_client_coords(win, &xy[0], &xy[1]);
  }

  switch (command.phase) {
    case IOS_ASSET_DRAG_BEGIN:
    case IOS_ASSET_ADD: {
      asset_system::AssetRepresentation *asset = ios_asset_find(command.key);
      if (!asset || dragging ||
          (command.phase == IOS_ASSET_ADD &&
           !ios_asset_add_location(C, asset->get_id_type(), xy)))
      {
        break;
      }
      ios_asset_drag_start(C, *asset);
      ios_asset_drag_xy = int2(xy[0], xy[1]);
      ios_asset_drag_event(win, xy, MOUSEMOVE, KM_NOTHING);
      if (command.phase == IOS_ASSET_ADD) {
        /* Release on the next update, once the move has entered the drop box and its snap
         * target exists, as when a finger lifts after a drag. */
        ios_ui_messages.push(IOSAssetDragCommand{IOS_ASSET_ADD_RELEASE, "", xy[0], xy[1]});
      }
      break;
    }
    case IOS_ASSET_ADD_RELEASE:
      if (dragging) {
        ios_asset_drop_snap_update(C, xy);
        ios_asset_drag_event(win, xy, LEFTMOUSE, KM_RELEASE);
      }
      break;
    case IOS_ASSET_DRAG_MOVE:
      if (dragging) {
        ios_asset_drag_xy = int2(xy[0], xy[1]);
        ios_asset_drag_event(win, xy, MOUSEMOVE, KM_NOTHING);
      }
      break;
    case IOS_ASSET_DRAG_DROP:
      if (dragging) {
        /* The release over a drop box becomes #EVT_DROP, as when releasing a dragged button. */
        ios_asset_drop_snap_update(C, xy);
        ios_asset_drag_event(win, xy, MOUSEMOVE, KM_NOTHING);
        ios_asset_drag_event(win, xy, LEFTMOUSE, KM_RELEASE);
      }
      break;
    case IOS_ASSET_DRAG_CANCEL:
      if (dragging) {
        /* Escape cancels a drag; queued, it also follows any pending moves. */
        ios_asset_drag_event(win, win->runtime->eventstate->xy, EVT_ESCKEY, KM_PRESS);
      }
      break;
  }
  ios_context_pop(C, context_store);
}

/** \} */

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
  std::string object_modes_json = "[]";
  const char *object_mode = "OBJECT";
  if (has_view3d_context) {
    object_modes_json = ios_object_modes_json(C);
    if (const Object *active = CTX_data_active_object(C)) {
      RNA_enum_identifier(rna_enum_object_mode_items, active->mode, &object_mode);
    }
  }
  const std::string mesh_select_mode = mode == CTX_MODE_EDIT_MESH ?
                                           ios_mesh_select_mode_string(tool_settings) :
                                           "";
  bToolRef *active_tool = has_view3d_context ? WM_toolsystem_ref_from_context(C) : nullptr;
  const char *active_tool_id = active_tool ? active_tool->idname : "builtin.select_box";
  std::string tools_json = "[]";
  std::string panels_json = "null";
  std::string render_json = "null";
  std::string viewport_json = "null";
  std::string quick_actions_json;
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
    for (auto [expression, target] :
         {std::pair{"bl_ui.native_render.status_json(bpy.context)", &render_json},
          std::pair{"bl_ui.native_render.viewport_json(bpy.context)", &viewport_json}})
    {
      result = nullptr;
      if (BPY_run_string_as_string(C, imports, expression, nullptr, &result)) {
        *target = result;
        MEM_delete(result);
      }
    }
    if (!ios_constraint_operator(CTX_wm_window(C))) {
      /* Quick actions for the mode, tool and selection, chosen by `bl_ui.native_actions`. */
      const std::string expression = "bl_ui.native_actions.actions_json(bpy.context, \"" +
                                     ios_json_escape(active_tool_id ? active_tool_id : "") + "\")";
      result = nullptr;
      if (BPY_run_string_as_string(C, imports, expression.c_str(), nullptr, &result)) {
        quick_actions_json = result;
        MEM_delete(result);
      }
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
  const std::string tool_settings_json = has_view3d_context ?
      ui::native_properties_query(C, "{\"tab\":\"TOOL\"}", false) : "{}";
  std::string navigation_json = "null";
  if (has_view3d_context) {
    const wmWindow *win = CTX_wm_window(C);
    ARegion *region = CTX_wm_region(C);
    rcti visible = *ED_region_visible_rect(region);
    visible.ymin = std::max(visible.ymin, WM_apple_ios_ui_bottom_tray_ymin(win, region));
    const float width = WM_window_native_pixel_x(win);
    const float height = WM_window_native_pixel_y(win);
    const float radius = U.gizmo_size_navigate_v3d * 0.5f * U.scale_factor;
    const float inset = radius + 10.0f * U.scale_factor;
    if (width > 0 && height > 0) {
      /* Match view3d_gizmo_navigate.cc, in normalized UIKit window coordinates.
       * The native shell can follow the last frame during asynchronous resizing. */
      navigation_json = "[" +
                        std::to_string((region->winrct.xmin + visible.xmax - inset) / width) +
                        "," +
                        std::to_string(1.0f - (region->winrct.ymin + visible.ymin + inset) / height) +
                        "," + std::to_string(radius / width) + "]";
    }
  }
  const std::string redo_json = has_view3d_context ? ui::native_operator_redo_query(C, "", false) :
                                                     "null";
  if (!has_view3d_context) {
    ios_brushes_json = "null";
  }
  else if (const auto now = std::chrono::steady_clock::now(); now >= ios_brushes_next_snapshot) {
    /* Asset polling runs Python per brush, so follow the native refresh cadence. */
    ios_brushes_next_snapshot = now + std::chrono::milliseconds(250);
    ios_brushes_json = ios_brushes_build_json(C, mode);
    if (ios_brushes_json.find("\"pending\":true") != std::string::npos ||
        ios_brushes_json.find("\"loading\":true") != std::string::npos)
    {
      /* Keep loading previews without waiting for the native cadence. */
      ios_brushes_next_snapshot = now + std::chrono::milliseconds(30);
    }
  }
  ios_context_pop(C, context_store);

  std::string json = "{\"mode\":\"";
  json += ios_json_escape(mode_string ? mode_string : "objectmode");
  json += "\",\"shading\":\"";
  json += shading;
  json += "\",\"desktop\":";
  json += ios_desktop_ui ? "true" : "false";
  json += ",\"objectMode\":\"";
  json += ios_json_escape(object_mode);
  json += "\",\"modes\":";
  json += object_modes_json;
  json += ",\"meshSelectMode\":\"";
  json += ios_json_escape(mesh_select_mode);
  json += "\",\"active\":\"";
  json += ios_json_escape(active_tool_id ? active_tool_id : "");
  json += "\",\"canUndo\":";
  json += can_undo ? "true" : "false";
  json += ",\"settings\":";
  json += settings_json;
  json += ",\"toolSettings\":";
  json += tool_settings_json;
  json += ",\"propertiesAck\":" + std::to_string(ios_panels_sequence);
  json += ",\"redo\":";
  json += redo_json;
  json += ",\"navigationGizmo\":";
  json += navigation_json;
  json += ",\"tools\":";
  json += tools_json;
  json += ",\"brushes\":";
  json += ios_brushes_json;
  json += ",\"panels\":";
  json += panels_json;
  json += ",\"render\":";
  json += render_json;
  json += ",\"viewport\":";
  json += viewport_json;
  if (actions.empty() && !quick_actions_json.empty()) {
    json += ",\"actions\":" + quick_actions_json + "}";
    return json;
  }
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

void ios_timeline_build(bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  const wmWindowManager *wm = CTX_wm_manager(C);
  if (scene == nullptr || wm == nullptr) {
    return;
  }
#ifdef WITH_PYTHON
  if (const auto now = std::chrono::steady_clock::now();
      ios_timeline_visible && now >= ios_timeline_next_keys)
  {
    ios_timeline_next_keys = now + std::chrono::milliseconds(250);
    const char *imports[] = {"bpy", "bl_ui", nullptr};
    char *result = nullptr;
    if (BPY_run_string_as_string(
            C, imports, "bl_ui.native_timeline.keys_json(bpy.context)", nullptr, &result))
    {
      ios_timeline_keys_json = result;
      MEM_delete(result);
    }
  }
#endif
  /* Playback changes the frame every redraw; publish only what UIKit has not seen. */
  std::string json = "{\"keys\":" + (ios_timeline_visible ? ios_timeline_keys_json : "[]") +
                     ",\"frame\":" + std::to_string(scene->r.cfra) +
                     ",\"start\":" + std::to_string(PSFRA) + ",\"end\":" + std::to_string(PEFRA) +
                     ",\"preview\":" + ((scene->r.flag & SCER_PRV_RANGE) ? "true" : "false") +
                     ",\"playing\":" + (ED_screen_animation_playing(wm) ? "true" : "false") +
                     ",\"autokey\":" +
                     ((scene->toolsettings && (scene->toolsettings->autokey_mode & AUTOKEY_ON)) ?
                          "true" :
                          "false") +
                     "}";
  if (json != ios_timeline_json) {
    ios_timeline_json = json;
    ios_timeline_snapshot.publish(std::move(json));
  }
}

void ios_toolbar_build(bContext *C)
{
  std::string json = ios_toolbar_build_json_for_context(C);
  ios_toolbar_snapshot.publish(std::move(json));
  ios_timeline_build(C);
  ios_assets_build(C);
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

void WM_apple_ios_starter_library_ensure()
{
  const std::optional<std::string> path = BKE_appdir_folder_id(BLENDER_DATAFILES,
                                                               "starter_assets");
  if (!path) {
    return;
  }
  bUserAssetLibrary *library = BKE_preferences_asset_library_find_by_name(
      &U, IOS_STARTER_LIBRARY_NAME);
  if (library == nullptr) {
    library = BKE_preferences_asset_library_add(&U, IOS_STARTER_LIBRARY_NAME, path->c_str());
  }
  else if (!STREQ(library->dirpath, path->c_str())) {
    /* The app's install location changes with every install and update. */
    BKE_preferences_asset_library_path_set(library, path->c_str());
  }
  /* Dropping the same material twice reuses it rather than adding a copy. */
  library->import_method = ASSET_IMPORT_APPEND_REUSE;
}

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

bool WM_apple_ios_pencil_hover_active()
{
  return ios_hover_request.active;
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

extern "C" void WM_apple_ios_ui_brush_activate(const char *brush_id)
{
  if (brush_id == nullptr || brush_id[0] == '\0') {
    return;
  }

  ios_ui_messages.push(IOSBrushCommand{brush_id});
}

/**
 * Native asset pane drag: \a phase is a #IOSAssetDragPhase, \a x and \a y are GHOST client
 * coordinates. \a asset_id is only needed to begin a drag or add an asset.
 */
extern "C" void WM_apple_ios_ui_asset_drag(int phase, const char *asset_id, int x, int y)
{
  ios_ui_messages.push(IOSAssetDragCommand{phase, asset_id ? asset_id : "", x, y});
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

extern "C" char *WM_apple_ios_ui_timeline_take_json()
{
  std::string json;
  if (!ios_timeline_snapshot.take(json)) {
    return nullptr;
  }
  char *copy = static_cast<char *>(std::malloc(json.size() + 1));
  if (copy) {
    std::memcpy(copy, json.c_str(), json.size() + 1);
  }
  return copy;
}

extern "C" char *WM_apple_ios_ui_assets_take_json()
{
  std::string json;
  if (!ios_assets_snapshot.take(json)) {
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
    /* Brushes are picked from the native brush rail. The shelf shows itself again whenever a
     * brush mode creates its shelf (`DEFAULT_VISIBLE`), so keep it hidden here. */
    for (ARegion &shelf_region : area->regionbase) {
      if (ELEM(shelf_region.regiontype, RGN_TYPE_ASSET_SHELF, RGN_TYPE_ASSET_SHELF_HEADER) &&
          !(shelf_region.flag & RGN_FLAG_HIDDEN))
      {
        shelf_region.flag |= RGN_FLAG_HIDDEN;
        ED_region_visibility_change_update(C, area, &shelf_region);
      }
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

/** Name the toggle by where it goes. Menus show the operator's registered name, as they have no
 * properties to ask #wmOperatorType::get_name with. */
static void ios_interface_toggle_name_update()
{
  wmOperatorType *ot = WM_operatortype_find("WM_OT_apple_ios_interface_toggle", false);
  if (ot == nullptr) {
    return;
  }
  ot->name = ios_desktop_ui ? "Return to iPad Interface" : "Switch to Desktop Interface";
  RNA_def_struct_ui_text(ot->srna, ot->name, ot->description);
}

/** Switch between the native interface and Blender's own, as on a Mac. */
void ios_interface_set(bContext *C, const bool desktop)
{
  if (ios_desktop_ui == desktop) {
    return;
  }
  ios_desktop_ui = desktop;
  ios_interface_toggle_name_update();
  const IOSContextStore context_store = ios_context_push_view3d(C);
  wmWindow *win = CTX_wm_window(C);
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = CTX_wm_area(C);
  if (desktop && win != nullptr && screen != nullptr && area != nullptr && screen->state == SCREENFULL) {
    /* Leave the full-screen 3D View for the screen's editors, headers and toolbars. */
    ED_screen_state_toggle(C, win, area, SCREENFULL);
    area = ios_find_view3d_area(WM_window_get_active_screen(win));
  }
  /* The navigation gizmo moves between the native bottom-right and Blender's top-right. */
  if (ARegion *region = ios_find_window_region(area)) {
    if (region->runtime->gizmo_map != nullptr) {
      WM_gizmomap_reinit(region->runtime->gizmo_map);
      WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
    }
    ED_region_tag_redraw(region);
  }
  ios_ui_gizmo_refresh_pending = true;
  ios_context_pop(C, context_store);
  if (win != nullptr) {
    WM_event_add_notifier(C, NC_WINDOW, nullptr);
  }
}

static wmOperatorStatus ios_interface_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  ios_interface_set(C, !ios_desktop_ui);
  return OPERATOR_FINISHED;
}

static std::string ios_interface_toggle_get_name(wmOperatorType * /*ot*/, PointerRNA * /*ptr*/)
{
  return ios_desktop_ui ? IFACE_("Return to iPad Interface") : IFACE_("Switch to Desktop Interface");
}

void WM_OT_apple_ios_interface_toggle(wmOperatorType *ot)
{
  ot->name = "Switch to Desktop Interface";
  ot->idname = "WM_OT_apple_ios_interface_toggle";
  ot->description =
      "Switch between the iPad interface and Blender's own interface, as on a desktop computer";
  ot->exec = ios_interface_toggle_exec;
  ot->get_name = ios_interface_toggle_get_name;
}

extern "C" bool WM_apple_ios_ui_desktop_mode()
{
  return ios_desktop_ui;
}

extern "C" void WM_apple_ios_ui_ensure_layout(bContext *C)
{
  if (C == nullptr) {
    return;
  }

  if (ios_desktop_ui) {
    /* Blender's own interface keeps whatever screens the files bring. */
    return;
  }

  /* The native interface shows the 3D View full screen. Every new or opened file brings its own
   * screen with Blender's editors, so check on every update, not only at startup. */
  const IOSContextStore context_store = ios_context_push_view3d(C);
  wmWindow *win = CTX_wm_window(C);
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = CTX_wm_area(C);
  if (win != nullptr && screen != nullptr && area != nullptr && screen->state == SCREENNORMAL) {
    ED_screen_state_toggle(C, win, area, SCREENFULL);
    ios_ui_gizmo_refresh_pending = true;
  }
  else if (win != nullptr && screen != nullptr && area != nullptr && screen->state == SCREENFULL) {
    /* A file saved on the iPad reopens already full screen, but with the 3D View's headers and
     * tool settings shown again. Hide them as entering full screen does. */
    bool changed = false;
    for (ARegion &region : area->regionbase) {
      if (ELEM(region.regiontype,
               RGN_TYPE_UI,
               RGN_TYPE_HEADER,
               RGN_TYPE_TOOL_HEADER,
               RGN_TYPE_FOOTER,
               RGN_TYPE_TOOLS,
               RGN_TYPE_NAV_BAR,
               RGN_TYPE_EXECUTE,
               RGN_TYPE_ASSET_SHELF) &&
          !(region.flag & RGN_FLAG_HIDDEN))
      {
        region.flag |= RGN_FLAG_HIDDEN;
        changed = true;
      }
    }
    if (changed) {
      ED_area_init(C, win, area);
      ED_area_tag_redraw(area);
    }
  }
  ios_ui_layout_ready = true;
  ios_context_pop(C, context_store);

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

extern "C" void WM_apple_ios_ui_bottom_tray_set(const float fraction)
{
  if (ios_bottom_tray.exchange(fraction) != fraction) {
    ios_navigation_redraw_pending.store(true);
  }
}

extern "C" int WM_apple_ios_ui_bottom_tray_ymin(const wmWindow *win, const ARegion *region)
{
  /* The navigation gizmo sits above the tray, in region pixels. */
  return int(ios_bottom_tray.load() * WM_window_native_pixel_y(win)) - region->winrct.ymin;
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
  const auto *menu = std::get_if<IOSMenuRequest>(&message);
  if ((!menu || menu->execute) && (!setting || (setting->setting != "native_properties_edit" &&
                   setting->setting != "native_properties_state" &&
                   setting->setting != "native_shading_state" &&
                   setting->setting != "native_panels" &&
                   setting->setting != "native_shading_visible"))) {
    const IOSContextStore context_store = ios_context_push_view3d(C);
    ui::native_properties_interaction_finish(C);
    ios_context_pop(C, context_store);
  }
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
        else if constexpr (std::is_same_v<T, IOSBrushCommand>) {
          ios_brush_activate(C, command.key);
        }
        else if constexpr (std::is_same_v<T, IOSAssetDragCommand>) {
          ios_asset_drag(C, command);
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

  auto supersedes_preview = [](const IOSUIMessage &previous, const IOSUIMessage &next) {
    const auto *a = std::get_if<IOSSettingCommand>(&previous);
    const auto *b = std::get_if<IOSSettingCommand>(&next);
    if (a && b && a->setting == "timeline_frame" && b->setting == "timeline_frame") {
      return true;
    }
    const auto *drag_a = std::get_if<IOSAssetDragCommand>(&previous);
    const auto *drag_b = std::get_if<IOSAssetDragCommand>(&next);
    if (drag_a && drag_b && drag_a->phase == IOS_ASSET_DRAG_MOVE &&
        drag_b->phase == IOS_ASSET_DRAG_MOVE)
    {
      return true;
    }
    if (!a || !b || a->setting != b->setting ||
        (a->setting != "native_properties_edit" && a->setting != "native_redo_edit")) {
      return false;
    }
    std::istringstream a_stream(a->value), b_stream(b->value);
    auto a_value = io::serialize::JsonFormatter().deserialize(a_stream);
    auto b_value = io::serialize::JsonFormatter().deserialize(b_stream);
    const auto *a_request = a_value ? a_value->as_dictionary_value() : nullptr;
    const auto *b_request = b_value ? b_value->as_dictionary_value() : nullptr;
    return a_request && b_request && a_request->lookup_str("phase").value_or("") == "update" &&
           !a_request->lookup_str("interaction").value_or("").is_empty() &&
           a_request->lookup_str("interaction") == b_request->lookup_str("interaction") &&
           a_request->lookup_str("id") == b_request->lookup_str("id") &&
           a_request->lookup_str("key") == b_request->lookup_str("key");
  };
  for (IOSUIMessage &message : ios_ui_messages.take_coalesced(supersedes_preview)) {
    ios_ui_dispatch(C, message);
  }

  const bool hover_changed = ios_hover_requests.take(ios_hover_request);
  {
    const IOSContextStore context_store = ios_context_push_view3d(C);
    ARegion *region = CTX_wm_region(C);
    wmWindow *window = CTX_wm_window(C);
    if (hover_changed && window) {
      /* Removing a cursor needs a frame even when no mesh hover target changed. */
      WM_paint_cursor_tag_redraw(window, region);
      if (bScreen *screen = WM_window_get_active_screen(window)) {
        screen->do_draw_gesture = true;
      }
    }
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
    /* Dragging a material or a modifier, the object it would drop on is highlighted as Pencil
     * hover highlights it. */
    const wmDrag *drag = window ? static_cast<const wmDrag *>(
                                      CTX_wm_manager(C)->runtime->drags.first) :
                                  nullptr;
    const bool drag_to_object = drag && region &&
                                (WM_drag_is_ID_type(drag, ID_MA) || WM_drag_is_ID_type(drag, ID_NT));
    if (drag_to_object) {
      active = true;
      mval[0] = ios_asset_drag_xy.x - region->winrct.xmin;
      mval[1] = ios_asset_drag_xy.y - region->winrct.ymin;
    }
    ED_view3d_pencil_hover_update(C, active, mval, drag_to_object);
    const View3DPencilHover hit = ED_view3d_pencil_hover_get(region);
    ios_hover_snapshot.publish("{\"object\":" + std::to_string(hit.object_uid) +
                               ",\"kind\":" + std::to_string(hit.kind) +
                               ",\"index\":" + std::to_string(hit.index) +
                               ",\"cursorVisible\":" + (ios_hover_request.active ? "true" : "false") + "}");
    ios_context_pop(C, context_store);
  }

  ios_toolbar_build(C);
}

}  // namespace blender
