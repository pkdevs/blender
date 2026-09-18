/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include "DNA_space_types.h"

#include "BLI_listbase.hh"
#include "BLI_string_utf8.hh"

#include "BKE_appdir.hh"
#include "BKE_blender_copybuffer.hh"
#include "BKE_blendfile.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_idmap.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_report.hh"

#include "BLO_readfile.hh"

#include "BLI_path_utils.hh"

#include "ED_node.hh"
#include "ED_render.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "DEG_depsgraph_build.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

/* -------------------------------------------------------------------- */
/** \name Local Utilities
 * \{ */

static void node_copybuffer_filepath_get(char filepath[FILE_MAX], size_t filepath_maxncpy)
{
  BLI_path_join(filepath, filepath_maxncpy, BKE_tempdir_base(), "copybuffer_nodes.blend");
}

/** Returns the number of nodes copied. */
static int node_copy_local(bNodeTree &from_tree,
                           bNodeTree &to_tree,
                           const bool allow_duplicate_names,
                           const bool do_user_count,
                           const float2 offset,
                           const bool snap_to_grid,
                           ReportList *reports,
                           bContext *C,
                           const Map<const UString, UString> *io_map = nullptr)
{
  node_select_paired(from_tree);

  Map<const bNode *, bNode *> node_map;
  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  const char *disabled_hint = nullptr;

  for (bNode *node : get_selected_nodes(from_tree)) {
    if (!node->typeinfo->poll_instance ||
        node->typeinfo->poll_instance(node, &to_tree, &disabled_hint))
    {
      int flags = LIB_ID_COPY_DEFAULT;
      if (!do_user_count) {
        flags |= LIB_ID_CREATE_NO_USER_REFCOUNT;
      }

      bNode *new_node = bke::node_copy_with_mapping(
          &to_tree, *node, flags, std::nullopt, std::nullopt, socket_map, allow_duplicate_names);
      node_map.add_new(node, new_node);
      new_node->location[0] += offset.x;
      new_node->location[1] += offset.y;
      if (snap_to_grid) {
        new_node->location[0] = nearest_node_grid_coord(new_node->location[0]);
        new_node->location[1] = nearest_node_grid_coord(new_node->location[1]);
      }
    }
    else {
      if (disabled_hint) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Cannot add node %s into node tree %s: %s",
                    node->name,
                    to_tree.id.name + 2,
                    disabled_hint);
      }
      else {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Cannot add node %s into node tree %s",
                    node->name,
                    to_tree.id.name + 2);
      }
    }
  }

  if (node_map.is_empty()) {
    return 0;
  }

  for (bNode *new_node : node_map.values()) {
    /* Parent pointer must be redirected to new node or detached if parent is not copied. */
    if (new_node->parent) {
      if (node_map.contains(new_node->parent)) {
        new_node->parent = node_map.lookup(new_node->parent);
      }
      else {
        bke::node_detach_node(to_tree, *new_node);
      }
    }
  }

  remap_node_pairing(to_tree, node_map);

  /* Group inputs and outputs from #from_tree and #to_tree don't match, so some form of update is
   * required for the group node to have the correct sockets. */
  BKE_main_ensure_invariants(*CTX_data_main(C), to_tree.id);

  /* Copy links between selected nodes. */
  for (bNodeLink &link : from_tree.links) {
    BLI_assert(link.tonode);
    BLI_assert(link.fromnode);
    if (link.tonode->is_selected() && link.fromnode->is_selected()) {
      if (!node_map.contains(link.tonode) || !node_map.contains(link.fromnode)) {
        /* If copying a node fails, skip copying their links. */
        continue;
      };
      bNode *from_node = node_map.lookup(link.fromnode);
      bNode *to_node = node_map.lookup(link.tonode);

      bNodeSocket *from = nullptr;
      bNodeSocket *to = nullptr;

      if (io_map) {
        if (link.fromnode->is_group_input() && io_map->contains(link.fromsock->identifier_ustr()))
        {
          UString from_socket_identifier{io_map->lookup(link.fromsock->identifier_ustr())};
          if (from_socket_identifier.is_empty()) {
            continue;
          }
          from = bke::node_find_socket(*from_node, SOCK_OUT, from_socket_identifier);
        }
        if (link.tonode->is_group_output() && io_map->contains(link.tosock->identifier_ustr())) {
          UString to_socket_identifier{io_map->lookup(link.tosock->identifier_ustr())};
          if (to_socket_identifier.is_empty()) {
            continue;
          }
          to = bke::node_find_socket(*to_node, SOCK_IN, to_socket_identifier);
        }
      }

      if (!from) {
        from = bke::node_find_socket(*from_node, SOCK_OUT, link.fromsock->identifier_ustr());
      }
      if (!to) {
        to = bke::node_find_socket(*to_node, SOCK_IN, link.tosock->identifier_ustr());
      }
      if (!from || !to) {
        continue;
      }

      bNodeLink &new_link = bke::node_add_link(to_tree, *from_node, *from, *to_node, *to);
      new_link.multi_input_sort_id = link.multi_input_sort_id;
    }
  }

  to_tree.ensure_topology_cache();
  for (bNode *new_node : node_map.values()) {
    /* Update multi input socket indices in case all connected nodes weren't copied. */
    update_multi_input_indices_for_removed_links(*new_node);
  }

  return node_map.size();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy
 * \{ */

static wmOperatorStatus node_clipboard_copy_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::blendfile;

  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree *node_tree = snode->edittree;

  PartialWriteContext copy_buffer{*bmain};
  bNodeTree *copy_tree = reinterpret_cast<bNodeTree *>(
      copy_buffer.id_create(ID_NT,
                            "Copy Tree",
                            nullptr,
                            {(PartialWriteContext::IDAddOperations::SET_FAKE_USER |
                              PartialWriteContext::IDAddOperations::SET_CLIPBOARD_MARK)}));

  STRNCPY_UTF8(copy_tree->idname, node_tree->typeinfo->idname.c_str());
  bke::node_tree_set_type(*copy_tree);

  if (copy_tree->id.properties == nullptr) {
    copy_tree->id.properties = IDP_New(IDP_GROUP, nullptr, "ID_Properties");
  }

  /* FIXME: Store the original tree's id name as String IDP property. This can cause problems if
   * trees are renamed or have the same name. */
  IDProperty *id_name_prop = IDP_NewString(node_tree->id.name, "clipboard_source_id_name");
  IDP_AddToGroup(copy_tree->id.properties, id_name_prop);

  /* Copy node interface to avoid losing links to Group Input and Group Output nodes.
   * Note: this doesn't create new interface items if they don't exist. */
  /* #bNodeTreeInterface::copy_data() allocates runtime memory, so we need to free it to avoid
   * memory leak. */
  copy_tree->tree_interface.free_data();
  copy_tree->tree_interface.copy_data(node_tree->tree_interface, LIB_ID_COPY_DEFAULT);

  const int num_copied = node_copy_local(
      *node_tree, *copy_tree, true, false, float2(0), false, op->reports, C);
  if (num_copied == 0) {
    return OPERATOR_CANCELLED;
  }

  auto add_tree_ids_dependencies_cb = [&copy_buffer,
                                       copy_tree](LibraryIDLinkCallbackData *cb_data) -> int {
    /* Embedded or null IDs usages can be ignored here. */
    if (cb_data->cb_flag & (IDWALK_CB_EMBEDDED | IDWALK_CB_EMBEDDED_NOT_OWNING)) {
      return IDWALK_RET_NOP;
    }
    ID *id_src = *cb_data->id_pointer;
    if (!id_src) {
      return IDWALK_RET_NOP;
    }

    ID *id_dst = nullptr;

    if (id_src == &copy_tree->id) {
      /* #copy_tree is just a container for the copied nodes, so it can be safely ignored. */
      return IDWALK_RET_NOP;
    }

    auto partial_write_dependencies_filter_cb = [](LibraryIDLinkCallbackData *cb_deps_data,
                                                   PartialWriteContext::IDAddOptions /*options*/) {
      ID *id_deps_src = *cb_deps_data->id_pointer;
      const ID_Type id_type = id_deps_src->id_type();
      if (id_type == ID_SCE) {
        /* Note: Scenes referenced in the Render Layers node are cleared. At this stage, we
         * don't know if the target blender instance will have a scene with identical name, so
         * they are saved in the copy buffer as empty scenes. The pasting code deletes the
         * extra scenes, see #node_clipboard_paste_exec(). */
        return PartialWriteContext::IDAddOperations::CLEAR_DEPENDENCIES;
      }
      /* All ID datablocks exposed through nodes are added here. */
      return PartialWriteContext::IDAddOperations::ADD_DEPENDENCIES;
    };

    id_dst = copy_buffer.id_add(
        id_src, {PartialWriteContext::IDAddOperations::NOP}, partial_write_dependencies_filter_cb);

    *cb_data->id_pointer = id_dst;
    return IDWALK_RET_NOP;
  };

  BKE_library_foreach_ID_link(
      nullptr, &copy_tree->id, add_tree_ids_dependencies_cb, nullptr, IDWALK_NOP);

  char filepath[FILE_MAX];
  node_copybuffer_filepath_get(filepath, sizeof(filepath));
  if (!copy_buffer.write_as_copypaste_buffer(filepath, *op->reports)) {
    BLI_assert_unreachable();
    BKE_report(op->reports, RPT_ERROR, "Unable to write to copy buffer on disk.");
    return OPERATOR_CANCELLED;
  };

  BKE_reportf(op->reports, RPT_INFO, "Copied %d selected node(s)", num_copied);
  return OPERATOR_FINISHED;
}

void NODE_OT_clipboard_copy(wmOperatorType *ot)
{
  ot->name = "Copy to Clipboard";
  ot->description = "Copy the selected nodes to the internal clipboard";
  ot->idname = "NODE_OT_clipboard_copy";

  ot->exec = node_clipboard_copy_exec;
  ot->poll = ED_operator_node_active;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paste
 * \{ */

static StringRef scene_lib_filepath(const Scene &scene)
{
  if (scene.id.lib && scene.id.lib->runtime) {
    return scene.id.lib->runtime->filepath_abs;
  }
  return "";
}

/**
 * @brief
 * Adds interface item to #tree and all the panels the item may be nested in.
 *
 * @param tree bNodeTree to add the #item to.
 * @param item Item to be added.
 * @param panel_map Map of an item to its parent panel.
 * @param added_panels Map of a panel to the copy that was already added to the tree.
 */
static bNodeTreeInterfaceItem *add_item_and_panels_recursive(
    bNodeTree &tree,
    const bNodeTreeInterfaceItem *item,
    const Map<const bNodeTreeInterfaceItem *, const bNodeTreeInterfaceItem *> &panel_map,
    Map<const bNodeTreeInterfaceItem *, bNodeTreeInterfaceItem *> &added_panels)
{
  if (added_panels.contains(item)) {
    return added_panels.lookup(item);
  }

  bNodeTreeInterfacePanel *parent_panel{nullptr};

  if (panel_map.contains(item)) {
    const bNodeTreeInterfaceItem *parent{panel_map.lookup(item)};
    parent_panel = reinterpret_cast<bNodeTreeInterfacePanel *>(
        add_item_and_panels_recursive(tree, parent, panel_map, added_panels));
  }

  if (bke::node_interface::detail::item_is_type<bNodeTreeInterfacePanel>(*item)) {
    const auto *panel{reinterpret_cast<const bNodeTreeInterfacePanel *>(item)};
    bNodeTreeInterfacePanel *added_panel{tree.tree_interface.add_panel(
        panel->name(), panel->description(), panel->flag, parent_panel)};
    added_panels.add(item, &added_panel->item);
    return &added_panel->item;
  }
  else {
    BLI_assert(bke::node_interface::detail::item_is_type<bNodeTreeInterfaceSocket>(*item));
    return tree.tree_interface.add_item_copy(*item, parent_panel);
  }
}

static bool isUsedInterfaceSocket(const bNodeTreeInterfaceSocket &socket, const bNodeTree &tree)
{
  if (socket.flag & NODE_INTERFACE_SOCKET_INPUT) {
    for (const bNode *input_node : tree.group_input_nodes()) {
      const bNodeSocket *input{input_node->output_by_identifier(UString(socket.identifier))};
      if (input->is_directly_linked()) {
        return true;
      }
    }
  }
  else if (socket.flag & NODE_INTERFACE_SOCKET_OUTPUT) {
    for (const bNode *output_node : tree.group_output_nodes()) {
      const bNodeSocket *output{output_node->input_by_identifier(UString(socket.identifier))};
      if (output->is_directly_linked()) {
        return true;
      }
    }
  }
  return false;
}

static wmOperatorStatus node_clipboard_paste_exec(bContext *C, wmOperator *op)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  Main *bmain_dst = CTX_data_main(C);

  char filepath[FILE_MAX];
  node_copybuffer_filepath_get(filepath, sizeof(filepath));
  Main *bmain_src = BKE_copybuffer_read(*bmain_dst, filepath, op->reports, FILTER_ID_NT);
  if (!bmain_src) {
    BKE_report(op->reports, RPT_ERROR, "No data to paste");
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  /* We don't want to paste scenes referenced by the Render Layers node if they don't exist in the
   * destination bmain. */
  Set<std::pair<StringRef, StringRef>> dst_scenes;
  for (Scene &scene : bmain_dst->scenes) {
    /* Packed scenes are currently not needed so they are skipped.
     * TODO: Support packed scenes. */
    if (ID_IS_PACKED(&scene.id)) {
      continue;
    }
    dst_scenes.add({scene.id.name, scene_lib_filepath(scene)});
  }

  for (Scene &scene : bmain_src->scenes.items_mutable()) {
    if (ID_IS_PACKED(&scene.id)) {
      continue;
    }
    /* All scenes that will be added through merging the two bmains are removed. */
    if (!dst_scenes.contains({scene.id.name, scene_lib_filepath(scene)})) {
      BKE_id_delete(bmain_src, &scene.id);
    }
  }

  bNodeTree *from_tree = nullptr;
  FOREACH_NODETREE_BEGIN (bmain_src, node_tree, id) {
    if (node_tree->id.flag & ID_FLAG_CLIPBOARD_MARK) {
      from_tree = node_tree;
      break;
    }
  }
  FOREACH_NODETREE_END;
  if (from_tree == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No data to paste");
    BKE_main_free(bmain_src);
    return OPERATOR_CANCELLED;
  }

  MainMergeReport merge_reports = {};
  /* We need to ensure that the source 'clipboard marked' main NodeTree is always merged into
   * destination Main, even in case there would be a name collision with an existing ID (see also
   * #158049). */
  Set<ID *> force_merge_ids = {id_cast<ID *>(from_tree)};
  /* Frees bmain_src. */
  BKE_main_merge(bmain_dst, &force_merge_ids, &bmain_src, merge_reports);

  bNodeTree *to_tree = snode->edittree;
  node_deselect_all(*to_tree);

  from_tree->ensure_topology_cache();

  /* Adds used interface sockets in order. */
  VectorSet<const bNodeTreeInterfaceItem *> used_io_items{};
  for (const bNodeTreeInterfaceItem *iface_item : from_tree->interface_items()) {
    if (auto *iface_socket = bke::node_interface::get_item_as<bNodeTreeInterfaceSocket>(
            iface_item))
    {
      if (isUsedInterfaceSocket(*iface_socket, *from_tree)) {
        used_io_items.add(iface_item);
      }
    }
  }

  BLI_assert(from_tree->id.properties);
  const std::optional<StringRef> &source_id_name = IDP_group_lookup_string(
      *from_tree->id.properties, "clipboard_source_id_name");
  BLI_assert(source_id_name);
  bool is_same_tree{STREQ(source_id_name.value().data(), to_tree->id.name)};

  /* Adds io sockets and panels to tree and maps old sockets to new ones. */

  /* Maps all interface items to the panel they belong to */
  Map<const bNodeTreeInterfaceItem *, const bNodeTreeInterfaceItem *> panel_map;
  for (const bNodeTreeInterfacePanel *panel : from_tree->interface_panels()) {
    for (const bNodeTreeInterfaceItem *item : panel->items()) {
      panel_map.add(item, &panel->item);
    }
  }
  Map<const bNodeTreeInterfaceItem *, bNodeTreeInterfaceItem *> added_panels;

  Map<const UString, UString> iface_socket_map;
  for (const bNodeTreeInterfaceItem *src_io_item : used_io_items) {
    auto *src_io_socket{reinterpret_cast<const bNodeTreeInterfaceSocket *>(src_io_item)};
    /* Ensures default behaviour of copying and pasting within one node tree stays the same. */
    if (is_same_tree) {
      if (bke::node_find_interface_input_by_identifier(*to_tree, src_io_socket->identifier) ||
          bke::node_find_interface_output_by_identifier(*to_tree, src_io_socket->identifier))
      {
        continue;
      }
      /* If the socket is not found, the interface has changed and the socket needs to be readded.
       */
    }
    /* If the node tree doesn't support this socket type, don't add it and map it to an empty
     * string. */
    if (!bke::node_tree_type_supports_socket_type_static(to_tree->type,
                                                         src_io_socket->socket_typeinfo()->type))
    {
      iface_socket_map.add_new(UString(src_io_socket->identifier), UString());
      continue;
    }

    auto *dst_io_socket{reinterpret_cast<bNodeTreeInterfaceSocket *>(
        add_item_and_panels_recursive(*to_tree, src_io_item, panel_map, added_panels))};
    iface_socket_map.add_new(UString(src_io_socket->identifier),
                             UString(dst_io_socket->identifier));
  }

  BKE_main_ensure_invariants(*CTX_data_main(C), to_tree->id);

  float2 offset(0);
  PropertyRNA *offset_prop = RNA_struct_find_property(op->ptr, "offset");
  if (RNA_property_is_set(op->ptr, offset_prop)) {
    rctf bbox;
    float2 center;
    BLI_rctf_init_minmax(&bbox);
    for (bNode *node : from_tree->all_nodes()) {
      bbox.xmin = math::min(node->location[0], bbox.xmin);
      bbox.ymin = math::min(node->location[1], bbox.ymin);

      bbox.xmax = math::max(node->location[0] + node->width, bbox.xmax);
      bbox.ymax = math::max(node->location[1] - node->height / 2, bbox.ymax);
    }
    center.x = BLI_rctf_cent_x(&bbox);
    center.y = BLI_rctf_cent_y(&bbox);

    float2 mouse_location;
    RNA_property_float_get_array(op->ptr, offset_prop, mouse_location);
    offset = mouse_location / UI_SCALE_FAC - center;
  }

  const bool snap_to_grid = CTX_data_scene(C)->toolsettings->snap_flag_node & SCE_SNAP;
  const int num_copied = node_copy_local(
      *from_tree, *to_tree, false, true, offset, snap_to_grid, op->reports, C, &iface_socket_map);
  if (num_copied == 0) {
    /* Note: we don't return OPERATOR_CANCELLED here although the copy fails to avoid corrupting
     * the undo stack after merging two bmains. */
  };
  BKE_id_delete(bmain_dst, &from_tree->id);

  BKE_main_ensure_invariants(*bmain_dst);
  /* Pasting nodes can create arbitrary new relations because nodes can reference IDs. */
  DEG_relations_tag_update(bmain_dst);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus node_clipboard_paste_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  float2 cursor;
  ui::view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &cursor.x, &cursor.y);
  RNA_float_set_array(op->ptr, "offset", cursor);
  return node_clipboard_paste_exec(C, op);
}

void NODE_OT_clipboard_paste(wmOperatorType *ot)
{
  ot->name = "Paste from Clipboard";
  ot->description = "Paste nodes from the internal clipboard to the active node tree";
  ot->idname = "NODE_OT_clipboard_paste";

  ot->invoke = node_clipboard_paste_invoke;
  ot->exec = node_clipboard_paste_exec;
  ot->poll = ED_operator_node_editable;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_float_array(
      ot->srna,
      "offset",
      2,
      nullptr,
      -FLT_MAX,
      FLT_MAX,
      "Location",
      "The 2D view location for the center of the new nodes, or unchanged if not set",
      -FLT_MAX,
      FLT_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

}  // namespace blender::ed::space_node
