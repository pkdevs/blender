/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BLI_compute_context.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {
struct Main;
}

namespace blender::nodes {

namespace eval_log {
class ViewerNodeLog;
}

namespace debug_view {

/** Prefer a custom node label, then a meaningfully customized node name. */
std::string viewer_name_from_node(StringRef label, StringRef node_name);

/** Decide whether logged Viewer data can replace modifier geometry. */
bool is_geometry_candidate(bool is_debug_view, bool is_shown, bool has_geometry);

/**
 * Hash the original node-tree datablock session UIDs in deterministic outer-to-inner order.
 *
 * Session UIDs are stable for the current Blender session and across memfile undo, but are
 * intentionally regenerated when a file is loaded normally.
 */
ComputeContextHash node_tree_lineage_hash(Span<uint32_t> tree_session_uids);

/** Stable identity of a Viewer node within one Geometry Nodes modifier evaluation. */
struct Identifier {
  ComputeContextHash compute_context_hash;
  ComputeContextHash node_tree_lineage_hash;
  int32_t viewer_node_id = 0;

  uint64_t hash() const;

  friend bool operator==(const Identifier &, const Identifier &) = default;
};

/**
 * A selectable persistent geometry Debug View.
 *
 * The log pointer is only valid while the owning #NodesEvalLog is kept alive. It is never stored
 * as modifier selection state.
 */
struct Candidate {
  Identifier identifier;
  const eval_log::ViewerNodeLog *viewer_log = nullptr;

  /** Node order at every nested group level, followed by the Viewer node order. */
  Vector<int> sort_order;
  /** Human-readable names of the nested group instances. */
  Vector<std::string> context_names;
  /** Empty for the standard Viewer name, which is translated only when presented in the UI. */
  std::string viewer_name;
  /** Original node trees and group-node identifiers used to resolve presentation names live. */
  Vector<uint32_t> tree_session_uids;
  Vector<int32_t> group_node_ids;
};

struct CandidatePresentation {
  std::string display_name;
  std::string full_name;
};

/** Runtime-only enabled state for one Geometry Nodes modifier. */
struct State {
  Set<Identifier> enabled;
  bool initialized = false;
};

/** Sort candidates deterministically and remove duplicate identities. */
void finalize_candidates(Vector<Candidate> &candidates);

/**
 * Resolve labels from original node trees and disambiguate every candidate in one linear pass.
 * Logged names are retained only as a fallback when a tree or node no longer exists.
 */
Vector<CandidatePresentation> candidate_presentations(Main &bmain,
                                                      Span<Candidate> candidates,
                                                      StringRef default_viewer_name);

/** Enable the first available candidate once, without reinitializing an empty state later. */
void initialize_state(Span<Candidate> candidates, State &state);

/** Toggle an available candidate by stable identity. */
bool toggle_candidate(Span<Candidate> candidates, const Identifier &identifier, State &state);

/** Candidate indices enabled in deterministic candidate order. Stale identities are preserved. */
Vector<int> enabled_candidate_indices(Span<Candidate> candidates, const State &state);

/**
 * Stable namespace for this Debug View in a dupli persistent-ID chain.
 *
 * #INT_MAX is reserved as the end marker of a persistent ID.
 */
int persistent_id_namespace(const Identifier &identifier);

/** Reserve a namespace, probing deterministically if another enabled source has the same hash. */
int unique_persistent_id_namespace(const Identifier &identifier, Set<int> &used_namespaces);

}  // namespace debug_view
}  // namespace blender::nodes
