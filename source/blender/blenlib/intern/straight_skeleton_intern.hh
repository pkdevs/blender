/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_vector.hh"

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name Skeleton Graph
 * \{ */

struct SkeletonNode {
  /** Node index in SkeletonGraph. */
  int id{-1};
  /** 2D position (x, y). */
  double2 pos{0.0, 0.0};
  /** Event time / height t (0 for input verts). */
  double time{0.0};
  /** Index of coincident input vertex, or -1 for interior nodes. */
  int orig_vert{-1};
  /** Incident SkeletonEdge indices. */
  Vector<int> incident_edges;
};

struct SkeletonEdge {
  /** Unique edge index in SkeletonGraph. */
  int id{-1};
  /** First endpoint (lower time or start). */
  int node1{-1};
  /** Second endpoint (higher time or end). */
  int node2{-1};
  /** Coincident input edge index, or -1 for skeleton arcs. */
  int orig_edge{-1};
  /** Incident input face index, or -1. */
  int input_face{-1};
  /** Side 0 or Side 1. */
  int side{0};
  /** Time at node1. */
  double time_start{0.0};
  /** Time at node2. */
  double time_end{0.0};
  /** True for input edges / face boundaries at t=0. */
  bool is_input_boundary{false};
};

class SkeletonGraph {
 public:
  Vector<SkeletonNode> nodes;
  Vector<SkeletonEdge> edges;
  double epsilon{1e-8};

  int add_node(const double2 &pos, double time, int orig_vert = -1);
  int add_edge(int node1,
               int node2,
               int orig_edge = -1,
               int input_face = -1,
               int side = 0,
               double time_start = 0.0,
               double time_end = 0.0,
               bool is_input_boundary = false);
};

/**
 * Build the full StraightSkeletonOutput from a completed SkeletonGraph,
 * including face extraction, edge assembly, and offset band clipping.
 */
StraightSkeletonOutput build_straight_skeleton_output(const StraightSkeletonInput &input,
                                                      const SkeletonGraph &skel,
                                                      int events_processed_num,
                                                      int flips_num,
                                                      bool hit_max_events);

/** \} */

/**
 * Compute the straight skeleton using the kinetic Delaunay triangulation algorithm
 * (Palfrader, Held, Huber 2012).
 */
StraightSkeletonOutput straight_skeleton_calc_kinetic(const StraightSkeletonInput &input);

/**
 * Compute the straight skeleton using the motorcycle graph algorithm
 * (Eppstein & Erickson 1999, Huber & Held 2011).
 */
StraightSkeletonOutput straight_skeleton_calc_motorcycle(const StraightSkeletonInput &input);

/**
 * Compute the straight skeleton using the step-by-step wavefront simulation algorithm
 * with kdtree spatial acceleration.
 */
StraightSkeletonOutput straight_skeleton_calc_simulation(const StraightSkeletonInput &input);

}  // namespace blender::straightskeleton
