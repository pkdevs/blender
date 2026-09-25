/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Motorcycle graph computation for straight skeletons.
 *
 * Reference:
 * - S. Huber, M. Held (2011), "Motorcycle Graphs: Stochastic Properties Motivate
 *   an Efficient Yet Simple Implementation", ACM J. Exp. Algorithmics 16.
 * - S. Huber, M. Held (2012), "A Fast Straight-Skeleton Algorithm Based on
 *   Generalized Motorcycle Graphs", Int. J. Comput. Geom. Appl. 22(5).
 */

#include <limits>
#include <queue>
#include <vector>

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "straight_skeleton_intern.hh"

namespace blender::straightskeleton {

/**
 * Type of obstacle a motorcycle crashed into.
 */
enum class MotorcycleCrashType {
  NONE,
  WALL,
  MOTORCYCLE_TRACE,
  CONCURRENT_MOTORCYCLE,
  BOUNDARY,
};

/**
 * A single motorcycle and its computed trajectory trace.
 */
struct MotorcycleTrack {
  int id = -1;
  double2 start_pos{0.0, 0.0};
  double start_time = 0.0;
  double2 velocity{0.0, 0.0};
  double2 crash_pos{0.0, 0.0};
  double crash_time = 0.0;
  bool crashed = false;

  MotorcycleCrashType crash_type = MotorcycleCrashType::NONE;
  int crashed_into_id = -1; /* Index of wall or other motorcycle track. */
  int left_wall = -1;       /* Input wall index defining left wavefront arm. */
  int right_wall = -1;      /* Input wall index defining right wavefront arm. */
  int origin_vert = -1;     /* Input vertex index from which motorcycle originated. */
  Vector<int> ancestor_ids; /* Ancestor motorcycle IDs if spawned by concurrent crash. */
};

/**
 * Complete representation of the computed generalized motorcycle graph M(G).
 */
struct MotorcycleGraph {
  Vector<MotorcycleTrack> tracks;
  int crashes_num = 0;
  int concurrent_crashes_num = 0;
};

/**
 * Compute the generalized motorcycle graph M(G) of the input polygon or PSLG
 * using spatial grid hashing per Huber & Held (2011, 2012).
 */
MotorcycleGraph compute_motorcycle_graph(const StraightSkeletonInput &input);

/**
 * Dump the motorcycle graph to an SVG file for debugging and visual verification.
 */
void dump_motorcycle_graph_to_svg(const StraightSkeletonInput &input,
                                  const MotorcycleGraph &mg,
                                  const char *filepath);

/* -------------------------------------------------------------------- */
/** \name Phase 2: Extended Wavefront W*(0)
 * \{ */

/**
 * Classification of vertices in the extended wavefront W*(t).
 */
enum class WavefrontVertexType {
  CONVEX,          /* Convex boundary vertex of original polygon. */
  REFLEX,          /* Reflex boundary vertex (motorcycle spawn point). */
  MOVING_STEINER,  /* Steiner vertex on a moving wavefront wall. */
  RESTING_STEINER, /* Steiner vertex at an internal crash point not yet reached. */
  MULTI_STEINER,   /* Steiner vertex from simultaneous / multi-motorcycle crash. */
};

/**
 * A vertex in the extended wavefront kinetic planar graph.
 */
struct ExtendedVertex {
  int id = -1;
  double2 pos{0.0, 0.0};
  double2 velocity{0.0, 0.0};
  double birth_time = 0.0;
  WavefrontVertexType type = WavefrontVertexType::CONVEX;
  int halfedge = -1;       /* An outgoing halfedge index. */
  int origin_vert = -1;    /* Input vertex index, if applicable. */
  int input_wall = -1;     /* Input wall index, if on a wall. */
  int motorcycle_id = -1;  /* Associated motorcycle ID, if applicable. */
  bool is_active = true;
};

/**
 * A directed half-edge in the extended wavefront kinetic planar graph.
 */
struct ExtendedHalfEdge {
  int id = -1;
  int origin = -1;         /* Vertex where this directed halfedge starts. */
  int dest = -1;           /* Vertex where this directed halfedge ends. */
  int twin = -1;           /* Opposite halfedge in adjacent face (-1 if unbounded). */
  int next = -1;           /* Next halfedge along face boundary (CCW). */
  int prev = -1;           /* Previous halfedge along face boundary (CCW). */
  int face = -1;           /* Incident face ID (-1 for exterior). */
  bool is_steiner = false; /* True if along a motorcycle track, false if on boundary wall. */
  int wall_id = -1;        /* Associated input wall ID if boundary. */

  /* Collapse event information. */
  double collapse_time = std::numeric_limits<double>::infinity();
  double2 collapse_pos{0.0, 0.0};
  bool is_active = true;
};

/**
 * A convex cell face in the extended wavefront partition.
 */
struct ExtendedFace {
  int id = -1;
  int halfedge = -1;       /* One halfedge on the face boundary. */
  bool is_active = true;
  bool is_convex = true;   /* Verified convex face invariant (Lemma 5). */
  double area = 0.0;
  double2 centroid{0.0, 0.0};
  int edge_count = 0;
  Vector<int> vert_indices;
};

/**
 * Event types for the extended wavefront priority queue.
 */
enum class WavefrontEventType {
  EDGE_COLLAPSE, /* Two convex vertices meet. */
  SPLIT,         /* Reflex vertex meets moving Steiner vertex along track. */
  START,         /* Moving vertex meets resting Steiner vertex. */
  SWITCH,        /* Convex vertex meets moving Steiner / reflex vertex. */
  MULTI_SPLIT,   /* Multi-motorcycle simultaneous collision. */
};

/**
 * An event in the priority queue representing an edge collapse or vertex collision.
 */
struct WavefrontEvent {
  double time = 0.0;
  WavefrontEventType type = WavefrontEventType::EDGE_COLLAPSE;
  int edge_id = -1;
  int u = -1;
  int v = -1;
  double2 pos{0.0, 0.0};
  uint32_t version = 0;

  bool operator>(const WavefrontEvent &other) const
  {
    return time > other.time;
  }
};

/**
 * The complete extended wavefront data structure W*(t).
 */
struct ExtendedWavefront {
  Vector<ExtendedVertex> vertices;
  Vector<ExtendedHalfEdge> halfedges;
  Vector<ExtendedFace> faces;
  std::priority_queue<WavefrontEvent, std::vector<WavefrontEvent>, std::greater<WavefrontEvent>>
      queue;
  double current_time = 0.0;
  bool is_valid = true;
  double total_interior_area = 0.0;
  bool all_faces_convex = true;
};

/**
 * Construct the initial extended wavefront W*(0) by slicing the input polygon
 * boundaries with the generalized motorcycle graph M(G).
 */
ExtendedWavefront build_initial_extended_wavefront(const StraightSkeletonInput &input,
                                                  const MotorcycleGraph &mg);

/**
 * Dump the extended wavefront W*(0) to an SVG file showing the convex cell partition,
 * classified vertices, and initial collapse events.
 */
void dump_extended_wavefront_to_svg(const StraightSkeletonInput &input,
                                    const ExtendedWavefront &ew,
                                    const char *filepath);

/** \} */

}  // namespace blender::straightskeleton
