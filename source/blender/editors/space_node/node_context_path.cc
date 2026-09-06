/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 * \brief Node breadcrumbs drawing
 */

#include "BLI_listbase.hh"
#include "BLI_vector.hh"

#include "DNA_node_types.h"

#include "BKE_compositor.hh"
#include "BKE_context.hh"
#include "BKE_material.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "ED_node_c.hh"
// #include "ED_render.hh"
#include "ED_screen.hh"

#include "SEQ_modifier.hh"
#include "SEQ_select.hh"
#include "SEQ_sequencer.hh"

#include "WM_api.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_intern.hh"

namespace blender {

struct Material;

namespace ed::space_node {

static void context_path_add_object_data(Vector<ui::ContextPathItem> &path, Object &object)
{
  if (!object.data) {
    return;
  }
  if (object.type == OB_MESH) {
    ui::context_path_add_generic(path, *RNA_Mesh, object.data);
  }
  else if (object.type == OB_CURVES) {
    ui::context_path_add_generic(path, *RNA_Curves, object.data);
  }
  else if (object.type == OB_LAMP) {
    ui::context_path_add_generic(path, *RNA_Light, object.data);
  }
  else if (ELEM(object.type, OB_CURVES_LEGACY, OB_FONT, OB_SURF)) {
    ui::context_path_add_generic(path, *RNA_Curve, object.data);
  }
}

static std::function<void(bContext &)> tree_path_handle_func(int i)
{
  return [i](bContext &C) {
    wmOperatorType *ot = WM_operatortype_find("NODE_OT_tree_path_parent", false);
    PointerRNA op_props = WM_operator_properties_create_ptr(ot);
    RNA_int_set(&op_props, "parent_tree_index", i);
    WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::InvokeDefault, &op_props, nullptr);
    WM_operator_properties_free(&op_props);
  };
}

static bNode *find_group_node_in_tree(bNodeTree *tree, bNodeTree *group_tree)
{
  if (!tree || !group_tree) {
    return nullptr;
  }

  for (bNode *node : tree->all_nodes()) {
    if (node->id == reinterpret_cast<ID *>(group_tree)) {
      return node;
    }
  }
  return nullptr;
}

static void activate_and_push_node_tree(ARegion *region,
                                        SpaceNode *snode,
                                        bNodeTree *group_tree,
                                        bNode *group_node)
{
  bke::node_set_active(*snode->edittree, *group_node);
  bke::node_set_selected(*group_node, true);
  ED_node_tree_push(region, snode, group_tree, group_node);
}

static bool navigate_through_history_path(ARegion *region,
                                          SpaceNode *snode,
                                          Span<bNodeTree *> history_path,
                                          bNodeTree *target_tree = nullptr)
{
  for (bNodeTree *history_tree : history_path) {
    if (!snode->edittree) {
      return false;
    }

    bNode *group_node = find_group_node_in_tree(snode->edittree, history_tree);
    if (group_node == nullptr) {
      return false;
    }

    activate_and_push_node_tree(region, snode, history_tree, group_node);

    if (target_tree && snode->edittree == target_tree) {
      return true;
    }
  }
  return true;
}

static bool node_tree_has_group_node(const bNodeTree *ntree)
{
  if (ntree == nullptr) {
    return false;
  }

  for (const bNode *node : ntree->all_nodes()) {
    if (node->is_group() && node->id != nullptr) {
      return true;
    }
  }
  return false;
}

static void navigate_menu_draw_fn(bContext *C, ui::Layout *layout, void *arg)
{
  if (arg == nullptr) {
    return;
  }
  bNodeTree *clicked_tree = nullptr;
  ID *id = static_cast<ID *>(arg);
  if (GS(id->name) == ID_NT) {
    clicked_tree = id_cast<bNodeTree *>(id);
  }
  else {
    SpaceNode *snode = CTX_wm_space_node(C);
    clicked_tree = snode->edittree;
  }
  if (!clicked_tree) {
    return;
  }

  Set<bNodeTree *> added_groups;
  for (bNode *node : clicked_tree->all_nodes()) {
    if (!node->is_group() || !node->id) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node->id);
    if (added_groups.contains(group)) {
      continue;
    }
    added_groups.add(group);

    auto enter_group_func = [clicked_tree, node, group](bContext &C) {
      SpaceNode *snode = CTX_wm_space_node(&C);
      ARegion *region = CTX_wm_region(&C);

      if (snode->edittree != clicked_tree) {
        bool is_parent = false;
        int index = 0;
        for (bNodeTreePath &path_item : snode->treepath) {
          if (path_item.nodetree == clicked_tree) {
            is_parent = true;
            break;
          }
          index++;
        }

        if (is_parent) {
          while (snode->edittree != clicked_tree) {
            ED_node_tree_pop(region, snode);
          }
        }
        else {
          Vector<bNodeTree *> &history_path_trees = snode->runtime->navigate_history_path;
          if (history_path_trees.is_empty()) {
            return;
          }
          Span<bNodeTree *> history_tail_path = history_path_trees.as_span().drop_front(index);
          navigate_through_history_path(region, snode, history_tail_path, clicked_tree);
        }
      }
      if (snode->edittree == clicked_tree) {
        activate_and_push_node_tree(region, snode, group, node);
        // WM_event_add_notifier(&C, NC_SCENE | ND_NODES, nullptr);
        // WM_event_add_notifier(&C, NC_NODE | ND_NODE_GIZMO, nullptr);
      }
    };

    layout->button(node->id->name + 2, ICON_NODETREE, enter_group_func);
  }
}

static void context_path_add_top_level_shader_node_tree(const SpaceNode &snode,
                                                        Vector<ui::ContextPathItem> &path,
                                                        StructRNA &rna_type,
                                                        void *ptr)
{
  const bool in_root_tree = (snode.nodetree != snode.edittree);
  const bool has_group = node_tree_has_group_node(snode.edittree);
  ui::context_path_add_generic(path,
                               rna_type,
                               ptr,
                               ICON_NONE,
                               in_root_tree ? tree_path_handle_func(0) : nullptr,
                               has_group ? navigate_menu_draw_fn : nullptr);
}

static void context_path_add_node_tree_and_node_groups(const SpaceNode &snode,
                                                       Vector<ui::ContextPathItem> &path,
                                                       const bool skip_base = false)
{

  for (const auto [i, path_item] : snode.treepath.enumerate()) {
    if (skip_base && &path_item == snode.treepath.first) {
      continue;
    }
    if (path_item.nodetree == nullptr) {
      continue;
    }

    int icon = ICON_NODETREE;
    if (ID_IS_PACKED(&path_item.nodetree->id)) {
      icon = ICON_PACKAGE;
    }
    else if (ID_IS_LINKED(&path_item.nodetree->id)) {
      icon = ICON_LINKED;
    }
    else if (ID_IS_ASSET(&path_item.nodetree->id)) {
      icon = ICON_ASSET_MANAGER;
    }

    /* We don't need to add handle function to last node-tree. */
    const bool is_last_item = (&path_item == snode.treepath.last);
    const bool has_group = node_tree_has_group_node(path_item.nodetree);
    ui::context_path_add_generic(path,
                                 *RNA_NodeTree,
                                 path_item.nodetree,
                                 icon,
                                 is_last_item ? nullptr : tree_path_handle_func(i),
                                 has_group ? navigate_menu_draw_fn : nullptr);
  }
}

static void get_context_path_node_shader(const bContext &C,
                                         SpaceNode &snode,
                                         Vector<ui::ContextPathItem> &path)
{
  if (snode.flag & SNODE_PIN) {
    if (snode.shaderfrom == SNODE_SHADER_WORLD) {
      Scene *scene = CTX_data_scene(&C);
      ui::context_path_add_generic(path, *RNA_Scene, scene);
      if (scene != nullptr) {
        context_path_add_top_level_shader_node_tree(snode, path, *RNA_World, scene->world);
      }
      /* Skip the base node tree here, because the world contains a node tree already. */
      context_path_add_node_tree_and_node_groups(snode, path, true);
    }
    else {
      context_path_add_node_tree_and_node_groups(snode, path);
    }
  }
  else {
    Object *object = CTX_data_active_object(&C);
    if (snode.shaderfrom == SNODE_SHADER_OBJECT && object != nullptr) {
      ui::context_path_add_generic(path, *RNA_Object, object);
      if (!(object->matbits && object->matbits[object->actcol - 1])) {
        context_path_add_object_data(path, *object);
      }
      Material *material = BKE_object_material_get(object, object->actcol);
      context_path_add_top_level_shader_node_tree(snode, path, *RNA_Material, material);
    }
    else if (snode.shaderfrom == SNODE_SHADER_WORLD) {
      Scene *scene = CTX_data_scene(&C);
      ui::context_path_add_generic(path, *RNA_Scene, scene);
      if (scene != nullptr) {
        context_path_add_top_level_shader_node_tree(snode, path, *RNA_World, scene->world);
      }
    }
#ifdef WITH_FREESTYLE
    else if (snode.shaderfrom == SNODE_SHADER_LINESTYLE) {
      ViewLayer *viewlayer = CTX_data_view_layer(&C);
      FreestyleLineStyle *linestyle = BKE_linestyle_active_from_view_layer(viewlayer);
      ui::context_path_add_generic(path, *RNA_ViewLayer, viewlayer);
      Material *mat = BKE_object_material_get(object, object->actcol);
      ui::context_path_add_generic(path, *RNA_Material, mat);
    }
#endif
    context_path_add_node_tree_and_node_groups(snode, path, true);
  }
}

static void get_context_path_node_compositor(const bContext &C,
                                             SpaceNode &snode,
                                             Vector<ui::ContextPathItem> &path)
{
  if (snode.flag & SNODE_PIN) {
    context_path_add_node_tree_and_node_groups(snode, path);
    return;
  }

  if (snode.node_tree_sub_type == SNODE_COMPOSITOR_SEQUENCER) {
    bool skip_base = false;
    Scene *sequencer_scene = CTX_data_sequencer_scene(&C);
    if (sequencer_scene) {
      ui::context_path_add_generic(path, *RNA_Scene, sequencer_scene, ICON_SCENE);
      Strip *strip = seq::select_active_get(sequencer_scene);
      if (strip) {
        ui::context_path_add_generic(path, *RNA_Strip, strip, ICON_SEQ_STRIP);
        bNodeTree *node_group = nullptr;
        if (strip->type == STRIP_TYPE_COMPOSITOR && strip->effectdata) {
          CompositorEffectVars *comp_data = static_cast<CompositorEffectVars *>(strip->effectdata);
          node_group = comp_data->node_group;
        }
        else {
          StripModifierData *smd = seq::modifier_get_active(strip);
          if (smd && smd->type == eSeqModifierType_Compositor) {
            SequencerCompositorModifierData *scmd =
                reinterpret_cast<SequencerCompositorModifierData *>(smd);
            node_group = scmd->node_group;
          }
        }

        if (node_group != nullptr) {
          context_path_add_top_level_shader_node_tree(snode, path, *RNA_NodeTree, node_group);
          skip_base = true;
        }
      }
    }
    context_path_add_node_tree_and_node_groups(snode, path, skip_base);
    return;
  }

  Scene *scene = CTX_data_scene(&C);
  ui::context_path_add_generic(path, *RNA_Scene, scene);
  SceneCompositorEffect *effect = bke::compositor::get_active_effect(*scene);
  if (!effect) {
    context_path_add_node_tree_and_node_groups(snode, path);
    return;
  }

  ui::context_path_add_generic(path, *RNA_SceneCompositorEffect, effect, ICON_NODE_COMPOSITING);
  context_path_add_node_tree_and_node_groups(snode, path);
}

static void get_context_path_node_geometry(const bContext &C,
                                           SpaceNode &snode,
                                           Vector<ui::ContextPathItem> &path)
{
  if (snode.flag & SNODE_PIN || snode.node_tree_sub_type == SNODE_GEOMETRY_TOOL) {
    context_path_add_node_tree_and_node_groups(snode, path);
  }
  else {
    Object *object = CTX_data_active_object(&C);
    if (!object) {
      context_path_add_node_tree_and_node_groups(snode, path);
      return;
    }
    ui::context_path_add_generic(path, *RNA_Object, object);
    ModifierData *modifier = BKE_object_active_modifier(object);
    if (!modifier) {
      context_path_add_node_tree_and_node_groups(snode, path);
      return;
    }
    ui::context_path_add_generic(path, *RNA_Modifier, modifier, ICON_GEOMETRY_NODES);
    context_path_add_node_tree_and_node_groups(snode, path);
  }
}

static std::function<void(bContext &)> tree_path_navigate_history(Span<bNodeTree *> history_path)
{
  return [history_path](bContext &C) {
    SpaceNode *snode = CTX_wm_space_node(&C);
    ARegion *region = CTX_wm_region(&C);
    // ED_preview_kill_jobs(CTX_wm_manager(&C), CTX_data_main(&C));
    navigate_through_history_path(region, snode, history_path);
    // WM_event_add_notifier(&C, NC_SCENE | ND_NODES, nullptr);
    // WM_event_add_notifier(&C, NC_NODE | ND_NODE_GIZMO, nullptr);
  };
}

static void context_path_add_history_trees(SpaceNode &snode, Vector<ui::ContextPathItem> &path)
{
  if (snode.runtime == nullptr) {
    return;
  }
  Vector<bNodeTree *> active_path_trees;
  for (const bNodeTreePath &path_item : snode.treepath) {
    if (path_item.nodetree) {
      active_path_trees.append(path_item.nodetree);
    }
  }
  Vector<bNodeTree *> &history_path_trees = snode.runtime->navigate_history_path;
  if (active_path_trees.is_empty()) {
    history_path_trees.clear();
    return;
  }

  bool valid_history = false;
  if (active_path_trees.size() < history_path_trees.size()) {
    valid_history = true;
    for (const int i : active_path_trees.index_range()) {
      if (active_path_trees[i] != history_path_trees[i]) {
        valid_history = false;
        history_path_trees.clear();
        break;
      }
    }
  }

  if (valid_history) {
    Span<bNodeTree *> history_tail_path = history_path_trees.as_span().drop_front(
        active_path_trees.size());
    bNodeTree *parent_tree = active_path_trees.last();
    for (const int i : history_tail_path.index_range()) {
      bNodeTree *history_tree = history_tail_path[i];
      if (!find_group_node_in_tree(parent_tree, history_tree)) {
        history_path_trees.resize(active_path_trees.size() + i);
        break;
      }
      parent_tree = history_tree;

      const bool has_group = node_tree_has_group_node(history_tree);
      ui::context_path_add_generic(path,
                                   *RNA_NodeTree,
                                   history_tree,
                                   ICON_NODETREE,
                                   tree_path_navigate_history(history_tail_path.take_front(i + 1)),
                                   has_group ? navigate_menu_draw_fn : nullptr,
                                   true);
    }
  }
  else {
    history_path_trees = active_path_trees;
  }
}

Vector<ui::ContextPathItem> context_path_for_space_node(const bContext &C)
{
  SpaceNode *snode = CTX_wm_space_node(&C);
  if (snode == nullptr) {
    return {};
  }

  Vector<ui::ContextPathItem> context_path;

  if (ED_node_is_geometry(snode)) {
    get_context_path_node_geometry(C, *snode, context_path);
  }
  else if (ED_node_is_shader(snode)) {
    get_context_path_node_shader(C, *snode, context_path);
  }
  else if (ED_node_is_compositor(snode)) {
    get_context_path_node_compositor(C, *snode, context_path);
  }
  context_path_add_history_trees(*snode, context_path);

  return context_path;
}

}  // namespace ed::space_node

}  // namespace blender
