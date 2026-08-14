/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BLI_listbase.hh"
#include "BLI_ustring.hh"

#include "BKE_context.hh"
#include "BKE_dynamic_override.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DNA_dynamic_override_types.h"

#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_message.hh"

#include <fmt/format.h>

namespace blender::ui {
namespace dynoverride {

using namespace blender::bke::dynoverride;

class DynamicOverrideTreeView : public AbstractTreeView {
 public:
  explicit DynamicOverrideTreeView(DynamicOverride &dynoverride) : dynoverride_(dynoverride) {}

  void build_tree() override;

 private:
  void build_tree_rule(TreeViewOrItem &parent, DynamicOverrideRule &rule);
  void build_tree_rule_property_recursive(TreeViewOrItem &parent,
                                          DynamicOverrideRule &rule,
                                          DynamicOverrideRuleProperty &rule_prop,
                                          const int index_in_parsed_rna_path);
  DynamicOverride &dynoverride_;
};

#if 0
class LayerNodeDropTarget : public TreeViewItemDropTarget {
  TreeNode &drop_tree_node_;

 public:
  LayerNodeDropTarget(AbstractTreeViewItem &item, TreeNode &drop_tree_node, DropBehavior behavior)
      : TreeViewItemDropTarget(item, behavior), drop_tree_node_(drop_tree_node)
  {
  }

  bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    if (!ELEM(drag.type, WM_DRAG_GREASE_PENCIL_LAYER, WM_DRAG_GREASE_PENCIL_GROUP)) {
      return false;
    }

    wmDragGreasePencilLayer *active_drag_node = static_cast<wmDragGreasePencilLayer *>(drag.poin);
    if (active_drag_node->node->wrap().is_layer()) {
      return true;
    }

    LayerGroup &group = active_drag_node->node->wrap().as_group();
    if (drop_tree_node_.is_child_of(group)) {
      /* Don't drop group node into its child node. */
      return false;
    }
    return true;
  }

  std::string drop_tooltip(const DragInfo &drag_info) const override
  {
    const wmDragGreasePencilLayer *drag_grease_pencil =
        static_cast<const wmDragGreasePencilLayer *>(drag_info.drag_data.poin);
    TreeNode &drag_node = drag_grease_pencil->node->wrap();

    const StringRef drag_name = drag_node.name();
    const StringRef drop_name = drop_tree_node_.name();
    const StringRef node_type = drag_node.is_layer() ? "layer" : "group";

    switch (drag_info.drop_location) {
      case DropLocation::Into:
        return fmt::format(
            fmt::runtime(TIP_("Move {} {} into {}")), node_type, drag_name, drop_name);
      case DropLocation::Before:
        return fmt::format(
            fmt::runtime(TIP_("Move {} {} above {}")), node_type, drag_name, drop_name);
      case DropLocation::After:
        return fmt::format(
            fmt::runtime(TIP_("Move {} {} below {}")), node_type, drag_name, drop_name);
      default:
        BLI_assert_unreachable();
        break;
    }

    return "";
  }

  bool on_drop(bContext *C, const DragInfo &drag_info) const override
  {
    const wmDragGreasePencilLayer *drag_grease_pencil =
        static_cast<const wmDragGreasePencilLayer *>(drag_info.drag_data.poin);
    GreasePencil &grease_pencil = *drag_grease_pencil->grease_pencil;
    TreeNode &drag_node = drag_grease_pencil->node->wrap();

    if (!drop_tree_node_.parent_group()) {
      /* Root node is not added to the tree view, so there should never be a drop target for this.
       */
      BLI_assert_unreachable();
      return false;
    }

    if (&drop_tree_node_ == &drag_node) {
      return false;
    }

    switch (drag_info.drop_location) {
      case DropLocation::Into: {
        BLI_assert_msg(drop_tree_node_.is_group(),
                       "Inserting should not be possible for layers, only for groups, because "
                       "only groups use DropBehavior::Reorder_and_Insert");
        LayerGroup &drop_group = drop_tree_node_.as_group();
        grease_pencil.move_node_into(drag_node, drop_group);
        break;
      }
      case DropLocation::Before: {
        /* Draw order is inverted, so inserting before (above) means inserting the node after. */
        grease_pencil.move_node_after(drag_node, drop_tree_node_);
        break;
      }
      case DropLocation::After: {
        /* Draw order is inverted, so inserting after (below) means inserting the node before. */
        grease_pencil.move_node_before(drag_node, drop_tree_node_);
        break;
      }
      default: {
        BLI_assert_unreachable();
        return false;
      }
    }

    if (drag_node.is_layer()) {
      WM_msg_publish_rna_prop(
          CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3Layers, active);
      WM_msg_publish_rna_prop(
          CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencil, layers);
    }
    else if (drag_node.is_group()) {
      WM_msg_publish_rna_prop(CTX_wm_message_bus(C),
                              &grease_pencil.id,
                              &grease_pencil,
                              GreasePencilv3LayerGroup,
                              active);
      WM_msg_publish_rna_prop(
          CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencil, layer_groups);
    }

    ED_undo_push(C, "Reorder Layers");

    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);
    return true;
  }
};

class LayerViewItemDragController : public AbstractViewItemDragController {
  GreasePencil &grease_pencil_;
  TreeNode &dragged_node_;

 public:
  LayerViewItemDragController(LayerTreeView &tree_view,
                              GreasePencil &grease_pencil,
                              GreasePencilLayerTreeNode &node)
      : AbstractViewItemDragController(tree_view),
        grease_pencil_(grease_pencil),
        dragged_node_(node.wrap())
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    if (dragged_node_.wrap().is_layer()) {
      return WM_DRAG_GREASE_PENCIL_LAYER;
    }
    return WM_DRAG_GREASE_PENCIL_GROUP;
  }

  void *create_drag_data() const override
  {
    wmDragGreasePencilLayer *drag_data = MEM_new_zeroed<wmDragGreasePencilLayer>(__func__);
    drag_data->node = &dragged_node_;
    drag_data->grease_pencil = &grease_pencil_;
    return drag_data;
  }

  void on_drag_start(bContext & /*C*/, AbstractViewItem & /*item*/) override
  {
    grease_pencil_.set_active_node(&dragged_node_);
  }
};
#endif

/**
 * One item in the RNA path leading to a RuleProperty.
 *
 * Never a tree leaf.
 */
class RNAPathViewItem : public AbstractTreeViewItem {
 public:
  RNAPathViewItem(DynamicOverride &dynoverride, LayerGroup &group)
      : dynoverride_(dynoverride), group_(group)
  {
    this->label_ = group_.name();
  }

  std::optional<bool> should_be_collapsed() const override
  {
    const bool is_collapsed = !group_.is_expanded();
    return is_collapsed;
  }

  bool set_collapsed(const bool collapsed) override
  {
    if (!AbstractTreeViewItem::set_collapsed(collapsed)) {
      return false;
    }
    group_.set_expanded(!collapsed);
    return true;
  }

  void on_collapse_change(bContext &C, const bool is_collapsed) override
  {
    const bool is_expanded = !is_collapsed;

    /* Let RNA handle the property change. This makes sure all the notifiers and DEG
     * update calls are properly called. */
    PointerRNA group_ptr = RNA_pointer_create_discrete(
        &dynoverride_.id, RNA_GreasePencilLayerGroup, &group_);
    PropertyRNA *prop = RNA_struct_find_property(&group_ptr, "is_expanded");

    RNA_property_boolean_set(&group_ptr, prop, is_expanded);
    RNA_property_update(&C, &group_ptr, prop);
  }

  void build_row(Layout &row) override
  {
    build_layer_group_name(row);

    Layout &sub = row.row(true);
    sub.use_property_decorate_set(false);

    build_layer_group_buttons(sub);
  }

  std::optional<bool> should_be_active() const override
  {
    if (this->dynoverride_.has_active_group()) {
      return &group_ == this->dynoverride_.get_active_group();
    }
    return {};
  }

  void build_context_menu(bContext &C, Layout &layout) const override
  {
    MenuType *mt = WM_menutype_find("GREASE_PENCIL_MT_group_context_menu", true);
    if (!mt) {
      return;
    }
    menutype_draw(&C, mt, &layout);
  }

  void on_activate(bContext &C) override
  {
    PointerRNA grease_pencil_ptr = RNA_pointer_create_discrete(
        &dynoverride_.id, RNA_GreasePencilv3LayerGroup, nullptr);
    PointerRNA value_ptr = RNA_pointer_create_discrete(
        &dynoverride_.id, RNA_GreasePencilLayerGroup, &group_);

    PropertyRNA *prop = RNA_struct_find_property(&grease_pencil_ptr, "active");

    if (dynoverride_.has_active_layer()) {
      WM_msg_publish_rna_prop(
          CTX_wm_message_bus(&C), &dynoverride_.id, &dynoverride_, GreasePencilv3Layers, active);
    }

    RNA_property_pointer_set(&grease_pencil_ptr, prop, value_ptr, nullptr);
    RNA_property_update(&C, &grease_pencil_ptr, prop);

    ED_undo_push(&C, "Active Grease Pencil Group");
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    PointerRNA group_ptr = RNA_pointer_create_discrete(
        &dynoverride_.id, RNA_GreasePencilLayerGroup, &group_);
    PropertyRNA *prop = RNA_struct_find_property(&group_ptr, "name");

    RNA_property_string_set(&group_ptr, prop, new_name.c_str());
    RNA_property_update(&const_cast<bContext &>(C), &group_ptr, prop);

    ED_undo_push(&const_cast<bContext &>(C), "Rename Grease Pencil Layer Group");
    return true;
  }

  StringRef get_rename_string() const override
  {
    return group_.name();
  }

  void delete_item(bContext *C) override
  {
    dynoverride_.remove_group(group_);
    DEG_id_tag_update(&dynoverride_.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, nullptr);
    ED_undo_push(C, "Delete Grease Pencil Group");
  }

  std::unique_ptr<AbstractViewItemDragController> create_drag_controller() const override
  {
    return std::make_unique<LayerViewItemDragController>(
        static_cast<LayerTreeView &>(get_tree_view()), dynoverride_, group_.base);
  }

  std::unique_ptr<TreeViewItemDropTarget> create_drop_target() override
  {
    return std::make_unique<LayerNodeDropTarget>(
        *this, group_.as_node(), DropBehavior::ReorderAndInsert);
  }

 private:
  DynamicOverride &dynoverride_;
  LayerGroup &group_;

  void build_layer_group_name(Layout &row)
  {
    short icon = ICON_GREASEPENCIL_LAYER_GROUP;
    if (group_.color_tag != LAYERGROUP_COLOR_NONE) {
      icon = ICON_LAYERGROUP_COLOR_01 + int(group_.color_tag);
    }

    Button *but = uiItemL_ex(&row, group_.name(), icon, false, false);
    if (ID_IS_LINKED(&dynoverride_)) {
      button_flag_enable(but, BUT_DISABLED);
    }
    else if (!group_.is_editable()) {
      button_disable(but, "Layer Group is locked or not visible");
    }
  }

  void build_layer_group_buttons(Layout &row)
  {
    PointerRNA group_ptr = RNA_pointer_create_discrete(
        &dynoverride_.id, RNA_GreasePencilLayerGroup, &group_);

    Layout *sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(group_.as_node().parent_group()->use_masks());
    }
    sub->prop(&group_ptr, "use_masks", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);

    sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(group_.as_node().parent_group()->use_onion_skinning());
    }
    sub->prop(&group_ptr, "use_onion_skinning", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);

    sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(group_.as_node().parent_group()->is_visible());
    }
    sub->prop(&group_ptr, "hide", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);

    sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(!group_.as_node().parent_group()->is_locked());
    }
    sub->prop(&group_ptr, "lock", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
  }
};

/**
 * Item representing one dynoverride rule.
 */
class RuleViewItem : public AbstractTreeViewItem {
 public:
  RuleViewItem(DynamicOverride &dynoverride, DynamicOverrideRule &rule)
      : dynoverride_(dynoverride), rule_(rule_)
  {
    this->label_ = rule_.name;
  }

  void build_row(Layout &row) override
  {
    build_layer_name(row);

    Layout &sub = row.row(true);
    sub.use_property_decorate_set(false);

    build_layer_buttons(sub);
  }

  bool supports_collapsing() const override
  {
    return true;
  }

  bool supports_renaming() const override
  {
    return false;
  }

 private:
  DynamicOverride &dynoverride_;
  DynamicOverrideRule &rule_;

  /* Map allowing to group all properties by the items in their RNA path. */
  Map<StringRef, RNAPathViewItem *> rna_path_items;

  void build_layer_name(Layout &row)
  {
    uiItemL_ex(&row, rule_.name, ICON_NONE, false, false);
  }

  void build_layer_buttons(Layout & /*row*/) {}
};

/**
 * Item representing one dynoverride RNA property.
 *
 * Always a tree leaf currently.
 */
class PropertyViewItem : public AbstractTreeViewItem {
 public:
  PropertyViewItem(DynamicOverride &dynoverride, LayerGroup &group)
      : dynoverride_(dynoverride), group_(group)
  {
    this->label_ = group_.name();
  }

  bool supports_collapsing() const override
  {
    return false;
  }

  bool supports_renaming() const override
  {
    return false;
  }

  void build_row(Layout &row) override
  {
    build_layer_group_name(row);

    Layout &sub = row.row(true);
    sub.use_property_decorate_set(false);

    build_layer_group_buttons(sub);
  }

 private:
  DynamicOverride &dynoverride_;
  LayerGroup &group_;

  void build_layer_group_name(Layout &row)
  {
    short icon = ICON_GREASEPENCIL_LAYER_GROUP;
    if (group_.color_tag != LAYERGROUP_COLOR_NONE) {
      icon = ICON_LAYERGROUP_COLOR_01 + int(group_.color_tag);
    }

    Button *but = uiItemL_ex(&row, group_.name(), icon, false, false);
    if (ID_IS_LINKED(&dynoverride_)) {
      button_flag_enable(but, BUT_DISABLED);
    }
    else if (!group_.is_editable()) {
      button_disable(but, "Layer Group is locked or not visible");
    }
  }

  void build_layer_group_buttons(Layout &row)
  {
    PointerRNA group_ptr = RNA_pointer_create_discrete(
        &dynoverride_.id, RNA_GreasePencilLayerGroup, &group_);

    Layout *sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(group_.as_node().parent_group()->use_masks());
    }
    sub->prop(&group_ptr, "use_masks", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);

    sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(group_.as_node().parent_group()->use_onion_skinning());
    }
    sub->prop(&group_ptr, "use_onion_skinning", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);

    sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(group_.as_node().parent_group()->is_visible());
    }
    sub->prop(&group_ptr, "hide", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);

    sub = &row.row(true);
    if (group_.as_node().parent_group()) {
      sub->active_set(!group_.as_node().parent_group()->is_locked());
    }
    sub->prop(&group_ptr, "lock", ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
  }
};

void DynamicOverrideTreeView::build_tree_rule(TreeViewOrItem &parent, DynamicOverrideRule &rule)
{
  if (node.is_layer()) {
    parent.add_tree_item<LayerViewItem>(this->dynoverride_, node.as_layer());
  }
  else if (node.is_group()) {
    LayerGroupViewItem &group_item = parent.add_tree_item<LayerGroupViewItem>(this->dynoverride_,
                                                                              node.as_group());
    for (GreasePencilLayerTreeNode &node_ : node.as_group().children.items_reversed()) {
      build_tree_node_recursive(group_item, node_.wrap());
    }
  }
}

void DynamicOverrideTreeView::build_tree()
{
  for (DynamicOverrideRule &rule : this->dynoverride_.rules) {
    this->build_tree_rule(*this, rule);
  }
}

}  // namespace dynoverride

void template_dynamic_override_tree(Layout *layout, bContext *C)
{
  DynamicOverride *dynoverride = CTX_data_dynamic_override(C);

  if (!dynoverride) {
    return;
  }

  Block *block = layout->block();

  AbstractTreeView *tree_view = block_add_view(
      *block,
      "Dynamic Override Tree View",
      std::make_unique<dynoverride::DynamicOverrideTreeView>(*dynoverride));
  tree_view->set_context_menu_title("Dynamic Override");
  tree_view->set_default_rows(12);

  TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}

}  // namespace blender::ui
