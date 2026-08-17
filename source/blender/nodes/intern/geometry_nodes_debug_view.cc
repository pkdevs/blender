/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <bit>
#include <climits>
#include <optional>

#include <fmt/format.h>

#include "BLI_hash.hh"
#include "BLI_hash_c.hh"
#include "BLI_map.hh"
#include "BLI_string_utils.hh"

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

  /* A direct Group Input -> Viewer.Show link is the deliberately conservative exposure rule. */
  for (const bNode *node : tree.nodes_by_type("GeometryNodeViewer"_ustr)) {
    if (node->is_muted() || is_inside_zone(tree, *node)) {
      continue;
    }
    const auto &storage = *static_cast<const NodeGeometryViewer *>(node->storage);
    if (!(storage.flag & NODE_GEO_VIEWER_FLAG_DEBUG_VIEW)) {
      continue;
    }
    const bNodeSocket &show_socket = node->input_socket(storage.items_num);
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

uint64_t Identifier::hash() const
{
  return get_default_hash(compute_context_hash, node_tree_lineage_hash, viewer_node_id);
}

ComputeContextHash node_tree_lineage_hash(const Span<uint32_t> tree_session_uids)
{
  return ComputeContextHash::from_bytes(tree_session_uids.data(),
                                        tree_session_uids.size_in_bytes());
}

std::string viewer_name_from_node(const StringRef label, const StringRef node_name)
{
  if (!label.is_empty()) {
    return label;
  }
  int name_number;
  const StringRef name_base = BLI_string_split_name_number(node_name, '.', name_number);
  if (name_base != "Viewer") {
    return node_name;
  }
  return {};
}

bool is_geometry_candidate(const bool is_debug_view, const bool is_shown, const bool has_geometry)
{
  return is_debug_view && is_shown && has_geometry;
}

static bool candidate_less(const Candidate &a, const Candidate &b)
{
  if (std::lexicographical_compare(
          a.sort_order.begin(), a.sort_order.end(), b.sort_order.begin(), b.sort_order.end()))
  {
    return true;
  }
  if (std::lexicographical_compare(
          b.sort_order.begin(), b.sort_order.end(), a.sort_order.begin(), a.sort_order.end()))
  {
    return false;
  }
  if (a.identifier.compute_context_hash.v1 != b.identifier.compute_context_hash.v1) {
    return a.identifier.compute_context_hash.v1 < b.identifier.compute_context_hash.v1;
  }
  if (a.identifier.compute_context_hash.v2 != b.identifier.compute_context_hash.v2) {
    return a.identifier.compute_context_hash.v2 < b.identifier.compute_context_hash.v2;
  }
  if (a.identifier.node_tree_lineage_hash.v1 != b.identifier.node_tree_lineage_hash.v1) {
    return a.identifier.node_tree_lineage_hash.v1 < b.identifier.node_tree_lineage_hash.v1;
  }
  if (a.identifier.node_tree_lineage_hash.v2 != b.identifier.node_tree_lineage_hash.v2) {
    return a.identifier.node_tree_lineage_hash.v2 < b.identifier.node_tree_lineage_hash.v2;
  }
  return a.identifier.viewer_node_id < b.identifier.viewer_node_id;
}

Vector<CandidatePresentation> candidate_presentations(const Span<Candidate> candidates,
                                                      const StringRef default_viewer_name)
{
  Vector<std::string> viewer_names;
  Vector<std::string> full_names;
  viewer_names.reserve(candidates.size());
  full_names.reserve(candidates.size());
  Map<std::string, int> viewer_name_counts;
  for (const Candidate &candidate : candidates) {
    const std::string viewer_name = candidate.viewer_name.empty() ?
                                        std::string(default_viewer_name) :
                                        candidate.viewer_name;
    viewer_name_counts.lookup_or_add(viewer_name, 0)++;
    std::string full_name;
    for (const std::string &context_name : candidate.context_names) {
      if (!context_name.empty()) {
        if (!full_name.empty()) {
          full_name += " / ";
        }
        full_name += context_name;
      }
    }
    if (!full_name.empty()) {
      full_name += " / ";
    }
    full_name += viewer_name;
    viewer_names.append(std::move(viewer_name));
    full_names.append(std::move(full_name));
  }

  Vector<CandidatePresentation> presentations;
  presentations.reserve(candidates.size());
  Map<std::string, int> full_name_counts;
  for (const int i : candidates.index_range()) {
    CandidatePresentation presentation;
    presentation.full_name = full_names[i];
    if (viewer_name_counts.lookup(viewer_names[i]) == 1) {
      presentation.display_name = viewer_names[i];
    }
    else {
      const int duplicate_number = ++full_name_counts.lookup_or_add(full_names[i], 0);
      presentation.display_name = full_names[i];
      if (duplicate_number > 1) {
        presentation.display_name = fmt::format(
            "{} ({})", presentation.display_name, duplicate_number);
      }
    }
    presentations.append(std::move(presentation));
  }
  return presentations;
}

void finalize_candidates(Vector<Candidate> &candidates)
{
  std::ranges::sort(candidates, candidate_less);

  Vector<Candidate> unique_candidates;
  unique_candidates.reserve(candidates.size());
  for (Candidate &candidate : candidates) {
    if (!unique_candidates.is_empty() &&
        unique_candidates.last().identifier == candidate.identifier)
    {
      continue;
    }
    unique_candidates.append(std::move(candidate));
  }
  candidates = std::move(unique_candidates);
}

int persistent_id_namespace(const Identifier &identifier)
{
  uint hash = BLI_hash_int_2d(uint(identifier.compute_context_hash.v1),
                              uint(identifier.compute_context_hash.v1 >> 32));
  hash = BLI_hash_int_2d(hash, uint(identifier.compute_context_hash.v2));
  hash = BLI_hash_int_2d(hash, uint(identifier.compute_context_hash.v2 >> 32));
  hash = BLI_hash_int_2d(hash, uint(identifier.node_tree_lineage_hash.v1));
  hash = BLI_hash_int_2d(hash, uint(identifier.node_tree_lineage_hash.v1 >> 32));
  hash = BLI_hash_int_2d(hash, uint(identifier.node_tree_lineage_hash.v2));
  hash = BLI_hash_int_2d(hash, uint(identifier.node_tree_lineage_hash.v2 >> 32));
  hash = BLI_hash_int_2d(hash, std::bit_cast<uint>(identifier.viewer_node_id));
  if (hash == uint(INT_MAX)) {
    hash = uint(INT_MIN);
  }
  return std::bit_cast<int>(hash);
}

int unique_persistent_id_namespace(const Identifier &identifier, Set<int> &used_namespaces)
{
  uint namespace_bits = std::bit_cast<uint>(persistent_id_namespace(identifier));
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
