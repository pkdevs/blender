/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <bit>
#include <climits>
#include <optional>

#include "BLI_hash.hh"
#include "BLI_hash_c.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_zones.hh"

#include "NOD_geo_viewer.hh"
#include "NOD_geometry_nodes_debug_view.hh"

#include "DEG_depsgraph_query.hh"

namespace blender::nodes::debug_view {

static std::optional<int> group_input_index(const bNodeTree &tree, const bNodeSocket &socket)
{
  if (!socket.owner_node().is_group_input() || socket.type != SOCK_BOOLEAN) {
    return std::nullopt;
  }
  const int index = socket.index();
  if (index < 0 || index >= tree.interface_inputs().size()) {
    return std::nullopt;
  }
  return index;
}

static bool is_inside_zone(const bNodeTree &tree, const bNode &node)
{
  const bke::bNodeTreeZones *zones = tree.zones();
  return zones && zones->get_zone_by_node(node.identifier) != nullptr;
}

static TreeDebugViewPropagation build_tree_debug_view_propagation(bNodeTree &tree)
{
  tree.ensure_topology_cache();
  tree.ensure_interface_cache();

  TreeDebugViewPropagation propagation;

  /* Compute structural availability during tree updates, never during modifier UI redraws.
   * Mirror the supported contexts of MOD_nodes' persistent side-effect collection. */
  const bke::bNodeTreeZones *zones = tree.zones();
  if (zones) {
    for (const bNode *node : tree.all_nodes()) {
      if (node->is_muted()) {
        continue;
      }
      const bke::bNodeTreeZone *zone = zones->get_zone_by_node(node->identifier);
      bool supported = true;
      for (const bke::bNodeTreeZone *parent = zone; parent; parent = parent->parent_zone) {
        const bNode *output = parent->output_node();
        if (!output || output->type_legacy != GEO_NODE_SIMULATION_OUTPUT) {
          supported = false;
          break;
        }
      }
      if (!supported) {
        continue;
      }
      if (node->is_type("GeometryNodeViewer"_ustr)) {
        const auto &storage = *static_cast<const NodeGeometryViewer *>(node->storage);
        if (storage.flag & NODE_GEO_VIEWER_FLAG_DEBUG_VIEW) {
          propagation.has_debug_views = true;
          propagation.has_nested_debug_views = true;
          propagation.has_simulation_debug_views |= zone != nullptr;
        }
      }
      else if (node->is_group() && node->id) {
        const bNodeTree &group = *reinterpret_cast<const bNodeTree *>(node->id);
        const auto *child = DEG_get_original(&group)->runtime->debug_view_propagation.get();
        if (!child) {
          continue;
        }
        propagation.has_nested_debug_views |= child->has_nested_debug_views;
        propagation.has_simulation_debug_views |= child->has_simulation_debug_views ||
                                                  (zone && child->has_nested_debug_views);
        for (const int input : child->viewers_by_group_input.keys()) {
          if (input < node->input_sockets().size() && node->input_socket(input).is_available()) {
            propagation.has_debug_views = true;
            break;
          }
        }
      }
    }
  }
  propagation.has_debug_views |= propagation.has_simulation_debug_views;

  /* A direct Group Input -> Viewer.Show link is the deliberately conservative exposure rule. */
  for (const bNode *node : tree.nodes_by_type("GeometryNodeViewer"_ustr)) {
    if (node->is_muted() || is_inside_zone(tree, *node)) {
      continue;
    }
    const auto &storage = *static_cast<const NodeGeometryViewer *>(node->storage);
    if (!(storage.flag & NODE_GEO_VIEWER_FLAG_DEBUG_VIEW)) {
      continue;
    }
    const bNodeSocket &show_socket = *node->input_by_identifier("Show"_ustr);
    for (const bNodeLink *link : show_socket.directly_linked_links()) {
      if (!link->is_used()) {
        continue;
      }
      const std::optional<int> input_index = group_input_index(tree, *link->fromsock);
      if (!input_index) {
        continue;
      }
      propagation.viewers_by_group_input.add(*input_index, {node->identifier, {}});
    }
  }

  /* Propagate only when a child group input is directly fed by this tree's Group Input. */
  for (const bNode *node : tree.all_nodes()) {
    if (node->is_muted() || !node->is_group() || node->id == nullptr ||
        is_inside_zone(tree, *node))
    {
      continue;
    }
    const bNodeTree &group_tree = *reinterpret_cast<const bNodeTree *>(node->id);
    const bNodeTree &propagation_tree = *DEG_get_original(&group_tree);
    const TreeDebugViewPropagation *group_propagation =
        propagation_tree.runtime->debug_view_propagation.get();
    if (group_propagation == nullptr) {
      continue;
    }

    for (const auto item : group_propagation->viewers_by_group_input.items()) {
      if (item.key < 0 || item.key >= node->input_sockets().size()) {
        continue;
      }
      const bNodeSocket &group_input_socket = node->input_socket(item.key);
      if (!group_input_socket.is_available()) {
        continue;
      }
      for (const bNodeLink *link : group_input_socket.directly_linked_links()) {
        if (!link->is_used()) {
          continue;
        }
        const std::optional<int> input_index = group_input_index(tree, *link->fromsock);
        if (!input_index) {
          continue;
        }
        for (const PropagatedViewer &viewer : item.value) {
          PropagatedViewer propagated = viewer;
          propagated.group_node_path.prepend(node->identifier);
          propagation.viewers_by_group_input.add(*input_index, std::move(propagated));
        }
      }
    }
  }

  return propagation;
}

bool update_tree_debug_view_propagation(bNodeTree &tree)
{
  tree.ensure_topology_cache();
  if (tree.has_available_link_cycle()) {
    const bool changed = tree.runtime->debug_view_propagation != nullptr;
    tree.runtime->debug_view_propagation.reset();
    return changed;
  }

  TreeDebugViewPropagation new_propagation = build_tree_debug_view_propagation(tree);
  const bool changed = tree.runtime->debug_view_propagation ?
                           *tree.runtime->debug_view_propagation != new_propagation :
                           true;
  tree.runtime->debug_view_propagation = std::make_unique<TreeDebugViewPropagation>(
      std::move(new_propagation));
  return changed;
}

static int persistent_id_namespace(const ComputeContextHash &compute_context_hash,
                                   const int32_t viewer_node_id)
{
  uint hash = BLI_hash_int_2d(uint(compute_context_hash.v1), uint(compute_context_hash.v1 >> 32));
  hash = BLI_hash_int_2d(hash, uint(compute_context_hash.v2));
  hash = BLI_hash_int_2d(hash, uint(compute_context_hash.v2 >> 32));
  hash = BLI_hash_int_2d(hash, std::bit_cast<uint>(viewer_node_id));
  if (hash == uint(INT_MAX)) {
    hash = uint(INT_MIN);
  }
  return std::bit_cast<int>(hash);
}

int unique_persistent_id_namespace(const ComputeContextHash &compute_context_hash,
                                   const int32_t viewer_node_id,
                                   Set<int> &used_namespaces)
{
  uint namespace_bits = std::bit_cast<uint>(
      persistent_id_namespace(compute_context_hash, viewer_node_id));
  while (!used_namespaces.add(std::bit_cast<int>(namespace_bits))) {
    /* Resolve a 32-bit hash collision deterministically so simultaneously generated sources can
     * never receive the same top-level namespace. */
    do {
      namespace_bits++;
    } while (namespace_bits == uint(INT_MAX));
  }
  return std::bit_cast<int>(namespace_bits);
}

}  // namespace blender::nodes::debug_view
