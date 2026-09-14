/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_compute_context.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

namespace blender {

struct bNodeTree;

namespace nodes {

namespace eval_log {
class ViewerNodeLog;
}

namespace debug_view {

/**
 * A Debug Viewer that can be exposed through one of the node tree's group inputs.
 *
 * The group node path starts in the tree that owns the group input and ends in the tree that owns
 * the Viewer node. The path is empty for a local Viewer directly connected to a Group Input node.
 */
struct PropagatedViewer {
  int32_t viewer_node_id = 0;
  Vector<int32_t> group_node_path;

  friend bool operator==(const PropagatedViewer &, const PropagatedViewer &) = default;
};

/** Static Debug View exposure information cached on a node tree. */
struct TreeDebugViewPropagation {
  /** Supported views when this tree is used as a modifier, independent of Show values. */
  bool has_debug_views = false;
  /** Supported simulation views reached even through unexposed groups. */
  bool has_simulation_debug_views = false;
  /** Local and nested views that become supported when called inside a simulation. */
  bool has_nested_debug_views = false;

  /** Viewers exposed through the corresponding group input index. */
  MultiValueMap<int, PropagatedViewer> viewers_by_group_input;

  friend bool operator==(const TreeDebugViewPropagation &,
                         const TreeDebugViewPropagation &) = default;
};

/** Update the cached explicit Debug View exposure information on a node tree. */
bool update_tree_debug_view_propagation(bNodeTree &tree);

/** A logged persistent geometry Debug View source. */
struct Candidate {
  ComputeContextHash compute_context_hash;
  int32_t viewer_node_id = 0;
  const eval_log::ViewerNodeLog *viewer_log = nullptr;
};

/**
 * Reserve a stable namespace for this Debug View in a dupli persistent-ID chain, probing
 * deterministically if another source has the same hash.
 *
 * #INT_MAX is reserved as the end marker of a persistent ID.
 */
int unique_persistent_id_namespace(const ComputeContextHash &compute_context_hash,
                                   int32_t viewer_node_id,
                                   Set<int> &used_namespaces);

}  // namespace debug_view
}  // namespace nodes
}  // namespace blender
