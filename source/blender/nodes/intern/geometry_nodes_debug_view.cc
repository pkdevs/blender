/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <bit>
#include <climits>

#include <fmt/format.h>

#include "BLI_hash.hh"
#include "BLI_hash_c.hh"
#include "BLI_listbase.hh"
#include "BLI_string_utils.hh"

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "MOD_nodes.hh"

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

static StringRef candidate_viewer_name(const Candidate &candidate,
                                       const StringRef default_viewer_name)
{
  return candidate.viewer_name.empty() ? default_viewer_name : StringRef(candidate.viewer_name);
}

std::string candidate_full_name(const Candidate &candidate, const StringRef default_viewer_name)
{
  std::string name;
  for (const std::string &context_name : candidate.context_names) {
    if (!name.empty()) {
      name += " / ";
    }
    name += context_name;
  }
  if (!name.empty()) {
    name += " / ";
  }
  name += candidate_viewer_name(candidate, default_viewer_name);
  return name;
}

std::string candidate_display_name(const Span<Candidate> candidates,
                                   const int candidate_index,
                                   const StringRef default_viewer_name)
{
  if (!candidates.index_range().contains(candidate_index)) {
    return {};
  }
  const Candidate &candidate = candidates[candidate_index];
  const StringRef viewer_name = candidate_viewer_name(candidate, default_viewer_name);
  const bool has_duplicate_name = std::ranges::any_of(candidates, [&](const Candidate &other) {
    return &other != &candidate &&
           candidate_viewer_name(other, default_viewer_name) == viewer_name;
  });
  if (!has_duplicate_name) {
    return viewer_name;
  }

  std::string display_name = candidate_full_name(candidate, default_viewer_name);
  int duplicate_number = 1;
  for (const int previous_i : IndexRange(candidate_index)) {
    const Candidate &previous = candidates[previous_i];
    if (candidate_viewer_name(previous, default_viewer_name) == viewer_name &&
        candidate_full_name(previous, default_viewer_name) == display_name)
    {
      duplicate_number++;
    }
  }
  if (duplicate_number > 1) {
    display_name = fmt::format("{} ({})", display_name, duplicate_number);
  }
  return display_name;
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

void preserve_states_on_undo(const Object &old_object, Object &new_object)
{
  for (ModifierData &new_md : new_object.modifiers) {
    if (new_md.type != eModifierType_Nodes) {
      continue;
    }
    const ModifierData *old_md = nullptr;
    for (const ModifierData &candidate_md : old_object.modifiers) {
      if (candidate_md.persistent_uid == new_md.persistent_uid) {
        old_md = &candidate_md;
        break;
      }
    }
    if (old_md == nullptr || old_md->type != eModifierType_Nodes) {
      continue;
    }

    const auto &old_nmd = reinterpret_cast<const NodesModifierData &>(*old_md);
    auto &new_nmd = reinterpret_cast<NodesModifierData &>(new_md);
    if (old_nmd.runtime != nullptr && new_nmd.runtime != nullptr) {
      new_nmd.runtime->debug_view_state = old_nmd.runtime->debug_view_state;
    }
  }
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

bool persistent_id_namespace_fits(const int current_depth, const int max_depth)
{
  return current_depth >= 0 && current_depth < max_depth - 1;
}

}  // namespace blender::nodes::debug_view
