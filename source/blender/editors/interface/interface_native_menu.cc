/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Native menus are projections of Blender layouts, not a second command registry. */
#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BLT_translation.hh"
#include "BLI_serialize.hh"
#include "ED_undo.hh"
#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "UI_interface_layout.hh"
#include "WM_api.hh"
#include "WM_native_interface.hh"
#include "WM_types.hh"
#include "interface_intern.hh"

#include <sstream>

namespace blender::ui {
namespace {
using namespace io::serialize;

std::string encode(const Value &value)
{
  std::ostringstream stream;
  JsonFormatter().serialize(stream, value);
  return stream.str();
}

std::string label(const Button &but)
{
  return but.drawstr.substr(
      0, (but.flag & BUT_HAS_SEP_CHAR) ? but.drawstr.find(UI_SEP_CHAR) : std::string::npos);
}

/** The key that runs the button's operator, as the menu shows it, or null. Mouse buttons and
 * keys the native interface keeps from the operator have none. */
const wmKeyMapItem *shortcut(const bContext *C, const Button &but)
{
  if (!but.optype) {
    return nullptr;
  }
  IDProperty *properties = but.opptr ? static_cast<IDProperty *>(but.opptr->data) : nullptr;
  const wmKeyMapItem *kmi = WM_key_event_operator(C,
                                                  but.optype->idname,
                                                  but.opcontext,
                                                  properties,
                                                  EVT_TYPE_MASK_KEYBOARD,
                                                  EVT_TYPE_MASK_KEYBOARD_MODIFIER,
                                                  nullptr);
  if (kmi == nullptr) {
    return nullptr;
  }
  const wmNativeInterface *native = WM_native_interface();
  if (native && native->keymap_item_hidden && native->keymap_item_hidden(kmi)) {
    return nullptr;
  }
  return kmi;
}

/* Validate the whole route again at activation. Never keep RNA pointers, buttons, or
 * list indices across UIKit callbacks, undo, file loads, or add-on registration. */
std::string identity(bContext *C, Button &but)
{
  ArrayValue key;
  key.append_str(label(but));
  key.append_int(int(but.type));
  if (MenuType *mt = button_menutype_get(&but)) {
    key.append_str(mt->idname);
  }
  if (but.optype) {
    key.append_str(but.optype->idname);
    key.append_int(int(but.opcontext));
    if (but.opptr) {
      key.append_str(RNA_pointer_as_string_keywords(C, but.opptr, false, false, true, INT_MAX));
    }
  }
  if (but.rnaprop) {
    key.append_str(RNA_property_identifier(but.rnaprop));
    key.append_int(but.rnaindex);
    if (RNA_property_type(but.rnaprop) == PROP_ENUM) {
      key.append_int(int(but.hardmax));
    }
  }
  return encode(key);
}

struct Invocation {
  wmOperatorType *type = nullptr;
  PointerRNA *properties = nullptr;
  wm::OpCallContext opcontext = wm::OpCallContext::InvokeRegionWin;
  std::unique_ptr<bContextStore> context;
  PointerRNA rnapoin = {};
  PropertyRNA *rnaprop = nullptr;
  int index = -1;
  int enum_value = 0;
  std::string title;

  ~Invocation()
  {
    if (properties) {
      WM_operator_properties_free(properties);
      MEM_delete(properties);
    }
  }

  void capture(Button &but)
  {
    title = label(but);
    if (but.context) {
      context = std::make_unique<bContextStore>(*but.context);
    }
    type = but.optype;
    opcontext = but.opcontext;
    if (type) {
      properties = but.opptr;
      but.opptr = nullptr;
    }
    else {
      rnapoin = but.rnapoin;
      rnaprop = but.rnaprop;
      index = but.rnaindex;
      enum_value = int(but.hardmax);
    }
  }

  void execute(bContext *C)
  {
    const bContextStore *previous = CTX_store_get(C);
    CTX_store_set(C, context.get());
    if (type && WM_operator_poll_context(C, type, opcontext)) {
      WM_operator_name_call_ptr_with_depends_on_cursor(
          C, type, opcontext, properties, nullptr, title);
    }
    else if (rnaprop && RNA_property_editable(&rnapoin, rnaprop)) {
      if (RNA_property_type(rnaprop) == PROP_BOOLEAN) {
        if (RNA_property_array_check(rnaprop)) {
          RNA_property_boolean_set_index(
              &rnapoin, rnaprop, index, !RNA_property_boolean_get_index(&rnapoin, rnaprop, index));
        }
        else {
          RNA_property_boolean_set(
              &rnapoin, rnaprop, !RNA_property_boolean_get(&rnapoin, rnaprop));
        }
      }
      else if (RNA_property_type(rnaprop) == PROP_ENUM) {
        const int value = RNA_property_flag(rnaprop) & PROP_ENUM_FLAG ?
                              RNA_property_enum_get(&rnapoin, rnaprop) ^ enum_value :
                              enum_value;
        RNA_property_enum_set(&rnapoin, rnaprop, value);
      }
      RNA_property_update(C, &rnapoin, rnaprop);
      if (rnapoin.owner_id && RNA_struct_undo_check(rnapoin.type)) {
        ED_undo_push(C, title.c_str());
      }
    }
    CTX_store_set(C, previous);
  }
};

bool is_submenu(Button &but)
{
  return button_menutype_get(&but) || (but.menu_create_func && !button_paneltype_get(&but));
}

bool is_action(Button &but)
{
  return but.optype ||
         (but.rnaprop && ELEM(RNA_property_type(but.rnaprop), PROP_BOOLEAN, PROP_ENUM));
}

void project(bContext *C,
             MenuType *mt,
             Button *parent,
             const std::string &root,
             const ArrayValue &path,
             int depth,
             ArrayValue &result,
             Invocation *invocation)
{
  if (depth > 16) {
    return;
  }
  const bContextStore *previous = CTX_store_get(C);
  if (parent && parent->context) {
    CTX_store_set(C, parent->context);
  }
  if (mt && !WM_menutype_poll(C, mt)) {
    CTX_store_set(C, previous);
    return;
  }
  /* Detached blocks cannot replace visible regions' blocks or survive a file load. */
  Block *block = block_begin(C, nullptr, __func__, EmbossType::Emboss);
  Layout &layout = block_layout(block,
                                  LayoutDirection::Vertical,
                                  LayoutType::Menu,
                                  0,
                                  0,
                                  200,
                                  0,
                                  UI_MENU_PADDING,
                                  style_get_dpi());
  layout.operator_context_set(wm::OpCallContext::InvokeRegionWin);
  if (parent && parent->context) {
    layout.context_copy(parent->context);
  }
  block_flag_enable(block, BLOCK_SHOW_SHORTCUT_ALWAYS);
  if (mt) {
    menutype_draw(C, mt, &layout);
  }
  else if (parent && parent->menu_create_func) {
    parent->menu_create_func(C, &layout, parent->poin);
  }
  block_end(C, block);

  for (const auto &button : block->buttons) {
    Button &but = *button;
    const bool submenu = is_submenu(but);
    const bool action = is_action(but);
    const bool disabled = but.flag & (BUT_DISABLED | BUT_INACTIVE);
    if (depth < path.elements().size()) {
      const StringValue *step = path.elements()[depth]->as_string_value();
      if (!step || identity(C, but) != step->value()) {
        continue;
      }
      if (!disabled) {
        if (invocation && depth + 1 == path.elements().size() && action && !submenu) {
          invocation->capture(but);
        }
        else if (submenu) {
          project(C, button_menutype_get(&but), &but, root, path, depth + 1, result, invocation);
        }
      }
      break;
    }
    if (invocation) {
      continue;
    }
    if (ELEM(but.type, ButtonType::Sepr, ButtonType::SeprLine)) {
      result.append_dict()->append_int("separator", 1);
      continue;
    }
    std::string title = label(but);
    if (title.empty()) {
      if (MenuType *child = button_menutype_get(&but)) {
        title = child->label;
      }
    }
    if (title.empty() && but.optype) {
      title = WM_operatortype_name(but.optype, but.opptr);
    }
    if (title.empty()) {
      continue;
    }
    auto item = result.append_dict();
    item->append_str("title", title);
    item->append_int("disabled", disabled || (!submenu && !action));
    item->append_int("checked", action && but.rnaprop && button_is_pushed(&but));
    item->append_int("submenu", submenu);
    item->append_int("action", action);
    item->append_str("operator", but.optype ? but.optype->idname : "");
    /* Preserve the layout's icon, not a guess based on the operator. Enum choices
     * and submenus can have different artwork even when their command is shared. */
    const int icon = button_icon(&but);
    const char *icon_name = nullptr;
    if (!ELEM(icon, ICON_NONE, ICON_BLANK1) &&
        RNA_enum_identifier(rna_enum_icon_items, icon, &icon_name))
    {
      item->append_str("icon", icon_name);
    }
    if (MenuType *child = button_menutype_get(&but)) {
      item->append_str("menu", child->idname);
    }
    const char *key_name = nullptr;
    if (const wmKeyMapItem *kmi = shortcut(C, but);
        kmi && RNA_enum_identifier(rna_enum_event_type_items, kmi->type, &key_name))
    {
      auto key = item->append_dict("key");
      key->append_str("type", key_name);
      key->append_int("shift", kmi->shift == KM_MOD_HELD);
      key->append_int("ctrl", kmi->ctrl == KM_MOD_HELD);
      key->append_int("alt", kmi->alt == KM_MOD_HELD);
      key->append_int("oskey", kmi->oskey == KM_MOD_HELD);
    }
    auto route = item->append_dict("route");
    route->append_str("root", root);
    auto steps = route->append_array("path");
    for (const auto &step : path.elements()) {
      steps->append(step);
    }
    steps->append_str(identity(C, but));
  }
  block_free(nullptr, block);
  CTX_store_set(C, previous);
}
}  // namespace

static const char *native_context_menu_id(const bContext *C)
{
  switch (CTX_data_mode_enum(C)) {
    case CTX_MODE_OBJECT: return "VIEW3D_MT_object_context_menu";
    case CTX_MODE_EDIT_MESH: return "VIEW3D_MT_edit_mesh_context_menu";
    case CTX_MODE_EDIT_CURVE:
    case CTX_MODE_EDIT_SURFACE: return "VIEW3D_MT_edit_curve_context_menu";
    case CTX_MODE_EDIT_TEXT: return "VIEW3D_MT_edit_font_context_menu";
    case CTX_MODE_EDIT_ARMATURE: return "VIEW3D_MT_armature_context_menu";
    case CTX_MODE_EDIT_METABALL: return "VIEW3D_MT_edit_metaball_context_menu";
    case CTX_MODE_EDIT_LATTICE: return "VIEW3D_MT_edit_lattice_context_menu";
    case CTX_MODE_EDIT_CURVES: return "VIEW3D_MT_edit_curves_context_menu";
    case CTX_MODE_EDIT_GREASE_PENCIL: return "VIEW3D_MT_greasepencil_edit_context_menu";
    case CTX_MODE_POSE: return "VIEW3D_MT_pose_context_menu";
    case CTX_MODE_PARTICLE: return "VIEW3D_MT_particle_context_menu";
    case CTX_MODE_SCULPT: return "VIEW3D_PT_sculpt_context_menu";
    case CTX_MODE_PAINT_WEIGHT: return "VIEW3D_PT_paint_weight_context_menu";
    case CTX_MODE_PAINT_VERTEX: return "VIEW3D_PT_paint_vertex_context_menu";
    case CTX_MODE_PAINT_TEXTURE: return "VIEW3D_PT_paint_texture_context_menu";
    case CTX_MODE_PAINT_GREASE_PENCIL: return "VIEW3D_PT_greasepencil_draw_context_menu";
    case CTX_MODE_SCULPT_GREASE_PENCIL: return "VIEW3D_PT_greasepencil_sculpt_context_menu";
    case CTX_MODE_WEIGHT_GREASE_PENCIL: return "VIEW3D_PT_greasepencil_weight_context_menu";
    case CTX_MODE_VERTEX_GREASE_PENCIL: return "VIEW3D_PT_greasepencil_vertex_paint_context_menu";
    default: return nullptr;
  }
}

std::string native_menu_query(bContext *C, const std::string &request, const bool execute)
{
  ArrayValue result;
  std::istringstream stream(request);
  std::unique_ptr<Value> value = JsonFormatter().deserialize(stream);
  const DictionaryValue *query = value ? value->as_dictionary_value() : nullptr;
  if (!query) {
    return encode(result);
  }
  const auto root = query->lookup_str("root");
  const ArrayValue *path = query->lookup_array("path");
  if (root && *root == "menubar" && path && path->elements().is_empty() && !execute) {
    for (const char *idname : {"TOPBAR_MT_editor_menus", "VIEW3D_MT_editor_menus"}) {
      if (MenuType *mt = WM_menutype_find(idname, true)) {
        project(C, mt, nullptr, idname, *path, 0, result, nullptr);
      }
    }
    return encode(result);
  }
  if (!root || !path || (execute && *root == "context")) {
    return encode(result);
  }
  const char *context_id = native_context_menu_id(C);
  const std::string resolved = *root == "context" ? (context_id ? context_id : "") : *root;
  /* Properties requests reconstruct a detached Properties editor before arriving here.
   * Its registered panel menus use the same poll and route validation as viewport menus. */
  const bool properties_menu = CTX_wm_space_properties(C) &&
                               WM_menutype_find(resolved.c_str(), true);
  if (!ELEM(resolved,
            "TOPBAR_MT_editor_menus",
            "VIEW3D_MT_editor_menus",
            "VIEW3D_MT_add",
            /* The Share menu's Import and Export. */
            "TOPBAR_MT_file_import",
            "TOPBAR_MT_file_export") &&
      (!context_id || resolved != context_id) && !properties_menu)
  {
    return encode(result);
  }
  if (*root == "context" && path->elements().is_empty() && !execute) {
    /* Name the radial menu as Blender titles the context menu. Edit Mesh's menu has no label of
     * its own, so it uses the mode name instead (e.g. "Mesh"). */
    std::string heading;
    if (MenuType *mt = WM_menutype_find(resolved.c_str(), true)) {
      heading = CTX_IFACE_(mt->translation_context, mt->label);
    }
    else if (PanelType *pt = WM_paneltype_find(resolved.c_str(), true)) {
      heading = CTX_IFACE_(pt->translation_context, pt->label);
    }
    const char *mode_name = nullptr;
    if (heading.empty() &&
        RNA_enum_name(rna_enum_context_mode_items, CTX_data_mode_enum(C), &mode_name))
    {
      heading = IFACE_(mode_name);
      const std::string suffix = " Edit";
      if (heading.size() > suffix.size() && heading.ends_with(suffix)) {
        heading.resize(heading.size() - suffix.size());
      }
    }
    if (!heading.empty()) {
      result.append_dict()->append_str("heading", heading);
    }
  }
  if (MenuType *mt = WM_menutype_find(resolved.c_str(), true)) {
    Invocation invocation;
    project(C, mt, nullptr, resolved, *path, 0, result, execute ? &invocation : nullptr);
    if (execute) {
      invocation.execute(C);
    }
  }
  else if (PanelType *pt = WM_paneltype_find(resolved.c_str(), true)) {
    /* Brush contexts are settings panels, not command lists. Preserve their actual
     * controls instead of inventing unrelated radial actions for these modes. */
    if (pt->poll && !pt->poll(C, pt)) {
      return encode(result);
    }
    if (!execute && path->elements().is_empty()) {
      auto item = result.append_dict();
      item->append_str("title", std::string(pt->label) + " Settings");
      item->append_int("action", 1);
      item->append_str("operator", "WM_OT_call_panel");
      auto route = item->append_dict("route");
      route->append_str("root", resolved);
      route->append_array("path")->append_str("panel");
    }
    else if (execute && path->elements().size() == 1) {
      const StringValue *step = path->elements()[0]->as_string_value();
      if (step && step->value() == "panel") {
        PointerRNA properties = WM_operator_properties_create("WM_OT_call_panel");
        RNA_string_set(&properties, "name", resolved.c_str());
        WM_operator_name_call(C, "WM_OT_call_panel", wm::OpCallContext::InvokeRegionWin,
                              &properties, nullptr);
        WM_operator_properties_free(&properties);
      }
    }
  }
  return encode(result);
}
}  // namespace blender::ui
