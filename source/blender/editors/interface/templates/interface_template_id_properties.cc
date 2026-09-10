/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <fmt/format.h>

#include "BLI_listbase.hh"
#include "BLI_string.hh"
#include "BLI_string_utils.hh"

#include "BKE_idprop.hh"
#include "BLT_translation.hh"

#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_message.hh"

#include "interface_intern.hh"

namespace blender::ui::id_properties {

class IDPropertyView : public AbstractTreeView {
 public:
  const char *data_path_;
  IDPropertyView(PointerRNA *dataptr, const char *data_path)
      : data_path_(data_path), dataptr_(dataptr)
  {
    is_flat_ = true;
    user_properties_ = RNA_struct_idprops(dataptr_, false);
  }

  bool use_persistent_active_item() const override
  {
    return true;
  }

  void build_tree() override;

 private:
  PointerRNA *dataptr_;
  IDProperty *user_properties_;
};

struct DragDropData {
  IDProperty *user_properties_;
  IDProperty *idprop_;

  DragDropData() = default;
  DragDropData(IDProperty *user_properties, IDProperty *idprop)
      : user_properties_(user_properties), idprop_(idprop)
  {
  }
};

class IDPropertyDragController : public ui::AbstractViewItemDragController {
 private:
  DragDropData drag_data_;

 public:
  IDPropertyDragController(IDPropertyView &view, IDProperty *user_properties, IDProperty *idprop)
      : AbstractViewItemDragController(view), drag_data_(user_properties, idprop)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_IDPROPERTY;
  }

  void *create_drag_data() const override
  {
    DragDropData *drag_data = MEM_new_zeroed<DragDropData>(__func__);
    *drag_data = drag_data_;
    return drag_data;
  }
};

class IDPropertyDropTarget : public ui::TreeViewItemDropTarget {
 private:
  DragDropData drop_data_;

 public:
  IDPropertyDropTarget(ui::AbstractTreeViewItem &item,
                       ui::DropBehavior behavior,
                       IDProperty *user_properties,
                       IDProperty *idprop)
      : TreeViewItemDropTarget(item, behavior), drop_data_(user_properties, idprop)
  {
  }

  bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    if (drag.type != WM_DRAG_IDPROPERTY) {
      return false;
    }

    const DragDropData *drag_data = static_cast<DragDropData *>(drag.poin);
    return drag_data->user_properties_ == drop_data_.user_properties_;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const DragDropData *drag_data = static_cast<DragDropData *>(drag_info.drag_data.poin);
    const StringRef drag_name = drag_data->idprop_->name;
    const StringRef drop_name = drop_data_.idprop_->name;

    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        BLI_assert_unreachable();
        break;
      case ui::DropLocation::Before:
        return fmt::format(fmt::runtime(TIP_("Move {} above {}")), drag_name, drop_name);
      case ui::DropLocation::After:
        return fmt::format(fmt::runtime(TIP_("Move {} below {}")), drag_name, drop_name);
      default:
        BLI_assert_unreachable();
        break;
    }

    return "";
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    DragDropData *drag_data = static_cast<DragDropData *>(drag_info.drag_data.poin);
    ListBaseT<IDProperty> &idprop_list = drag_data->user_properties_->data.group;
    IDProperty *drag_idprop = drag_data->idprop_;
    IDProperty *drop_target = drop_data_.idprop_;

    BLI_remlink(&idprop_list, drag_idprop);

    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        BLI_assert_unreachable();
        break;
      case ui::DropLocation::Before:
        BLI_insertlinkafter(&idprop_list, drop_target->prev, drag_idprop);
        break;
      case ui::DropLocation::After:
        BLI_insertlinkbefore(&idprop_list, drop_target->next, drag_idprop);
        break;
      default:
        BLI_assert_unreachable();
        break;
    }

    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, nullptr);
    ED_undo_push(C, "Reorder ID Property");
    return true;
  }
};

class IDPropertyItem : public AbstractTreeViewItem {
 private:
  PointerRNA *dataptr_;
  IDProperty *user_properties_;
  IDProperty *idprop_;

 public:
  IDPropertyItem(PointerRNA *dataptr, IDProperty *idprop) : dataptr_(dataptr), idprop_(idprop)
  {
    label_ = idprop_->name;
    user_properties_ = RNA_struct_idprops(dataptr_, false);
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    user_properties_->data.children_map->children.remove_contained(idprop_);
    STRNCPY(idprop_->name, new_name.c_str());
    BLI_uniquename(&user_properties_->data.group,
                   idprop_,
                   "prop",
                   '.',
                   offsetof(IDProperty, name),
                   sizeof(idprop_->name));
    ED_undo_push(&const_cast<bContext &>(C), new_name.c_str());
    /* Update children map after rename, otherwise ends up with dirty idprop list. */
    user_properties_->data.children_map->children.add_new(idprop_);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, nullptr);
    return true;
  }

  void build_row(ui::Layout &row) override
  {
    uiItemL_ex(&row, idprop_->name, ICON_NONE, false, false);

    ui::Layout &sub = row.row(false);

    /* Use different emboss for widget style to color buttons when keyframe/drivers are present. */
    const EmbossType emboss = [&]() -> EmbossType {
      if (ELEM(idprop_->type, IDP_INT, IDP_FLOAT, IDP_DOUBLE)) {
        return EmbossType::NoneOrStatus;
      }
      return EmbossType::Emboss;
    }();

    sub.emboss_set(emboss);
    sub.alignment_set(idprop_->type == IDP_BOOLEAN ? LayoutAlign::Right : LayoutAlign::Expand);

    const std::string prop_name = "[\"" + BLI_str_escape(idprop_->name) + "\"]";

    const bool is_array = (idprop_->type == IDP_ARRAY) || !IDP_ui_data_supported(idprop_);
    const bool is_color = idprop_->ui_data &&
                          ELEM(idprop_->ui_data->rna_subtype, PROP_COLOR, PROP_COLOR_GAMMA);

    if (is_array && !is_color) {
      /* Use edit value operator to tweak array and python properties. */
      const IDPropertyView &view = static_cast<IDPropertyView &>(get_tree_view());
      PointerRNA op_ptr = sub.op("WM_OT_properties_edit_value", "Edit value", ICON_NONE);
      RNA_string_set(&op_ptr, "data_path", view.data_path_);
      RNA_string_set(&op_ptr, "property_name", idprop_->name);
    }
    else {
      sub.prop(dataptr_, prop_name, UI_ITEM_NONE, "", ICON_NONE);
    }
  }

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override
  {
    return std::make_unique<IDPropertyDragController>(
        static_cast<IDPropertyView &>(get_tree_view()), user_properties_, idprop_);
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    return std::make_unique<IDPropertyDropTarget>(
        *this, ui::DropBehavior::Reorder, user_properties_, idprop_);
  }
};

void IDPropertyView::build_tree()
{
  if (user_properties_ == nullptr) {
    return;
  }

  for (IDProperty &idprop : user_properties_->data.group) {
    this->add_tree_item<IDPropertyItem>(dataptr_, &idprop);
  }
}

std::string template_tree_idname(const StructRNA *type)
{
  /* The RNA name of the template (see `rna_ui_api.cc`), scripts & files rely on it. */
  return list_idname_compose("template_id_properties_tree", RNA_struct_identifier(type));
}

void template_tree(ui::Layout *layout, bContext *C, PointerRNA *dataptr, const char *data_path)
{
  if (dataptr == nullptr) {
    return;
  }

  Block *block = layout->block();

  ui::AbstractTreeView *tree_view = block_add_view(
      *block,
      template_tree_idname(dataptr->type),
      std::make_unique<IDPropertyView>(dataptr, data_path));
  tree_view->set_context_menu_title("ID Property");
  tree_view->set_default_rows(4);

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}

/* Callback to reset object pointer when ID data type is changed. */
static void idproperty_id_type_set_fn(bContext * /*C*/, void *but_arg1, void * /*arg2*/)
{
  IDProperty *prop = static_cast<IDProperty *>(but_arg1);
  prop->data.pointer = nullptr;
}

/* Callback to convert property to python type when type changed to unsupported. */
static void idproperty_python_prop_add_fn(bContext * /*C*/, void *but_arg1, void *arg2)
{
  IDProperty *prop = static_cast<IDProperty *>(but_arg1);
  IDProperty *user_properties = static_cast<IDProperty *>(arg2);

  if (IDP_ui_data_supported(prop)) {
    return;
  }

  IDPropertyTemplate prop_template{0};
  /* Remove existing property to add a new idprop of python type. */
  IDProperty *python_prop = IDP_New(IDP_GROUP, &prop_template, prop->name);
  IDP_ReplaceInGroup_ex(user_properties, python_prop, prop, 0);
}

void draw_id_properties_value(ui::Layout *layout, PointerRNA *dataptr, const char *property_name)
{
  IDProperty *user_properties = RNA_struct_idprops(dataptr, false);
  if (user_properties == nullptr) {
    return;
  }
  IDProperty *active_prop = IDP_GetPropertyFromGroup(user_properties, property_name);
  if (active_prop == nullptr) {
    return;
  }

  ID *id = dataptr->owner_id;
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  PointerRNA prop_ptr = RNA_pointer_create_discrete(id, RNA_IDProperty, active_prop);
  layout->prop(&prop_ptr, "type", UI_ITEM_NONE, "Type", ICON_NONE);
  Button *but = button_last(layout->block());
  button_func_set(but, idproperty_python_prop_add_fn, active_prop, user_properties);

  if (active_prop->ui_data == nullptr) {
    return;
  }

  if (!IDP_ui_data_supported(active_prop)) {
    return;
  }

  auto get_prop_type = [&](const char type) -> StructRNA * {
    switch (type) {
      case IDP_INT:
        return RNA_IDPropertyUIDataInt;
      case IDP_FLOAT:
      case IDP_DOUBLE:
        return RNA_IDPropertyUIDataFloat;
      case IDP_BOOLEAN:
        return RNA_IDPropertyUIDataBool;
      case IDP_STRING:
        return RNA_IDPropertyUIDataString;
      case IDP_ID:
        return RNA_IDPropertyUIDataID;
      default:
        break;
    }

    return nullptr;
  };

  StructRNA *srna = active_prop->type == IDP_ARRAY ? get_prop_type(active_prop->subtype) :
                                                     get_prop_type(active_prop->type);

  PointerRNA propui_ptr = RNA_pointer_create_discrete(id, srna, active_prop->ui_data);

  const bool is_number = ELEM(srna, RNA_IDPropertyUIDataInt, RNA_IDPropertyUIDataFloat);
  const bool supports_default_value = srna != RNA_IDPropertyUIDataID;
  const bool is_array = active_prop->type == IDP_ARRAY;

  /* Draw `ui_data` of active IDProperty. */
  if (supports_default_value) {
    if (is_array) {
      layout->prop(&prop_ptr, "length", UI_ITEM_NONE, "Length", ICON_NONE);
      layout->prop(&propui_ptr, "default_array", ui::ITEM_R_EXPAND, IFACE_("Default"), ICON_NONE);
    }
    else {
      layout->prop(&propui_ptr, "default_value", UI_ITEM_NONE, "Default Value", ICON_NONE);
    }
  }

  if (is_number) {
    Layout &col = layout->column(true);
    col.prop(&propui_ptr, "min", UI_ITEM_NONE, "Hard Min", ICON_NONE);
    col.prop(&propui_ptr, "max", UI_ITEM_NONE, "Max", ICON_NONE);
    col.prop(&propui_ptr, "use_soft_limits", UI_ITEM_NONE, "Use Soft Limits", ICON_NONE);
    if (active_prop->ui_data->flag & IDP_UI_USE_SOFT_LIMITS) {
      col.prop(&propui_ptr, "soft_min", UI_ITEM_NONE, "Soft Min", ICON_NONE);
      col.prop(&propui_ptr, "soft_max", UI_ITEM_NONE, "Max", ICON_NONE);
    }
    layout->prop(&propui_ptr, "step", UI_ITEM_NONE, "Step", ICON_NONE);
  }

  if (srna == RNA_IDPropertyUIDataFloat) {
    layout->prop(&propui_ptr, "precision", UI_ITEM_NONE, "Precision", ICON_NONE);
    layout->prop(&propui_ptr, "subtype", UI_ITEM_NONE, "Sub Type", ICON_NONE);
  }

  if (srna == RNA_IDPropertyUIDataID) {
    layout->prop(&propui_ptr, "id_type", UI_ITEM_NONE, "Data-Block type", ICON_NONE);
    Button *but = button_last(layout->block());
    button_func_set(but, idproperty_id_type_set_fn, active_prop, nullptr);
  }

  layout->prop(&propui_ptr, "description", UI_ITEM_NONE, "Description", ICON_NONE);
  layout->prop(
      &prop_ptr, "is_overridable_library", UI_ITEM_NONE, "Library Overridable", ICON_NONE);
}

}  // namespace blender::ui::id_properties
