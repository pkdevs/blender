/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation for simple polygons with optional holes.
 *
 * The algorithm and implementation are similar to the one described in
 * "On Computing Straight Skeletons by Means of Kinetic Triangulations" by
 * Palfrader, Held and Huber (2012), and also
 * "Computing Mitered Offset Curves Based on Straight Skeletons" by
 * Palfreder and Held (2015).
 */

#include "BLI_straight_skeleton.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

#include "BLI_array.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_heap.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "PRF_profile.hh"

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name Debug Output Controls
 *
 * Uncomment DEBUG_STRAIGHT_SKELETON_SVG below (or define it via compile flags)
 * to draw the straight skeleton output as SVG into an HTML file.
 * Can also be enabled at runtime via the STRAIGHT_SKELETON_SVG environment variable.
 * \{ */

#define DEBUG_STRAIGHT_SKELETON_SVG

static bool debug_new_file = true;
static const char *debug_html_filepath = "/tmp/straight_skeleton.html";

[[maybe_unused]] static void debug_reset_file()
{
  debug_new_file = true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Numerical Tolerances and Stability Constants
 *
 * Named constants for geometric predicates, event detection, and numerical
 * stability as documented in mydocs/straight_skeleton.md Section 6.
 * \{ */

/** Default distance below which two vertices or nodes are considered coincident. */
static constexpr double default_epsilon = 1e-8;

/** Cross product (determinant) threshold identifying parallel/collinear wavefront normals. */
static constexpr double det_parallel_tol = 1e-9;

/** Determinant threshold for non-parallel bisector velocity solver. */
static constexpr double det_zero_tol = 1e-12;

/** Dot product threshold identifying opposing anti-parallel wavefront edges. */
static constexpr double antiparallel_dot_tol = -0.999999;

/** Threshold for cross product of normals indicating a reflex vertex. */
static constexpr double reflex_cross_tol = -1e-6;

/** Barycentric coordinate margin on an edge separating SPLIT from MULTI_SPLIT / FLIP events. */
static constexpr double bary_edge_tol = 0.05;

/** Timestamp tolerance for grouping simultaneous events in the priority queue. */
static constexpr double concurrent_time_tol = 1e-6;

/** Tolerance for event time validity and comparisons (guarding floating-point roundoff). */
static constexpr double event_time_tol = 1e-9;

/** Timestamp tolerance for matching repeated collapse triangles in the flip cycle journal. */
static constexpr double journal_time_tol = 1e-5;

/** Maximum distance between opposing endpoints to consider parallel corridor closing together. */
static constexpr double parallel_corridor_close_tol = 1.5;

/** Minimum area below which reconstructed or output roof faces are discarded as degenerate. */
static constexpr double min_face_area = 1e-9;

/** Minimum area for valid subdivided polygon faces during offset band extraction. */
static constexpr double min_subface_area = 1e-12;

/** Very small threshold to guard against division by zero in vector normalization. */
static constexpr double div_by_zero_tol = 1e-14;

/** Threshold for relative velocity squared magnitude to detect degenerate motion. */
static constexpr double small_speed_sq_tol = 1e-12;


/** Collinear threshold for reflex vertex projection during split side assignment. */
[[maybe_unused]] static constexpr double side_collinear_tol = 1e-9;

/** Advance in simulation time beyond which previous flip cycle journal history is cleared. */
static constexpr double time_advance_tol = 1e-7;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Half-edge Triangle Navigation
 * \{ */

BLI_INLINE int next_triangle_edge(const int edge_index)
{
  const int tri_start = edge_index - (edge_index % 3);
  return tri_start + ((edge_index % 3) + 1) % 3;
}

BLI_INLINE int prev_triangle_edge(const int edge_index)
{
  const int tri_start = edge_index - (edge_index % 3);
  return tri_start + ((edge_index % 3) + 2) % 3;
}

class TriangleEdges {
  /** start_[3 * ti + k] is the kinetic vertex index at corner k of triangle ti. */
  Vector<int> start_;
  /** paired_[3 * ti + k] is the opposite half-edge in neighboring triangle, or -1 for boundary. */
  Vector<int> paired_;

 public:
  int triangles_num() const
  {
    return int(start_.size()) / 3;
  }

  int edges_num() const
  {
    return int(start_.size());
  }

  void reserve_triangles(const int n)
  {
    start_.reserve(3 * n);
    paired_.reserve(3 * n);
  }

  void add_triangle(const int v0, const int v1, const int v2)
  {
    start_.extend({v0, v1, v2});
    paired_.extend({-1, -1, -1});
  }

  int start_vert(const int edge_index) const
  {
    return start_[edge_index];
  }

  int end_vert(const int edge_index) const
  {
    return start_[next_triangle_edge(edge_index)];
  }

  void set_start_vert(const int edge_index, const int vert)
  {
    start_[edge_index] = vert;
  }

  int paired_edge(const int edge_index) const
  {
    return paired_[edge_index];
  }

  void set_paired(const int edge_index, const int paired_index)
  {
    paired_[edge_index] = paired_index;
  }

  bool is_boundary(const int edge_index) const
  {
    return !is_triangle_deleted(edge_index / 3) && paired_[edge_index] == -1;
  }

  bool is_triangle_deleted(const int ti) const
  {
    return start_[3 * ti] == -1;
  }

  void mark_triangle_deleted(const int ti)
  {
    start_[3 * ti] = -1;
    start_[3 * ti + 1] = -1;
    start_[3 * ti + 2] = -1;
  }

  /** Rotate CCW around the destination of edge_index, returning the index
   * of the incoming edge at that vertex. May be -1. */
  int rotate_ccw(const int edge_index) const
  {
    return paired_[next_triangle_edge(edge_index)];
  }

  /** Rotate CW around the destination of edge_index, returning the index
   * of the incoming edge at that vertex. Maybe - 1. */
  int rotate_cw(const int edge_index) const
  {
    const int pe = paired_[edge_index];
    return (pe == -1) ? -1 : prev_triangle_edge(pe);
  }

  int triangle(const int edge_index) const
  {
    return edge_index / 3;
  }

  void set_triangle_verts(const int ti, const int v0, const int v1, const int v2)
  {
    start_[3 * ti] = v0;
    start_[3 * ti + 1] = v1;
    start_[3 * ti + 2] = v2;
  }

  int active_triangles_count() const
  {
    int count = 0;
    for (const int ti : IndexRange(triangles_num())) {
      if (!is_triangle_deleted(ti)) {
        count++;
      }
    }
    return count;
  }

  /** Find outgoing boundary edge starting at vert (i.e. start_vert == vert and is_boundary).
   * Seed edge must have vert as one of its endpoints. */
  int find_boundary_edge_out(const int seed_edge, const int vert) const
  {
    if (seed_edge < 0 || seed_edge >= edges_num() || is_triangle_deleted(seed_edge / 3)) {
      return -1;
    }
    int cur = seed_edge;
    if (start_[cur] != vert) {
      if (end_vert(cur) == vert) {
        cur = next_triangle_edge(cur);
      }
      else {
        cur = prev_triangle_edge(cur);
      }
      if (start_[cur] != vert) {
        return -1;
      }
    }

    const int max_iters = triangles_num();
    for (int iter = 0; iter < max_iters; iter++) {
      if (is_boundary(cur)) {
        return cur;
      }
      const int pe = paired_[cur];
      if (pe == -1) {
        return cur;
      }
      const int next_out = next_triangle_edge(pe);
      if (start_[next_out] != vert || is_triangle_deleted(next_out / 3)) {
        break;
      }
      cur = next_out;
      if (cur == seed_edge) {
        break; /* Closed wheel */
      }
    }
    /* Fallback: direct search across active triangles. */
    for (const int ti : IndexRange(triangles_num())) {
      if (is_triangle_deleted(ti)) {
        continue;
      }
      for (const int k : IndexRange(3)) {
        const int e = 3 * ti + k;
        if (start_[e] == vert && is_boundary(e)) {
          return e;
        }
      }
    }
    return -1;
  }

  /** Find incoming boundary edge ending at vert (i.e. end_vert == vert and is_boundary).
   * Seed edge must have vert as one of its endpoints. */
  int find_boundary_edge_in(const int seed_edge, const int vert) const
  {
    if (seed_edge < 0 || seed_edge >= edges_num() || is_triangle_deleted(seed_edge / 3)) {
      return -1;
    }
    int cur = seed_edge;
    if (start_[cur] != vert) {
      if (end_vert(cur) == vert) {
        cur = next_triangle_edge(cur);
      }
      else {
        cur = prev_triangle_edge(cur);
      }
      if (start_[cur] != vert) {
        return -1;
      }
    }

    const int max_iters = triangles_num();
    for (int iter = 0; iter < max_iters; iter++) {
      const int in_edge = prev_triangle_edge(cur);
      if (is_boundary(in_edge)) {
        return in_edge;
      }
      const int pe = paired_[in_edge];
      if (pe == -1) {
        return in_edge;
      }
      if (start_[pe] != vert || is_triangle_deleted(pe / 3)) {
        break;
      }
      cur = pe;
      if (cur == seed_edge) {
        break; /* Closed wheel */
      }
    }
    /* Fallback: direct search across active triangles. */
    for (const int ti : IndexRange(triangles_num())) {
      if (is_triangle_deleted(ti)) {
        continue;
      }
      for (const int k : IndexRange(3)) {
        const int e = 3 * ti + k;
        if (end_vert(e) == vert && is_boundary(e)) {
          return e;
        }
      }
    }
    return -1;
  }
};

/**
 * Flip the internal diagonal shared by two adjacent triangles in TriangleEdges.
 * Returns true if the flip succeeded, false if edge cannot be flipped.
 * If \a r_new_edge is non-null, stores the half-edge index of the new diagonal in ti1.
 *
 *        BEFORE FLIP                                AFTER FLIP
 *
 *             v                                        v
 *            / \                                      /|\
 *           /   \                                    / | \
 *    p1_2  /     \  p1_1                      p1_2  /  |  \  p1_1
 *   (h1_2)/  ti1  \(h1_1)                    (s1_0)/   |   \(s2_1)
 *        /   e1    \                              / ti1|ti2 \
 *       v1 -------> v2                           v1    |     v2
 *       |  <-------  |                            \    |^s1_2/
 *       |    e2      |                             \   |v   /
 *   p2_1\    ti2    /p2_2                      p2_1 \  |s2_2  p2_2
 *   (h2_1)\       /(h2_2)                    (s1_1) \ | /   (s2_0)
 *          \     /                                   \|/
 *           \   /                                     v3
 *             v3
 *
 *  Vertices:
 *    v1, v2 : endpoints of shared diagonal (e1: v1->v2 in ti1, e2: v2->v1 in ti2)
 *    v      : apex vertex opposite to e1 in ti1
 *    v3     : apex vertex opposite to e2 in ti2
 *
 *  Before flip:
 *    ti1 (CCW): h1_0 (e1): v1->v2
 *               h1_1:      v2->v  (paired with p1_1)
 *               h1_2:      v->v1  (paired with p1_2)
 *    ti2 (CCW): h2_0 (e2): v2->v1
 *               h2_1:      v1->v3 (paired with p2_1)
 *               h2_2:      v3->v2 (paired with p2_2)
 *
 *  After flip:
 *    ti1 (CCW): slot1_0:   v->v1  (paired with p1_2)
 *               slot1_1:   v1->v3 (paired with p2_1)
 *               slot1_2:   v3->v  (new diagonal, paired with slot2_2)
 *    ti2 (CCW): slot2_0:   v3->v2 (paired with p2_2)
 *               slot2_1:   v2->v  (paired with p1_1)
 *               slot2_2:   v->v3  (new diagonal, paired with slot1_2)
 */
static bool flip_edge(TriangleEdges &tri, const int edge_index, int *r_new_edge = nullptr)
{
  const int e1 = edge_index;
  const int e2 = tri.paired_edge(e1);
  if (e2 == -1) {
    return false;
  }

  const int ti1 = tri.triangle(e1);
  const int ti2 = tri.triangle(e2);
  if (ti1 == ti2 || tri.is_triangle_deleted(ti1) || tri.is_triangle_deleted(ti2)) {
    return false;
  }

  const int h1_0 = e1;
  const int h1_1 = next_triangle_edge(h1_0);
  const int h1_2 = prev_triangle_edge(h1_0);

  const int v1 = tri.start_vert(h1_0);
  const int v2 = tri.start_vert(h1_1);
  const int v = tri.start_vert(h1_2);

  const int p1_1 = tri.paired_edge(h1_1);
  const int p1_2 = tri.paired_edge(h1_2);

  const int h2_0 = e2;
  const int h2_1 = next_triangle_edge(h2_0);
  const int h2_2 = prev_triangle_edge(h2_0);

  if (tri.start_vert(h2_0) != v2 || tri.end_vert(h2_0) != v1) {
    return false;
  }

  const int v3 = tri.start_vert(h2_2);

  const int p2_1 = tri.paired_edge(h2_1);
  const int p2_2 = tri.paired_edge(h2_2);

  /* New triangle 1: (v, v1, v3)
   *  edge 0 (3 * ti1 + 0): v  -> v1, paired with p1_2
   *  edge 1 (3 * ti1 + 1): v1 -> v3, paired with p2_1
   *  edge 2 (3 * ti1 + 2): v3 -> v,  paired with (3 * ti2 + 2) [new diagonal] */
  const int slot1_0 = 3 * ti1 + 0;
  const int slot1_1 = 3 * ti1 + 1;
  const int slot1_2 = 3 * ti1 + 2;

  tri.set_start_vert(slot1_0, v);
  tri.set_start_vert(slot1_1, v1);
  tri.set_start_vert(slot1_2, v3);

  tri.set_paired(slot1_0, p1_2);
  if (p1_2 != -1) {
    tri.set_paired(p1_2, slot1_0);
  }

  tri.set_paired(slot1_1, p2_1);
  if (p2_1 != -1) {
    tri.set_paired(p2_1, slot1_1);
  }

  /* New triangle 2: (v3, v2, v)
   *   edge 0 (3 * ti2 + 0): v3 -> v2, paired with p2_2
   *   edge 1 (3 * ti2 + 1): v2 -> v,  paired with p1_1
   *   edge 2 (3 * ti2 + 2): v  -> v3, paired with (3 * ti1 + 2) [new diagonal] */
  const int slot2_0 = 3 * ti2 + 0;
  const int slot2_1 = 3 * ti2 + 1;
  const int slot2_2 = 3 * ti2 + 2;

  tri.set_start_vert(slot2_0, v3);
  tri.set_start_vert(slot2_1, v2);
  tri.set_start_vert(slot2_2, v);

  tri.set_paired(slot2_0, p2_2);
  if (p2_2 != -1) {
    tri.set_paired(p2_2, slot2_0);
  }

  tri.set_paired(slot2_1, p1_1);
  if (p1_1 != -1) {
    tri.set_paired(p1_1, slot2_1);
  }

  /* Connect new diagonal between ti1 and ti2 */
  tri.set_paired(slot1_2, slot2_2);
  tri.set_paired(slot2_2, slot1_2);

  if (r_new_edge != nullptr) {
    *r_new_edge = slot1_2;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Kinetic Vertices (Structure of Arrays)
 * \{ */

struct KineticVertices {
  /** 2D position at birth time. */
  Vector<double2> start_pos;
  /** 2D velocity vector v. */
  Vector<double2> velocity;
  /** Direction vector of incoming wavefront boundary. */
  Vector<double2> ccw_dir;
  /** Direction vector of outgoing wavefront boundary. */
  Vector<double2> cw_dir;
  /** Birth time (t_birth). */
  Vector<double> start_time;
  /** Collapse / termination timestamp. */
  Vector<double> end_time;
  /** SkeletonNode index where vertex was born. */
  Vector<int> start_node;
  /** SkeletonNode index where vertex terminated. */
  Vector<int> end_node;
  /** Coincident input vertex index, or -1. */
  Vector<int> orig_node;
  /** Incident boundary edge index, or -1. */
  Vector<int> input_edge;
  /** True if vertex is currently moving. */
  Vector<bool> is_active;
  /** True if velocity is infinite (parallel collapse). */
  Vector<bool> is_infinitely_fast;

  int size() const
  {
    return int(start_pos.size());
  }

  double2 position_at(const int vi, const double t) const
  {
    return start_pos[vi] + (t - start_time[vi]) * velocity[vi];
  }

  int append(const double2 &pos,
             const double2 &vel,
             const double2 &ccw,
             const double2 &cw,
             const double t_start,
             const int node_id,
             const int orig_v = -1,
             const int in_edge = -1,
             const bool is_inf = false)
  {
    const int id = int(start_pos.size());
    start_pos.append(pos);
    velocity.append(vel);
    ccw_dir.append(ccw);
    cw_dir.append(cw);
    start_time.append(t_start);
    end_time.append(std::numeric_limits<double>::infinity());
    start_node.append(node_id);
    end_node.append(-1);
    orig_node.append(orig_v);
    input_edge.append(in_edge);
    is_active.append(true);
    is_infinitely_fast.append(is_inf);
    return id;
  }
};

/** \} */

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
  double epsilon{default_epsilon};

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

int SkeletonGraph::add_node(const double2 &pos, const double time, const int orig_vert)
{
  if (orig_vert == -1) {
    for (const SkeletonNode &existing : nodes) {
      if (std::abs(existing.time - time) < epsilon && math::length(existing.pos - pos) < epsilon) {
        return existing.id;
      }
    }
  }
  const int id = int(nodes.size());
  SkeletonNode node;
  node.id = id;
  node.pos = pos;
  node.time = time;
  node.orig_vert = orig_vert;
  nodes.append(node);
  return id;
}

int SkeletonGraph::add_edge(const int node1,
                            const int node2,
                            const int orig_edge,
                            const int input_face,
                            const int side,
                            const double time_start,
                            const double time_end,
                            const bool is_input_boundary)
{
  if (node1 == node2) {
    return -1;
  }
  for (const SkeletonEdge &existing : edges) {
    if ((existing.node1 == node1 && existing.node2 == node2) ||
        (existing.node1 == node2 && existing.node2 == node1))
    {
      return existing.id;
    }
  }
  const int id = int(edges.size());
  SkeletonEdge edge;
  edge.id = id;
  edge.node1 = node1;
  edge.node2 = node2;
  edge.orig_edge = orig_edge;
  edge.input_face = input_face;
  edge.side = side;
  edge.time_start = time_start;
  edge.time_end = time_end;
  edge.is_input_boundary = is_input_boundary;
  edges.append(edge);

  nodes[node1].incident_edges.append(id);
  nodes[node2].incident_edges.append(id);
  return id;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Priority Queue of Events
 * \{ */

enum class TriangleEventType {
  NONE,
  EDGE_COLLAPSE,
  SPLIT,
  FLIP,
  MULTI_SPLIT,
};

[[maybe_unused]] static const char *triangle_event_type_name(TriangleEventType type)
{
  switch (type) {
    case TriangleEventType::NONE:
      return "NONE";
    case TriangleEventType::EDGE_COLLAPSE:
      return "EDGE_COLLAPSE";
    case TriangleEventType::SPLIT:
      return "SPLIT";
    case TriangleEventType::FLIP:
      return "FLIP";
    case TriangleEventType::MULTI_SPLIT:
      return "MULTI_SPLIT";
  }
  return "UNKNOWN";
}

struct TriangleEvent {
  /** Index of the triangle in the triangulation. */
  int triangle_index{-1};
  /** Event time. */
  double time{std::numeric_limits<double>::infinity()};
  /** Type of event. */
  TriangleEventType type{TriangleEventType::NONE};
  /** Index of the half-edge involved in the event. */
  int edge_index{-1};
  /** Index of the vertex involved in the event. */
  int vert_index{-1};
  /** Coordinates where the event occurs. */
  double2 event_pos{0.0, 0.0};
  /** Pointer to the node in the heap. */
  HeapNode *heap_node{nullptr};
};

class TrianglePriorityQueue {
  Heap *heap_{nullptr};
  Array<TriangleEvent> events_;

 public:
  TrianglePriorityQueue() = default;

  ~TrianglePriorityQueue()
  {
    if (heap_ != nullptr) {
      BLI_heap_free(heap_, nullptr);
    }
  }

  void reinitialize(const int triangles_num)
  {
    if (heap_ != nullptr) {
      BLI_heap_free(heap_, nullptr);
    }
    heap_ = BLI_heap_new();
    events_.reinitialize(triangles_num);
    for (int ti = 0; ti < triangles_num; ti++) {
      events_[ti].triangle_index = ti;
      events_[ti].time = std::numeric_limits<double>::infinity();
      events_[ti].type = TriangleEventType::NONE;
      events_[ti].heap_node = nullptr;
    }
  }

  void insert_or_update(const int ti, const TriangleEvent &ev)
  {
    if (ti < 0 || ti >= events_.size()) {
      return;
    }
    if (ev.type == TriangleEventType::NONE || std::isinf(ev.time)) {
      remove(ti);
      return;
    }
    HeapNode *existing_node = events_[ti].heap_node;
    events_[ti] = ev;
    events_[ti].triangle_index = ti;
    events_[ti].heap_node = existing_node;

    if (events_[ti].heap_node != nullptr) {
      BLI_heap_node_value_update(heap_, events_[ti].heap_node, float(ev.time));
    }
    else {
      events_[ti].heap_node = BLI_heap_insert(heap_, float(ev.time), POINTER_FROM_INT(ti));
    }
  }

  void remove(const int ti)
  {
    if (ti < 0 || ti >= events_.size()) {
      return;
    }
    TriangleEvent &entry = events_[ti];
    if (entry.heap_node != nullptr) {
      BLI_heap_remove(heap_, entry.heap_node);
      entry.heap_node = nullptr;
    }
    entry.type = TriangleEventType::NONE;
    entry.time = std::numeric_limits<double>::infinity();
  }

  bool is_empty() const
  {
    return (heap_ == nullptr) || BLI_heap_is_empty(heap_);
  }

  TriangleEvent pop()
  {
    if (is_empty()) {
      TriangleEvent dummy;
      dummy.type = TriangleEventType::NONE;
      return dummy;
    }
    const int ti = POINTER_AS_INT(BLI_heap_pop_min(heap_));
    TriangleEvent result = events_[ti];
    events_[ti].heap_node = nullptr;
    return result;
  }

  /** Pop the next event from the heap. If multiple events occur at the same timestamp
   * (within numerical tolerance), prefer an internal diagonal flip between two collapsing
   * triangles over an event that flips into non-collapsing cavity triangles.
   * This ensures that collinear cycles (such as the 2-cycle in Palfreder et al. Section 3.2,
   * Fig. 3(b)) are processed as flip events rather than being preempted by cavity flips. */
  TriangleEvent pop_with_flip_priority(const TriangleEdges &triangles)
  {
    if (is_empty()) {
      TriangleEvent dummy;
      dummy.type = TriangleEventType::NONE;
      return dummy;
    }
    const int ti_min = POINTER_AS_INT(BLI_heap_pop_min(heap_));
    TriangleEvent min_ev = events_[ti_min];
    events_[ti_min].heap_node = nullptr;

    auto check_internal = [&](const TriangleEvent &ev) {
      const int e1 = ev.edge_index;
      const int e2 = triangles.paired_edge(e1);
      if (e2 == -1) {
        return false;
      }
      const int p_ti = triangles.triangle(e2);
      if (p_ti < 0 || p_ti >= int(events_.size())) {
        return false;
      }
      if (p_ti == ti_min || events_[p_ti].heap_node != nullptr) {
        if (std::abs(events_[p_ti].time - min_ev.time) <= concurrent_time_tol) {
          return true;
        }
      }
      return false;
    };

    const bool min_is_internal = (min_ev.type == TriangleEventType::FLIP) &&
                                 check_internal(min_ev);

    int best_flip_ti = -1;
    bool best_flip_is_internal = false;

    for (const int ti : events_.index_range()) {
      if (events_[ti].heap_node != nullptr && events_[ti].type == TriangleEventType::FLIP &&
          std::abs(events_[ti].time - min_ev.time) <= concurrent_time_tol)
      {
        const bool is_int = check_internal(events_[ti]);
        if (best_flip_ti == -1 || (!best_flip_is_internal && is_int)) {
          best_flip_ti = ti;
          best_flip_is_internal = is_int;
        }
      }
    }

    if (best_flip_ti != -1 && best_flip_ti != ti_min) {
      if (min_ev.type == TriangleEventType::FLIP && !min_is_internal && best_flip_is_internal) {
        insert_or_update(ti_min, min_ev);
        TriangleEvent flip_ev = events_[best_flip_ti];
        remove(best_flip_ti);
        return flip_ev;
      }
    }

    /* When multiple events occur concurrently, prioritize boundary EDGE_COLLAPSE over
     * internal MULTI_SPLIT events so that shrinking boundary wavefronts are processed first. */
    if (min_ev.type == TriangleEventType::MULTI_SPLIT) {
      int best_collapse_ti = -1;
      for (const int ti : events_.index_range()) {
        if (events_[ti].heap_node != nullptr &&
            events_[ti].type == TriangleEventType::EDGE_COLLAPSE &&
            std::abs(events_[ti].time - min_ev.time) <= concurrent_time_tol)
        {
          best_collapse_ti = ti;
          break;
        }
      }
      if (best_collapse_ti != -1) {
        insert_or_update(ti_min, min_ev);
        TriangleEvent collapse_ev = events_[best_collapse_ti];
        remove(best_collapse_ti);
        return collapse_ev;
      }
    }

    return min_ev;
  }

  const TriangleEvent &peek() const
  {
    const int ti = POINTER_AS_INT(BLI_heap_node_ptr(BLI_heap_top(heap_)));
    return events_[ti];
  }

  const Array<TriangleEvent> &events() const
  {
    return events_;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Flip Event Loop Journal (see Palfreder et al. paper Section 3.2)
 * \{ */

/**
 * History journal entry for processed flip events, used to detect when
 * we are in an infinite flip cycle.
 * Stores a triple (t*, t, tri) where t* is the number of remaining active triangles,
 * t is the collapse timestamp, and tri is the sorted vertex-triple of the collapsed triangle.
 */
struct JournalEntry {
  int triangles_remaining_num{0}; /* t* */
  double time{0.0};               /* t */
  int3 triangle_verts{0, 0, 0};   /* Sorted triangle vertex indices (tri) */
  int flipped_edge{-1};           /* Half-edge before the flip */
  int new_edge{-1};               /* Half-edge of the new diagonal after the flip */
  int ti1{-1};                    /* First triangle */
  int ti2{-1};                    /* Second triangle */
  int parent_entry_index{-1};     /* Pointer to earlier entry T1 if reconfigured */
};

/** Return the vertex indices of triangle \a ti in canonically sorted order (v0 < v1 < v2). */
static int3 get_sorted_triangle_verts(const TriangleEdges &triangles, const int ti)
{
  int v0 = triangles.start_vert(3 * ti);
  int v1 = triangles.start_vert(3 * ti + 1);
  int v2 = triangles.start_vert(3 * ti + 2);
  if (v0 > v1) {
    std::swap(v0, v1);
  }
  if (v1 > v2) {
    std::swap(v1, v2);
  }
  if (v0 > v1) {
    std::swap(v0, v1);
  }
  return int3(v0, v1, v2);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bisector Velocity Calculation
 * \{ */

static std::pair<double2, bool> compute_bisector_vel(const double2 &n1, const double2 &n2)
{
  const double det = math::cross(n1, n2);
  const double dot = math::dot(n1, n2);
  const bool is_inf = (dot < antiparallel_dot_tol);
  double2 vel(0.0, 0.0);
  if (!is_inf) {
    if (std::abs(det) > det_zero_tol) {
      vel = double2((n2.y - n1.y) / det, (n1.x - n2.x) / det);
    }
    else if (dot > 0.0) {
      vel = n1;
    }
  }
  return {vel, is_inf};
}

static bool is_vertex_reflex(const KineticVertices &kinetic_verts, const int vi)
{
  const double2 ccw = kinetic_verts.ccw_dir[vi];
  const double2 cw = kinetic_verts.cw_dir[vi];
  const double2 n_in(ccw.y, -ccw.x);
  const double2 n_out(-cw.y, cw.x);
  return (math::cross(n_in, n_out) < reflex_cross_tol);
}

static bool is_edge_flippable_at_time(const TriangleEdges &triangles,
                                      const KineticVertices &kinetic_verts,
                                      const int e1,
                                      const double time)
{
  if (triangles.is_boundary(e1)) {
    return false;
  }
  const int e2 = triangles.paired_edge(e1);
  if (e2 == -1) {
    return false;
  }
  const int ti1 = triangles.triangle(e1);
  const int ti2 = triangles.triangle(e2);
  if (ti1 == ti2 || triangles.is_triangle_deleted(ti1) || triangles.is_triangle_deleted(ti2)) {
    return false;
  }

  const int h1_0 = e1;
  const int h1_1 = next_triangle_edge(h1_0);
  const int h1_2 = prev_triangle_edge(h1_0);
  const int v1 = triangles.start_vert(h1_0);
  const int v2 = triangles.start_vert(h1_1);
  const int v = triangles.start_vert(h1_2);

  const int h2_0 = e2;
  const int h2_2 = prev_triangle_edge(h2_0);
  const int v3 = triangles.start_vert(h2_2);

  const double2 pv = kinetic_verts.position_at(v, time);
  const double2 pv1 = kinetic_verts.position_at(v1, time);
  const double2 pv2 = kinetic_verts.position_at(v2, time);
  const double2 pv3 = kinetic_verts.position_at(v3, time);

  const double area_new1 = math::cross(pv1 - pv, pv3 - pv);
  const double area_new2 = math::cross(pv2 - pv3, pv - pv3);

  return (area_new1 > 1e-12 && area_new2 > 1e-12);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Event Detection
 * \{ */

static TriangleEvent compute_triangle_collapse(const int ti,
                                               const TriangleEdges &triangles,
                                               const KineticVertices &kinetic_verts,
                                               const double current_time,
                                               const double epsilon = default_epsilon)
{
  TriangleEvent best_event;
  best_event.triangle_index = ti;
  best_event.time = std::numeric_limits<double>::infinity();
  best_event.type = TriangleEventType::NONE;

  if (triangles.is_triangle_deleted(ti)) {
    return best_event;
  }

  const int total_verts_num = kinetic_verts.size();
  const int e0 = 3 * ti;
  const int v0 = triangles.start_vert(e0);
  const int v1 = triangles.start_vert(e0 + 1);
  const int v2 = triangles.start_vert(e0 + 2);

  if (v0 < 0 || v0 >= total_verts_num || v1 < 0 || v1 >= total_verts_num || v2 < 0 ||
      v2 >= total_verts_num)
  {
    return best_event;
  }
  if (!kinetic_verts.is_active[v0] || !kinetic_verts.is_active[v1] || !kinetic_verts.is_active[v2])
  {
    return best_event;
  }
  if (kinetic_verts.is_infinitely_fast[v0] || kinetic_verts.is_infinitely_fast[v1] ||
      kinetic_verts.is_infinitely_fast[v2])
  {
    return best_event;
  }

  const int tri_v[3] = {v0, v1, v2};
  const double2 p[3] = {kinetic_verts.position_at(v0, current_time),
                        kinetic_verts.position_at(v1, current_time),
                        kinetic_verts.position_at(v2, current_time)};
  const double2 v[3] = {
      kinetic_verts.velocity[v0], kinetic_verts.velocity[v1], kinetic_verts.velocity[v2]};

  auto consider_candidate = [&](const TriangleEvent &cand) {
    if (cand.time < best_event.time - event_time_tol) {
      best_event = cand;
    }
    else if (std::abs(cand.time - best_event.time) <= event_time_tol) {
      if (cand.type == TriangleEventType::EDGE_COLLAPSE &&
          best_event.type != TriangleEventType::EDGE_COLLAPSE)
      {
        best_event = cand;
      }
      else if (cand.type == TriangleEventType::MULTI_SPLIT &&
               best_event.type != TriangleEventType::EDGE_COLLAPSE &&
               best_event.type != TriangleEventType::MULTI_SPLIT)
      {
        best_event = cand;
      }
    }
  };

  int num_boundary = 0;
  int boundary_k = -1;
  for (const int k : IndexRange(3)) {
    if (triangles.is_boundary(e0 + k)) {
      num_boundary++;
      boundary_k = k;
    }
  }

  /* 1. Edge collapse / multi-split: closest approach of edge endpoints.
   * As shown by Palfreder & Held (2015), the derivative of pairwise distance
   * is linear in time and avoids roots of multiplicity 2. */
  for (const int k : IndexRange(3)) {
    const int ei = e0 + k;
    const int next_k = (k + 1) % 3;
    const double2 dp = p[next_k] - p[k];
    const double2 dv = v[next_k] - v[k];
    const double dv_sq = math::dot(dv, dv);
    if (dv_sq > div_by_zero_tol) {
      const double dt = -math::dot(dp, dv) / dv_sq;
      const bool is_valid_time = (current_time > 0.0 && dt > -event_time_tol) ||
                                 (dt > event_time_tol);
      if (is_valid_time) {
        const double eff_dt = std::max(0.0, dt);
        if (triangles.is_boundary(ei)) {
          TriangleEvent cand;
          cand.triangle_index = ti;
          cand.time = current_time + eff_dt;
          cand.type = TriangleEventType::EDGE_COLLAPSE;
          cand.edge_index = ei;
          cand.vert_index = tri_v[k];
          cand.event_pos = p[k] + eff_dt * v[k];
          consider_candidate(cand);
        }
        else {
          /* Internal triangulation edge: collapses only if distance reaches zero.
           * In straight skeleton theory, a multi-split is initiated by at least one reflex
           * vertex. Furthermore, collapsing an ear chord cannot split a polygon. */
          const double2 p_at_t = dp + eff_dt * dv;
          if (math::length(p_at_t) < epsilon) {
            const int u_idx = tri_v[k];
            const int v_idx = tri_v[next_k];
            const bool has_reflex = is_vertex_reflex(kinetic_verts, u_idx) ||
                                    is_vertex_reflex(kinetic_verts, v_idx);
            const int p_ei = triangles.paired_edge(ei);
            const int p_ti = (p_ei != -1) ? triangles.triangle(p_ei) : -1;
            int p_num_boundary = 0;
            if (p_ti != -1) {
              const int p_e0 = p_ti * 3;
              for (const int p_k : IndexRange(3)) {
                if (triangles.is_boundary(p_e0 + p_k)) {
                  p_num_boundary++;
                }
              }
            }
            if (has_reflex && num_boundary < 2 && p_num_boundary < 2) {
              TriangleEvent cand;
              cand.triangle_index = ti;
              cand.time = current_time + eff_dt;
              cand.type = TriangleEventType::MULTI_SPLIT;
              cand.edge_index = ei;
              cand.vert_index = tri_v[k];
              cand.event_pos = p[k] + eff_dt * v[k];
              consider_candidate(cand);
            }
          }
        }
      }
    }
  }

  /* 2. Vertices moving into edges: area collapse of 1-boundary triangle.
   * Solves exact quadratic polynomial for triangle area reaching zero. */
  if (num_boundary == 1) {
    const int k = boundary_k;
    const int ei = e0 + k;
    const int k_a = k;
    const int k_b = (k + 1) % 3;
    const int k_opp = (k + 2) % 3;

    const double2 e01_p = p[1] - p[0];
    const double2 e02_p = p[2] - p[0];
    const double2 e01_v = v[1] - v[0];
    const double2 e02_v = v[2] - v[0];

    const double c0 = math::cross(e01_p, e02_p);
    const double c1 = math::cross(e01_p, e02_v) + math::cross(e01_v, e02_p);
    const double c2 = math::cross(e01_v, e02_v);

    double dt_root = -1.0;
    if (std::abs(c2) > div_by_zero_tol) {
      const double disc = c1 * c1 - 4.0 * c2 * c0;
      if (disc >= 0.0) {
        const double sqrt_disc = std::sqrt(disc);
        const double r1 = (-c1 - sqrt_disc) / (2.0 * c2);
        const double r2 = (-c1 + sqrt_disc) / (2.0 * c2);
        double best_r = std::numeric_limits<double>::infinity();
        for (const double r : {r1, r2}) {
          const bool is_valid_r = (current_time > 0.0 && r >= -event_time_tol) ||
                                  (r > event_time_tol);
          if (is_valid_r && r < best_r) {
            best_r = r;
          }
        }
        if (!std::isinf(best_r)) {
          dt_root = best_r;
        }
      }
    }
    else if (std::abs(c1) > div_by_zero_tol) {
      const double r = -c0 / c1;
      const bool is_valid_r = (current_time > 0.0 && r >= -event_time_tol) || (r > event_time_tol);
      if (is_valid_r) {
        dt_root = r;
      }
    }

    const bool is_valid_dt = (current_time > 0.0 && dt_root >= -event_time_tol) ||
                             (dt_root > event_time_tol);
    if (is_valid_dt) {
      const double eff_dt = std::max(0.0, dt_root);
      const double2 pt_a = p[k_a] + eff_dt * v[k_a];
      const double2 pt_b = p[k_b] + eff_dt * v[k_b];
      const double2 pt_opp = p[k_opp] + eff_dt * v[k_opp];

      const double2 edge_dir_t = pt_b - pt_a;
      const double edge_len_t = math::length(edge_dir_t);
      const double2 to_opp_t = pt_opp - pt_a;
      const double d_along = (edge_len_t > div_by_zero_tol) ?
                                 math::dot(to_opp_t, edge_dir_t) / (edge_len_t * edge_len_t) :
                                 0.0;

      TriangleEvent cand;
      cand.triangle_index = ti;
      cand.time = current_time + eff_dt;
      cand.edge_index = ei;
      cand.vert_index = tri_v[k_opp];
      cand.event_pos = pt_a + std::clamp(d_along, 0.0, 1.0) * edge_dir_t;

      if (edge_len_t < epsilon) {
        cand.type = TriangleEventType::EDGE_COLLAPSE;
        consider_candidate(cand);
      }
      else if (d_along < -bary_edge_tol) {
        /* Crosses supporting line outside segment near endpoint a: flip internal edge. */
        const int e_flip = e0 + ((k + 2) % 3);
        if (is_edge_flippable_at_time(triangles, kinetic_verts, e_flip, cand.time)) {
          cand.type = TriangleEventType::FLIP;
          cand.edge_index = e_flip;
          consider_candidate(cand);
        }
      }
      else if (d_along > 1.0 + bary_edge_tol) {
        /* Crosses supporting line outside segment near endpoint b: flip internal edge. */
        const int e_flip = e0 + ((k + 1) % 3);
        if (is_edge_flippable_at_time(triangles, kinetic_verts, e_flip, cand.time)) {
          cand.type = TriangleEventType::FLIP;
          cand.edge_index = e_flip;
          consider_candidate(cand);
        }
      }
      else if (d_along <= bary_edge_tol) {
        const int e_opp_a = e0 + ((k + 2) % 3);
        cand.type = triangles.is_boundary(e_opp_a) ? TriangleEventType::EDGE_COLLAPSE :
                                                     TriangleEventType::MULTI_SPLIT;
        cand.edge_index = e_opp_a;
        consider_candidate(cand);
      }
      else if (d_along >= 1.0 - bary_edge_tol) {
        const int e_b_opp = e0 + ((k + 1) % 3);
        cand.type = triangles.is_boundary(e_b_opp) ? TriangleEventType::EDGE_COLLAPSE :
                                                     TriangleEventType::MULTI_SPLIT;
        cand.edge_index = e_b_opp;
        consider_candidate(cand);
      }
      else if (is_vertex_reflex(kinetic_verts, tri_v[k_opp])) {
        cand.type = TriangleEventType::SPLIT;
        consider_candidate(cand);
      }
      else {
        /* Opposite vertex is convex or flat: collapses towards closer boundary endpoint. */
        if (d_along >= 0.5) {
          const int e_b_opp = e0 + ((k + 1) % 3);
          cand.type = triangles.is_boundary(e_b_opp) ? TriangleEventType::EDGE_COLLAPSE :
                                                       TriangleEventType::MULTI_SPLIT;
          cand.edge_index = e_b_opp;
        }
        else {
          const int e_opp_a = e0 + ((k + 2) % 3);
          cand.type = triangles.is_boundary(e_opp_a) ? TriangleEventType::EDGE_COLLAPSE :
                                                       TriangleEventType::MULTI_SPLIT;
          cand.edge_index = e_opp_a;
        }
        consider_candidate(cand);
      }
    }
  }

  /* 2b. Two boundary edges (num_boundary == 2): corner vertex moves towards internal chord.
   * Area reaches zero when the 3 vertices become collinear. If the internal chord can flip,
   * a flip event updates the triangulation. */
  if (num_boundary == 2) {
    int k_int = -1;
    for (const int k : IndexRange(3)) {
      if (!triangles.is_boundary(e0 + k)) {
        k_int = k;
        break;
      }
    }
    if (k_int != -1) {
      const double2 e01_p = p[1] - p[0];
      const double2 e02_p = p[2] - p[0];
      const double2 e01_v = v[1] - v[0];
      const double2 e02_v = v[2] - v[0];

      const double c0 = math::cross(e01_p, e02_p);
      const double c1 = math::cross(e01_p, e02_v) + math::cross(e01_v, e02_p);
      const double c2 = math::cross(e01_v, e02_v);

      double dt_root = -1.0;
      if (std::abs(c2) > div_by_zero_tol) {
        const double disc = c1 * c1 - 4.0 * c2 * c0;
        if (disc >= 0.0) {
          const double sqrt_disc = std::sqrt(disc);
          const double r1 = (-c1 - sqrt_disc) / (2.0 * c2);
          const double r2 = (-c1 + sqrt_disc) / (2.0 * c2);
          double best_r = std::numeric_limits<double>::infinity();
          for (const double r : {r1, r2}) {
            const bool is_valid_r = (current_time > 0.0 && r >= -event_time_tol) ||
                                    (r > event_time_tol);
            if (is_valid_r && r < best_r) {
              best_r = r;
            }
          }
          if (!std::isinf(best_r)) {
            dt_root = best_r;
          }
        }
      }
      else if (std::abs(c1) > div_by_zero_tol) {
        const double r = -c0 / c1;
        const bool is_valid_r = (current_time > 0.0 && r >= -event_time_tol) ||
                                (r > event_time_tol);
        if (is_valid_r) {
          dt_root = r;
        }
      }

      const bool is_valid_dt = (current_time > 0.0 && dt_root >= -event_time_tol) ||
                               (dt_root > event_time_tol);
      if (is_valid_dt && (current_time + std::max(0.0, dt_root) < best_event.time)) {
        const double eff_dt = std::max(0.0, dt_root);
        const double cand_time = current_time + eff_dt;
        const int e_int = e0 + k_int;
        if (is_edge_flippable_at_time(triangles, kinetic_verts, e_int, cand_time)) {
          const int k_opp = (k_int + 2) % 3;
          TriangleEvent cand;
          cand.triangle_index = ti;
          cand.time = cand_time;
          cand.type = TriangleEventType::FLIP;
          cand.edge_index = e_int;
          cand.vert_index = tri_v[k_opp];
          cand.event_pos = p[k_opp] + eff_dt * v[k_opp];
          consider_candidate(cand);
        }
      }
    }
  }

  /* 3. Pure internal triangles (num_boundary == 0): area collapse indicates flip event. */
  if (num_boundary == 0) {
    const double2 e01_p = p[1] - p[0];
    const double2 e02_p = p[2] - p[0];
    const double2 e01_v = v[1] - v[0];
    const double2 e02_v = v[2] - v[0];

    const double c0 = math::cross(e01_p, e02_p);
    const double c1 = math::cross(e01_p, e02_v) + math::cross(e01_v, e02_p);
    const double c2 = math::cross(e01_v, e02_v);

    double dt_root = -1.0;
    if (std::abs(c2) > div_by_zero_tol) {
      const double disc = c1 * c1 - 4.0 * c2 * c0;
      if (disc >= 0.0) {
        const double sqrt_disc = std::sqrt(disc);
        const double r1 = (-c1 - sqrt_disc) / (2.0 * c2);
        const double r2 = (-c1 + sqrt_disc) / (2.0 * c2);
        double best_r = std::numeric_limits<double>::infinity();
        for (const double r : {r1, r2}) {
          const bool is_valid_r = (current_time > 0.0 && r >= -event_time_tol) ||
                                  (r > event_time_tol);
          if (is_valid_r && r < best_r) {
            best_r = r;
          }
        }
        if (!std::isinf(best_r)) {
          dt_root = best_r;
        }
      }
    }
    else if (std::abs(c1) > div_by_zero_tol) {
      const double r = -c0 / c1;
      const bool is_valid_r = (current_time > 0.0 && r >= -event_time_tol) || (r > event_time_tol);
      if (is_valid_r) {
        dt_root = r;
      }
    }

    const bool is_valid_dt = (current_time > 0.0 && dt_root >= -event_time_tol) ||
                             (dt_root > event_time_tol);
    const double eff_dt = std::max(0.0, dt_root);
    if (is_valid_dt && (current_time + eff_dt < best_event.time)) {
      const double2 pt[3] = {p[0] + eff_dt * v[0], p[1] + eff_dt * v[1], p[2] + eff_dt * v[2]};

      /* Check if any edge has collapsed to zero length. */
      int zero_k = -1;
      double min_len_sq = 1e30;
      for (const int k : IndexRange(3)) {
        const double2 ed = pt[(k + 1) % 3] - pt[k];
        const double lsq = math::dot(ed, ed);
        if (lsq < min_len_sq) {
          min_len_sq = lsq;
          zero_k = k;
        }
      }

      if (min_len_sq < epsilon * epsilon) {
        /* Edge collapse / multi-split: two vertices meet at a point. */
        const int ei = e0 + zero_k;
        TriangleEvent cand;
        cand.triangle_index = ti;
        cand.time = current_time + eff_dt;
        cand.type = triangles.is_boundary(ei) ? TriangleEventType::EDGE_COLLAPSE :
                                                TriangleEventType::MULTI_SPLIT;
        cand.edge_index = ei;
        cand.vert_index = tri_v[zero_k];
        cand.event_pos = pt[zero_k];
        consider_candidate(cand);
      }
      else {
        /* Pure collinearity with non-zero edge lengths: flip event on longest flippable edge. */
        int best_k = -1;
        double max_len_sq = -1.0;
        for (const int k : IndexRange(3)) {
          const int e_cand = e0 + k;
          if (is_edge_flippable_at_time(triangles, kinetic_verts, e_cand, current_time + eff_dt)) {
            const double2 ed = pt[(k + 1) % 3] - pt[k];
            const double lsq = math::dot(ed, ed);
            if (lsq > max_len_sq) {
              max_len_sq = lsq;
              best_k = k;
            }
          }
        }

        if (best_k != -1) {
          TriangleEvent cand;
          cand.triangle_index = ti;
          cand.time = current_time + eff_dt;
          cand.type = TriangleEventType::FLIP;
          cand.edge_index = e0 + best_k;
          cand.vert_index = tri_v[(best_k + 2) % 3];
          cand.event_pos = 0.5 * (pt[best_k] + pt[(best_k + 1) % 3]);
          consider_candidate(cand);
        }
      }
    }
  }

  /* 4. Isolated 3-gon with 3 boundary edges: if no forward collapse was found,
   * the 3-gon has already reached collapse and must terminate immediately. */
  if (num_boundary == 3 && best_event.type == TriangleEventType::NONE) {
    TriangleEvent cand;
    cand.triangle_index = ti;
    cand.time = current_time;
    cand.type = TriangleEventType::EDGE_COLLAPSE;
    cand.edge_index = e0;
    cand.vert_index = tri_v[0];
    cand.event_pos = (p[0] + p[1] + p[2]) / 3.0;
    consider_candidate(cand);
  }

  return best_event;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name StraightSkeletonState
 * \{ */

class StraightSkeletonState {
 public:
  TriangleEdges triangles;
  KineticVertices kinetic_verts;
  SkeletonGraph skeleton;
  TrianglePriorityQueue pq;
  double current_time_{0.0};
  double epsilon{default_epsilon};
  const StraightSkeletonInput *input_{nullptr};
  bool handle_edge_collapses_{true};

  /* Event statistics. */
  int events_processed_num_{0};
  int flips_count_{0};
  int edge_collapses_count_{0};
  int multi_splits_count_{0};
  int splits_count_{0};
  bool hit_max_events_{false};

  /* Section 3.2 Flip event loop journal (history H and search structure S). */
  Vector<JournalEntry> journal_history_;
  Map<int3, int> journal_search_;
  int last_active_triangles_count_{0};

  explicit StraightSkeletonState(const StraightSkeletonInput &input)
      : epsilon(input.epsilon <= 0.0 ? default_epsilon : input.epsilon), input_(&input)
  {
    skeleton.epsilon = this->epsilon;
  }

  void set_handle_edge_collapses(const bool val)
  {
    handle_edge_collapses_ = val;
  }

  void journal_clear()
  {
    journal_history_.clear();
    journal_search_.clear();
  }

  void journal_record_flip(
      int ti1, int ti2, int flipped_edge, int new_edge, double time, const int3 &sorted_verts);
  bool check_and_handle_flip_cycle(const TriangleEvent &ev);
  bool retriangulate_collapsed_polygon(const Set<int> &p_triangles,
                                       Span<int> p_verts,
                                       double t_star,
                                       int t1_entry_idx);

  void build_initial_kinetic_triangulation();
  bool process_next_event();
  bool process_edge_collapse_event(const TriangleEvent &ev);
  bool process_multi_split_event(const TriangleEvent &ev);
  bool process_split_event(const TriangleEvent &ev);
  bool process_flip_event(const TriangleEvent &ev);
  void handle_infinite_speed_vertex(int v_prime,
                                    int node_id,
                                    double event_time,
                                    int initial_seed_edge = -1);
  void run_event_loop(int max_events);
};

void StraightSkeletonState::build_initial_kinetic_triangulation()
{
  PRF_scope(ProfileCategory::Core);
  const StraightSkeletonInput &input = *input_;

  /* 1. Build CDT input. */
  meshintersect::CDT_input<double> cdt_in;
  cdt_in.vert = input.vert;
  cdt_in.epsilon = epsilon;
  cdt_in.needed_ids = CDT_ORIG_VERTS | CDT_ORIG_EDGES;

  /* Outer polygon (CCW) is face 0. */
  Vector<int> cdt_face_offsets = {0};
  Vector<int> cdt_face_verts;

  for (const int vi : input.outer_polygon) {
    cdt_face_verts.append(vi);
  }
  cdt_face_offsets.append(int(cdt_face_verts.size()));

  /* Holes (CW) are subsequent faces. */
  const int n_holes = input.hole_offsets.size();
  for (const int h : IndexRange(n_holes)) {
    for (const int vi : input.hole_vert_indices.slice(input.hole_offsets[h])) {
      cdt_face_verts.append(vi);
    }
    cdt_face_offsets.append(int(cdt_face_verts.size()));
  }

  cdt_in.face_offsets = OffsetIndices<int>(cdt_face_offsets.as_span());
  cdt_in.face_vert_indices = cdt_face_verts.as_span();

  /* Run CDT. */
  const CDT_output_type cdt_type = (n_holes > 0) ? CDT_INSIDE_WITH_HOLES_NONZERO : CDT_INSIDE;
  const meshintersect::CDT_result<double> cdt = meshintersect::delaunay_2d_calc(cdt_in, cdt_type);
  const int cdt_tris_num = int(cdt.face.size());

  /* 2. Build TriangleEdges from CDT faces. */
  triangles = TriangleEdges{};
  triangles.reserve_triangles(4 * cdt_tris_num);
  for (const Vector<int> &f : cdt.face) {
    BLI_assert(int(f.size()) == 3);
    triangles.add_triangle(f[0], f[1], f[2]);
  }

  const int edges_num = 3 * cdt_tris_num;
  Map<int2, int> edge_map;
  edge_map.reserve(edges_num);
  for (const int ei : IndexRange(edges_num)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    edge_map.add(int2(u, v), ei);
  }

  for (const int ei : IndexRange(edges_num)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    const int *opp = edge_map.lookup_ptr(int2(v, u));
    if (opp != nullptr) {
      triangles.set_paired(ei, *opp);
    }
    /* Boundary edges retain paired == -1. */
  }

  /* 3. Map CDT vertices back to input vertices. */
  const int n_cdt_verts = int(cdt.vert.size());
  Array<int> cdt_to_input_vert(n_cdt_verts, -1);
  for (const int ci : IndexRange(n_cdt_verts)) {
    if (!cdt.vert_orig[ci].is_empty()) {
      cdt_to_input_vert[ci] = int(cdt.vert_orig[ci][0]);
    }
  }

  /* 4. Find boundary edges and compute initial velocities for all vertices.
   * For each boundary edge (paired == -1):
   * start_vert(ei) -> end_vert(ei) has the interior to its left. */
  Array<int> boundary_in_edge(n_cdt_verts, -1);
  Array<int> boundary_out_edge(n_cdt_verts, -1);

  for (const int ei : IndexRange(edges_num)) {
    if (triangles.is_boundary(ei)) {
      const int u = triangles.start_vert(ei);
      const int v = triangles.end_vert(ei);
      boundary_out_edge[u] = ei;
      boundary_in_edge[v] = ei;
    }
  }

  /* Array of kinetic vertex index for each CDT vertex. */
  Array<int> cdt_vert_to_kv(n_cdt_verts, -1);

  for (const int ci : IndexRange(n_cdt_verts)) {
    const int e_in = boundary_in_edge[ci];
    const int e_out = boundary_out_edge[ci];
    if (e_in == -1 || e_out == -1) {
      continue; /* Interior CDT vertex (if any); receives velocity when incident edge collapses. */
    }

    const int v_prev = triangles.start_vert(e_in);
    const int v_next = triangles.end_vert(e_out);

    const double2 pos_v = cdt.vert[ci];
    const double2 pos_prev = cdt.vert[v_prev];
    const double2 pos_next = cdt.vert[v_next];

    /* Incoming edge v_prev -> pos_v: inward normal is to the left (-dy, dx). */
    const double2 d_in = math::normalize(pos_v - pos_prev);
    const double2 n1(-d_in.y, d_in.x);

    /* Outgoing edge pos_v -> pos_next: inward normal is to the left (-dy, dx). */
    const double2 d_out = math::normalize(pos_next - pos_v);
    const double2 n2(-d_out.y, d_out.x);

    const auto [vel, is_inf] = compute_bisector_vel(n1, n2);
    const int orig_vi = cdt_to_input_vert[ci];
    const int node_id = skeleton.add_node(pos_v, 0.0, orig_vi);

    const double2 ccw_dir = -d_in;
    const double2 cw_dir = d_out;
    const int kv_idx = kinetic_verts.append(
        pos_v, vel, ccw_dir, cw_dir, 0.0, node_id, orig_vi, -1, is_inf);
    cdt_vert_to_kv[ci] = kv_idx;

    /* Assign kv_idx to all triangle corner slots at vertex ci by rotating CCW around ci. */
    int e_cur = e_in;
    while (true) {
      const int e_out_tri = next_triangle_edge(e_cur);
      triangles.set_start_vert(e_out_tri, kv_idx);
      const int e_next = triangles.rotate_ccw(e_cur);
      if (e_next == -1 || e_next == e_in) {
        break;
      }
      e_cur = e_next;
    }
  }

  /* 5. Initialize priority queue with events for all initial triangles. */
  pq.reinitialize(triangles.triangles_num());
  for (int ti = triangles.triangles_num() - 1; ti >= 0; ti--) {
    const TriangleEvent ev = compute_triangle_collapse(ti, triangles, kinetic_verts, 0.0, epsilon);
    if (ev.type != TriangleEventType::NONE && !std::isinf(ev.time)) {
      pq.insert_or_update(ti, ev);
    }
  }

  last_active_triangles_count_ = triangles.active_triangles_count();
  journal_clear();
}

void StraightSkeletonState::handle_infinite_speed_vertex(const int initial_v_prime,
                                                         const int initial_node_id,
                                                         const double event_time,
                                                         const int initial_seed_edge)
{
  PRF_scope(ProfileCategory::Core);
  int curr_v_prime = initial_v_prime;
  int curr_node_id = initial_node_id;
  int curr_seed_edge = initial_seed_edge;

  for (int iter = 0; iter < 100; iter++) {
    if (curr_v_prime < 0 || curr_v_prime >= kinetic_verts.size() ||
        !kinetic_verts.is_active[curr_v_prime] || !kinetic_verts.is_infinitely_fast[curr_v_prime])
    {
      break;
    }

    int e_out_prime = -1;
    int e_in_prime = -1;
    if (curr_seed_edge != -1) {
      e_out_prime = triangles.find_boundary_edge_out(curr_seed_edge, curr_v_prime);
      e_in_prime = triangles.find_boundary_edge_in(curr_seed_edge, curr_v_prime);
    }
    /* Fallback scan if seed edge was not provided or topological search failed. */
    if (e_out_prime == -1 || e_in_prime == -1) {
      for (const int ei : IndexRange(triangles.edges_num())) {
        if (triangles.is_boundary(ei)) {
          if (triangles.start_vert(ei) == curr_v_prime) {
            e_out_prime = ei;
          }
          if (triangles.end_vert(ei) == curr_v_prime) {
            e_in_prime = ei;
          }
        }
      }
    }
    if (e_out_prime == -1 || e_in_prime == -1) {
      break;
    }

    const int v_prev = triangles.start_vert(e_in_prime);
    const int v_next = triangles.end_vert(e_out_prime);
    if (v_prev < 0 || v_next < 0 || v_prev >= kinetic_verts.size() ||
        v_next >= kinetic_verts.size())
    {
      break;
    }
    const double2 p_prev = kinetic_verts.position_at(v_prev, event_time);
    const double2 p_next = kinetic_verts.position_at(v_next, event_time);
    const double2 p_prime = kinetic_verts.position_at(curr_v_prime, event_time);

    const bool is_closed_loop = (v_prev == v_next) || [&]() {
      const int out_from_next = triangles.find_boundary_edge_out(next_triangle_edge(e_out_prime),
                                                                 v_next);
      if (out_from_next != -1) {
        return triangles.end_vert(out_from_next) == v_prev;
      }
      for (const int ei : IndexRange(triangles.edges_num())) {
        if (triangles.is_boundary(ei)) {
          if (triangles.start_vert(ei) == v_next && triangles.end_vert(ei) == v_prev) {
            return true;
          }
        }
      }
      return false;
    }();

    const double len_prev = math::distance(p_prev, p_prime);
    const double len_next = math::distance(p_next, p_prime);
    const double dist_ends = math::distance(p_prev, p_next);
    const bool both_ends = (dist_ends < parallel_corridor_close_tol) ||
                           (dist_ends < 0.1 * std::max(len_prev, len_next));

    if (both_ends && is_closed_loop) {
      /* Both ends collide at the midpoint between p_prev and p_next at event_time. */
      const double2 p_final = 0.5 * (p_prev + p_next);
      int final_node = curr_node_id;
      if (math::distance(p_prime, p_final) >= epsilon) {
        final_node = skeleton.add_node(p_final, event_time);
        skeleton.add_edge(curr_node_id, final_node, -1, 0, 0, event_time, event_time);
      }

      kinetic_verts.end_node[v_prev] = final_node;
      kinetic_verts.end_time[v_prev] = event_time;
      kinetic_verts.is_active[v_prev] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_prev],
                        final_node,
                        kinetic_verts.input_edge[v_prev],
                        0,
                        0,
                        kinetic_verts.start_time[v_prev],
                        event_time);

      kinetic_verts.end_node[v_next] = final_node;
      kinetic_verts.end_time[v_next] = event_time;
      kinetic_verts.is_active[v_next] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_next],
                        final_node,
                        kinetic_verts.input_edge[v_next],
                        0,
                        0,
                        kinetic_verts.start_time[v_next],
                        event_time);

      kinetic_verts.end_node[curr_v_prime] = final_node;
      kinetic_verts.end_time[curr_v_prime] = event_time;
      kinetic_verts.is_active[curr_v_prime] = false;

      bool any_active = false;
      for (const int vi : IndexRange(kinetic_verts.size())) {
        if (kinetic_verts.is_active[vi]) {
          any_active = true;
          break;
        }
      }
      if (!any_active) {
        for (const int tj : IndexRange(triangles.triangles_num())) {
          triangles.mark_triangle_deleted(tj);
          pq.remove(tj);
        }
      }
      else {
        for (const int tj : IndexRange(triangles.triangles_num())) {
          if (triangles.is_triangle_deleted(tj)) {
            continue;
          }
          const int v_a = triangles.start_vert(3 * tj);
          const int v_b = triangles.start_vert(3 * tj + 1);
          const int v_c = triangles.start_vert(3 * tj + 2);
          if (!kinetic_verts.is_active[v_a] && !kinetic_verts.is_active[v_b] &&
              !kinetic_verts.is_active[v_c])
          {
            triangles.mark_triangle_deleted(tj);
            pq.remove(tj);
          }
        }
      }
      break;
    }
    else if (both_ends) {
      /* Both parallel wavefront boundaries reached their opposite ends together.
       * The corridor closes completely, merging the two boundary loops at final_node. */
      const double2 p_final = 0.5 * (p_prev + p_next);
      int final_node = curr_node_id;
      if (math::distance(p_prime, p_final) >= epsilon) {
        final_node = skeleton.add_node(p_final, event_time);
        skeleton.add_edge(curr_node_id, final_node, -1, 0, 0, event_time, event_time);
      }

      kinetic_verts.end_node[v_prev] = final_node;
      kinetic_verts.end_time[v_prev] = event_time;
      kinetic_verts.is_active[v_prev] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_prev],
                        final_node,
                        kinetic_verts.input_edge[v_prev],
                        0,
                        0,
                        kinetic_verts.start_time[v_prev],
                        event_time);

      kinetic_verts.end_node[v_next] = final_node;
      kinetic_verts.end_time[v_next] = event_time;
      kinetic_verts.is_active[v_next] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_next],
                        final_node,
                        kinetic_verts.input_edge[v_next],
                        0,
                        0,
                        kinetic_verts.start_time[v_next],
                        event_time);

      kinetic_verts.end_node[curr_v_prime] = final_node;
      kinetic_verts.end_time[curr_v_prime] = event_time;
      kinetic_verts.is_active[curr_v_prime] = false;

      const double2 ccw_dir = kinetic_verts.ccw_dir[v_prev];
      const double2 cw_dir = kinetic_verts.cw_dir[v_next];
      const double2 norm_in(ccw_dir.y, -ccw_dir.x);
      const double2 norm_out(-cw_dir.y, cw_dir.x);
      const auto [vel_bridge, is_inf_bridge] = compute_bisector_vel(norm_in, norm_out);

      const int v_bridge = kinetic_verts.append(
          p_final, vel_bridge, ccw_dir, cw_dir, event_time, final_node, -1, -1, is_inf_bridge);

      VectorSet<int> modified_triangles;

      /* Replace v_prev, v_next, curr_v_prime with v_bridge across all non-deleted triangles. */
      for (const int tj : IndexRange(triangles.triangles_num())) {
        if (triangles.is_triangle_deleted(tj)) {
          continue;
        }
        for (const int k : IndexRange(3)) {
          const int v = triangles.start_vert(3 * tj + k);
          if (v == v_prev || v == v_next || v == curr_v_prime) {
            triangles.set_start_vert(3 * tj + k, v_bridge);
            modified_triangles.add(tj);
          }
        }
      }

      /* Clean up any triangles that have collapsed (2 or 3 identical vertices). */
      bool found_degenerate = true;
      while (found_degenerate) {
        found_degenerate = false;
        for (const int tj : IndexRange(triangles.triangles_num())) {
          if (triangles.is_triangle_deleted(tj)) {
            continue;
          }
          const int v0 = triangles.start_vert(3 * tj);
          const int v1 = triangles.start_vert(3 * tj + 1);
          const int v2 = triangles.start_vert(3 * tj + 2);

          if (v0 == v1 && v1 == v2) {
            triangles.mark_triangle_deleted(tj);
            pq.remove(tj);
            modified_triangles.remove(tj);
            found_degenerate = true;
            continue;
          }

          int k_deg = -1;
          if (v0 == v1) {
            k_deg = 0;
          }
          else if (v1 == v2) {
            k_deg = 1;
          }
          else if (v2 == v0) {
            k_deg = 2;
          }

          if (k_deg != -1) {
            const int e_after = 3 * tj + ((k_deg + 1) % 3);
            const int e_before = 3 * tj + ((k_deg + 2) % 3);
            const int p_after = triangles.paired_edge(e_after);
            const int p_before = triangles.paired_edge(e_before);

            if (p_after != -1 && p_before != -1) {
              triangles.set_paired(p_after, p_before);
              triangles.set_paired(p_before, p_after);
            }
            else if (p_after != -1) {
              triangles.set_paired(p_after, -1);
            }
            else if (p_before != -1) {
              triangles.set_paired(p_before, -1);
            }

            triangles.mark_triangle_deleted(tj);
            pq.remove(tj);
            modified_triangles.remove(tj);

            if (p_after != -1) {
              modified_triangles.add(p_after / 3);
            }
            if (p_before != -1) {
              modified_triangles.add(p_before / 3);
            }
            found_degenerate = true;
          }
        }
      }

      int surviving_triangles = 0;
      for (const int tj : IndexRange(triangles.triangles_num())) {
        if (!triangles.is_triangle_deleted(tj)) {
          surviving_triangles++;
        }
      }

      if (surviving_triangles == 0) {
        kinetic_verts.is_active[v_bridge] = false;
        if (final_node != initial_node_id) {
          skeleton.add_edge(final_node, initial_node_id, -1, 0, 0, event_time, event_time);
        }
        break;
      }

      /* Recompute events for all surviving modified triangles. */
      for (const int mod_ti : modified_triangles) {
        if (triangles.is_triangle_deleted(mod_ti)) {
          continue;
        }
        const TriangleEvent new_ev = compute_triangle_collapse(
            mod_ti, triangles, kinetic_verts, event_time, epsilon);
        if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
          pq.insert_or_update(mod_ti, new_ev);
        }
        else {
          pq.remove(mod_ti);
        }
      }

      if (is_inf_bridge) {
        curr_v_prime = v_bridge;
        curr_node_id = final_node;
        curr_seed_edge = -1;
        continue;
      }
      break;
    }

    /* Ridge propagation along parallel opposing boundary edges to closer endpoint. */
    const bool choose_next = (len_next <= len_prev);
    const int target_v = choose_next ? v_next : v_prev;
    const double2 target_pos = choose_next ? p_next : p_prev;
    const int target_e = choose_next ? e_out_prime : e_in_prime;

    int next_node = curr_node_id;
    if (math::distance(p_prime, target_pos) > epsilon) {
      next_node = skeleton.add_node(target_pos, event_time);
      skeleton.add_edge(curr_node_id, next_node, -1, 0, 0, event_time, event_time);
    }

    kinetic_verts.end_node[curr_v_prime] = next_node;
    kinetic_verts.end_time[curr_v_prime] = event_time;
    kinetic_verts.is_active[curr_v_prime] = false;

    kinetic_verts.end_node[target_v] = next_node;
    kinetic_verts.end_time[target_v] = event_time;
    kinetic_verts.is_active[target_v] = false;
    skeleton.add_edge(kinetic_verts.start_node[target_v],
                      next_node,
                      kinetic_verts.input_edge[target_v],
                      0,
                      0,
                      kinetic_verts.start_time[target_v],
                      event_time);

    /* Determine new bisector velocity at target_pos.
     * When choose_next (outgoing edge e_out_prime collapsed to target_v):
     *   - surviving incoming edge is curr_v_prime's ccw_dir.
     *   - surviving outgoing edge is target_v's cw_dir.
     * When !choose_next (incoming edge e_in_prime collapsed to target_v):
     *   - surviving incoming edge is target_v's ccw_dir.
     *   - surviving outgoing edge is curr_v_prime's cw_dir. */
    double2 ccw_dir, cw_dir;
    if (choose_next) {
      ccw_dir = kinetic_verts.ccw_dir[curr_v_prime];
      cw_dir = kinetic_verts.cw_dir[target_v];
    }
    else {
      ccw_dir = kinetic_verts.ccw_dir[target_v];
      cw_dir = kinetic_verts.cw_dir[curr_v_prime];
    }
    const double2 norm_in(ccw_dir.y, -ccw_dir.x);
    const double2 norm_out(-cw_dir.y, cw_dir.x);
    const auto [vel_new, is_inf_new] = compute_bisector_vel(norm_in, norm_out);

    const int v_new = kinetic_verts.append(
        target_pos, vel_new, ccw_dir, cw_dir, event_time, next_node, -1, -1, is_inf_new);

    /* Collapse the triangle containing target_e (connecting curr_v_prime and target_v). */
    const int ti_deg = target_e / 3;
    const int k_target = target_e % 3;
    const int e_next = 3 * ti_deg + ((k_target + 1) % 3);
    const int e_prev = 3 * ti_deg + ((k_target + 2) % 3);
    const int pair_next = triangles.paired_edge(e_next);
    const int pair_prev = triangles.paired_edge(e_prev);
    const int v_start_deg = triangles.start_vert(target_e);
    const int v_end_deg = triangles.end_vert(target_e);

    VectorSet<int> modified_triangles;

    /* Traverse fan around v_start_deg and replace with v_new. */
    if (pair_prev != -1) {
      int cur_edge = pair_prev;
      while (cur_edge != -1) {
        const int cur_ti = cur_edge / 3;
        if (cur_ti == ti_deg || triangles.is_triangle_deleted(cur_ti)) {
          break;
        }
        if (triangles.start_vert(cur_edge) != v_start_deg) {
          break;
        }
        modified_triangles.add(cur_ti);
        triangles.set_start_vert(cur_edge, v_new);

        const int in_edge = prev_triangle_edge(cur_edge);
        cur_edge = triangles.paired_edge(in_edge);
      }
    }

    /* Traverse fan around v_end_deg and replace with v_new. */
    if (pair_next != -1) {
      int cur_edge = pair_next;
      while (cur_edge != -1) {
        const int cur_ti = cur_edge / 3;
        if (cur_ti == ti_deg || triangles.is_triangle_deleted(cur_ti)) {
          break;
        }
        const int out_edge = next_triangle_edge(cur_edge);
        if (triangles.start_vert(out_edge) != v_end_deg) {
          break;
        }
        modified_triangles.add(cur_ti);
        triangles.set_start_vert(out_edge, v_new);

        cur_edge = triangles.paired_edge(out_edge);
      }
    }

    /* Stitch hole left by collapsed triangle. */
    if (pair_next != -1 && pair_prev != -1) {
      triangles.set_paired(pair_next, pair_prev);
      triangles.set_paired(pair_prev, pair_next);
    }
    else if (pair_next != -1) {
      triangles.set_paired(pair_next, -1);
    }
    else if (pair_prev != -1) {
      triangles.set_paired(pair_prev, -1);
    }

    triangles.mark_triangle_deleted(ti_deg);
    pq.remove(ti_deg);

    if (pair_next != -1) {
      modified_triangles.add(pair_next / 3);
    }
    if (pair_prev != -1) {
      modified_triangles.add(pair_prev / 3);
    }

#ifndef NDEBUG
    for (const int check_ti : IndexRange(triangles.triangles_num())) {
      if (check_ti == ti_deg || triangles.is_triangle_deleted(check_ti)) {
        continue;
      }
      for (const int k : IndexRange(3)) {
        const int vert = triangles.start_vert(3 * check_ti + k);
        BLI_assert(vert != curr_v_prime);
        BLI_assert(vert != target_v);
      }
    }
#endif

    for (const int mod_ti : modified_triangles) {
      if (mod_ti == ti_deg || triangles.is_triangle_deleted(mod_ti)) {
        continue;
      }
      const TriangleEvent new_ev = compute_triangle_collapse(
          mod_ti, triangles, kinetic_verts, event_time, epsilon);
      if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
        pq.insert_or_update(mod_ti, new_ev);
      }
      else {
        pq.remove(mod_ti);
      }
    }

    curr_seed_edge = (pair_prev != -1) ? pair_prev :
                                         (pair_next != -1 ? next_triangle_edge(pair_next) : -1);
    curr_v_prime = v_new;
    curr_node_id = next_node;
  }
}

bool StraightSkeletonState::process_edge_collapse_event(const TriangleEvent &ev)
{
  PRF_scope(ProfileCategory::Core);
  if (ev.triangle_index < 0 || triangles.is_triangle_deleted(ev.triangle_index)) {
    return false;
  }
  const int e_coll = ev.edge_index;
  if (!triangles.is_boundary(e_coll)) {
    return false;
  }

  const int ti = ev.triangle_index;
  const int e0 = 3 * ti;
  const int k = e_coll - e0;
  const int v_start = triangles.start_vert(e_coll);
  const int v_end = triangles.end_vert(e_coll);
  const int v_opp = triangles.start_vert(e0 + ((k + 2) % 3));

  if (!kinetic_verts.is_active[v_start] || !kinetic_verts.is_active[v_end]) {
    return false;
  }

  current_time_ = ev.time;

  /* Add skeleton node at collapse position. */
  const int node_id = skeleton.add_node(ev.event_pos, ev.time);

  /* Terminate collapsing vertices. */
  kinetic_verts.end_node[v_start] = node_id;
  kinetic_verts.end_time[v_start] = ev.time;
  kinetic_verts.is_active[v_start] = false;
  skeleton.add_edge(kinetic_verts.start_node[v_start],
                    node_id,
                    kinetic_verts.input_edge[v_start],
                    0,
                    0,
                    kinetic_verts.start_time[v_start],
                    ev.time);

  kinetic_verts.end_node[v_end] = node_id;
  kinetic_verts.end_time[v_end] = ev.time;
  kinetic_verts.is_active[v_end] = false;
  skeleton.add_edge(kinetic_verts.start_node[v_end],
                    node_id,
                    kinetic_verts.input_edge[v_end],
                    0,
                    0,
                    kinetic_verts.start_time[v_end],
                    ev.time);

  /* The two remaining edges of triangle ti. */
  const int e_before = e0 + ((k + 2) % 3); /* v_opp -> v_start */
  const int e_after = e0 + ((k + 1) % 3);  /* v_end -> v_opp */

  const int pe_before = triangles.paired_edge(e_before);
  const int pe_after = triangles.paired_edge(e_after);

  /* Check if this was a 3-gon collapse (triangle completely collapses). */
  const bool all_boundary = (pe_before == -1 && pe_after == -1);
  if (all_boundary) {
    if (kinetic_verts.is_active[v_opp]) {
      kinetic_verts.end_node[v_opp] = node_id;
      kinetic_verts.end_time[v_opp] = ev.time;
      kinetic_verts.is_active[v_opp] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_opp],
                        node_id,
                        kinetic_verts.input_edge[v_opp],
                        0,
                        0,
                        kinetic_verts.start_time[v_opp],
                        ev.time);
    }
    triangles.mark_triangle_deleted(ti);
    pq.remove(ti);
    edge_collapses_count_++;
    return true;
  }

  /* Compute velocity of new kinetic vertex born at node_id. */
  const double2 n_in = kinetic_verts.ccw_dir[v_start];
  const double2 n_out = kinetic_verts.cw_dir[v_end];

  const double2 norm_in(n_in.y, -n_in.x);
  const double2 norm_out(-n_out.y, n_out.x);

  auto [vel_prime, is_inf_prime] = compute_bisector_vel(norm_in, norm_out);

  const int v_prime = kinetic_verts.append(
      ev.event_pos, vel_prime, n_in, n_out, ev.time, node_id, -1, -1, is_inf_prime);

  VectorSet<int> modified_triangles;

  auto replace_vert_in_all_triangles = [&](const int old_v, const int new_v) {
    if (old_v == new_v || old_v < 0) {
      return;
    }
    for (const int tj : IndexRange(triangles.triangles_num())) {
      if (triangles.is_triangle_deleted(tj)) {
        continue;
      }
      bool mod = false;
      for (const int k_edge : IndexRange(3)) {
        if (triangles.start_vert(3 * tj + k_edge) == old_v) {
          triangles.set_start_vert(3 * tj + k_edge, new_v);
          mod = true;
        }
      }
      if (mod) {
        modified_triangles.add(tj);
      }
    }
  };

  auto cleanup_degenerate_triangles = [&]() {
    bool found_degenerate = true;
    while (found_degenerate) {
      found_degenerate = false;
      for (const int tj : IndexRange(triangles.triangles_num())) {
        if (triangles.is_triangle_deleted(tj)) {
          continue;
        }
        const int v0 = triangles.start_vert(3 * tj);
        const int v1 = triangles.start_vert(3 * tj + 1);
        const int v2 = triangles.start_vert(3 * tj + 2);

        if (v0 == v1 && v1 == v2) {
          triangles.mark_triangle_deleted(tj);
          pq.remove(tj);
          modified_triangles.remove(tj);
          found_degenerate = true;
          continue;
        }

        int k_deg = -1;
        if (v0 == v1) {
          k_deg = 0;
        }
        else if (v1 == v2) {
          k_deg = 1;
        }
        else if (v2 == v0) {
          k_deg = 2;
        }

        if (k_deg != -1) {
          const int e_after = 3 * tj + ((k_deg + 1) % 3);
          const int e_before = 3 * tj + ((k_deg + 2) % 3);
          const int p_after = triangles.paired_edge(e_after);
          const int p_before = triangles.paired_edge(e_before);

          if (p_after != -1 && p_before != -1) {
            triangles.set_paired(p_after, p_before);
            triangles.set_paired(p_before, p_after);
          }
          else if (p_after != -1) {
            triangles.set_paired(p_after, -1);
          }
          else if (p_before != -1) {
            triangles.set_paired(p_before, -1);
          }

          triangles.mark_triangle_deleted(tj);
          pq.remove(tj);
          modified_triangles.remove(tj);

          if (p_after != -1) {
            modified_triangles.add(p_after / 3);
          }
          if (p_before != -1) {
            modified_triangles.add(p_before / 3);
          }
          found_degenerate = true;
          break;
        }
      }
    }
  };

  /* Traverse and update the fan of surviving triangles around v_start with v_prime. */
  if (pe_before != -1) {
    int cur_edge = pe_before;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (cur_ti == ti || triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      if (triangles.start_vert(cur_edge) != v_start) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(cur_edge, v_prime);

      const int in_edge = prev_triangle_edge(cur_edge);
      cur_edge = triangles.paired_edge(in_edge);
    }
  }

  /* Traverse and update the fan of surviving triangles around v_end with v_prime. */
  if (pe_after != -1) {
    int cur_edge = pe_after;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (cur_ti == ti || triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      const int out_edge = next_triangle_edge(cur_edge);
      if (triangles.start_vert(out_edge) != v_end) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(out_edge, v_prime);

      cur_edge = triangles.paired_edge(out_edge);
    }
  }

  /* Mark triangle ti deleted and remove from priority queue. */
  triangles.mark_triangle_deleted(ti);
  pq.remove(ti);

  /* Stitch hole left by collapsed triangle: pair pe_before and pe_after if both exist. */
  if (pe_before != -1 && pe_after != -1) {
    triangles.set_paired(pe_before, pe_after);
    triangles.set_paired(pe_after, pe_before);
  }
  else if (pe_before != -1) {
    triangles.set_paired(pe_before, -1);
  }
  else if (pe_after != -1) {
    triangles.set_paired(pe_after, -1);
  }

  if (pe_before != -1) {
    modified_triangles.add(pe_before / 3);
  }
  if (pe_after != -1) {
    modified_triangles.add(pe_after / 3);
  }

  replace_vert_in_all_triangles(v_start, v_prime);
  replace_vert_in_all_triangles(v_end, v_prime);

  /* Check for immediate cascaded collapses of tiny adjacent boundary edges that have already
   * crossed the supporting line of the opposite boundary edge. */
  int seed_prime = (pe_before != -1) ? pe_before : pe_after;
  bool cascaded = true;
  while (cascaded && seed_prime != -1 && kinetic_verts.is_active[v_prime]) {
    cascaded = false;

    /* 1. Check outgoing boundary edge starting at v_prime. */
    const int out_e = triangles.find_boundary_edge_out(seed_prime, v_prime);
    if (out_e != -1) {
      const int w = triangles.end_vert(out_e);
      if (w != v_prime && kinetic_verts.is_active[w]) {
        const double2 p_prime = kinetic_verts.position_at(v_prime, ev.time);
        const double2 p_w = kinetic_verts.position_at(w, ev.time);
        const double dist_w = math::distance(p_prime, p_w);

        const double2 n_in = kinetic_verts.ccw_dir[v_prime];
        const double2 norm_in(n_in.y, -n_in.x);
        const double h_w = math::dot(p_w - p_prime, norm_in);

        if (dist_w < 0.05 && h_w <= 1e-5) {
          const int ti_w = out_e / 3;
          const int e_w_after = next_triangle_edge(out_e);
          const int e_w_before = prev_triangle_edge(out_e);
          const int pe_w_before = triangles.paired_edge(e_w_before);
          const int pe_w_after = triangles.paired_edge(e_w_after);

          if (pe_w_before == -1 && pe_w_after == -1) {
            const int v_opp = triangles.start_vert(e_w_before);
            if (kinetic_verts.is_active[v_opp]) {
              kinetic_verts.end_node[v_opp] = node_id;
              kinetic_verts.end_time[v_opp] = ev.time;
              kinetic_verts.is_active[v_opp] = false;
              skeleton.add_edge(kinetic_verts.start_node[v_opp],
                                node_id,
                                kinetic_verts.input_edge[v_opp],
                                0,
                                0,
                                kinetic_verts.start_time[v_opp],
                                ev.time);
            }
            if (kinetic_verts.is_active[w]) {
              kinetic_verts.end_node[w] = node_id;
              kinetic_verts.end_time[w] = ev.time;
              kinetic_verts.is_active[w] = false;
              skeleton.add_edge(kinetic_verts.start_node[w],
                                node_id,
                                kinetic_verts.input_edge[w],
                                0,
                                0,
                                kinetic_verts.start_time[w],
                                ev.time);
            }
            kinetic_verts.end_node[v_prime] = node_id;
            kinetic_verts.end_time[v_prime] = ev.time;
            kinetic_verts.is_active[v_prime] = false;
            skeleton.add_edge(kinetic_verts.start_node[v_prime],
                              node_id,
                              kinetic_verts.input_edge[v_prime],
                              0,
                              0,
                              kinetic_verts.start_time[v_prime],
                              ev.time);
            triangles.mark_triangle_deleted(ti_w);
            pq.remove(ti_w);
            break;
          }

          kinetic_verts.end_node[w] = node_id;
          kinetic_verts.end_time[w] = ev.time;
          kinetic_verts.is_active[w] = false;
          skeleton.add_edge(kinetic_verts.start_node[w],
                            node_id,
                            kinetic_verts.input_edge[w],
                            0,
                            0,
                            kinetic_verts.start_time[w],
                            ev.time);

          kinetic_verts.cw_dir[v_prime] = kinetic_verts.cw_dir[w];
          const double2 new_n_in = kinetic_verts.ccw_dir[v_prime];
          const double2 new_n_out = kinetic_verts.cw_dir[v_prime];
          const double2 new_norm_in(new_n_in.y, -new_n_in.x);
          const double2 new_norm_out(-new_n_out.y, new_n_out.x);
          const auto [new_vel, new_is_inf] = compute_bisector_vel(new_norm_in, new_norm_out);
          kinetic_verts.velocity[v_prime] = new_vel;
          kinetic_verts.is_infinitely_fast[v_prime] = new_is_inf;
          if (new_is_inf) {
            is_inf_prime = true;
          }

          if (pe_w_after != -1) {
            int cur_edge = pe_w_after;
            while (cur_edge != -1) {
              const int cur_ti = cur_edge / 3;
              if (cur_ti == ti_w || triangles.is_triangle_deleted(cur_ti)) {
                break;
              }
              const int out_w = next_triangle_edge(cur_edge);
              if (triangles.start_vert(out_w) != w) {
                break;
              }
              modified_triangles.add(cur_ti);
              triangles.set_start_vert(out_w, v_prime);
              cur_edge = triangles.paired_edge(out_w);
            }
          }

          triangles.mark_triangle_deleted(ti_w);
          pq.remove(ti_w);

          if (pe_w_before != -1 && pe_w_after != -1) {
            triangles.set_paired(pe_w_before, pe_w_after);
            triangles.set_paired(pe_w_after, pe_w_before);
          }
          else if (pe_w_before != -1) {
            triangles.set_paired(pe_w_before, -1);
          }
          else if (pe_w_after != -1) {
            triangles.set_paired(pe_w_after, -1);
          }

          if (pe_w_before != -1) {
            modified_triangles.add(pe_w_before / 3);
            seed_prime = pe_w_before;
          }
          if (pe_w_after != -1) {
            modified_triangles.add(pe_w_after / 3);
            seed_prime = pe_w_after;
          }
          replace_vert_in_all_triangles(w, v_prime);
          cascaded = true;
          continue;
        }
      }
    }

    /* 2. Check incoming boundary edge ending at v_prime. */
    const int in_e = triangles.find_boundary_edge_in(seed_prime, v_prime);
    if (in_e != -1) {
      const int u = triangles.start_vert(in_e);
      if (u != v_prime && kinetic_verts.is_active[u]) {
        const double2 p_prime = kinetic_verts.position_at(v_prime, ev.time);
        const double2 p_u = kinetic_verts.position_at(u, ev.time);
        const double dist_u = math::distance(p_prime, p_u);

        const double2 n_out = kinetic_verts.cw_dir[v_prime];
        const double2 norm_out(-n_out.y, n_out.x);
        const double h_u = math::dot(p_u - p_prime, norm_out);

        if (dist_u < 0.05 && h_u <= 1e-5) {
          const int ti_u = in_e / 3;
          const int e_u_after = next_triangle_edge(in_e);
          const int e_u_before = prev_triangle_edge(in_e);
          const int pe_u_before = triangles.paired_edge(e_u_before);
          const int pe_u_after = triangles.paired_edge(e_u_after);

          if (pe_u_before == -1 && pe_u_after == -1) {
            const int v_opp = triangles.end_vert(e_u_after);
            if (kinetic_verts.is_active[v_opp]) {
              kinetic_verts.end_node[v_opp] = node_id;
              kinetic_verts.end_time[v_opp] = ev.time;
              kinetic_verts.is_active[v_opp] = false;
              skeleton.add_edge(kinetic_verts.start_node[v_opp],
                                node_id,
                                kinetic_verts.input_edge[v_opp],
                                0,
                                0,
                                kinetic_verts.start_time[v_opp],
                                ev.time);
            }
            if (kinetic_verts.is_active[u]) {
              kinetic_verts.end_node[u] = node_id;
              kinetic_verts.end_time[u] = ev.time;
              kinetic_verts.is_active[u] = false;
              skeleton.add_edge(kinetic_verts.start_node[u],
                                node_id,
                                kinetic_verts.input_edge[u],
                                0,
                                0,
                                kinetic_verts.start_time[u],
                                ev.time);
            }
            kinetic_verts.end_node[v_prime] = node_id;
            kinetic_verts.end_time[v_prime] = ev.time;
            kinetic_verts.is_active[v_prime] = false;
            skeleton.add_edge(kinetic_verts.start_node[v_prime],
                              node_id,
                              kinetic_verts.input_edge[v_prime],
                              0,
                              0,
                              kinetic_verts.start_time[v_prime],
                              ev.time);
            triangles.mark_triangle_deleted(ti_u);
            pq.remove(ti_u);
            break;
          }

          kinetic_verts.end_node[u] = node_id;
          kinetic_verts.end_time[u] = ev.time;
          kinetic_verts.is_active[u] = false;
          skeleton.add_edge(kinetic_verts.start_node[u],
                            node_id,
                            kinetic_verts.input_edge[u],
                            0,
                            0,
                            kinetic_verts.start_time[u],
                            ev.time);

          kinetic_verts.ccw_dir[v_prime] = kinetic_verts.ccw_dir[u];
          const double2 new_n_in = kinetic_verts.ccw_dir[v_prime];
          const double2 new_n_out = kinetic_verts.cw_dir[v_prime];
          const double2 new_norm_in(new_n_in.y, -new_n_in.x);
          const double2 new_norm_out(-new_n_out.y, new_n_out.x);
          const auto [new_vel, new_is_inf] = compute_bisector_vel(new_norm_in, new_norm_out);
          kinetic_verts.velocity[v_prime] = new_vel;
          kinetic_verts.is_infinitely_fast[v_prime] = new_is_inf;
          if (new_is_inf) {
            is_inf_prime = true;
          }

          if (pe_u_before != -1) {
            int cur_edge = pe_u_before;
            while (cur_edge != -1) {
              const int cur_ti = cur_edge / 3;
              if (cur_ti == ti_u || triangles.is_triangle_deleted(cur_ti)) {
                break;
              }
              if (triangles.start_vert(cur_edge) != u) {
                break;
              }
              modified_triangles.add(cur_ti);
              triangles.set_start_vert(cur_edge, v_prime);
              const int prev_e = prev_triangle_edge(cur_edge);
              cur_edge = triangles.paired_edge(prev_e);
            }
          }

          triangles.mark_triangle_deleted(ti_u);
          pq.remove(ti_u);

          if (pe_u_before != -1 && pe_u_after != -1) {
            triangles.set_paired(pe_u_before, pe_u_after);
            triangles.set_paired(pe_u_after, pe_u_before);
          }
          else if (pe_u_before != -1) {
            triangles.set_paired(pe_u_before, -1);
          }
          else if (pe_u_after != -1) {
            triangles.set_paired(pe_u_after, -1);
          }

          if (pe_u_before != -1) {
            modified_triangles.add(pe_u_before / 3);
            seed_prime = pe_u_before;
          }
          if (pe_u_after != -1) {
            modified_triangles.add(pe_u_after / 3);
            seed_prime = pe_u_after;
          }
          replace_vert_in_all_triangles(u, v_prime);
          cascaded = true;
          continue;
        }
      }
    }
  }

  cleanup_degenerate_triangles();

#ifndef NDEBUG
  for (const int check_ti : IndexRange(triangles.triangles_num())) {
    if (check_ti == ti || triangles.is_triangle_deleted(check_ti)) {
      continue;
    }
    for (const int k_edge : IndexRange(3)) {
      const int vert = triangles.start_vert(3 * check_ti + k_edge);
      BLI_assert(vert != v_start);
      BLI_assert(vert != v_end);
    }
  }
#endif

  /* Recompute collapse events for all modified surviving triangles. */
  for (const int mod_ti : modified_triangles) {
    if (mod_ti == ti || triangles.is_triangle_deleted(mod_ti)) {
      continue;
    }
    const TriangleEvent new_ev = compute_triangle_collapse(
        mod_ti, triangles, kinetic_verts, current_time_, epsilon);
    if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
      pq.insert_or_update(mod_ti, new_ev);
    }
    else {
      pq.remove(mod_ti);
    }
  }

  if (is_inf_prime || (v_prime >= 0 && v_prime < kinetic_verts.size() &&
                       kinetic_verts.is_infinitely_fast[v_prime]))
  {
    const int seed = (pe_before != -1) ? pe_before :
                                         (pe_after != -1 ? next_triangle_edge(pe_after) : -1);
    handle_infinite_speed_vertex(v_prime, node_id, ev.time, seed);
  }

  const int current_active = triangles.active_triangles_count();
  if (current_active < last_active_triangles_count_) {
    journal_clear();
    last_active_triangles_count_ = current_active;
  }

  edge_collapses_count_++;
  return true;
}

bool StraightSkeletonState::process_next_event()
{
  while (!pq.is_empty()) {
    const TriangleEvent ev = pq.pop_with_flip_priority(triangles);
    if (ev.triangle_index < 0 || triangles.is_triangle_deleted(ev.triangle_index)) {

      continue;
    }

    if (ev.type == TriangleEventType::NONE || std::isinf(ev.time)) {
      continue;
    }

    switch (ev.type) {
      case TriangleEventType::EDGE_COLLAPSE: {
        if (handle_edge_collapses_) {
          if (!process_edge_collapse_event(ev)) {
            continue;
          }
        }
        return true;
      }
      case TriangleEventType::FLIP: {
        if (!process_flip_event(ev)) {
          continue;
        }
        return true;
      }
      case TriangleEventType::MULTI_SPLIT: {
        if (!process_multi_split_event(ev)) {
          continue;
        }
        return true;
      }
      case TriangleEventType::SPLIT: {
        if (!process_split_event(ev)) {
          continue;
        }
        return true;
      }
      case TriangleEventType::NONE:
        continue;
    }
  }
  return false;
}

bool StraightSkeletonState::process_multi_split_event(const TriangleEvent &ev)
{
  PRF_scope(ProfileCategory::Core);
  if (ev.triangle_index < 0 || triangles.is_triangle_deleted(ev.triangle_index)) {
    return false;
  }
  const int e1 = ev.edge_index;
  if (triangles.is_boundary(e1)) {
    return false;
  }
  const int e2 = triangles.paired_edge(e1);
  if (e2 == -1) {
    return false;
  }
  const int ti1 = triangles.triangle(e1);
  const int ti2 = triangles.triangle(e2);
  if (ti1 == ti2 || triangles.is_triangle_deleted(ti1) || triangles.is_triangle_deleted(ti2)) {
    return false;
  }

  const int u = triangles.start_vert(e1);
  const int v = triangles.end_vert(e1);
  if (u < 0 || v < 0 || u >= kinetic_verts.size() || v >= kinetic_verts.size()) {
    return false;
  }
  if (!kinetic_verts.is_active[u] || !kinetic_verts.is_active[v]) {
    return false;
  }

  current_time_ = ev.time;

  /* Add skeleton node at collision position. */
  const int node_id = skeleton.add_node(ev.event_pos, ev.time);

  /* Terminate colliding vertices u and v. */
  kinetic_verts.end_node[u] = node_id;
  kinetic_verts.end_time[u] = ev.time;
  kinetic_verts.is_active[u] = false;
  skeleton.add_edge(kinetic_verts.start_node[u],
                    node_id,
                    kinetic_verts.input_edge[u],
                    0,
                    0,
                    kinetic_verts.start_time[u],
                    ev.time);

  kinetic_verts.end_node[v] = node_id;
  kinetic_verts.end_time[v] = ev.time;
  kinetic_verts.is_active[v] = false;
  skeleton.add_edge(kinetic_verts.start_node[v],
                    node_id,
                    kinetic_verts.input_edge[v],
                    0,
                    0,
                    kinetic_verts.start_time[v],
                    ev.time);

  /* Determine normals and directions for the two new vertices born at node_id. */
  const double2 ccw_u = kinetic_verts.ccw_dir[u];
  const double2 cw_u = kinetic_verts.cw_dir[u];
  const double2 norm_in_u(ccw_u.y, -ccw_u.x);
  const double2 norm_out_u(-cw_u.y, cw_u.x);

  const double2 ccw_v = kinetic_verts.ccw_dir[v];
  const double2 cw_v = kinetic_verts.cw_dir[v];
  const double2 norm_in_v(ccw_v.y, -ccw_v.x);
  const double2 norm_out_v(-cw_v.y, cw_v.x);

  /* Vector along edge e1 from u to v as they approach collision. */
  double2 d_uv = kinetic_verts.velocity[u] - kinetic_verts.velocity[v];
  if (math::dot(d_uv, d_uv) < small_speed_sq_tol) {
    d_uv = kinetic_verts.start_pos[v] - kinetic_verts.start_pos[u];
  }

  /* Cross product with d_uv to determine which boundary edges lie on side 1 (ti1, left of d_uv)
   * versus side 2 (ti2, right of d_uv). */
  const double c_in_u = math::cross(d_uv, ccw_u);
  const double c_out_u = math::cross(d_uv, cw_u);

  double2 n_in_1, n_out_1, ccw_1, cw_1;
  double2 n_in_2, n_out_2, ccw_2, cw_2;

  if (c_in_u >= 0.0) {
    n_in_1 = norm_in_u;
    ccw_1 = ccw_u;
    n_in_2 = norm_in_v;
    ccw_2 = ccw_v;
  }
  else {
    n_in_1 = norm_in_v;
    ccw_1 = ccw_v;
    n_in_2 = norm_in_u;
    ccw_2 = ccw_u;
  }

  if (c_out_u >= 0.0) {
    n_out_1 = norm_out_u;
    cw_1 = cw_u;
    n_out_2 = norm_out_v;
    cw_2 = cw_v;
  }
  else {
    n_out_1 = norm_out_v;
    cw_1 = cw_v;
    n_out_2 = norm_out_u;
    cw_2 = cw_u;
  }

  const auto [vel_1, is_inf_1] = compute_bisector_vel(n_in_1, n_out_1);
  const auto [vel_2, is_inf_2] = compute_bisector_vel(n_in_2, n_out_2);

  const int v1_prime = kinetic_verts.append(
      ev.event_pos, vel_1, ccw_1, cw_1, ev.time, node_id, -1, -1, is_inf_1);
  const int v2_prime = kinetic_verts.append(
      ev.event_pos, vel_2, ccw_2, cw_2, ev.time, node_id, -1, -1, is_inf_2);

  const int e1_next = next_triangle_edge(e1);
  const int e1_prev = prev_triangle_edge(e1);
  const int p1_next = triangles.paired_edge(e1_next);
  const int p1_prev = triangles.paired_edge(e1_prev);

  const int e2_next = next_triangle_edge(e2);
  const int e2_prev = prev_triangle_edge(e2);
  const int p2_next = triangles.paired_edge(e2_next);
  const int p2_prev = triangles.paired_edge(e2_prev);

  VectorSet<int> modified_triangles;

  /* Traverse fan around u on Side 1 and replace with v1_prime. */
  if (p1_prev != -1) {
    int cur_edge = p1_prev;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (cur_ti == ti1 || cur_ti == ti2 || triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      if (triangles.start_vert(cur_edge) != u) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(cur_edge, v1_prime);
      const int in_edge = prev_triangle_edge(cur_edge);
      cur_edge = triangles.paired_edge(in_edge);
    }
  }

  /* Traverse fan around v on Side 1 and replace with v1_prime. */
  if (p1_next != -1) {
    int cur_edge = p1_next;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (cur_ti == ti1 || cur_ti == ti2 || triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      const int out_edge = next_triangle_edge(cur_edge);
      if (triangles.start_vert(out_edge) != v) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(out_edge, v1_prime);
      cur_edge = triangles.paired_edge(out_edge);
    }
  }

  /* Traverse fan around u on Side 2 and replace with v2_prime. */
  if (p2_next != -1) {
    int cur_edge = p2_next;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (cur_ti == ti1 || cur_ti == ti2 || triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      const int out_edge = next_triangle_edge(cur_edge);
      if (triangles.start_vert(out_edge) != u) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(out_edge, v2_prime);
      cur_edge = triangles.paired_edge(out_edge);
    }
  }

  /* Traverse fan around v on Side 2 and replace with v2_prime. */
  if (p2_prev != -1) {
    int cur_edge = p2_prev;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (cur_ti == ti1 || cur_ti == ti2 || triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      if (triangles.start_vert(cur_edge) != v) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(cur_edge, v2_prime);
      const int in_edge = prev_triangle_edge(cur_edge);
      cur_edge = triangles.paired_edge(in_edge);
    }
  }

  /* Stitch holes in triangulation left by deleting ti1 and ti2. */
  if (p1_next != -1 && p1_prev != -1) {
    triangles.set_paired(p1_next, p1_prev);
    triangles.set_paired(p1_prev, p1_next);
  }
  else if (p1_next != -1) {
    triangles.set_paired(p1_next, -1);
  }
  else if (p1_prev != -1) {
    triangles.set_paired(p1_prev, -1);
  }
  else {
    /* ti1 was an ear / 3-gon with 2 boundary edges collapsing simultaneously.
     * Terminate the third vertex so it is not orphaned. */
    const int v_opp1 = triangles.end_vert(e1_next);
    if (v_opp1 >= 0 && v_opp1 < kinetic_verts.size() && kinetic_verts.is_active[v_opp1]) {
      kinetic_verts.end_node[v_opp1] = node_id;
      kinetic_verts.end_time[v_opp1] = ev.time;
      kinetic_verts.is_active[v_opp1] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_opp1],
                        node_id,
                        kinetic_verts.input_edge[v_opp1],
                        0,
                        0,
                        kinetic_verts.start_time[v_opp1],
                        ev.time);
    }
  }
  triangles.mark_triangle_deleted(ti1);
  pq.remove(ti1);

  if (p2_next != -1 && p2_prev != -1) {
    triangles.set_paired(p2_next, p2_prev);
    triangles.set_paired(p2_prev, p2_next);
  }
  else if (p2_next != -1) {
    triangles.set_paired(p2_next, -1);
  }
  else if (p2_prev != -1) {
    triangles.set_paired(p2_prev, -1);
  }
  else {
    /* ti2 was an ear / 3-gon with 2 boundary edges collapsing simultaneously.
     * Terminate the third vertex so it is not orphaned. */
    const int v_opp2 = triangles.end_vert(e2_next);
    if (v_opp2 >= 0 && v_opp2 < kinetic_verts.size() && kinetic_verts.is_active[v_opp2]) {
      kinetic_verts.end_node[v_opp2] = node_id;
      kinetic_verts.end_time[v_opp2] = ev.time;
      kinetic_verts.is_active[v_opp2] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_opp2],
                        node_id,
                        kinetic_verts.input_edge[v_opp2],
                        0,
                        0,
                        kinetic_verts.start_time[v_opp2],
                        ev.time);
    }
  }
  triangles.mark_triangle_deleted(ti2);
  pq.remove(ti2);

  if (p1_next != -1) {
    modified_triangles.add(p1_next / 3);
  }
  if (p1_prev != -1) {
    modified_triangles.add(p1_prev / 3);
  }
  if (p2_next != -1) {
    modified_triangles.add(p2_next / 3);
  }
  if (p2_prev != -1) {
    modified_triangles.add(p2_prev / 3);
  }

#ifndef NDEBUG
  for (const int check_ti : IndexRange(triangles.triangles_num())) {
    if (check_ti == ti1 || check_ti == ti2 || triangles.is_triangle_deleted(check_ti)) {
      continue;
    }
    for (const int k_edge : IndexRange(3)) {
      const int vert = triangles.start_vert(3 * check_ti + k_edge);
      BLI_assert(vert != u);
      BLI_assert(vert != v);
    }
  }
#endif

  /* Recompute collapse events for all modified triangles. */
  for (const int mod_ti : modified_triangles) {
    if (mod_ti == ti1 || mod_ti == ti2 || triangles.is_triangle_deleted(mod_ti)) {
      continue;
    }
    const TriangleEvent new_ev = compute_triangle_collapse(
        mod_ti, triangles, kinetic_verts, current_time_, epsilon);
    if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
      pq.insert_or_update(mod_ti, new_ev);
    }
    else {
      pq.remove(mod_ti);
    }
  }

  if (is_inf_1) {
    const int seed1 = (p1_prev != -1) ? p1_prev :
                                        (p1_next != -1 ? next_triangle_edge(p1_next) : -1);
    handle_infinite_speed_vertex(v1_prime, node_id, ev.time, seed1);
  }
  if (is_inf_2) {
    const int seed2 = (p2_next != -1) ? next_triangle_edge(p2_next) :
                                        (p2_prev != -1 ? p2_prev : -1);
    handle_infinite_speed_vertex(v2_prime, node_id, ev.time, seed2);
  }

  const int current_active = triangles.active_triangles_count();
  if (current_active < last_active_triangles_count_) {
    journal_clear();
    last_active_triangles_count_ = current_active;
  }

  multi_splits_count_++;
  return true;
}

bool StraightSkeletonState::process_split_event(const TriangleEvent &ev)
{
  PRF_scope(ProfileCategory::Core);
  if (ev.triangle_index < 0 || triangles.is_triangle_deleted(ev.triangle_index)) {
    return false;
  }
  const int ti = ev.triangle_index;
  const int e0 = 3 * ti;
  const int e_hit = ev.edge_index;
  const int k = e_hit - e0;
  const int v_start = triangles.start_vert(e_hit);
  const int v_end = triangles.end_vert(e_hit);
  const int v_opp = triangles.start_vert(e0 + ((k + 2) % 3));

  if (!kinetic_verts.is_active[v_opp]) {
    return false;
  }

  current_time_ = ev.time;

  /* Add skeleton node at split position. */
  const int node_id = skeleton.add_node(ev.event_pos, ev.time);

  /* Terminate v_opp at node_id. */
  kinetic_verts.end_node[v_opp] = node_id;
  kinetic_verts.end_time[v_opp] = ev.time;
  kinetic_verts.is_active[v_opp] = false;
  skeleton.add_edge(kinetic_verts.start_node[v_opp],
                    node_id,
                    kinetic_verts.input_edge[v_opp],
                    0,
                    0,
                    kinetic_verts.start_time[v_opp],
                    ev.time);

  const int e_before = e0 + ((k + 2) % 3); /* v_opp -> v_start */
  const int e_after = e0 + ((k + 1) % 3);  /* v_end -> v_opp */
  const int pe_before = triangles.paired_edge(e_before);
  const int pe_after = triangles.paired_edge(e_after);

  /* If all boundary or triangle completely collapses (3-gon collapse): */
  const bool all_boundary = (pe_before == -1 && pe_after == -1);
  if (all_boundary) {
    if (kinetic_verts.is_active[v_start]) {
      kinetic_verts.end_node[v_start] = node_id;
      kinetic_verts.end_time[v_start] = ev.time;
      kinetic_verts.is_active[v_start] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_start],
                        node_id,
                        kinetic_verts.input_edge[v_start],
                        0,
                        0,
                        kinetic_verts.start_time[v_start],
                        ev.time);
    }
    if (kinetic_verts.is_active[v_end]) {
      kinetic_verts.end_node[v_end] = node_id;
      kinetic_verts.end_time[v_end] = ev.time;
      kinetic_verts.is_active[v_end] = false;
      skeleton.add_edge(kinetic_verts.start_node[v_end],
                        node_id,
                        kinetic_verts.input_edge[v_end],
                        0,
                        0,
                        kinetic_verts.start_time[v_end],
                        ev.time);
    }
    triangles.mark_triangle_deleted(ti);
    pq.remove(ti);
    splits_count_++;
    return true;
  }

  /* Compute velocities for the two newly born vertices at node_id. */
  const double2 pos_start = kinetic_verts.position_at(v_start, ev.time);
  const double2 pos_end = kinetic_verts.position_at(v_end, ev.time);
  const double2 d_hit = math::normalize(pos_end - pos_start);
  const double2 n_hit(-d_hit.y, d_hit.x);

  const double2 ccw_opp = kinetic_verts.ccw_dir[v_opp];
  const double2 cw_opp = kinetic_verts.cw_dir[v_opp];
  const double2 n_in_opp(ccw_opp.y, -ccw_opp.x);
  const double2 n_out_opp(-cw_opp.y, cw_opp.x);

  /* Vertex A (Loop A: v_start -> P -> v_opp_next): */
  const double2 ccw_a = -d_hit;
  const double2 cw_a = cw_opp;
  const double2 n_in_a = n_hit;
  const double2 n_out_a = n_out_opp;
  const auto [vel_a, is_inf_a] = compute_bisector_vel(n_in_a, n_out_a);

  /* Vertex B (Loop B: v_opp_prev -> P -> v_end): */
  const double2 ccw_b = ccw_opp;
  const double2 cw_b = d_hit;
  const double2 n_in_b = n_in_opp;
  const double2 n_out_b = n_hit;
  const auto [vel_b, is_inf_b] = compute_bisector_vel(n_in_b, n_out_b);

  const int kv_a = kinetic_verts.append(
      ev.event_pos, vel_a, ccw_a, cw_a, ev.time, node_id, -1, -1, is_inf_a);
  const int kv_b = kinetic_verts.append(
      ev.event_pos, vel_b, ccw_b, cw_b, ev.time, node_id, -1, -1, is_inf_b);

  VectorSet<int> modified_triangles;

  /* Traverse and update the 'before' fan of triangles around v_opp with kv_a. */
  if (pe_before != -1) {
    triangles.set_paired(pe_before, -1);
    int cur_edge = pe_before;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      const int out_edge = next_triangle_edge(cur_edge);
      if (triangles.start_vert(out_edge) != v_opp) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(out_edge, kv_a);
      cur_edge = triangles.paired_edge(out_edge);
    }
  }

  /* Traverse and update the 'after' fan of triangles around v_opp with kv_b. */
  if (pe_after != -1) {
    triangles.set_paired(pe_after, -1);
    int cur_edge = pe_after;
    while (cur_edge != -1) {
      const int cur_ti = cur_edge / 3;
      if (triangles.is_triangle_deleted(cur_ti)) {
        break;
      }
      if (triangles.start_vert(cur_edge) != v_opp) {
        break;
      }
      modified_triangles.add(cur_ti);
      triangles.set_start_vert(cur_edge, kv_b);
      const int in_edge = prev_triangle_edge(cur_edge);
      cur_edge = triangles.paired_edge(in_edge);
    }
  }

  /* Mark ti deleted and remove from priority queue. */
  triangles.mark_triangle_deleted(ti);
  pq.remove(ti);

  /* Recompute collapse events for all modified triangles. */
  for (const int mod_ti : modified_triangles) {
    if (mod_ti == ti || triangles.is_triangle_deleted(mod_ti)) {
      continue;
    }
    const TriangleEvent new_ev = compute_triangle_collapse(
        mod_ti, triangles, kinetic_verts, current_time_, epsilon);
    if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
      pq.insert_or_update(mod_ti, new_ev);
    }
    else {
      pq.remove(mod_ti);
    }
  }

  if (is_inf_a) {
    const int seed_a = (pe_before != -1) ? next_triangle_edge(pe_before) : -1;
    handle_infinite_speed_vertex(kv_a, node_id, ev.time, seed_a);
  }
  if (is_inf_b) {
    const int seed_b = (pe_after != -1) ? pe_after : -1;
    handle_infinite_speed_vertex(kv_b, node_id, ev.time, seed_b);
  }

  const int current_active = triangles.active_triangles_count();
  if (current_active < last_active_triangles_count_) {
    journal_clear();
    last_active_triangles_count_ = current_active;
  }

  splits_count_++;
  return true;
}

void StraightSkeletonState::journal_record_flip(const int ti1,
                                                const int ti2,
                                                const int flipped_edge,
                                                const int new_edge,
                                                const double time,
                                                const int3 &sorted_verts)
{
  JournalEntry entry;
  entry.triangles_remaining_num = triangles.active_triangles_count();
  entry.time = time;
  entry.triangle_verts = sorted_verts;
  entry.flipped_edge = flipped_edge;
  entry.new_edge = new_edge;
  entry.ti1 = ti1;
  entry.ti2 = ti2;
  entry.parent_entry_index = -1;

  const int entry_idx = int(journal_history_.size());
  journal_history_.append(entry);
  journal_search_.add(sorted_verts, entry_idx);
}

bool StraightSkeletonState::retriangulate_collapsed_polygon(const Set<int> &p_triangles,
                                                            const Span<int> p_verts,
                                                            const double /*t_star*/,
                                                            const int t1_entry_idx)
{
  PRF_scope(ProfileCategory::Core);
  const int k = int(p_verts.size());

  if (k < 3 || p_triangles.is_empty()) {
    return false;
  }

  /* Map each vertex in P to its index 0 <= idx < k along the collapse line. */
  Map<int, int> vert_to_idx;
  for (const int i : IndexRange(k)) {
    vert_to_idx.add(p_verts[i], i);
  }

  /* Boundary edges of P and attached exterior triangles tri_e. */
  struct BoundaryEdgeInfo {
    int u{-1};
    int v{-1};
    int u_idx{-1};
    int v_idx{-1};
    int pe_ext{-1};
    int ti_ext{-1};
    int ve{-1};
    bool is_forward{false};
  };

  Vector<BoundaryEdgeInfo> boundary_edges;

  for (const int ti : p_triangles) {
    if (triangles.is_triangle_deleted(ti)) {
      continue;
    }
    for (const int corner : IndexRange(3)) {
      const int e = 3 * ti + corner;
      const int pe = triangles.paired_edge(e);
      const int p_tri = (pe != -1) ? triangles.triangle(pe) : -1;
      if (p_tri == -1 || !p_triangles.contains(p_tri)) {
        /* Edge e is on the boundary of polygon P. */
        const int u = triangles.start_vert(e);
        const int v = triangles.end_vert(e);
        const int *u_idx_ptr = vert_to_idx.lookup_ptr(u);
        const int *v_idx_ptr = vert_to_idx.lookup_ptr(v);
        if (u_idx_ptr == nullptr || v_idx_ptr == nullptr || *u_idx_ptr == *v_idx_ptr) {
          continue;
        }
        const int u_idx = *u_idx_ptr;
        const int v_idx = *v_idx_ptr;

        int ve = -1;
        if (pe != -1) {
          const int pe_prev = prev_triangle_edge(pe);
          ve = triangles.start_vert(pe_prev);
        }

        BoundaryEdgeInfo info;
        info.u = u;
        info.v = v;
        info.u_idx = u_idx;
        info.v_idx = v_idx;
        info.pe_ext = pe;
        info.ti_ext = p_tri;
        info.ve = ve;
        info.is_forward = (u_idx < v_idx);
        boundary_edges.append(info);
      }
    }
  }

  /* Map each monotone segment i in [0, k-2] to its forward and backward boundary edges. */
  Vector<int> seg_fwd_bnd(k - 1, -1);
  Vector<int> seg_bwd_bnd(k - 1, -1);
  for (const int b_idx : boundary_edges.index_range()) {
    const BoundaryEdgeInfo &be = boundary_edges[b_idx];
    if (be.is_forward) {
      for (int i = be.u_idx; i < be.v_idx; i++) {
        seg_fwd_bnd[i] = b_idx;
      }
    }
    else {
      for (int i = be.v_idx; i < be.u_idx; i++) {
        seg_bwd_bnd[i] = b_idx;
      }
    }
  }

  for (const int i : IndexRange(k - 1)) {
    if (seg_fwd_bnd[i] == -1 || seg_bwd_bnd[i] == -1) {
      return false;
    }
  }

  /* Collect available triangle indices from P and attached tri_e triangles. */
  Vector<int> available_triangles;
  for (const int ti : p_triangles) {
    if (!available_triangles.contains(ti)) {
      available_triangles.append(ti);
    }
  }
  for (const BoundaryEdgeInfo &be : boundary_edges) {
    if (be.ti_ext != -1 && !available_triangles.contains(be.ti_ext)) {
      available_triangles.append(be.ti_ext);
    }
  }

  /* Collect external half-edges connecting to tri_e triangles from outside R. */
  Vector<int> outer_half_edges;
  for (const BoundaryEdgeInfo &be : boundary_edges) {
    if (be.pe_ext != -1) {
      const int pe_next = next_triangle_edge(be.pe_ext);
      const int pe_prev = prev_triangle_edge(be.pe_ext);
      const int out_pair_next = triangles.paired_edge(pe_next);
      const int out_pair_prev = triangles.paired_edge(pe_prev);
      if (out_pair_next != -1 && !available_triangles.contains(triangles.triangle(out_pair_next)))
      {
        if (!outer_half_edges.contains(out_pair_next)) {
          outer_half_edges.append(out_pair_next);
        }
      }
      if (out_pair_prev != -1 && !available_triangles.contains(triangles.triangle(out_pair_prev)))
      {
        if (!outer_half_edges.contains(out_pair_prev)) {
          outer_half_edges.append(out_pair_prev);
        }
      }
    }
  }

  const int num_needed = 2 * (k - 1);
  while (available_triangles.size() < num_needed) {
    triangles.add_triangle(-1, -1, -1);
    available_triangles.append(triangles.triangles_num() - 1);
  }

  /* Remove any prior events from pq for all available triangles. */
  for (const int ti : available_triangles) {
    pq.remove(ti);
  }

  /* Assign the 2*(k-1) replacement triangles:
   * For segment i:
   *   Side -: [p_verts[i], p_verts[i+1], ve_minus]
   *   Side +: [p_verts[i+1], p_verts[i], ve_plus] */
  for (const int i : IndexRange(k - 1)) {
    const int ti_minus = available_triangles[2 * i];
    const int ti_plus = available_triangles[2 * i + 1];

    const int ve_minus = boundary_edges[seg_bwd_bnd[i]].ve;
    const int ve_plus = boundary_edges[seg_fwd_bnd[i]].ve;

    triangles.set_triangle_verts(ti_minus, p_verts[i], p_verts[i + 1], ve_minus);
    triangles.set_triangle_verts(ti_plus, p_verts[i + 1], p_verts[i], ve_plus);

    /* Initialize paired_ to -1. */
    for (const int corner : IndexRange(3)) {
      triangles.set_paired(3 * ti_minus + corner, -1);
      triangles.set_paired(3 * ti_plus + corner, -1);
    }
  }

  /* Build map of directed edges in the newly formed triangles: (u, v) -> half_edge. */
  Map<std::pair<int, int>, int> directed_to_halfedge;
  for (const int idx : IndexRange(num_needed)) {
    const int ti = available_triangles[idx];
    for (const int corner : IndexRange(3)) {
      const int h = 3 * ti + corner;
      const int u = triangles.start_vert(h);
      const int v = triangles.end_vert(h);
      directed_to_halfedge.add({u, v}, h);
    }
  }

  /* Connect half-edge pairs:
   * 1. Internal pairs within R: if (v, u) is in directed_to_halfedge.
   * 2. External pairs to outer mesh: match with outer_half_edges. */
  Set<int> updated_external_triangles;
  for (const int idx : IndexRange(num_needed)) {
    const int ti = available_triangles[idx];
    for (const int corner : IndexRange(3)) {
      const int h = 3 * ti + corner;
      if (triangles.paired_edge(h) != -1) {
        continue;
      }
      const int u = triangles.start_vert(h);
      const int v = triangles.end_vert(h);

      const int *twin_h_ptr = directed_to_halfedge.lookup_ptr({v, u});
      if (twin_h_ptr != nullptr) {
        const int twin_h = *twin_h_ptr;
        triangles.set_paired(h, twin_h);
        triangles.set_paired(twin_h, h);
      }
      else {
        /* Look for an outer half-edge connecting v -> u. */
        for (const int out_h : outer_half_edges) {
          if (triangles.start_vert(out_h) == v && triangles.end_vert(out_h) == u) {
            triangles.set_paired(h, out_h);
            triangles.set_paired(out_h, h);
            updated_external_triangles.add(triangles.triangle(out_h));
            break;
          }
        }
      }
    }
  }

  /* Any remaining triangles beyond num_needed are marked deleted. */
  for (int idx = num_needed; idx < available_triangles.size(); idx++) {
    triangles.mark_triangle_deleted(available_triangles[idx]);
  }

  /* Recompute collapse events for external neighbor triangles whose pairings changed. */
  for (const int ext_ti : updated_external_triangles) {
    if (!triangles.is_triangle_deleted(ext_ti)) {
      const TriangleEvent rev = compute_triangle_collapse(
          ext_ti, triangles, kinetic_verts, current_time_, epsilon);
      pq.remove(ext_ti);
      if (rev.type != TriangleEventType::NONE && !std::isinf(rev.time) &&
          rev.time >= current_time_ - event_time_tol)
      {
        pq.insert_or_update(ext_ti, rev);
      }
    }
  }

  /* Schedule collapse events for the newly formed triangles. */
  for (const int idx : IndexRange(num_needed)) {
    const int ti = available_triangles[idx];
    const TriangleEvent rev = compute_triangle_collapse(
        ti, triangles, kinetic_verts, current_time_, epsilon);
    if (rev.type != TriangleEventType::NONE && !std::isinf(rev.time) &&
        rev.time >= current_time_ - event_time_tol)
    {
      pq.insert_or_update(ti, rev);
    }

    /* Record in journal pointing back to t1_entry_idx. */
    const int3 sorted_verts = get_sorted_triangle_verts(triangles, ti);
    JournalEntry entry;
    entry.ti1 = ti;
    entry.time = current_time_;
    entry.triangle_verts = sorted_verts;
    entry.parent_entry_index = t1_entry_idx;
    const int entry_idx = int(journal_history_.size());
    journal_history_.append(entry);
    journal_search_.add(sorted_verts, entry_idx);
  }
  return true;
}

bool StraightSkeletonState::check_and_handle_flip_cycle(const TriangleEvent &ev)
{
  PRF_scope(ProfileCategory::Core);
  const int ti = ev.triangle_index;
  if (ti < 0 || triangles.is_triangle_deleted(ti)) {
    return false;
  }
  const int3 sorted_verts = get_sorted_triangle_verts(triangles, ti);

  const int *t1_ptr = journal_search_.lookup_ptr(sorted_verts);
  if (t1_ptr == nullptr) {
    return false;
  }
  if (std::abs(journal_history_[*t1_ptr].time - ev.time) > journal_time_tol) {
    return false;
  }

  /* Triangle tri was already processed at the same t* (Palfreder et al. Section 3.2).
   * Potential loop detected between first occurrence T1 and current occurrence T2. */
  int t1 = *t1_ptr;
  while (journal_history_[t1].parent_entry_index != -1) {
    t1 = journal_history_[t1].parent_entry_index;
  }
  const int t2 = int(journal_history_.size());

  /* Identify the set of triangles that form the collapsed polygon P. */

  Set<int> p_triangles;
  for (int i = t1; i < t2; i++) {
    p_triangles.add(journal_history_[i].ti1);
    p_triangles.add(journal_history_[i].ti2);
  }
  p_triangles.add(ti);
  const int paired_e = triangles.paired_edge(ev.edge_index);
  if (paired_e != -1) {
    p_triangles.add(triangles.triangle(paired_e));
  }

  /* Identify all vertices in polygon P. */
  Set<int> p_verts_set;
  for (const int tri_idx : p_triangles) {
    if (!triangles.is_triangle_deleted(tri_idx)) {
      for (const int k : IndexRange(3)) {
        p_verts_set.add(triangles.start_vert(3 * tri_idx + k));
      }
    }
  }

  Vector<int> p_verts;
  p_verts.reserve(p_verts_set.size());
  for (const int v : p_verts_set) {
    p_verts.append(v);
  }

  /* Sort vertices along the line to which P collapsed at t* = ev.time. */
  if (p_verts.size() >= 2) {
    const double t_star = ev.time;
    auto get_pos = [&](const int vi) -> double2 {
      if (vi < kinetic_verts.size()) {
        return kinetic_verts.position_at(vi, t_star);
      }
      return double2(0.0, 0.0);
    };

    /* Find direction of the collinear line from the two vertices farthest apart. */
    double max_dist_sq = -1.0;
    double2 line_dir(1.0, 0.0);
    double2 ref_pos = get_pos(p_verts[0]);
    for (int i = 0; i < p_verts.size(); i++) {
      for (int j = i + 1; j < p_verts.size(); j++) {
        const double2 d = get_pos(p_verts[j]) - get_pos(p_verts[i]);
        const double d_sq = math::dot(d, d);
        if (d_sq > max_dist_sq) {
          max_dist_sq = d_sq;
          line_dir = d;
          ref_pos = get_pos(p_verts[i]);
        }
      }
    }

    std::sort(p_verts.begin(), p_verts.end(), [&](const int a, const int b) {
      const double proj_a = math::dot(get_pos(a) - ref_pos, line_dir);
      const double proj_b = math::dot(get_pos(b) - ref_pos, line_dir);
      return proj_a < proj_b;
    });
  }

  /* Rollback: undo all flips in P from T2 down to T1 in reverse order. */
  for (int i = t2 - 1; i >= t1; i--) {
    const JournalEntry &entry = journal_history_[i];
    if (p_triangles.contains(entry.ti1) || p_triangles.contains(entry.ti2)) {
      /* Undo flip by flipping the new diagonal back to the original. */
      flip_edge(triangles, entry.new_edge);
      journal_search_.remove(entry.triangle_verts);
      journal_history_.pop_last();
    }
  }

  /* Section 3.2 (Figure 5): Retriangulate collapsed polygon P and attached triangles. */
  if (!retriangulate_collapsed_polygon(p_triangles, p_verts, ev.time, t1)) {
    return false;
  }

  journal_clear();
  return true;
}

bool StraightSkeletonState::process_flip_event(const TriangleEvent &ev)
{
  PRF_scope(ProfileCategory::Core);
  if (ev.triangle_index < 0 || triangles.is_triangle_deleted(ev.triangle_index)) {
    return false;
  }
  const int e1 = ev.edge_index;
  const int e2 = triangles.paired_edge(e1);
  if (e2 == -1) {
    return false;
  }
  const int ti1 = triangles.triangle(e1);
  const int ti2 = triangles.triangle(e2);
  if (ti1 == ti2 || triangles.is_triangle_deleted(ti1) || triangles.is_triangle_deleted(ti2)) {
    return false;
  }

  if (ev.time > current_time_ + time_advance_tol) {
    journal_clear();
  }

  /* Section 3.2: Check if this flip event repeats a collapsed triangle in the journal. */
  if (check_and_handle_flip_cycle(ev)) {
    return true;
  }

  current_time_ = ev.time;

  const int3 sorted_verts = get_sorted_triangle_verts(triangles, ti1);

  const int h1_0 = e1;
  const int h1_1 = next_triangle_edge(h1_0);
  const int h1_2 = prev_triangle_edge(h1_0);
  const int v1 = triangles.start_vert(h1_0);
  const int v2 = triangles.start_vert(h1_1);
  const int v = triangles.start_vert(h1_2);

  const int h2_0 = e2;
  const int h2_2 = prev_triangle_edge(h2_0);
  const int v3 = triangles.start_vert(h2_2);

  const double2 pv = kinetic_verts.position_at(v, ev.time);
  const double2 pv1 = kinetic_verts.position_at(v1, ev.time);
  const double2 pv2 = kinetic_verts.position_at(v2, ev.time);
  const double2 pv3 = kinetic_verts.position_at(v3, ev.time);

  const double area_new1 = math::cross(pv1 - pv, pv3 - pv);
  const double area_new2 = math::cross(pv2 - pv3, pv - pv3);
  if (area_new1 <= 1e-12 || area_new2 <= 1e-12) {
    return false;
  }

  int new_edge = -1;
  if (!flip_edge(triangles, e1, &new_edge)) {
    return false;
  }

  flips_count_++;

  journal_record_flip(ti1, ti2, e1, new_edge, current_time_, sorted_verts);

  const TriangleEvent ev1 = compute_triangle_collapse(
      ti1, triangles, kinetic_verts, current_time_, epsilon);
  const TriangleEvent ev2 = compute_triangle_collapse(
      ti2, triangles, kinetic_verts, current_time_, epsilon);

  pq.remove(ti1);
  pq.remove(ti2);

  if (ev1.type != TriangleEventType::NONE && !std::isinf(ev1.time)) {
    pq.insert_or_update(ti1, ev1);
  }

  if (ev2.type != TriangleEventType::NONE && !std::isinf(ev2.time)) {
    pq.insert_or_update(ti2, ev2);
  }

  return true;
}

void StraightSkeletonState::run_event_loop(const int max_events)
{
  PRF_scope(ProfileCategory::Core);
  int count = 0;
  while (!pq.is_empty() && count < max_events) {
    if (!process_next_event()) {
      break;
    }
    count++;
  }

  events_processed_num_ = count;

  if (count >= max_events && !pq.is_empty()) {
    hit_max_events_ = true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Output Face Construction
 * \{ */

static void build_output_faces(const StraightSkeletonInput &input,
                               const SkeletonGraph &skel,
                               StraightSkeletonOutput &r_output)
{
  PRF_scope(ProfileCategory::Core);
  const int nodes_num = int(skel.nodes.size());
  if (nodes_num == 0) {
    return;
  }

  /* Merge internal skeleton nodes connected by micro-edges. */
  Array<int> parent(nodes_num);
  for (const int i : IndexRange(nodes_num)) {
    parent[i] = i;
  }
  auto find_root = [&](auto &self, const int i) -> int {
    if (parent[i] == i) {
      return i;
    }
    return parent[i] = self(self, parent[i]);
  };

  const double micro_edge_tol = 1e-4;
  for (const SkeletonEdge &se : skel.edges) {
    if (se.node1 >= 0 && se.node1 < nodes_num && se.node2 >= 0 && se.node2 < nodes_num) {
      if (skel.nodes[se.node1].orig_vert == -1 && skel.nodes[se.node2].orig_vert == -1) {
        const double d = math::distance(skel.nodes[se.node1].pos, skel.nodes[se.node2].pos);
        const double dt = std::abs(skel.nodes[se.node1].time - skel.nodes[se.node2].time);
        if (d < micro_edge_tol && dt < 1e-4) {
          const int r1 = find_root(find_root, se.node1);
          const int r2 = find_root(find_root, se.node2);
          if (r1 != r2) {
            parent[r1] = r2;
          }
        }
      }
    }
  }

  /* 1. Collect all planar graph edges: input boundary edges + skeleton edges. */
  Array<Vector<int>> adj(nodes_num);

  auto add_adj_edge = [&](const int u_raw, const int v_raw) {
    const int u = find_root(find_root, u_raw);
    const int v = find_root(find_root, v_raw);
    if (u >= 0 && u < nodes_num && v >= 0 && v < nodes_num && u != v) {
      if (!adj[u].contains(v)) {
        adj[u].append(v);
      }
      if (!adj[v].contains(u)) {
        adj[v].append(u);
      }
    }
  };

  /* Outer polygon boundary edges (t = 0). */
  const int n_outer = int(input.outer_polygon.size());
  for (const int i : IndexRange(n_outer)) {
    const int u = input.outer_polygon[i];
    const int v = input.outer_polygon[(i + 1) % n_outer];
    add_adj_edge(u, v);
  }

  /* Hole polygon boundary edges (t = 0). */
  const int n_holes = input.hole_offsets.size();
  for (const int h : IndexRange(n_holes)) {
    const Span<int> hole = input.hole_vert_indices.slice(input.hole_offsets[h]);
    const int n_hole = int(hole.size());
    for (const int i : IndexRange(n_hole)) {
      const int u = hole[i];
      const int v = hole[(i + 1) % n_hole];
      add_adj_edge(u, v);
    }
  }

  /* Straight skeleton edges (rays and ridges). */
  for (const SkeletonEdge &se : skel.edges) {
    add_adj_edge(se.node1, se.node2);
  }

  /* Prune dead-end internal skeleton nodes (degree <= 1).
   * Boundary vertices (orig_vert != -1) must not be pruned. */
  Vector<int> leaf_queue;
  for (const int u : IndexRange(nodes_num)) {
    if (skel.nodes[u].orig_vert == -1 && adj[u].size() <= 1) {
      leaf_queue.append(u);
    }
  }
  while (!leaf_queue.is_empty()) {
    const int leaf = leaf_queue.pop_last();
    if (adj[leaf].size() == 1) {
      const int neighbor = adj[leaf][0];
      adj[leaf].clear();
      adj[neighbor].remove_first_occurrence_and_reorder(leaf);
      if (skel.nodes[neighbor].orig_vert == -1 && adj[neighbor].size() <= 1) {
        leaf_queue.append(neighbor);
      }
    }
  }

  /* 2. Sort outgoing edges counter-clockwise around each vertex by polar angle. */
  for (const int u : IndexRange(nodes_num)) {
    const double2 p_u = skel.nodes[u].pos;
    std::sort(adj[u].begin(), adj[u].end(), [&](const int a, const int b) {
      const double2 p_a = skel.nodes[a].pos;
      const double2 p_b = skel.nodes[b].pos;
      const double ang_a = std::atan2(p_a.y - p_u.y, p_a.x - p_u.x);
      const double ang_b = std::atan2(p_b.y - p_u.y, p_b.x - p_u.x);
      return ang_a < ang_b;
    });
  }

  /* 3. For each input boundary edge u -> v, trace its roof face. */
  Vector<int> face_indices;
  Vector<int> face_lens;
  Vector<int> face_orig_list;
  VectorSet<Vector<int>> seen_faces;

  auto process_simple_cycle = [&](Span<int> sub_cycle, const int orig_face_idx) {
    const int m = int(sub_cycle.size());
    if (m < 3) {
      return;
    }

    /* Verify that face has positive (CCW) signed area. */
    double area = 0.0;
    for (const int i : IndexRange(m)) {
      const double2 p1 = skel.nodes[sub_cycle[i]].pos;
      const double2 p2 = skel.nodes[sub_cycle[(i + 1) % m]].pos;
      area += (p1.x * p2.y - p2.x * p1.y);
    }
    area *= 0.5;

    if (area <= min_face_area) {
      return;
    }

    /* Canonical representation: cyclically rotated so smallest vertex index is first. */
    int min_idx = 0;
    for (const int i : IndexRange(1, m - 1)) {
      if (sub_cycle[i] < sub_cycle[min_idx]) {
        min_idx = i;
      }
    }
    Vector<int> canonical(m);
    for (const int i : IndexRange(m)) {
      canonical[i] = sub_cycle[(min_idx + i) % m];
    }

    if (!seen_faces.add(canonical)) {
      return;
    }

    face_indices.extend(sub_cycle);
    face_lens.append(m);
    face_orig_list.append(orig_face_idx);
  };

  auto trace_face = [&](const int u_start, const int v_start, const int orig_face_idx) {
    Vector<int> cycle;
    cycle.append(u_start);

    int curr_u = u_start;
    int curr_v = v_start;
    int step = 0;
    const int max_steps = 4 * nodes_num;

    while (step < max_steps) {
      cycle.append(curr_v);
      if (curr_v == u_start) {
        break;
      }

      const Vector<int> &out_edges = adj[curr_v];
      const int out_num = int(out_edges.size());
      if (out_num == 0) {
        break;
      }

      const int64_t idx = out_edges.first_index_of_try(curr_u);
      if (idx == -1) {
        break;
      }

      /* Sharpest right turn: predecessor in CCW cyclic order. */
      const int next_idx = int((idx - 1 + out_num) % out_num);
      const int next_v = out_edges[next_idx];
      if (next_v == curr_u) {
        break;
      }

      curr_u = curr_v;
      curr_v = next_v;
      step++;
    }

    if (!(cycle.size() >= 4 && cycle.last() == u_start)) {
      return;
    }

    /* Decompose closed walk into simple non-self-intersecting cycles. */
    Vector<int> stack;
    Map<int, int> pos_in_stack;
    bool stitched = false;
    for (const int v : cycle) {
      const int *prev_pos = pos_in_stack.lookup_ptr(v);
      if (prev_pos != nullptr) {
        const int start_idx = *prev_pos;
        Vector<int> sub;
        for (int i = start_idx; i < stack.size(); i++) {
          sub.append(stack[i]);
          pos_in_stack.remove(stack[i]);
        }
        stack.resize(start_idx);
        if (sub.size() < 3) {
          /* A 2-cycle backtrack occurred. The stack is now bridged across a dead-end spur. */
          stitched = true;
        }
        else {
          /* Once a 2-cycle spur has been popped, any cycle ending back at start_idx == 0 is
           * a composite remainder bridging across two separate faces and must not be emitted. */
          if (!(start_idx == 0 && stitched)) {
            process_simple_cycle(sub.as_span(), orig_face_idx);
          }
        }
      }
      pos_in_stack.add(v, int(stack.size()));
      stack.append(v);
    }
  };

  /* Trace faces from outer polygon edges. */
  for (const int i : IndexRange(n_outer)) {
    const int u = input.outer_polygon[i];
    const int v = input.outer_polygon[(i + 1) % n_outer];
    trace_face(u, v, 0);
  }

  /* Trace faces from hole polygon edges. */
  for (const int h : IndexRange(n_holes)) {
    const Span<int> hole = input.hole_vert_indices.slice(input.hole_offsets[h]);
    const int n_hole = int(hole.size());
    for (const int i : IndexRange(n_hole)) {
      const int u = hole[i];
      const int v = hole[(i + 1) % n_hole];
      trace_face(u, v, h + 1);
    }
  }

  /* 4. Populate output structure. */
  const int face_count = int(face_lens.size());
  r_output.face_offsets_data.reinitialize(face_count + 1);
  r_output.face_offsets_data[0] = 0;
  for (const int i : IndexRange(face_count)) {
    r_output.face_offsets_data[i + 1] = r_output.face_offsets_data[i] + face_lens[i];
  }
  r_output.face_offsets = OffsetIndices<int>(r_output.face_offsets_data.as_span());
  r_output.face_vert_indices = Array<int>(face_indices.as_span());
  r_output.face_orig = Array<int>(face_orig_list.as_span());
  r_output.face_band = Array<int>(face_count, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Output Construction
 * \{ */

static StraightSkeletonOutput build_straight_skeleton_output(const StraightSkeletonState &state,
                                                             const StraightSkeletonInput &input)
{
  PRF_scope(ProfileCategory::Core);
  StraightSkeletonOutput output;
  output.hit_max_events = state.hit_max_events_;
  output.events_processed_num = state.events_processed_num_;
  output.flips_num = state.flips_count_;

  const SkeletonGraph &skel = state.skeleton;
  const int nodes_num = int(skel.nodes.size());

  output.vert.reinitialize(nodes_num);
  output.vert_time.reinitialize(nodes_num);
  output.vert_orig.reinitialize(nodes_num);

  for (const int i : IndexRange(nodes_num)) {
    output.vert[i] = skel.nodes[i].pos;
    output.vert_time[i] = skel.nodes[i].time;
    output.vert_orig[i] = skel.nodes[i].orig_vert;
  }

  /* Compute the effective offset and number of segments. */
  double max_collapse_time = 0.0;
  for (const SkeletonNode &node : skel.nodes) {
    max_collapse_time = std::max(max_collapse_time, node.time);
  }

  const bool is_full_skeleton = std::isinf(input.offset) ||
                                (input.offset >= max_collapse_time - event_time_tol);
  const double effective_offset = is_full_skeleton ? max_collapse_time :
                                                     std::min(input.offset, max_collapse_time);
  const int segments = std::max(1, input.segments);

  /* Fast path: full skeleton, single segment -- use the existing roof-face traversal. */
  if (is_full_skeleton && segments == 1) {
    /* Build output edges. */
    Vector<int2> out_edges;
    Vector<int> out_edge_orig;

    /* First: input boundary edges (at t = 0). */
    const int n_outer = int(input.outer_polygon.size());
    for (int i = 0; i < n_outer; i++) {
      const int u = input.outer_polygon[i];
      const int v = input.outer_polygon[(i + 1) % n_outer];
      out_edges.append(int2(u, v));
      out_edge_orig.append(i);
    }

    /* Hole boundary edges (at t = 0). */
    const int n_holes = input.hole_offsets.size();
    for (const int h : IndexRange(n_holes)) {
      const Span<int> hole = input.hole_vert_indices.slice(input.hole_offsets[h]);
      const int n_hole = int(hole.size());
      for (const int i : IndexRange(n_hole)) {
        const int u = hole[i];
        const int v = hole[(i + 1) % n_hole];
        out_edges.append(int2(u, v));
        out_edge_orig.append(-1);
      }
    }

    /* Skeleton edges (rays and ridges). */
    for (const SkeletonEdge &se : skel.edges) {
      out_edges.append(int2(se.node1, se.node2));
      out_edge_orig.append(se.orig_edge);
    }

    output.edge = Array<int2>(out_edges.as_span());
    output.edge_orig = Array<int>(out_edge_orig.as_span());

    build_output_faces(input, skel, output);
    const int face_num = int(output.face_offsets.size());
    output.face_band.reinitialize(face_num);
    output.face_band.fill(0);
    return output;
  }

  /* General path: offset clipping with one or more bands.
   *
   * Band b spans [t_lo, t_hi] where:
   *   t_lo = effective_offset * b       / segments
   *   t_hi = effective_offset * (b + 1) / segments
   *
   * We first build the full-skeleton output faces (using the skeleton node positions),
   * then for each band clip those faces with Sutherland-Hodgman and assemble the
   * side-strip faces and inner polygon.
   */

  StraightSkeletonOutput full_output;
  full_output.vert = Array<double2>(output.vert.as_span());
  full_output.vert_time = Array<double>(output.vert_time.as_span());
  full_output.vert_orig = Array<int>(output.vert_orig.as_span());
  build_output_faces(input, skel, full_output);

  if (full_output.face_offsets.size() == 0) {
    output.face_band.reinitialize(0);
    return output;
  }

  Vector<double2> out_vert(output.vert.as_span());
  Vector<double> out_vert_time(output.vert_time.as_span());
  Vector<int> out_vert_orig(output.vert_orig.as_span());

  auto get_or_create_skel_vert = [&](const int u) -> int {
    BLI_assert(u >= 0 && u < nodes_num);
    return u;
  };

  Map<std::pair<OrderedEdge, int>, int> cut_vert_map;

  auto get_or_create_cut_vert = [&](const int u, const int v, const int cut_level) -> int {
    const std::pair<OrderedEdge, int> key = {OrderedEdge(u, v), cut_level};
    if (const int *existing = cut_vert_map.lookup_ptr(key)) {
      return *existing;
    }
    const double t_cut = effective_offset * double(cut_level) / double(segments);
    const double tu = out_vert_time[u];
    const double tv = out_vert_time[v];
    const double dt = tv - tu;
    const double alpha = (std::abs(dt) > div_by_zero_tol) ?
                             std::clamp((t_cut - tu) / dt, 0.0, 1.0) :
                             0.5;
    const double2 pos = out_vert[u] + alpha * (out_vert[v] - out_vert[u]);
    const int new_idx = int(out_vert.size());
    out_vert.append(pos);
    out_vert_time.append(t_cut);
    out_vert_orig.append(-1);
    cut_vert_map.add(key, new_idx);
    return new_idx;
  };

  auto get_pos = [&](const int idx) -> double2 { return out_vert[idx]; };
  auto get_vert_t = [&](const int idx) -> double { return out_vert_time[idx]; };

  Vector<Vector<int>> result_faces;
  Vector<int> result_face_orig;
  Vector<int> result_face_band;

  const double eps = event_time_tol;

  for (const int band : IndexRange(segments)) {
    const double t_lo = effective_offset * double(band) / double(segments);
    const double t_hi = effective_offset * double(band + 1) / double(segments);

    Vector<int2> inner_directed_edges;

    for (const int fi : full_output.face_offsets.index_range()) {
      const IndexRange face_r = full_output.face_offsets[fi];
      const int face_verts_num = int(face_r.size());
      if (face_verts_num < 3) {
        continue;
      }

      const Span<int> src_verts = full_output.face_vert_indices.as_span().slice(face_r);
      const int orig_fi = full_output.face_orig[fi];

      /* Pass 1: t >= t_lo */
      Vector<int> pass1;
      auto append_p1 = [&](const int v_idx) {
        if (pass1.is_empty() || pass1.last() != v_idx) {
          pass1.append(v_idx);
        }
      };

      for (const int j : IndexRange(face_verts_num)) {
        const int u = src_verts[j];
        const int v = src_verts[(j + 1) % face_verts_num];
        const double tu = get_vert_t(u);
        const double tv = get_vert_t(v);
        const bool u_in = (tu >= t_lo - eps);
        const bool v_in = (tv >= t_lo - eps);

        if (u_in && v_in) {
          append_p1(get_or_create_skel_vert(v));
        }
        else if (u_in && !v_in) {
          append_p1(get_or_create_cut_vert(u, v, band));
        }
        else if (!u_in && v_in) {
          append_p1(get_or_create_cut_vert(u, v, band));
          append_p1(get_or_create_skel_vert(v));
        }
      }

      while (pass1.size() >= 2 && pass1.first() == pass1.last()) {
        pass1.pop_last();
      }
      if (pass1.size() < 3) {
        continue;
      }

      /* Pass 2: t <= t_hi */
      const int p1_num = int(pass1.size());
      Vector<int> clipped_verts;
      Vector<int> cuts_on_face;

      auto append_p2 = [&](const int v_idx) {
        if (clipped_verts.is_empty() || clipped_verts.last() != v_idx) {
          clipped_verts.append(v_idx);
        }
      };

      for (const int j : IndexRange(p1_num)) {
        const int u = pass1[j];
        const int v = pass1[(j + 1) % p1_num];
        const double tu = get_vert_t(u);
        const double tv = get_vert_t(v);
        const bool u_in = (tu <= t_hi + eps);
        const bool v_in = (tv <= t_hi + eps);

        if (u_in && v_in) {
          append_p2(v);
        }
        else if (u_in && !v_in) {
          const int cut_v = get_or_create_cut_vert(u, v, band + 1);
          append_p2(cut_v);
          cuts_on_face.append(cut_v);
        }
        else if (!u_in && !v_in) {
        }
        else {
          const int cut_v = get_or_create_cut_vert(u, v, band + 1);
          append_p2(cut_v);
          cuts_on_face.append(cut_v);
          append_p2(v);
        }
      }

      while (clipped_verts.size() >= 2 && clipped_verts.first() == clipped_verts.last()) {
        clipped_verts.pop_last();
      }

      if (clipped_verts.size() >= 3) {
        double area = 0.0;
        const int m = int(clipped_verts.size());
        for (const int i : IndexRange(m)) {
          const double2 p1 = get_pos(clipped_verts[i]);
          const double2 p2 = get_pos(clipped_verts[(i + 1) % m]);
          area += (p1.x * p2.y - p2.x * p1.y);
        }
        if (area > min_subface_area) {
          result_faces.append(std::move(clipped_verts));
          result_face_orig.append(orig_fi);
          result_face_band.append(band);
        }
      }

      if (cuts_on_face.size() == 2 && cuts_on_face[0] != cuts_on_face[1]) {
        inner_directed_edges.append(int2(cuts_on_face[1], cuts_on_face[0]));
      }
      else if (cuts_on_face.size() > 2) {
        for (int k = 0; k + 1 < int(cuts_on_face.size()); k += 2) {
          if (cuts_on_face[k] != cuts_on_face[k + 1]) {
            inner_directed_edges.append(int2(cuts_on_face[k + 1], cuts_on_face[k]));
          }
        }
      }
    } /* end per-face loop */

    /* Assemble inner polygon(s) at t_hi for the final band when !is_full_skeleton. */
    if (!inner_directed_edges.is_empty() && band == segments - 1 && !is_full_skeleton) {
      Map<int, int> next_vert_map;
      for (const int2 &e : inner_directed_edges) {
        if (e[0] != e[1]) {
          next_vert_map.add_overwrite(e[0], e[1]);
        }
      }

      while (!next_vert_map.is_empty()) {
        const int start_v = *next_vert_map.keys().begin();
        Vector<int> cycle;
        int curr = start_v;

        while (true) {
          cycle.append(curr);
          const int *nxt_ptr = next_vert_map.lookup_ptr(curr);
          if (nxt_ptr == nullptr) {
            break;
          }
          const int nxt = *nxt_ptr;
          next_vert_map.remove(curr);
          if (nxt == start_v || !next_vert_map.contains(nxt)) {
            break;
          }
          curr = nxt;
        }

        Vector<int> clean_cycle;
        for (const int v : cycle) {
          if (clean_cycle.is_empty() || clean_cycle.last() != v) {
            clean_cycle.append(v);
          }
        }
        while (clean_cycle.size() >= 2 && clean_cycle.first() == clean_cycle.last()) {
          clean_cycle.pop_last();
        }

        if (clean_cycle.size() >= 3) {
          double area = 0.0;
          const int m = int(clean_cycle.size());
          for (const int i : IndexRange(m)) {
            const double2 p1 = get_pos(clean_cycle[i]);
            const double2 p2 = get_pos(clean_cycle[(i + 1) % m]);
            area += (p1.x * p2.y - p2.x * p1.y);
          }
          if (area > min_subface_area) {
            const int inner_fi = full_output.face_orig.is_empty() ? 0 : full_output.face_orig[0];
            result_faces.append(std::move(clean_cycle));
            result_face_orig.append(inner_fi);
            result_face_band.append(band);
          }
        }
      }
    }
  } /* end per-band loop */

  /* Compact vertices:
   * Preserve input boundary vertices at indices 0 .. input_verts_num - 1 so that
   * callers (e.g. mesh_bevel.cc) can rely on correspondence with input vertices.
   * Only include vertices >= input_verts_num that are actively referenced by at least
   * one face in result_faces. This excludes unreached skeleton nodes that lie beyond
   * the offset cutoff. */
  const int input_verts_num = int(input.vert.size());
  Array<int> old_to_new(out_vert.size(), -1);
  Vector<double2> compact_vert;
  Vector<double> compact_vert_time;
  Vector<int> compact_vert_orig;

  compact_vert.reserve(out_vert.size());
  compact_vert_time.reserve(out_vert.size());
  compact_vert_orig.reserve(out_vert.size());

  for (const int i : IndexRange(input_verts_num)) {
    old_to_new[i] = i;
    compact_vert.append(out_vert[i]);
    compact_vert_time.append(out_vert_time[i]);
    compact_vert_orig.append(out_vert_orig[i]);
  }

  for (Vector<int> &f : result_faces) {
    for (int &v : f) {
      if (old_to_new[v] == -1) {
        old_to_new[v] = int(compact_vert.size());
        compact_vert.append(out_vert[v]);
        compact_vert_time.append(out_vert_time[v]);
        compact_vert_orig.append(out_vert_orig[v]);
      }
      v = old_to_new[v];
    }
  }

  const int total_faces = int(result_faces.size());
  Vector<int> face_indices;
  Vector<int> face_lens;
  for (const Vector<int> &f : result_faces) {
    face_lens.append(int(f.size()));
    face_indices.extend(f.as_span());
  }

  output.face_offsets_data.reinitialize(total_faces + 1);
  output.face_offsets_data[0] = 0;
  for (const int i : IndexRange(total_faces)) {
    output.face_offsets_data[i + 1] = output.face_offsets_data[i] + face_lens[i];
  }
  output.face_offsets = OffsetIndices<int>(output.face_offsets_data.as_span());
  output.face_vert_indices = Array<int>(face_indices.as_span());
  output.face_orig = Array<int>(result_face_orig.as_span());
  output.face_band = Array<int>(result_face_band.as_span());

  output.vert = Array<double2>(compact_vert.as_span());
  output.vert_time = Array<double>(compact_vert_time.as_span());
  output.vert_orig = Array<int>(compact_vert_orig.as_span());

  /* Collect all face boundary edges into output.edge. */
  VectorSet<OrderedEdge> all_face_edges;
  for (const Vector<int> &f : result_faces) {
    const int m = int(f.size());
    for (const int i : IndexRange(m)) {
      all_face_edges.add(OrderedEdge(f[i], f[(i + 1) % m]));
    }
  }
  Vector<int2> out_edges;
  Vector<int> out_edge_orig;
  for (const OrderedEdge &oe : all_face_edges) {
    out_edges.append(int2(oe.v_low, oe.v_high));
    out_edge_orig.append(-1);
  }
  output.edge = Array<int2>(out_edges.as_span());
  output.edge_orig = Array<int>(out_edge_orig.as_span());

  output.hit_max_events = state.hit_max_events_;
  output.events_processed_num = state.events_processed_num_;
  output.flips_num = state.flips_count_;

  return output;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug SVG Output
 * \{ */

static void write_debug_svg(const StraightSkeletonInput &input,
                            const StraightSkeletonOutput &output)
{
  if (output.vert.is_empty()) {
    return;
  }

  /* Compute bounding box of output and input vertices. */
  double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
  for (const double2 &v : output.vert) {
    if (std::isfinite(v.x) && std::isfinite(v.y)) {
      xmin = std::min(xmin, v.x);
      xmax = std::max(xmax, v.x);
      ymin = std::min(ymin, v.y);
      ymax = std::max(ymax, v.y);
    }
  }
  for (const double2 &v : input.vert) {
    if (std::isfinite(v.x) && std::isfinite(v.y)) {
      xmin = std::min(xmin, v.x);
      xmax = std::max(xmax, v.x);
      ymin = std::min(ymin, v.y);
      ymax = std::max(ymax, v.y);
    }
  }

  if (xmin > xmax) {
    return;
  }

  double dx = xmax - xmin;
  double dy = ymax - ymin;
  if (dx < 1e-6) {
    dx = 1.0;
  }
  if (dy < 1e-6) {
    dy = 1.0;
  }
  const double pad = std::max(dx, dy) * 0.05;
  xmin -= pad;
  xmax += pad;
  ymin -= pad;
  ymax += pad;
  dx = xmax - xmin;
  dy = ymax - ymin;

  const double base_dim = 600.0;
  const double scale = std::min(base_dim / dx, base_dim / dy);
  const double svg_w = dx * scale + 40.0;
  const double svg_h = dy * scale + 40.0;
  const double margin = 20.0;

  auto tx = [&](const double x) { return margin + (x - xmin) * scale; };
  auto ty = [&](const double y) { return svg_h - margin - (y - ymin) * scale; };

  std::ostringstream svg;
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << svg_w << " " << svg_h
      << "\" width=\"" << svg_w << "\" height=\"" << svg_h
      << "\" style=\"background-color:#ffffff; overflow:hidden;\">\n";

  /* Color palette for faces. */
  static const char *face_colors[] = {
      "#a8dadc", "#f4a261", "#e76f51", "#2a9d8f", "#e9c46a", "#b8bedd", "#f0e6ef",
      "#d0f4de", "#ffcad4", "#b5e2fa", "#edafb8", "#f3c68f", "#c9cba3", "#e2ece9",
      "#dfccf1", "#83c5be", "#ffddd2", "#cddafd", "#dfe7fd", "#e8dff5",
  };
  const int num_colors = int(sizeof(face_colors) / sizeof(face_colors[0]));

  /* Draw filled faces without labels. */
  const Span<int> fvi = output.face_vert_indices;
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange r = output.face_offsets[fi];
    if (r.size() < 3) {
      continue;
    }
    const char *col = face_colors[fi % num_colors];
    svg << "  <polygon points=\"";
    for (const int idx : r) {
      const double2 p = output.vert[fvi[idx]];
      svg << tx(p.x) << "," << ty(p.y) << " ";
    }
    svg << "\" fill=\"" << col
        << "\" stroke=\"#ffffff\" stroke-width=\"0.75\" opacity=\"0.85\"/>\n";
  }

  /* Draw internal straight skeleton edges in red/crimson. */
  for (const int ei : output.edge.index_range()) {
    if (output.edge_orig.size() > ei && output.edge_orig[ei] == -1) {
      const int2 &e = output.edge[ei];
      if (e[0] >= 0 && e[1] >= 0 && e[0] < output.vert.size() && e[1] < output.vert.size()) {
        const double2 p1 = output.vert[e[0]];
        const double2 p2 = output.vert[e[1]];
        svg << "  <line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
            << "\" y2=\"" << ty(p2.y)
            << "\" stroke=\"#e63946\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>\n";
      }
    }
  }

  /* Draw boundary edges in dark navy blue. */
  for (const int ei : output.edge.index_range()) {
    if (output.edge_orig.size() > ei && output.edge_orig[ei] != -1) {
      const int2 &e = output.edge[ei];
      if (e[0] >= 0 && e[1] >= 0 && e[0] < output.vert.size() && e[1] < output.vert.size()) {
        const double2 p1 = output.vert[e[0]];
        const double2 p2 = output.vert[e[1]];
        svg << "  <line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
            << "\" y2=\"" << ty(p2.y)
            << "\" stroke=\"#1d3557\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>\n";
      }
    }
  }

  svg << "</svg>\n";

  /* Determine card title. */
  std::ostringstream title_ss;
  if (input.label != nullptr) {
    title_ss << input.label;
  }
  else {
    title_ss << "Straight Skeleton";
  }
  title_ss << " (" << input.vert.size() << " verts, " << output.face_offsets.size() << " faces)";
  const std::string title = title_ss.str();

  /* Avoid duplicate writes when the same problem is run repeatedly in benchmarks. */
  static std::string last_written_title;
  if (!debug_new_file && title == last_written_title) {
    return;
  }
  last_written_title = title;

  /* Open HTML file (truncate on first write, append afterwards). */
  const char *html_path = debug_html_filepath ? debug_html_filepath :
                                                "/tmp/straight_skeleton.html";
  const std::ios::openmode mode = debug_new_file ? (std::ios::out | std::ios::trunc) :
                                                   (std::ios::out | std::ios::app);
  std::ofstream f(html_path, mode);
  if (!f.is_open()) {
    return;
  }

  if (debug_new_file) {
    f << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
      << "<meta charset=\"utf-8\">\n"
      << "<title>Straight Skeleton Debug Drawings</title>\n"
      << "<style>\n"
      << "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
         " background: #0f172a; color: #e2e8f0; margin: 0; padding: 24px; }\n"
      << "  h1 { font-size: 24px; color: #38bdf8; margin-bottom: 24px; border-bottom: 1px solid"
         " #334155; padding-bottom: 12px; }\n"
      << "  .card { display: inline-block; background: #1e293b; border: 1px solid #334155;"
         " border-radius: 8px; padding: 14px; margin: 10px; vertical-align: top;"
         " box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3); }\n"
      << "  .card-title { font-size: 13px; font-weight: 600; color: #94a3b8; margin-bottom: 10px;"
         " font-family: monospace; }\n"
      << "  .card svg { display: block; background: #ffffff; border-radius: 4px; }\n"
      << "</style>\n"
      << "</head>\n<body>\n"
      << "<h1>Straight Skeleton Debug Drawings</h1>\n";
    debug_new_file = false;
  }

  f << "<div class=\"card\">\n"
    << "  <div class=\"card-title\">" << title << "</div>\n"
    << svg.str() << "</div>\n";
  f.close();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input)
{
  PRF_scope(ProfileCategory::Core);
  StraightSkeletonState state(input);
  state.build_initial_kinetic_triangulation();
  const int max_events = std::max(1000, int(10 * input.vert.size()));
  state.run_event_loop(max_events);
  StraightSkeletonOutput output = build_straight_skeleton_output(state, input);

#if defined(DEBUG_STRAIGHT_SKELETON_SVG) || defined(DEBUG_SVG)
  write_debug_svg(input, output);
#else
  if (std::getenv("STRAIGHT_SKELETON_SVG") != nullptr) {
    write_debug_svg(input, output);
  }
#endif

  return output;
}

/** \} */

} /* namespace blender::straightskeleton */
