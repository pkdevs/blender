/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Native Properties projects the registered Blender panels and their RNA controls. */
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"
#include "BLI_listbase.h"
#include "BLI_serialize.hh"
#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"
#include "ED_buttons.hh"
#include "ED_undo.hh"
#include "IMB_colormanagement.hh"
#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"
#include "UI_interface_layout.hh"
#include "WM_api.hh"
#include "WM_types.hh"
#include "interface_intern.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace blender::ui {
std::string native_menu_query(bContext *C, const std::string &request, bool execute);
namespace {
using namespace io::serialize;

std::string encode(const Value &value)
{
  std::ostringstream stream;
  JsonFormatter().serialize(stream, value);
  return stream.str();
}

std::unique_ptr<Value> decode(const std::string &json)
{
  std::istringstream stream(json);
  return JsonFormatter().deserialize(stream);
}

/* A detached Properties space provides the same context as the desktop editor,
 * without changing the visible area or retaining scene pointers between requests. */
struct PropertiesContext {
  bContext *C;
  ScrArea *previous_area;
  ARegion *previous_region;
  ScrArea area = {};
  SpaceProperties *space;
  ARegion *region;

  PropertiesContext(bContext *context) : C(context)
  {
    previous_area = CTX_wm_area(C);
    previous_region = CTX_wm_region(C);
    area.spacetype = SPACE_PROPERTIES;
    area.type = BKE_spacetype_from_id(SPACE_PROPERTIES);
    SpaceLink *link = area.type->create(&area, CTX_data_scene(C));
    BLI_addtail(&area.spacedata, link);
    std::swap(area.regionbase, link->regionbase);
    space = reinterpret_cast<SpaceProperties *>(link);
    region = BKE_area_find_region_type(&area, RGN_TYPE_WINDOW);
    region->runtime->type = BKE_regiontype_from_id(area.type, RGN_TYPE_WINDOW);
    region->winx = 320;
    region->winy = 1200;
    area.winx = region->winx;
    area.winy = region->winy;
    CTX_wm_area_set(C, &area);
    CTX_wm_region_set(C, region);
    ED_buttons_native_context_prepare(C, space);
  }

  ~PropertiesContext()
  {
    CTX_wm_area_set(C, previous_area);
    CTX_wm_region_set(C, previous_region);
    BKE_screen_area_free(&area);
  }
};

std::string icon_name(const int icon)
{
  const char *name = nullptr;
  return RNA_enum_identifier(rna_enum_icon_items, icon, &name) ? name : "";
}

std::string property_key(const Button &but, int index)
{
  const auto path = RNA_path_from_ID_to_struct(&but.rnapoin);
  return std::to_string(but.rnapoin.owner_id ? but.rnapoin.owner_id->session_uid : 0) + ":" +
         (path ? *path : RNA_struct_identifier(but.rnapoin.type)) + ":" +
         RNA_property_identifier(but.rnaprop) + ":" + std::to_string(index);
}

bool panel_poll(bContext *C, PanelType *panel)
{
  return (!panel->parent || panel_poll(C, panel->parent)) &&
         (!panel->poll || panel->poll(C, panel));
}

bool is_color(PointerRNA *ptr, PropertyRNA *prop)
{
  return RNA_property_type(prop) == PROP_FLOAT &&
         ELEM(RNA_property_subtype(prop), PROP_COLOR, PROP_COLOR_GAMMA) &&
         ELEM(RNA_property_array_length(ptr, prop), 3, 4);
}

bool set_property(bContext *C, Button &but, int index, const DictionaryValue &request)
{
  PointerRNA *ptr = &but.rnapoin;
  PropertyRNA *prop = but.rnaprop;
  if (!RNA_property_editable(ptr, prop) || (but.flag & (BUT_DISABLED | BUT_INACTIVE))) {
    return false;
  }
  const bool array = RNA_property_array_check(prop);
  const auto number = request.lookup_double("value");
  const auto integer = request.lookup_int("value");
  const double numeric = number.value_or(integer.value_or(0));
  switch (RNA_property_type(prop)) {
    case PROP_FLOAT: {
      if (is_color(ptr, prop) && index == -1) {
        const auto *components = request.lookup_array("value");
        const int length = RNA_property_array_length(ptr, prop);
        if (!components || components->elements().size() != length) {
          return false;
        }
        float color[4] = {0, 0, 0, 1};
        for (int i = 0; i < length; i++) {
          const auto &component = components->elements()[i];
          const auto *real = component->as_double_value();
          const auto *integer_component = component->as_int_value();
          if (!real && !integer_component) {
            return false;
          }
          const double value = real ? real->value() : double(integer_component->value());
          if (!std::isfinite(value) || std::abs(value) > FLT_MAX) {
            return false;
          }
          color[i] = float(value);
        }
        if (RNA_property_subtype(prop) == PROP_COLOR) {
          IMB_colormanagement_srgb_to_scene_linear_v3(color, color);
        }
        float minimum, maximum;
        RNA_property_float_range(ptr, prop, &minimum, &maximum);
        for (int i = 0; i < length; i++) {
          if (!std::isfinite(color[i])) {
            return false;
          }
          color[i] = std::clamp(color[i], minimum, maximum);
        }
        if (length == 4) {
          color[3] = std::clamp(color[3], 0.0f, 1.0f);
        }
        float original[4];
        RNA_property_float_get_array(ptr, prop, original);
        bool changed = false;
        for (int i = 0; i < length; i++) {
          changed |= std::abs(original[i] - color[i]) >
                     1e-6f * std::max(1.0f, std::abs(original[i]));
        }
        if (!changed) {
          return true;
        }
        RNA_property_float_set_array(ptr, prop, color);
        break;
      }
      if ((!number && !integer) || !std::isfinite(numeric)) {
        return false;
      }
      float minimum, maximum;
      RNA_property_float_range(ptr, prop, &minimum, &maximum);
      const float value = std::clamp(float(numeric), minimum, maximum);
      if (request.lookup_dict("slider") && value == (array ? RNA_property_float_get_index(ptr, prop, index) : RNA_property_float_get(ptr, prop))) {
        return true;
      }
      if (array) {
        RNA_property_float_set_index(ptr, prop, index, value);
      }
      else {
        RNA_property_float_set(ptr, prop, value);
      }
      break;
    }
    case PROP_INT: {
      if ((!number && !integer) || !std::isfinite(numeric)) {
        return false;
      }
      int minimum, maximum;
      RNA_property_int_range(ptr, prop, &minimum, &maximum);
      const int value = int(std::clamp(numeric, double(minimum), double(maximum)));
      if (request.lookup_dict("slider") && value == (array ? RNA_property_int_get_index(ptr, prop, index) : RNA_property_int_get(ptr, prop))) {
        return true;
      }
      if (array) {
        RNA_property_int_set_index(ptr, prop, index, value);
      }
      else {
        RNA_property_int_set(ptr, prop, value);
      }
      break;
    }
    case PROP_BOOLEAN: {
      const auto *raw = request.lookup("value");
      const BooleanValue *value = raw ? (*raw)->as_boolean_value() : nullptr;
      if (!value) {
        return false;
      }
      if (array) {
        RNA_property_boolean_set_index(ptr, prop, index, value->value());
      }
      else {
        RNA_property_boolean_set(ptr, prop, value->value());
      }
      break;
    }
    case PROP_STRING: {
      const auto value = request.lookup_str("value");
      if (!value) {
        return false;
      }
      RNA_property_string_set(ptr, prop, value->c_str());
      break;
    }
    case PROP_ENUM: {
      const auto value = request.lookup_str("value");
      const EnumPropertyItem *items;
      int count;
      bool free;
      RNA_property_enum_items(C, ptr, prop, &items, &count, &free);
      int enum_value;
      const bool valid = value && RNA_enum_value_from_identifier(items, value->c_str(), &enum_value);
      if (free) {
        MEM_delete(items);
      }
      if (!valid) {
        return false;
      }
      if (RNA_property_flag(prop) & PROP_ENUM_FLAG) {
        enum_value ^= RNA_property_enum_get(ptr, prop);
      }
      RNA_property_enum_set(ptr, prop, enum_value);
      break;
    }
    case PROP_POINTER: {
      const auto value = request.lookup_str("value");
      if (!value || !RNA_struct_is_ID(RNA_property_pointer_type(ptr, prop))) {
        return false;
      }
      PointerRNA target = PointerRNA_NULL;
      if (!value->is_empty()) {
        ID *id;
        FOREACH_MAIN_ID_BEGIN (CTX_data_main(C), id) {
          if (std::to_string(id->session_uid) == *value) {
            target = RNA_id_pointer_create(id);
            break;
          }
        }
        FOREACH_MAIN_ID_END;
        if (!target.data || !RNA_struct_is_a(target.type, RNA_property_pointer_type(ptr, prop))) {
          return false;
        }
      }
      if (!RNA_property_pointer_poll(ptr, prop, &target)) {
        return false;
      }
      RNA_property_pointer_set(ptr, prop, target, nullptr);
      break;
    }
    default:
      return false;
  }
  RNA_property_update(C, ptr, prop);
  if (ptr->owner_id && RNA_struct_undo_check(ptr->type)) {
    ED_undo_push(C, RNA_property_ui_name(prop));
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return true;
}

void project_property(bContext *C, Button &but, int index, DictionaryValue &field)
{
  PointerRNA *ptr = &but.rnapoin;
  PropertyRNA *prop = but.rnaprop;
  const bool array = RNA_property_array_check(prop);
  const int length = RNA_property_array_length(ptr, prop);
  const PropertyType type = RNA_property_type(prop);
  if (is_color(ptr, prop)) {
    float color[4] = {0, 0, 0, 1};
    RNA_property_float_get_array(ptr, prop, color);
    const float exposure = std::max({1.0f, color[0], color[1], color[2]});
    if (RNA_property_subtype(prop) == PROP_COLOR) {
      IMB_colormanagement_scene_linear_to_srgb_v3(color, color);
    }
    field.append_str("type", "COLOR");
    field.append_str("label", RNA_property_ui_name(prop));
    field.append_str("key", property_key(but, -1));
    field.append_str("property", RNA_property_identifier(prop));
    field.append_str("owner", RNA_struct_is_a(ptr->type, RNA_Object) ? "object" : "data");
    field.append_int("alpha", length == 4);
    field.append_double("exposure", std::isfinite(exposure) ? exposure : 1);
    field.append_int("editable", RNA_property_editable(ptr, prop) &&
        !(but.flag & (BUT_DISABLED | BUT_INACTIVE)));
    auto values = field.append_array("value");
    for (int i = 0; i < length; i++) {
      values->append_double(std::isfinite(color[i]) ? color[i] : 0);
    }
    return;
  }
  std::string title = RNA_property_ui_name(prop);
  if (array && length <= 4) {
    const bool color = ELEM(RNA_property_subtype(prop), PROP_COLOR, PROP_COLOR_GAMMA);
    const char *axes = color ? "RGBA" : length == 4 ? "WXYZ" : "XYZW";
    title = std::string(1, axes[index]);
    field.append_str("group", RNA_property_ui_name(prop));
  }
  field.append_str("label", title);
  field.append_str("key", property_key(but, index));
  field.append_str("property", RNA_property_identifier(prop));
  field.append_str("owner", RNA_struct_is_a(ptr->type, RNA_Object) ? "object" : "data");
  if (array) {
    field.append_int("index", index);
  }
  field.append_int("editable", RNA_property_editable(ptr, prop) &&
      !(but.flag & (BUT_DISABLED | BUT_INACTIVE)));
  field.append_str("icon", icon_name(button_icon(&but)));
  if (but.type == ButtonType::NumSlider && ELEM(type, PROP_INT, PROP_FLOAT) &&
      std::isfinite(but.softmin) && std::isfinite(but.softmax) && but.softmax > but.softmin) {
    auto slider = field.append_dict("slider");
    slider->append_double("min", but.softmin);
    slider->append_double("max", but.softmax);
    const PropertyScaleType scale = button_scale_type(&but);
    slider->append_str("scale", scale == PROP_SCALE_LOG && but.softmin > 0 ? "LOG" :
                                scale == PROP_SCALE_CUBIC ? "CUBIC" : "LINEAR");
  }
  switch (type) {
    case PROP_BOOLEAN:
      field.append_str("type", "BOOLEAN");
      field.append("value", std::make_shared<BooleanValue>(array ? RNA_property_boolean_get_index(ptr, prop, index) : RNA_property_boolean_get(ptr, prop)));
      break;
    case PROP_INT: {
      field.append_str("type", "INT");
      field.append_int("value", array ? RNA_property_int_get_index(ptr, prop, index) : RNA_property_int_get(ptr, prop));
      int minimum, maximum;
      RNA_property_int_range(ptr, prop, &minimum, &maximum);
      field.append_int("min", minimum);
      field.append_int("max", maximum);
      break;
    }
    case PROP_FLOAT: {
      field.append_str("type", "FLOAT");
      const double value = array ? RNA_property_float_get_index(ptr, prop, index) : RNA_property_float_get(ptr, prop);
      field.append_double("value", std::isfinite(value) ? value : 0);
      float minimum, maximum;
      RNA_property_float_range(ptr, prop, &minimum, &maximum);
      field.append_double("min", minimum);
      field.append_double("max", maximum);
      field.append_int("angle", ELEM(RNA_property_subtype(prop), PROP_ANGLE, PROP_EULER));
      break;
    }
    case PROP_STRING:
      field.append_str("type", "STRING");
      field.append_str("value", RNA_property_string_get(ptr, prop));
      break;
    case PROP_ENUM: {
      field.append_str("type", "ENUM");
      const int selected = RNA_property_enum_get(ptr, prop);
      auto options = field.append_array("options");
      const EnumPropertyItem *items;
      int count;
      bool free;
      RNA_property_enum_items(C, ptr, prop, &items, &count, &free);
      std::string current;
      for (int i = 0; i < count; i++) {
        const auto &item = items[i];
        if (!item.identifier || !item.identifier[0]) {
          continue;
        }
        auto option = options->append_dict();
        option->append_str("value", item.identifier);
        option->append_str("label", item.name);
        option->append_str("icon", icon_name(item.icon));
        const bool checked = (RNA_property_flag(prop) & PROP_ENUM_FLAG) ?
                                 (selected & item.value) != 0 : selected == item.value;
        option->append_int("checked", checked);
        if (checked) {
          current = item.identifier;
        }
      }
      field.append_str("value", current);
      if (free) {
        MEM_delete(items);
      }
      break;
    }
    case PROP_POINTER: {
      field.append_str("type", "ENUM");
      PointerRNA current = RNA_property_pointer_get(ptr, prop);
      field.append_str("value", current.owner_id ? std::to_string(current.owner_id->session_uid) : "");
      auto options = field.append_array("options");
      auto empty = options->append_dict();
      empty->append_str("value", "");
      empty->append_str("label", "None");
      ID *id;
      FOREACH_MAIN_ID_BEGIN (CTX_data_main(C), id) {
        PointerRNA candidate = RNA_id_pointer_create(id);
        if (RNA_struct_is_a(candidate.type, RNA_property_pointer_type(ptr, prop)) &&
            RNA_property_pointer_poll(ptr, prop, &candidate)) {
          auto option = options->append_dict();
          option->append_str("value", std::to_string(id->session_uid));
          option->append_str("label", id->name + 2);
          option->append_str("icon", icon_name(RNA_struct_ui_icon(candidate.type)));
        }
      }
      FOREACH_MAIN_ID_END;
      break;
    }
    default:
      field.append_str("type", "LABEL");
      field.append_str("value", "");
      break;
  }
}
}  // namespace

std::string native_properties_query(bContext *C, const std::string &json, bool execute)
{
  DictionaryValue result;
  auto query_value = decode(json);
  const auto *query = query_value ? query_value->as_dictionary_value() : nullptr;
  if (!query || !CTX_data_scene(C)) {
    return "{}";
  }
  PropertiesContext context(C);
  PointerRNA space_ptr = RNA_pointer_create_discrete(nullptr, RNA_SpaceProperties, context.space);
  PropertyRNA *tab_prop = RNA_struct_find_property(&space_ptr, "context");
  const EnumPropertyItem *tabs;
  int tab_count;
  bool free_tabs;
  RNA_property_enum_items(C, &space_ptr, tab_prop, &tabs, &tab_count, &free_tabs);
  const std::string requested_tab = query->lookup_str("tab").value_or("OBJECT");
  int selected = BCONTEXT_OBJECT;
  if (!RNA_enum_value_from_identifier(tabs, requested_tab.c_str(), &selected)) {
    selected = context.space->mainb;
  }
  context.space->mainbuser = context.space->mainb = selected;
  ED_buttons_native_context_prepare(C, context.space);
  auto tab_items = result.append_array("tabs");
  std::string tab_id = "OBJECT";
  for (int i = 0; i < tab_count; i++) {
    const auto &tab = tabs[i];
    if (!tab.identifier || !tab.identifier[0]) {
      continue;
    }
    auto item = tab_items->append_dict();
    item->append_str("id", tab.identifier);
    item->append_str("title", tab.name);
    item->append_str("icon", icon_name(tab.icon));
    if (tab.value == context.space->mainb) {
      tab_id = tab.identifier;
    }
  }
  if (free_tabs) {
    MEM_delete(tabs);
  }
  std::string panel_context = tab_id;
  std::transform(panel_context.begin(), panel_context.end(), panel_context.begin(), ::tolower);
  if (panel_context == "particles") {
    panel_context = "particle";
  }
  result.append_str("tab", tab_id);
  result.append_str("name", "Properties");
  Object *object = CTX_data_active_object(C);
  result.append_str("id", tab_id + ":" + std::to_string(object ? object->id.session_uid : 0));
  auto sections = result.append_array("sections");
  const ArrayValue *expanded = query->lookup_array("expanded");
  const auto requested_panel = query->lookup_str("panel");
  const auto requested_key = query->lookup_str("key");
  bool applied = false;
  wmOperatorType *operator_type = nullptr;
  PointerRNA *operator_properties = nullptr;
  std::unique_ptr<bContextStore> operator_context;
  std::set<std::string> open_panels;
  std::map<std::string, std::string> outer_panels;
  std::map<std::string, std::shared_ptr<ArrayValue>> outer_fields;
  struct Candidate {
    PanelType *type;
    Panel *instance;
    std::string id;
    std::string parent;
  };
  std::vector<Candidate> candidates;
  std::set<Panel *> instances;
  for (PanelType &pt : context.region->runtime->type->paneltypes) {
    if (!(pt.flag & PANEL_TYPE_INSTANCED)) {
      candidates.push_back({&pt, nullptr, pt.idname, pt.parent_id});
    }
  }
  for (size_t candidate_index = 0; candidate_index < candidates.size(); candidate_index++) {
    const Candidate candidate = candidates[candidate_index];
    PanelType &pt = *candidate.type;
    if (!pt.draw || (!candidate.instance && panel_context != pt.context) ||
        ((pt.flag & PANEL_TYPE_NO_HEADER) && !pt.label[0]) || !panel_poll(C, &pt)) {
      continue;
    }
    const std::string outer = candidate.parent.empty() ? candidate.id : outer_panels[candidate.parent];
    outer_panels[candidate.id] = outer;
    const bool is_outer = outer == candidate.id;
    if (!is_outer && outer.empty()) {
      continue;
    }
    const bool open = is_outer ? (expanded ? std::any_of(expanded->elements().begin(), expanded->elements().end(),
        [&](const auto &item) { return item->as_string_value() && item->as_string_value()->value() == candidate.id; }) : sections->elements().is_empty()) : open_panels.count(outer) != 0;
    if (open) {
      open_panels.insert(outer);
    }
    std::string panel_title = pt.label;
    if (candidate.instance && !pt.parent) {
      if (PointerRNA *data = panel_custom_data_get(candidate.instance)) {
        char *name = RNA_struct_name_get_alloc(data, nullptr, 0, nullptr);
        if (name) {
          panel_title = name;
          MEM_delete(name);
        }
      }
    }
    if (is_outer) {
      auto section = sections->append_dict();
      section->append_str("id", candidate.id);
      section->append_str("title", panel_title);
      section->append_int("open", open);
      outer_fields[outer] = section->append_array("properties");
    }
    auto fields = outer_fields[outer];
    if (!fields) {
      continue;
    }
    auto append_field = [&]() {
      auto field = fields->append_dict();
      if (!is_outer) {
        field->append_str("section", panel_title);
      }
      return field;
    };
    const bool target_panel = requested_panel && *requested_panel == candidate.id;
    const bool instance_container = !candidate.instance &&
        ELEM(tab_id, "MODIFIER", "CONSTRAINT", "BONE_CONSTRAINT", "SHADERFX");
    if ((!execute && !open && !is_outer && !instance_container) ||
        (execute && !target_panel && !instance_container)) {
      continue;
    }
    Block *block = block_begin(C, nullptr, "native_properties", EmbossType::Emboss);
    Panel *panel = candidate.instance ? candidate.instance : BKE_panel_new(&pt);
    block->panel = panel;
    Layout &layout = block_layout(block, LayoutDirection::Vertical, LayoutType::Panel,
                                   0, 0, 320, 20, 0, style_get_dpi());
    panel->layout = &layout;
    layout.operator_context_set(wm::OpCallContext::ExecDefault);
    if (pt.draw_header) {
      pt.draw_header(C, panel);
    }
    std::set<Button *> header_buttons;
    for (const auto &button : block->buttons) {
      header_buttons.insert(button.get());
    }
    if (open || execute || instance_container) {
      /* Body activation must never disable its own enable switch. */
      Layout &body = block_layout(block, LayoutDirection::Vertical, LayoutType::Panel,
                                  0, 0, 320, 20, 0, style_get_dpi());
      body.operator_context_set(wm::OpCallContext::ExecDefault);
      panel->layout = &body;
      pt.draw(C, panel);
    }
    block_end(C, block);
    std::set<std::string> seen;
    for (const auto &button : block->buttons) {
      Button &but = *button;
      const bool header_toggle = header_buttons.count(&but) && but.rnaprop &&
                                RNA_property_type(but.rnaprop) == PROP_BOOLEAN &&
                                !RNA_property_array_check(but.rnaprop);
      if (!execute && !open && !header_toggle) {
        continue;
      }
      if (!execute) {
        if (MenuType *menu = button_menutype_get(&but)) {
          auto field = append_field();
          field->append_str("type", "MENU");
          field->append_str("label", but.str.empty() ? menu->label : but.str);
          field->append_str("key", menu->idname);
          field->append_str("menu", menu->idname);
          field->append_str("tab", tab_id);
          field->append_str("icon", icon_name(button_icon(&but)));
          field->append_int("editable", !(but.flag & (BUT_DISABLED | BUT_INACTIVE)));
          continue;
        }
      }
      if (but.optype && !(but.flag & UI_HIDDEN)) {
        const std::string key = std::string(but.optype->idname) + ":" +
            (but.opptr ? RNA_pointer_as_string_keywords(C, but.opptr, false, false, true, INT_MAX) : "") + ":" +
            std::to_string(object ? object->id.session_uid : 0);
        if (execute && !operator_type && target_panel && requested_key && *requested_key == key &&
            !(but.flag & (BUT_DISABLED | BUT_INACTIVE)) && but.optype->exec) {
          operator_type = but.optype;
          operator_properties = but.opptr;
          but.opptr = nullptr;
          if (but.context) {
            operator_context = std::make_unique<bContextStore>(*but.context);
          }
        }
        else if (!execute && seen.insert(key).second) {
          auto field = append_field();
          field->append_str("type", "OPERATOR");
          field->append_str("label", but.str.empty() ? WM_operatortype_name(but.optype, but.opptr) : but.str);
          field->append_str("key", key);
          field->append_str("icon", icon_name(button_icon(&but)));
          field->append_int("editable", !(but.flag & (BUT_DISABLED | BUT_INACTIVE)) && but.optype->exec);
          field->append_str("panel", candidate.id);
          field->append_str("tab", tab_id);
          field->append_int("native", 1);
        }
        continue;
      }
      if (!but.rnaprop || !but.rnapoin.data || (but.flag & UI_HIDDEN)) {
        continue;
      }
      if (!ELEM(RNA_property_type(but.rnaprop), PROP_BOOLEAN, PROP_INT, PROP_FLOAT, PROP_STRING, PROP_ENUM, PROP_POINTER) ||
          (RNA_property_type(but.rnaprop) == PROP_POINTER &&
           !RNA_struct_is_ID(RNA_property_pointer_type(&but.rnapoin, but.rnaprop)))) {
        continue;
      }
      const int length = RNA_property_array_length(&but.rnapoin, but.rnaprop);
      const bool color = is_color(&but.rnapoin, but.rnaprop);
      const int begin = !color && length ? std::max(0, but.rnaindex) : -1;
      const int end = !color && length && but.rnaindex < 0 ? std::min(length, 16) : begin + 1;
      for (int index = begin; index < end; index++) {
        const std::string key = property_key(but, index);
        if (!seen.insert(key).second) {
          continue;
        }
        if (execute) {
          if (target_panel && requested_key && *requested_key == key) {
            applied = set_property(C, but, index, *query);
          }
        }
        else {
          auto field = append_field();
          project_property(C, but, index, *field);
          if (header_toggle) {
            field->append_int("header", 1);
          }
          field->append_str("panel", candidate.id);
          field->append_str("tab", tab_id);
          field->append_int("native", 1);
        }
      }
    }
    panel->layout = nullptr;
    block->panel = nullptr;
    block_free(nullptr, block);
    if (!candidate.instance) {
      BKE_panel_free(panel);
    }
    auto add_instance = [&](auto &&self, Panel *instance, const std::string &parent) -> void {
      if (!instance->type || !instances.insert(instance).second) {
        return;
      }
      PointerRNA *data = panel_custom_data_get(instance);
      const auto path = data ? RNA_path_from_ID_to_struct(data) : std::nullopt;
      const std::string id = std::string(instance->type->idname) + ":" + path.value_or("");
      candidates.push_back({instance->type, instance, id, parent});
      for (Panel &child : instance->children) {
        self(self, &child, id);
      }
    };
    if (!candidate.instance) {
      for (Panel &instance : context.region->panels) {
        add_instance(add_instance, &instance, "");
      }
    }
    if (execute && target_panel) {
      break;
    }
  }
  if (operator_type) {
    const bContextStore *previous = CTX_store_get(C);
    CTX_store_set(C, operator_context.get());
    if (WM_operator_poll_context(C, operator_type, wm::OpCallContext::ExecDefault)) {
      applied = WM_operator_name_call_ptr(C, operator_type, wm::OpCallContext::ExecDefault,
                                         operator_properties, nullptr) & OPERATOR_FINISHED;
    }
    CTX_store_set(C, previous);
    if (operator_properties) {
      WM_operator_properties_free(operator_properties);
      MEM_delete(operator_properties);
    }
  }
  if (execute) {
    return applied ? "" : "This property is no longer available or editable.";
  }
  return encode(result);
}

std::string native_properties_menu_query(bContext *C, const std::string &json, bool execute)
{
  auto value = decode(json);
  const auto *request = value ? value->as_dictionary_value() : nullptr;
  if (!request) {
    return "[]";
  }
  const auto tab = request->lookup_str("propertiesTab");
  if (!tab) {
    return native_menu_query(C, json, execute);
  }
  PropertiesContext context(C);
  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, RNA_SpaceProperties, context.space);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "context");
  const EnumPropertyItem *items;
  int count, selected;
  bool free;
  RNA_property_enum_items(C, &ptr, prop, &items, &count, &free);
  if (tab && RNA_enum_value_from_identifier(items, tab->c_str(), &selected)) {
    context.space->mainbuser = selected;
    ED_buttons_native_context_prepare(C, context.space);
  }
  if (free) {
    MEM_delete(items);
  }
  const std::string menu_json = native_menu_query(C, json, execute);
  auto menu_value = decode(menu_json);
  if (tab && menu_value && menu_value->as_array_value()) {
    for (auto &item : menu_value->as_array_value()->elements()) {
      if (auto *entry = item->as_dictionary_value()) {
        if (auto *route = entry->lookup("route")) {
          if ((*route)->as_dictionary_value()) {
            static_cast<DictionaryValue *>(route->get())->append_str("propertiesTab", *tab);
          }
        }
      }
    }
    return encode(*menu_value);
  }
  return menu_json;
}
}  // namespace blender::ui
