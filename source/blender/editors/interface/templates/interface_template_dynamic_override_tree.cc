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
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "DNA_dynamic_override_types.h"

#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_message.hh"

#include <fmt/format.h>

namespace blender::ui {
namespace dynoverride {

using namespace blender::bke::dynoverride;

class RNAPathViewItem;

class DynamicOverrideTreeView : public AbstractTreeView {
 public:
  explicit DynamicOverrideTreeView(DynamicOverride &dynoverride) : dynoverride_(dynoverride) {}

  void build_tree() override;

 private:
  void build_tree_rule(TreeViewOrItem &parent, DynamicOverrideRule &rule);

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
 * Item representing one dynoverride rule.
 */
class RuleViewItem : public AbstractTreeViewItem {
 public:
  RuleViewItem(DynamicOverride &dynoverride, DynamicOverrideRule &rule)
      : dynoverride_(dynoverride), rule_(rule)
  {
    this->label_ = rule_.name;
  }

  void build_row(Layout &row) override
  {
    build_rule_name(row);

    Layout &sub = row.row(true);
    sub.use_property_decorate_set(false);

    build_rule_buttons(sub);
  }

  bool supports_collapsing() const override
  {
    return true;
  }

  bool supports_renaming() const override
  {
    return false;
  }

  void build_tree_rule_property(TreeViewOrItem &parent, DynamicOverrideRuleProperty &property);

 private:
  DynamicOverride &dynoverride_;
  DynamicOverrideRule &rule_;

  /* Map allowing to group all properties by the items in their RNA path. */
  Map<const ParsedRNAPathRef, RNAPathViewItem *> rna_path_items_;

  void build_rule_name(Layout &row)
  {
    uiItemL_ex(&row, rule_.name, ICON_NONE, false, false);
  }

  void build_rule_buttons(Layout &row)
  {
    if (rule_.type != DynamicOverrideRuleType::IDData) {
      return;
    }
    if (rule_.target_filter.type != DynamicOverrideRuleTargetFilterType::IDSingle) {
      return;
    }

    PointerRNA rule_ptr = RNA_pointer_create_id_subdata(
        dynoverride_.id, RNA_DynamicOverrideRuleIDData, &rule_);

    PointerRNA rule_filter_ptr = RNA_pointer_create_with_parent(
        rule_ptr, RNA_DynamicOverrideRuleTargetFilterIDSingle, &rule_.target_filter);
    Layout &sub_idref = row.row(true);
    sub_idref.enabled_set(false);
    sub_idref.prop(&rule_filter_ptr, "target_id", {}, {}, ICON_NONE);

    Layout &sub_butts = row.row(true);
    sub_butts.prop(&rule_ptr,
                   "is_muted",
                   ITEM_R_ICON_ONLY,
                   {},
                   bool(rule_.flag & DynamicOverrideRuleFlag::IsMuted) ? ICON_CHECKBOX_DEHLT :
                                                                         ICON_CHECKBOX_HLT);

    PointerRNA op_data = sub_butts.op("UI_OT_dynoverride_remove_rule", "", ICON_X);
    RNA_int_set(&op_data, "session_uid", int(dynoverride_.id.session_uid));
    RNA_string_set(&op_data, "rule_name", rule_.name);
  }
};

/**
 * One item in the RNA path leading to a RuleProperty.
 *
 * Never a tree leaf.
 */
class RNAPathViewItem : public AbstractTreeViewItem {
 public:
  RNAPathViewItem(DynamicOverride &dynoverride,
                  DynamicOverrideRule &rule,
                  const ParsedRNAPathRef rna_parsed_path)
      : dynoverride_(dynoverride),
        rule_(rule),
        rna_parsed_path_(rna_parsed_path),
        rna_path_(ParsedRNAPath<4>(rna_parsed_path_).to_string())
  {
    this->label_ = rna_path::to_string(rna_parsed_path_.last());
  }

  void build_row(Layout &row) override
  {
    build_rnapath_item_name(row);

    Layout &sub = row.row(true);
    sub.use_property_decorate_set(false);

    build_rnapath_item_buttons(sub);
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
  const ParsedRNAPathRef rna_parsed_path_;
  const std::string rna_path_;

  void build_rnapath_item_name(Layout &row)
  {
    uiItemL_ex(&row, label_, ICON_NONE, false, false);
  }

  void build_rnapath_item_buttons(Layout &row)
  {
    Layout &sub_butts = row.row(true);

    std::optional<ParsedRNAPath<4>> rna_path(rna_parsed_path_);
    BLI_assert(rna_path.has_value());
    Vector<DynamicOverrideRuleProperty *> properties =
        bke::dynoverride::rule_rna_property_lookup_root_rnapath(rule_, *rna_path);
    BLI_assert(!properties.is_empty());
    bool is_all_muted = std::all_of(
        properties.begin(), properties.end(), [](DynamicOverrideRuleProperty *p) -> bool {
          return bool(p->flag & DynamicOverrideRulePropertyFlag::IsMuted);
        });
    const int icon = is_all_muted ? ICON_CHECKBOX_DEHLT : ICON_CHECKBOX_HLT;
    PointerRNA op_data = sub_butts.op("UI_OT_dynoverride_toggle_mute_rule_property", "", icon);
    RNA_int_set(&op_data, "session_uid", int(dynoverride_.id.session_uid));
    RNA_string_set(&op_data, "rule_name", rule_.name);
    RNA_string_set(&op_data, "property_rna_path", rna_path_.c_str());

    op_data = sub_butts.op("UI_OT_dynoverride_remove_rule_property", "", ICON_X);
    RNA_int_set(&op_data, "session_uid", int(dynoverride_.id.session_uid));
    RNA_string_set(&op_data, "rule_name", rule_.name);
    RNA_string_set(&op_data, "property_rna_path", rna_path_.c_str());
  }
};

/**
 * Item representing one dynoverride RNA property.
 *
 * Always a tree leaf currently.
 */
class PropertyViewItem : public AbstractTreeViewItem {
 public:
  PropertyViewItem(DynamicOverride &dynoverride,
                   DynamicOverrideRule &rule,
                   DynamicOverrideRuleProperty &property)
      : dynoverride_(dynoverride), rule_(rule), property_(property)
  {
    this->label_ = rna_path::to_string(rule_property_rna_path_parsed(property).last());
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
    build_property_name(row);

    Layout &sub = row.row(true);
    sub.use_property_decorate_set(false);

    build_property_buttons(sub);
  }

 private:
  DynamicOverride &dynoverride_;
  DynamicOverrideRule &rule_;
  DynamicOverrideRuleProperty &property_;

  void build_property_name(Layout &row)
  {
    uiItemL_ex(&row, label_, ICON_NONE, false, false);
  }

  void build_property_buttons(Layout &row)
  {
    DynamicOverrideRuleIDData &iddata_rule = reinterpret_cast<DynamicOverrideRuleIDData &>(rule_);
    PointerRNA rule_ptr = RNA_pointer_create_id_subdata(
        dynoverride_.id, RNA_DynamicOverrideRuleIDData, &rule_);
    PointerRNA property_ptr = RNA_pointer_create_with_parent(
        rule_ptr, RNA_DynamicOverrideRuleProperty, &property_);
    PointerRNA override_values_ptr = RNA_pointer_create_with_parent(
        rule_ptr, rule_get_runtime_override_values_rna_struct(iddata_rule), &iddata_rule);

    Layout &sub_prop = row.row(true);
    sub_prop.emboss_set(EmbossType::Emboss);
    sub_prop.use_property_decorate_set(true);
    sub_prop.prop(
        &override_values_ptr, rule_property_rna_identifier(property_), {}, {}, ICON_NONE);

    Layout &sub_butts = row.row(true);
    sub_butts.prop(&property_ptr,
                   "is_muted",
                   ITEM_R_ICON_ONLY,
                   {},
                   bool(property_.flag & DynamicOverrideRulePropertyFlag::IsMuted) ?
                       ICON_CHECKBOX_DEHLT :
                       ICON_CHECKBOX_HLT);

    PointerRNA op_data = sub_butts.op("UI_OT_dynoverride_remove_rule_property", "", ICON_X);
    RNA_int_set(&op_data, "session_uid", int(dynoverride_.id.session_uid));
    RNA_string_set(&op_data, "rule_name", rule_.name);
    RNA_string_set(&op_data, "property_rna_path", property_.rna_path);
  }
};

void RuleViewItem::build_tree_rule_property(TreeViewOrItem &parent_root,
                                            DynamicOverrideRuleProperty &property)
{
  if (rule_.type != DynamicOverrideRuleType::IDData) {
    return;
  }

  /* Build intermediary RNA path items. */
  const ParsedRNAPathRef rna_parsed_path = rule_property_rna_path_parsed(property);
  TreeViewOrItem *parent = &parent_root;
  /* Note: last item in the parsed RNA path is skipped, as it's the actual overridden property,
   * which will be represented by a `PropertyViewItem`. */
  for (int slice_size = 1; slice_size < rna_parsed_path.size(); slice_size++) {
    const ParsedRNAPathRef rna_parsed_path_slice = rna_parsed_path.slice(0, slice_size);
    parent = rna_path_items_.lookup_or_add_cb(
        rna_parsed_path_slice,
        /* Add new treeview item to the current parent, if it does not exist yet. */
        [this, parent, rna_parsed_path_slice]() -> RNAPathViewItem * {
          return &parent->add_tree_item<RNAPathViewItem>(
              this->dynoverride_, this->rule_, rna_parsed_path_slice);
        });
  }

  parent->add_tree_item<PropertyViewItem>(this->dynoverride_, this->rule_, property);
}

void DynamicOverrideTreeView::build_tree_rule(TreeViewOrItem &parent, DynamicOverrideRule &rule)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return;
  }
  RuleViewItem &rule_item = parent.add_tree_item<RuleViewItem>(this->dynoverride_, rule);

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  for (DynamicOverrideRuleProperty &rule_prop : rule_iddata.properties) {
    rule_item.build_tree_rule_property(rule_item, rule_prop);
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
