/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Native Properties projects the registered Blender panels and their RNA controls. */
#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BLI_listbase.h"
#include "BLI_serialize.hh"
#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"
#include "DNA_workspace_types.h"
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
#include "WM_toolsystem.hh"
#include "interface_intern.hh"
#include "wm_event_system.hh"

#include <algorithm>
#include <chrono>
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
    /* Instanced modifier/constraint panels can start window-level animation handlers.
     * Retire those handlers and timers before freeing their temporary panels. */
    panels_free_instanced(C, region);
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

struct NativePropertyInteraction {
  std::string token;
  std::string undo_name;
  std::string closed_token;
};
NativePropertyInteraction native_property_interaction;

void finish_property_interaction(bContext *C)
{
  auto &interaction = native_property_interaction;
  if (interaction.token.empty()) {
    return;
  }
  if (!interaction.undo_name.empty()) {
    ED_undo_push(C, interaction.undo_name.c_str());
  }
  interaction.closed_token = std::move(interaction.token);
  interaction.token.clear();
  interaction.undo_name.clear();
}

bool set_property(bContext *C,
                  Button &but,
                  int index,
                  const DictionaryValue &request,
                  const bool push_undo = true)
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
  if (push_undo) {
    const bool undo = ptr->owner_id && RNA_struct_undo_check(ptr->type);
    const auto phase = request.lookup_str("phase").value_or("");
    const std::string token = request.lookup_str("interaction").value_or("");
    if (!token.empty() && phase == "update") {
      native_property_interaction.token = token;
      native_property_interaction.undo_name = undo ? RNA_property_ui_name(prop) : "";
    }
    else {
      if (undo && phase != "cancel") {
        ED_undo_push(C, RNA_property_ui_name(prop));
      }
      if (!token.empty()) {
        native_property_interaction.closed_token = token;
        native_property_interaction.token.clear();
        native_property_interaction.undo_name.clear();
      }
    }
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return true;
}

struct PropertyAnimationAction {
  std::string id;
  std::string title;
  const char *operator_id;
  const char *icon;
  bool all;
};

std::vector<PropertyAnimationAction> property_animation_actions(bContext *C, Button &but, int index)
{
  if (!RNA_property_anim_editable(&but.rnapoin, but.rnaprop) ||
      !RNA_property_path_from_ID_check(&but.rnapoin, but.rnaprop) ||
      (but.flag & (BUT_DISABLED | BUT_INACTIVE))) {
    return {};
  }
  const int old_index = but.rnaindex, old_flag = but.flag, old_drawflag = but.drawflag;
  but.rnaindex = index;
  const AnimationEvalContext eval = BKE_animsys_eval_context_construct(
      CTX_data_depsgraph_pointer(C), BKE_scene_frame_get(CTX_data_scene(C)));
  button_anim_flag(&but, &eval);
  const bool keyed = but.flag & BUT_ANIMATED_KEY;
  const bool animated = but.flag & BUT_ANIMATED;
  const bool driven = but.flag & BUT_DRIVEN;
  but.rnaindex = old_index;
  but.flag = old_flag;
  but.drawflag = old_drawflag;
  if (driven) {
    return {};
  }
  const bool component = RNA_property_array_check(but.rnaprop) && index >= 0;
  std::vector<PropertyAnimationAction> actions;
  auto add = [&](const char *id, const char *verb, const char *operator_id, const char *icon) {
    actions.push_back({std::string(id) + "_all",
                       std::string(verb) + (component || std::string(id) == "clear" ? " Keyframes" : " Keyframe"),
                       operator_id, icon, true});
    if (component) {
      actions.push_back({std::string(id) + "_single",
                         std::string(verb) + (std::string(id) == "clear" ? " Single Keyframes" : " Single Keyframe"),
                         operator_id, icon, false});
    }
  };
  add("insert", keyed ? "Replace" : "Insert", "ANIM_OT_keyframe_insert_button", "KEY_HLT");
  if (keyed) {
    add("delete", "Delete", "ANIM_OT_keyframe_delete_button", "KEY_DEHLT");
  }
  if (animated && but.rnapoin.type != RNA_NlaStrip) {
    add("clear", "Clear", "ANIM_OT_keyframe_clear_button", "KEY_DEHLT");
  }
  return actions;
}

bool animate_property(bContext *C, Button &but, int index, const std::string &action_id)
{
  for (const PropertyAnimationAction &action : property_animation_actions(C, but, index)) {
    if (action.id != action_id) {
      continue;
    }
    /* Reuse Blender's button operators, with this detached control as their active property. */
    ARegion *region = CTX_wm_region(C);
    ARegion *popup = CTX_wm_region_popup(C);
    const bContextStore *store = CTX_store_get(C);
    const int old_index = but.rnaindex, old_flag = but.flag;
    but.rnaindex = index;
    but.flag |= BUT_ACTIVE_OVERRIDE;
    BLI_addtail(&region->runtime->uiblocks, but.block);
    CTX_wm_region_popup_set(C, nullptr);
    if (but.context) {
      CTX_store_set(C, but.context);
    }
    PointerRNA properties = WM_operator_properties_create(action.operator_id);
    RNA_boolean_set(&properties, "all", action.all);
    const bool changed = WM_operator_name_call(C, action.operator_id, wm::OpCallContext::ExecDefault,
                                               &properties, nullptr) & OPERATOR_FINISHED;
    WM_operator_properties_free(&properties);
    CTX_store_set(C, store);
    CTX_wm_region_popup_set(C, popup);
    BLI_remlink(&region->runtime->uiblocks, but.block);
    but.rnaindex = old_index;
    but.flag = old_flag;
    return changed;
  }
  return false;
}

struct PropertyPair {
  std::string key;
  std::string title;
  std::string component;
};

std::map<Button *, PropertyPair> property_pairs(const Block &block)
{
  std::map<Button *, PropertyPair> pairs;
  Button *previous = nullptr;
  for (const auto &button : block.buttons) {
    Button &but = *button;
    if (!but.rnaprop || !but.rnapoin.data || (but.flag & UI_HIDDEN) ||
        button_is_decorator(&but)) {
      continue;
    }
    const bool scalar = ELEM(RNA_property_type(but.rnaprop), PROP_INT, PROP_FLOAT) &&
                        !RNA_property_array_check(but.rnaprop);
    if (scalar && previous && previous->rnapoin.data == but.rnapoin.data &&
        previous->rnapoin.type == but.rnapoin.type &&
        RNA_property_type(previous->rnaprop) == RNA_property_type(but.rnaprop)) {
      const std::string first = RNA_property_identifier(previous->rnaprop);
      const std::string second = RNA_property_identifier(but.rnaprop);
      for (const auto &suffix : {std::pair{"_x", "_y"}, {"_u", "_v"},
                                 {"_min", "_max"}, {"_start", "_end"}}) {
        if (!StringRef(first).endswith(suffix.first)) {
          continue;
        }
        const std::string stem = first.substr(0, first.size() - strlen(suffix.first));
        if (second != stem + suffix.second) {
          continue;
        }
        /* Separate scalar properties only form a pair when Blender groups them in an
         * aligned layout. Capture this before block_end() frees the layout tree. */
        bool aligned = false;
        for (Layout *a = previous->layout; a && !aligned; a = a->parent()) {
          if (!a->align()) {
            continue;
          }
          for (Layout *b = but.layout; b; b = b->parent()) {
            if (a == b) {
              aligned = true;
              break;
            }
          }
        }
        const std::string first_name = RNA_property_ui_name(previous->rnaprop);
        const std::string second_name = RNA_property_ui_name(but.rnaprop);
        size_t common = 0;
        while (common < first_name.size() && common < second_name.size() &&
               first_name[common] == second_name[common]) {
          common++;
        }
        /* Split at a word boundary, not midway through a translated label. */
        while (common && first_name[common - 1] != ' ') {
          common--;
        }
        if (aligned && common > 1 && common < first_name.size() && common < second_name.size()) {
          const std::string key = property_key(*previous, -1);
          const std::string title = first_name.substr(0, common - 1);
          pairs[previous] = {key, title, first_name.substr(common)};
          pairs[&but] = {key, title, second_name.substr(common)};
        }
        break;
      }
    }
    previous = scalar && !pairs.count(&but) ? &but : nullptr;
  }
  return pairs;
}

void project_property(bContext *C, Button &but, int index, DictionaryValue &field,
                      const PropertyPair *pair = nullptr)
{
  PointerRNA *ptr = &but.rnapoin;
  PropertyRNA *prop = but.rnaprop;
  const bool array = RNA_property_array_check(prop);
  const int length = RNA_property_array_length(ptr, prop);
  const PropertyType type = RNA_property_type(prop);
  const auto actions = property_animation_actions(C, but, index);
  if (!actions.empty()) {
    auto animation = field.append_array("animation");
    for (const PropertyAnimationAction &action : actions) {
      auto item = animation->append_dict();
      item->append_str("id", action.id);
      item->append_str("title", action.title);
      item->append_str("icon", action.icon);
    }
  }
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
  if (pair) {
    title = pair->component;
    field.append_str("group", pair->title);
    field.append_str("group_key", pair->key);
  }
  else if (array && length <= 4) {
    const bool color = ELEM(RNA_property_subtype(prop), PROP_COLOR, PROP_COLOR_GAMMA);
    const char *axes = color ? "RGBA" : length == 4 ? "WXYZ" : "XYZW";
    title = std::string(1, axes[index]);
    field.append_str("group", ELEM(std::string(RNA_property_identifier(prop)),
                                   "rotation_euler", "rotation_quaternion", "rotation_axis_angle") ?
                                  "Rotation" : RNA_property_ui_name(prop));
    field.append_str("group_key", property_key(but, -1));
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
  if (but.type == ButtonType::Num && ELEM(type, PROP_INT, PROP_FLOAT)) {
    const double step = type == PROP_INT ? 0.5 :
        0.01 * static_cast<ButtonNumber &>(but).step_size;
    field.append_double("dragStep", std::isfinite(step) && step > 0 ? step : 0.01);
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

namespace {
/* Engine-owned identity, invalidated before the operator's memory can be reused. */
wmOperator *native_redo_operator = nullptr;
uint64_t native_redo_generation = 0;
bool native_redo_dismissed = false;
int64_t native_redo_ack = 0;
std::string native_redo_error;
std::string native_redo_snapshot = "null";
std::chrono::steady_clock::time_point native_redo_next_snapshot;
}  // namespace

void native_properties_interaction_finish(bContext *C)
{
  finish_property_interaction(C);
}

void native_redo_operator_register(bContext *C, wmOperator *op)
{
  if ((op->type->flag & (OPTYPE_REGISTER | OPTYPE_UNDO)) != (OPTYPE_REGISTER | OPTYPE_UNDO)) {
    return;
  }
  native_redo_operator = CTX_wm_view3d(C) ? op : nullptr;
  ++native_redo_generation;
  native_redo_dismissed = false;
  native_redo_error.clear();
  native_redo_next_snapshot = {};
}

void native_redo_operator_free(wmOperator *op)
{
  if (native_redo_operator == op) {
    native_redo_operator = nullptr;
    native_redo_next_snapshot = {};
  }
}

void native_redo_operator_clear()
{
  native_redo_operator = nullptr;
  native_redo_next_snapshot = {};
}

std::string native_operator_redo_query(bContext *C, const std::string &json, bool execute)
{
  wmOperator *op = WM_operator_last_redo(C);
  if (!op || op != native_redo_operator || native_redo_dismissed || !CTX_wm_view3d(C) ||
      !CTX_wm_region(C))
  {
    return "null";
  }
  auto query_value = execute ? decode(json) : nullptr;
  const auto *query = query_value ? query_value->as_dictionary_value() : nullptr;
  const std::string identity = std::to_string(native_redo_generation);
  if (execute) {
    if (!query || query->lookup_str("id").value_or("") != identity) {
      return "null";
    }
    native_redo_ack = query->lookup_int("sequence").value_or(native_redo_ack);
    native_redo_next_snapshot = {};
    native_redo_error.clear();
    if (query->lookup_str("action").value_or("") == "dismiss") {
      native_redo_dismissed = true;
      return "null";
    }
  }
  bool enabled = !G.moving && WM_operator_repeat_check(C, op) && WM_operator_poll(C, op->type) &&
                 WM_operator_check_ui_enabled(C, op->type->name);
  if (wmWindow *window = CTX_wm_window(C)) {
    for (wmEventHandler &handler : window->runtime->modalhandlers) {
      if (handler.type == WM_HANDLER_TYPE_OP || handler.type == WM_HANDLER_TYPE_UI) {
        enabled = false;
        break;
      }
    }
  }
  if (!enabled) {
    return "null";
  }
  const auto now = std::chrono::steady_clock::now();
  if (!execute && now < native_redo_next_snapshot) {
    return native_redo_snapshot;
  }
  DictionaryValue result;
  result.append_str("id", identity);
  result.append_str("title", WM_operatortype_name(op->type, op->ptr));
  result.append_str("operator", op->type->idname);
  result.append_int("ack", native_redo_ack);
  auto fields = result.append_array("properties");
  Block *block = block_begin(C, nullptr, "native_operator_redo", EmbossType::Emboss);
  Layout &layout = block_layout(
      block, LayoutDirection::Vertical, LayoutType::Panel, 0, 0, 320, 20, 0, style_get_dpi());
  /* Uses the actual operator UI, including macros and poll_property visibility. */
  template_operator_redo_properties(&layout, C);
  const auto pairs = property_pairs(*block);
  block_end(C, block);
  std::set<std::string> seen;
  bool applied = false;
  for (const auto &button : block->buttons) {
    Button &but = *button;
    if (!but.rnaprop || !but.rnapoin.data || (but.flag & UI_HIDDEN) ||
        !RNA_struct_is_a(but.rnapoin.type, RNA_OperatorProperties) ||
        !ELEM(RNA_property_type(but.rnaprop),
              PROP_BOOLEAN,
              PROP_INT,
              PROP_FLOAT,
              PROP_STRING,
              PROP_ENUM))
    {
      continue;
    }
    const int length = RNA_property_array_length(&but.rnapoin, but.rnaprop);
    const bool color = is_color(&but.rnapoin, but.rnaprop);
    const int begin = !color && length ? std::max(0, but.rnaindex) : -1;
    const int end = !color && length && but.rnaindex < 0 ? length : begin + 1;
    for (int index = begin; index < end; ++index) {
      const std::string key = property_key(but, index);
      if (!seen.insert(key).second) {
        continue;
      }
      if (execute && query->lookup_str("key").value_or("") == key) {
        applied = set_property(C, but, index, *query, false);
      }
      else if (!execute) {
        auto field = fields->append_dict();
        const auto pair = pairs.find(&but);
        project_property(C, but, index, *field, pair == pairs.end() ? nullptr : &pair->second);
      }
    }
  }
  block_free(C, block);
  if (execute) {
    if (!applied || !ED_undo_operator_repeat(C, op)) {
      native_redo_error = "This operation could not be adjusted.";
    }
    /* Rebuild from the executed operator, whose check callback may change its UI. */
    return native_operator_redo_query(C, "", false);
  }
  result.append_str("error", native_redo_error);
  native_redo_snapshot = fields->elements().is_empty() ? "null" : encode(result);
  native_redo_next_snapshot = now + std::chrono::milliseconds(200);
  return native_redo_snapshot;
}

std::string native_properties_query(bContext *C, const std::string &json, bool execute)
{
  DictionaryValue result;
  auto query_value = decode(json);
  const auto *query = query_value ? query_value->as_dictionary_value() : nullptr;
  if (!query || !CTX_data_scene(C)) {
    return "{}";
  }
  if (execute) {
    const std::string token = query->lookup_str("interaction").value_or("");
    if (!token.empty() && token == native_property_interaction.closed_token) {
      return "";
    }
    if (token != native_property_interaction.token) {
      finish_property_interaction(C);
    }
    if (query->lookup_str("phase").value_or("") == "cancel" &&
        native_property_interaction.token.empty()) {
      return "";
    }
  }
  const bool shading = query->lookup_str("tab").value_or("") == "SHADING";
  const bool tool_settings = query->lookup_str("tab").value_or("") == "TOOL";
  const bool viewport = shading || tool_settings;
  std::unique_ptr<PropertiesContext> properties_context;
  if (!viewport) {
    properties_context = std::make_unique<PropertiesContext>(C);
  }
  else if (!CTX_wm_view3d(C)) {
    return "{}";
  }
  ARegion *panel_region = viewport ? CTX_wm_region(C) : properties_context->region;
  ARegionType *panel_region_type = viewport ?
      BKE_regiontype_from_id(CTX_wm_area(C)->type, tool_settings ? RGN_TYPE_UI : RGN_TYPE_HEADER) :
      panel_region->runtime->type;
  std::string tab_id = tool_settings ? "TOOL" : shading ? "SHADING" : "OBJECT";
  auto tab_items = result.append_array("tabs");
  if (!viewport) {
    PropertiesContext &context = *properties_context;
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
  }
  std::string panel_context = tab_id;
  std::transform(panel_context.begin(), panel_context.end(), panel_context.begin(), ::tolower);
  if (panel_context == "particles") {
    panel_context = "particle";
  }
  result.append_str("tab", tab_id);
  result.append_str("name", tool_settings ? "Tool Settings" : shading ? "Viewport Shading" : "Properties");
  Object *object = CTX_data_active_object(C);
  std::string identity = tab_id + ":" + std::to_string(object ? object->id.session_uid : 0);
  if (tool_settings) {
    const bToolRef *tool = WM_toolsystem_ref_from_context(C);
    if (!tool) { return execute ? "The active tool is no longer available." : "{}"; }
    identity += ":" + std::string(tool->idname) + ":" + CTX_data_mode_string(C);
    result.append_str("tool", tool->idname);
    if (execute && query->lookup_str("id").value_or("") != identity) {
      return "The active tool has changed.";
    }
  }
  result.append_str("id", identity);
  auto sections = result.append_array("sections");
  const ArrayValue *expanded = query->lookup_array("expanded");
  /* Sub-panel disclosure overrides keyed by panel id; absent ids use the panel type default. */
  const DictionaryValue *subpanel_states = query->lookup_dict("subpanels");
  const auto requested_panel = query->lookup_str("panel");
  const auto requested_key = query->lookup_str("key");
  bool applied = false;
  wmOperatorType *operator_type = nullptr;
  PointerRNA *operator_properties = nullptr;
  std::unique_ptr<bContextStore> operator_context;
  std::set<std::string> open_panels;
  /* Whether each candidate's body is visible, so nested sub-panels follow their parent. */
  std::map<std::string, bool> visible_panels;
  std::map<std::string, int> panel_depths;
  std::map<std::string, std::string> outer_panels;
  std::map<std::string, std::shared_ptr<ArrayValue>> outer_fields;
  struct Candidate {
    PanelType *type;
    Panel *instance;
    std::string id;
    std::string parent;
    bool enabled = true;
  };
  std::vector<Candidate> candidates;
  std::set<Panel *> instances;
  for (PanelType &pt : panel_region_type->paneltypes) {
    if (tool_settings && !STREQ(pt.idname, "VIEW3D_PT_active_tool") &&
        !STREQ(pt.idname, "VIEW3D_PT_native_sculpt_settings"))
    {
      continue;
    }
    if (shading && !STREQ(pt.idname, "VIEW3D_PT_shading") &&
        !STREQ(pt.parent_id, "VIEW3D_PT_shading")) {
      continue;
    }
    if (!(pt.flag & PANEL_TYPE_INSTANCED)) {
      candidates.push_back({&pt, nullptr, pt.idname, shading ? "" : pt.parent_id});
    }
  }
  for (size_t candidate_index = 0; candidate_index < candidates.size(); candidate_index++) {
    const Candidate candidate = candidates[candidate_index];
    PanelType &pt = *candidate.type;
    if (!pt.draw || (!viewport && !candidate.instance && panel_context != pt.context) ||
        ((pt.flag & PANEL_TYPE_NO_HEADER) && !pt.label[0]) || !panel_poll(C, &pt)) {
      continue;
    }
    const std::string outer = candidate.parent.empty() ? candidate.id : outer_panels[candidate.parent];
    outer_panels[candidate.id] = outer;
    const bool is_outer = outer == candidate.id;
    if (!is_outer && outer.empty()) {
      continue;
    }
    /* Properties tabs give every sub-panel its own disclosure; viewport popovers keep them
     * flattened into the outer panel. */
    const bool collapsible_subpanel = !is_outer && !viewport;
    if (collapsible_subpanel && !execute && !visible_panels[candidate.parent]) {
      continue;
    }
    bool subpanel_open = !(pt.flag & PANEL_TYPE_DEFAULT_CLOSED);
    if (collapsible_subpanel && subpanel_states) {
      if (const std::shared_ptr<Value> *state = subpanel_states->lookup(candidate.id)) {
        if (const BooleanValue *flag = (*state)->as_boolean_value()) {
          subpanel_open = flag->value();
        }
      }
    }
    const bool open = is_outer ? (expanded ? std::any_of(expanded->elements().begin(), expanded->elements().end(),
        [&](const auto &item) { return item->as_string_value() && item->as_string_value()->value() == candidate.id; }) : viewport || sections->elements().is_empty()) :
        open_panels.count(outer) != 0 && (!collapsible_subpanel || subpanel_open);
    if (open && is_outer) {
      open_panels.insert(outer);
    }
    visible_panels[candidate.id] = open;
    panel_depths[candidate.id] = is_outer ? 0 : panel_depths[candidate.parent] + 1;
    std::string panel_title = pt.label;
    bool panel_active = false;
    if (candidate.instance && !pt.parent) {
      if (PointerRNA *data = panel_custom_data_get(candidate.instance)) {
        char *name = RNA_struct_name_get_alloc(data, nullptr, 0, nullptr);
        if (name) {
          panel_title = name;
          MEM_delete(name);
        }
        /* Modifiers (is_active) and constraints (active) mark the stack's active item. */
        for (const char *identifier : {"is_active", "active"}) {
          PropertyRNA *prop = RNA_struct_find_property(data, identifier);
          if (prop && RNA_property_type(prop) == PROP_BOOLEAN) {
            panel_active = RNA_property_boolean_get(data, prop);
            break;
          }
        }
      }
    }
    if (is_outer) {
      auto section = sections->append_dict();
      section->append_str("id", candidate.id);
      section->append_str("title", panel_title);
      section->append_int("open", open);
      if (panel_active) {
        section->append_int("active", 1);
      }
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
        field->append_str("subpanel", candidate.id);
      }
      return field;
    };
    if (collapsible_subpanel && !execute) {
      /* Disclosure row for the sub-panel, emitted even when collapsed so it can be reopened. */
      auto field = append_field();
      field->append_str("type", "SUBPANEL");
      field->append_int("open", open);
      field->append_int("depth", panel_depths[candidate.id]);
    }
    const bool target_panel = requested_panel && *requested_panel == candidate.id;
    const bool instance_container = !candidate.instance &&
        ELEM(tab_id, "MODIFIER", "CONSTRAINT", "BONE_CONSTRAINT", "SHADERFX");
    if ((!execute && !open && !is_outer && !instance_container && !collapsible_subpanel) ||
        (execute && !target_panel && !instance_container && !shading)) {
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
      if (shading && candidate.id == "VIEW3D_PT_shading") {
        PointerRNA ptr = RNA_pointer_create_discrete(
            &CTX_wm_screen(C)->id, RNA_View3DShading, &CTX_wm_view3d(C)->shading);
        body.prop(&ptr, "type", eUI_Item_Flag(0), "Shading", ICON_NONE);
      }
    }
    const auto pairs = property_pairs(*block);
    std::map<Button *, std::string> tool_labels;
    if (tool_settings) {
      /* Split property layouts put the authored caption in a preceding label. An enum
       * button's own text is its selected value, not the property caption. */
      Button *previous = nullptr;
      for (const auto &button : block->buttons) {
        if (button->rnaprop && !RNA_property_array_check(button->rnaprop)) {
          if (previous && previous->type == ButtonType::Label &&
              (previous->drawflag & BUT_TEXT_RIGHT)) {
            tool_labels[button.get()] = previous->str;
          }
          else if (RNA_property_type(button->rnaprop) != PROP_ENUM && !button->str.empty()) {
            tool_labels[button.get()] = button->str;
          }
        }
        previous = button.get();
      }
    }
    block_end(C, block);
    for (const auto &button : block->buttons) {
      /* Icon templates keep their RNA target in callback data, not in the button itself. */
      if (template_icon_view_property_get(*button, button->rnapoin, button->rnaprop)) {
        button->rnaindex = -1;
      }
    }
    if (shading) {
      for (const auto &button : block->buttons) {
        if (!candidate.enabled) { button->flag |= BUT_DISABLED; }
        PanelType *nested = button_paneltype_get(button.get());
        if (nested && std::none_of(candidates.begin(), candidates.end(), [&](const Candidate &item) {
              return item.id == nested->idname;
            })) {
          candidates.push_back({nested, nullptr, nested->idname, "",
                                !(button->flag & (BUT_DISABLED | BUT_INACTIVE))});
        }
      }
    }
    /* Only replace locks which have a corresponding value in this actual panel layout. */
    std::map<std::string, std::pair<Button *, int>> field_locks;
    std::set<std::string> attached_locks;
    std::map<std::string, Button *> field_modes;
    std::set<std::string> attached_modes;
    for (const auto &value_button : block->buttons) {
      Button &value = *value_button;
      if (!value.rnaprop || !value.rnapoin.data || (value.flag & UI_HIDDEN) ||
          !ELEM(RNA_property_type(value.rnaprop), PROP_FLOAT, PROP_INT, PROP_STRING) ||
          is_color(&value.rnapoin, value.rnaprop)) {
        continue;
      }
      const std::string identifier = RNA_property_identifier(value.rnaprop);
      const int length = RNA_property_array_length(&value.rnapoin, value.rnaprop);
      const int begin = length ? std::max(0, value.rnaindex) : -1;
      const int end = length && value.rnaindex < 0 ? std::min(length, 16) : begin + 1;
      for (int index = begin; index < end; index++) {
        if (ELEM(identifier, "rotation_euler", "rotation_quaternion", "rotation_axis_angle")) {
          for (const auto &mode_button : block->buttons) {
            Button &mode = *mode_button;
            if (mode.rnaprop && mode.rnapoin.data == value.rnapoin.data &&
                mode.rnapoin.type == value.rnapoin.type && !(mode.flag & UI_HIDDEN) &&
                RNA_property_type(mode.rnaprop) == PROP_ENUM &&
                std::string(RNA_property_identifier(mode.rnaprop)) == "rotation_mode") {
              field_modes[property_key(value, index)] = &mode;
              attached_modes.insert(property_key(mode, -1));
              break;
            }
          }
        }
        std::string lock_name = "lock_" + identifier;
        int lock_index = index;
        if (identifier == "rotation_euler") {
          lock_name = "lock_rotation";
        }
        else if (ELEM(identifier, "rotation_quaternion", "rotation_axis_angle")) {
          lock_name = index == 0 ? "lock_rotation_w" : "lock_rotation";
          lock_index = index - 1;
        }
        for (const auto &lock_button : block->buttons) {
          Button &lock = *lock_button;
          if (!lock.rnaprop || lock.rnapoin.data != value.rnapoin.data ||
              lock.rnapoin.type != value.rnapoin.type || (lock.flag & UI_HIDDEN) ||
              RNA_property_type(lock.rnaprop) != PROP_BOOLEAN ||
              lock_name != RNA_property_identifier(lock.rnaprop)) {
            continue;
          }
          const int lock_length = RNA_property_array_length(&lock.rnapoin, lock.rnaprop);
          const int component = lock_length ? lock_index : -1;
          if ((lock_length && (component < 0 || component >= lock_length)) ||
              (lock_length && lock.rnaindex >= 0 && lock.rnaindex != component)) {
            continue;
          }
          field_locks[property_key(value, index)] = {&lock, component};
          attached_locks.insert(property_key(lock, component));
          break;
        }
      }
    }
    std::set<std::string> seen;
    for (const auto &button : block->buttons) {
      Button &but = *button;
      /* Keep only viewport enablement in modifier headers. Other display options belong
       * inside the expanded native panel. */
      const bool header_operator = header_buttons.count(&but) && but.optype && !(but.flag & UI_HIDDEN);
      if (header_operator && StringRef(but.optype->idname).endswith("_set_active")) {
        /* Native headers show which item is active as state, not as a button. */
        continue;
      }
      const bool header_toggle = header_buttons.count(&but) && but.rnaprop &&
                                RNA_property_type(but.rnaprop) == PROP_BOOLEAN &&
                                !RNA_property_array_check(but.rnaprop) &&
                                (!RNA_struct_is_a(but.rnapoin.type, RNA_Modifier) ||
                                 STREQ(RNA_property_identifier(but.rnaprop), "show_viewport"));
      if (!execute && !open && !header_toggle && !header_operator) {
        continue;
      }
      MenuType *menu = button_menutype_get(&but);
      if (!menu && but.optype && but.opptr && STREQ(but.optype->idname, "WM_OT_call_menu")) {
        /* Some panel menus are expressed as operators, including Add Modifier. */
        const std::string name = RNA_string_get(but.opptr, "name");
        menu = WM_menutype_find(name.c_str(), true);
      }
      if (menu) {
        if (!execute && !(but.flag & UI_HIDDEN)) {
          auto field = append_field();
          field->append_str("type", "MENU");
          field->append_str("label", but.str.empty() ? menu->label : but.str);
          field->append_str("key", menu->idname);
          field->append_str("menu", menu->idname);
          field->append_str("tab", tab_id);
          field->append_str("icon", icon_name(button_icon(&but)));
          field->append_int("editable", !(but.flag & (BUT_DISABLED | BUT_INACTIVE)));
        }
        /* Menu activation belongs to UIKit, never to a Blender popup operator. */
        continue;
      }
      if (but.optype && !(but.flag & UI_HIDDEN)) {
        if (shading && STREQ(but.optype->idname, "SCREEN_OT_userpref_show")) {
          /* This opens an unrelated Blender window, not a viewport shading setting. */
          continue;
        }
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
          if (ELEM(button_icon(&but), ICON_CHECKBOX_HLT, ICON_CHECKBOX_DEHLT)) {
            /* A toggle operator drawn as a checkbox (e.g. Dyntopo): native UI shows a switch. */
            field->append_int("checkbox", 1);
            field->append_int("value", button_icon(&but) == ICON_CHECKBOX_HLT);
          }
          field->append_int("editable", !(but.flag & (BUT_DISABLED | BUT_INACTIVE)) && but.optype->exec);
          if (header_operator) {
            field->append_int("header", 1);
          }
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
            const auto animation_action = query->lookup_str("animation_action");
            applied = animation_action ? animate_property(C, but, index, *animation_action) :
                                         set_property(C, but, index, *query);
          }
        }
        else {
          if (attached_locks.count(key) || attached_modes.count(key)) {
            continue;
          }
          auto field = append_field();
          const auto pair = pairs.find(&but);
          project_property(C, but, index, *field, pair == pairs.end() ? nullptr : &pair->second);
          if (but.type == ButtonType::Row && RNA_property_type(but.rnaprop) == PROP_ENUM) {
            /* Authored as an expanded row (e.g. selection mode), not a dropdown. */
            field->append_int("expand", 1);
          }
          if (const auto found = tool_labels.find(&but); found != tool_labels.end()) {
            std::string label = found->second;
            while (!label.empty() && ELEM(label.back(), ':', ' ')) {
              label.pop_back();
            }
            field->append_str("toolbar_label", label);
          }
          if (const auto found = field_modes.find(key); found != field_modes.end()) {
            auto mode = field->append_dict("group_menu");
            project_property(C, *found->second, -1, *mode);
            mode->append_str("panel", candidate.id);
            mode->append_str("tab", tab_id);
            mode->append_int("native", 1);
          }
          if (const auto found = field_locks.find(key); found != field_locks.end()) {
            auto lock = field->append_dict("lock");
            project_property(C, *found->second.first, found->second.second, *lock);
            lock->append_str("panel", candidate.id);
            lock->append_str("tab", tab_id);
            lock->append_int("native", 1);
          }
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
    if (!candidate.instance && !viewport) {
      for (Panel &instance : panel_region->panels) {
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
  if (!tab || *tab == "TOOL") {
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
