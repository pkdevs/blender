/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <bit>
#include <climits>

#include <fmt/format.h>

#include "BLI_hash.hh"
#include "BLI_hash_c.hh"
#include "BLI_map.hh"
#include "BLI_string_utils.hh"

#include "DNA_node_types.h"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "NOD_geometry_nodes_debug_view.hh"

namespace blender::nodes::debug_view {

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

static std::string live_context_name(Main &bmain,
                                     const Candidate &candidate,
                                     const int context_index)
{
  if (candidate.tree_session_uids.index_range().contains(context_index) &&
      candidate.group_node_ids.index_range().contains(context_index))
  {
    const auto *tree = id_cast<bNodeTree *>(
        BKE_libblock_find_session_uid(&bmain, ID_NT, candidate.tree_session_uids[context_index]));
    const bNode *group_node = tree ? tree->node_by_id(candidate.group_node_ids[context_index]) :
                                     nullptr;
    if (group_node && group_node->type_legacy == NODE_GROUP && group_node->id &&
        candidate.tree_session_uids.index_range().contains(context_index + 1) &&
        group_node->id->session_uid == candidate.tree_session_uids[context_index + 1])
    {
      if (group_node->label[0] != '\0') {
        return group_node->label;
      }
      return BKE_id_name(*group_node->id);
    }
  }
  return candidate.context_names.index_range().contains(context_index) ?
             candidate.context_names[context_index] :
             std::string();
}

static std::string live_viewer_name(Main &bmain,
                                    const Candidate &candidate,
                                    const StringRef default_viewer_name)
{
  if (!candidate.tree_session_uids.is_empty()) {
    const auto *tree = id_cast<bNodeTree *>(
        BKE_libblock_find_session_uid(&bmain, ID_NT, candidate.tree_session_uids.last()));
    const bNode *viewer_node = tree ? tree->node_by_id(candidate.identifier.viewer_node_id) :
                                      nullptr;
    if (viewer_node && viewer_node->type_legacy == GEO_NODE_VIEWER) {
      const std::string name = viewer_name_from_node(viewer_node->label, viewer_node->name);
      return name.empty() ? std::string(default_viewer_name) : name;
    }
  }
  return candidate.viewer_name.empty() ? std::string(default_viewer_name) : candidate.viewer_name;
}

Vector<CandidatePresentation> candidate_presentations(Main &bmain,
                                                      const Span<Candidate> candidates,
                                                      const StringRef default_viewer_name)
{
  Vector<std::string> viewer_names;
  Vector<std::string> full_names;
  viewer_names.reserve(candidates.size());
  full_names.reserve(candidates.size());
  Map<std::string, int> viewer_name_counts;
  for (const Candidate &candidate : candidates) {
    std::string viewer_name = live_viewer_name(bmain, candidate, default_viewer_name);
    viewer_name_counts.lookup_or_add(viewer_name, 0)++;
    std::string full_name;
    const int context_num = std::max(candidate.group_node_ids.size(),
                                     candidate.context_names.size());
    for (const int i : IndexRange(context_num)) {
      const std::string context_name = live_context_name(bmain, candidate, i);
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

void initialize_state(const Span<Candidate> candidates, State &state)
{
  if (state.initialized || candidates.is_empty()) {
    return;
  }
  state.enabled.add(candidates.first().identifier);
  state.initialized = true;
}

bool toggle_candidate(const Span<Candidate> candidates, const Identifier &identifier, State &state)
{
  initialize_state(candidates, state);
  const bool is_available = std::ranges::any_of(
      candidates, [&](const Candidate &candidate) { return candidate.identifier == identifier; });
  if (!is_available) {
    return false;
  }
  if (!state.enabled.remove(identifier)) {
    state.enabled.add_new(identifier);
  }
  return true;
}

Vector<int> enabled_candidate_indices(const Span<Candidate> candidates, const State &state)
{
  Vector<int> indices;
  for (const int i : candidates.index_range()) {
    if (state.enabled.contains(candidates[i].identifier)) {
      indices.append(i);
    }
  }
  return indices;
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
