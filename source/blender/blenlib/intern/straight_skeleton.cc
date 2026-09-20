/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation for simple polygons with optional holes.
 *
 * Designed for the bevel node:
 *   - 1 outer polygon (oriented CCW)
 *   - Any number of hole polygons (each oriented CW)
 *   - Wavefront advances inward from the outer polygon and outward from hole boundaries.
 *   - Operates directly on the interior Constrained Delaunay Triangulation (CDT_INSIDE).
 *   - No ghost / unbounded triangles.
 *   - KineticVertices (SOA) and TriangleEdges (half-edge adjacency).
 */

#include "BLI_straight_skeleton.hh"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>

#include "BLI_array.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_heap.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name Debug Output Controls
 * \{ */

static bool debug_new_file = true;
[[maybe_unused]] static const char *debug_html_filepath = nullptr;

[[maybe_unused]] static void debug_reset_file()
{
  debug_new_file = true;
}

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
  int triangle_index{-1};
  double time{std::numeric_limits<double>::infinity()};
  TriangleEventType type{TriangleEventType::NONE};
  int edge_index{-1};          /* Half-edge index involved in the event */
  int vert_index{-1};          /* Vertex index involved in the event */
  double2 event_pos{0.0, 0.0}; /* Coordinates where event occurs */
  HeapNode *heap_node{nullptr};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper geometry functions
 * \{ */

static double cross_2d(const double2 &a, const double2 &b)
{
  return a.x * b.y - a.y * b.x;
}

static double dot_2d(const double2 &a, const double2 &b)
{
  return a.x * b.x + a.y * b.y;
}

static double dist_2d(const double2 &a, const double2 &b)
{
  return std::hypot(a.x - b.x, a.y - b.y);
}

static double2 normalize_or_zero(const double2 &v)
{
  const double len = std::hypot(v.x, v.y);
  if (len > 1e-14) {
    return v / len;
  }
  return double2(0.0, 0.0);
}

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
    start_.append(v0);
    start_.append(v1);
    start_.append(v2);
    paired_.append(-1);
    paired_.append(-1);
    paired_.append(-1);
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
    return paired_[edge_index] == -1;
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

  int rotate_ccw(const int edge_index) const
  {
    return paired_[next_triangle_edge(edge_index)];
  }

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
};

/**
 * Flip the internal diagonal shared by two adjacent triangles in TriangleEdges.
 * Returns true if the flip succeeded, false if edge cannot be flipped.
 */
static bool flip_edge(TriangleEdges &tri, const int edge_index)
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

  /* Half-edges of triangle 1 in CCW cycle:
   *   h1_0 (e1): v1 -> v2
   *   h1_1:      v2 -> v
   *   h1_2:      v  -> v1
   * Vertex v is opposite to e1 in ti1. */
  const int h1_0 = e1;
  const int h1_1 = next_triangle_edge(h1_0);
  const int h1_2 = prev_triangle_edge(h1_0);

  const int v1 = tri.start_vert(h1_0);
  const int v2 = tri.start_vert(h1_1);
  const int v = tri.start_vert(h1_2);

  const int p1_1 = tri.paired_edge(h1_1);
  const int p1_2 = tri.paired_edge(h1_2);

  /* Half-edges of triangle 2 in CCW cycle:
   *   h2_0 (e2): v2 -> v1
   *   h2_1:      v1 -> v3
   *   h2_2:      v3 -> v2
   * Vertex v3 is opposite to e2 in ti2. */
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
   *   edge 0 (3 * ti1 + 0): v  -> v1, paired with p1_2
   *   edge 1 (3 * ti1 + 1): v1 -> v3, paired with p2_1
   *   edge 2 (3 * ti1 + 2): v3 -> v,  paired with (3 * ti2 + 2) [new diagonal] */
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

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Kinetic Vertices (Structure of Arrays)
 * \{ */

struct KineticVertices {
  Vector<double2> start_pos;
  Vector<double2> velocity;
  Vector<double2> ccw_dir;
  Vector<double2> cw_dir;
  Vector<double> start_time;
  Vector<double> end_time;
  Vector<int> start_node;
  Vector<int> end_node;
  Vector<int> orig_node;
  Vector<int> input_edge;
  Vector<bool> is_active;
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
  int id{-1};                 /* Node index in SkeletonGraph */
  double2 pos{0.0, 0.0};      /* 2D position (x, y) */
  double time{0.0};           /* Event time / height t (0 for input verts) */
  int orig_vert{-1};          /* Index of coincident input vertex, or -1 for interior nodes */
  Vector<int> incident_edges; /* Incident SkeletonEdge indices */
};

struct SkeletonEdge {
  int id{-1};                    /* Unique edge index in SkeletonGraph */
  int node1{-1};                 /* First endpoint (lower time or start) */
  int node2{-1};                 /* Second endpoint (higher time or end) */
  int orig_edge{-1};             /* Coincident input edge index, or -1 for skeleton arcs */
  int input_face{-1};            /* Incident input face index, or -1 */
  int side{0};                   /* Side 0 or Side 1 */
  double time_start{0.0};        /* Time at node1 */
  double time_end{0.0};          /* Time at node2 */
  bool is_input_boundary{false}; /* True for input edges / face boundaries at t=0 */
};

class SkeletonGraph {
 public:
  Vector<SkeletonNode> nodes;
  Vector<SkeletonEdge> edges;

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
      if (std::abs(existing.time - time) < 1e-6 &&
          std::hypot(existing.pos.x - pos.x, existing.pos.y - pos.y) < 1e-6)
      {
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
/** \name Priority Queue
 * \{ */

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
        if (std::abs(events_[p_ti].time - min_ev.time) <= 1e-6) {
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
          std::abs(events_[ti].time - min_ev.time) <= 1e-6)
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
/** \name Bisector Velocity Calculation
 * \{ */

static std::pair<double2, bool> compute_bisector_vel(const double2 &n1, const double2 &n2)
{
  const double det = n1.x * n2.y - n1.y * n2.x;
  const double dot = n1.x * n2.x + n1.y * n2.y;
  const bool is_inf = (std::abs(det) < 1e-9 && dot < -0.999999);
  double2 vel(0.0, 0.0);
  if (!is_inf) {
    if (std::abs(det) > 1e-12) {
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
  return (n_in.x * n_out.y - n_in.y * n_out.x < -1e-6);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Event Detection
 * \{ */

static TriangleEvent compute_triangle_collapse(const int ti,
                                               const TriangleEdges &triangles,
                                               const KineticVertices &kinetic_verts,
                                               const double current_time)
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
    if (cand.time < best_event.time - 1e-9) {
      best_event = cand;
    }
    else if (std::abs(cand.time - best_event.time) <= 1e-9) {
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
    const double dv_sq = dot_2d(dv, dv);
    if (dv_sq > 1e-14) {
      const double dt = -dot_2d(dp, dv) / dv_sq;
      const bool is_valid_time = (current_time > 0.0 && dt > -1e-9) || (dt > 1e-9);
      if (is_valid_time) {
        const double eff_dt = std::max(0.0, dt);
        if (triangles.is_boundary(ei)) {
          /* A wavefront edge is guaranteed by straight skeleton theory to collapse
           * at its time of closest approach. */
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
          /* Internal triangulation edge: collapses only if distance reaches zero. */
          const double2 p_at_t = dp + eff_dt * dv;
          if (std::hypot(p_at_t.x, p_at_t.y) < 1e-5) {
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

  /* 2. Vertices moving into edges (Palfreder & Held, Section 2.3).
   * For triangles with exactly one wavefront edge e, the opposite vertex v_opp
   * moves towards e. The distance is linear in time, robustly avoiding determinant roots. */
  if (num_boundary == 1) {
    const int k = boundary_k;
    const int ei = e0 + k;
    const int k_a = k;
    const int k_b = (k + 1) % 3;
    const int k_opp = (k + 2) % 3;

    const double2 edge_vec = p[k_b] - p[k_a];
    const double edge_len = std::hypot(edge_vec.x, edge_vec.y);
    if (edge_len > 1e-14) {
      const double2 d_e = edge_vec / edge_len;
      /* Inward unit normal (interior is to the left of CCW boundary edge). */
      const double2 n_e(-d_e.y, d_e.x);

      /* Orthogonal distance from v_opp to supporting line of edge e. */
      const double h = dot_2d(p[k_opp] - p[k_a], n_e);

      /* Project velocity of v_opp onto inward normal of e. */
      const double v_proj = dot_2d(v[k_opp], n_e);

      /* Edge e moves inward at unit speed 1.0 along n_e; v_opp moves at v_proj.
       * Speed of approach is 1.0 - v_proj. */
      const double v_approach = 1.0 - v_proj;

      if (v_approach > 1e-12) {
        const double dt = h / v_approach;
        const bool is_valid_time = (current_time > 0.0 && dt > -1e-9) || (dt > 1e-9);
        if (is_valid_time) {
          const double eff_dt = std::max(0.0, dt);
          const double2 pt_a = p[k_a] + eff_dt * v[k_a];
          const double2 pt_b = p[k_b] + eff_dt * v[k_b];
          const double2 pt_opp = p[k_opp] + eff_dt * v[k_opp];

          const double2 edge_dir_t = pt_b - pt_a;
          const double edge_len_t = std::hypot(edge_dir_t.x, edge_dir_t.y);
          const double2 to_opp_t = pt_opp - pt_a;
          const double d_along = (edge_len_t > 1e-14) ?
                                     dot_2d(to_opp_t, edge_dir_t) / (edge_len_t * edge_len_t) :
                                     0.0;

          TriangleEvent cand;
          cand.triangle_index = ti;
          cand.time = current_time + eff_dt;
          cand.edge_index = ei;
          cand.vert_index = tri_v[k_opp];
          cand.event_pos = pt_a + std::clamp(d_along, 0.0, 1.0) * edge_dir_t;

          if (edge_len_t < 1e-5) {
            cand.type = TriangleEventType::EDGE_COLLAPSE;
          }
          else if (d_along < -1e-4) {
            /* Crosses supporting line outside segment near endpoint a: flip internal edge. */
            cand.type = TriangleEventType::FLIP;
            cand.edge_index = e0 + ((k + 2) % 3);
          }
          else if (d_along > 1.0 + 1e-4) {
            /* Crosses supporting line outside segment near endpoint b: flip internal edge. */
            cand.type = TriangleEventType::FLIP;
            cand.edge_index = e0 + ((k + 1) % 3);
          }
          else if (d_along <= 1e-4) {
            const int e_opp_a = e0 + ((k + 2) % 3);
            cand.type = triangles.is_boundary(e_opp_a) ? TriangleEventType::EDGE_COLLAPSE :
                                                         TriangleEventType::MULTI_SPLIT;
            cand.edge_index = e_opp_a;
          }
          else if (d_along >= 1.0 - 1e-4) {
            const int e_b_opp = e0 + ((k + 1) % 3);
            cand.type = triangles.is_boundary(e_b_opp) ? TriangleEventType::EDGE_COLLAPSE :
                                                         TriangleEventType::MULTI_SPLIT;
            cand.edge_index = e_b_opp;
          }
          else if (is_vertex_reflex(kinetic_verts, tri_v[k_opp])) {
            cand.type = TriangleEventType::SPLIT;
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
          }

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

    const double c0 = cross_2d(e01_p, e02_p);
    const double c1 = cross_2d(e01_p, e02_v) + cross_2d(e01_v, e02_p);
    const double c2 = cross_2d(e01_v, e02_v);

    double dt_root = -1.0;
    if (std::abs(c2) > 1e-14) {
      const double disc = c1 * c1 - 4.0 * c2 * c0;
      if (disc >= 0.0) {
        const double sqrt_disc = std::sqrt(disc);
        const double r1 = (-c1 - sqrt_disc) / (2.0 * c2);
        const double r2 = (-c1 + sqrt_disc) / (2.0 * c2);
        double best_r = std::numeric_limits<double>::infinity();
        for (const double r : {r1, r2}) {
          const bool is_valid_r = (current_time > 0.0 && r >= -1e-9) || (r > 1e-9);
          if (is_valid_r && r < best_r) {
            best_r = r;
          }
        }
        if (!std::isinf(best_r)) {
          dt_root = best_r;
        }
      }
    }
    else if (std::abs(c1) > 1e-14) {
      const double r = -c0 / c1;
      const bool is_valid_r = (current_time > 0.0 && r >= -1e-9) || (r > 1e-9);
      if (is_valid_r) {
        dt_root = r;
      }
    }

    const bool is_valid_dt = (current_time > 0.0 && dt_root >= -1e-9) || (dt_root > 1e-9);
    const double eff_dt = std::max(0.0, dt_root);
    if (is_valid_dt && (current_time + eff_dt < best_event.time)) {
      const double2 pt[3] = {p[0] + eff_dt * v[0], p[1] + eff_dt * v[1], p[2] + eff_dt * v[2]};
      int best_k = 0;
      double max_len_sq = -1.0;
      for (const int k : IndexRange(3)) {
        const double2 ed = pt[(k + 1) % 3] - pt[k];
        const double lsq = dot_2d(ed, ed);
        if (lsq > max_len_sq) {
          max_len_sq = lsq;
          best_k = k;
        }
      }

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
  double epsilon{1e-8};
  const StraightSkeletonInput *input_{nullptr};
  bool handle_edge_collapses_{true};

  /* Event statistics. */
  int events_processed_num_{0};
  int flips_count_{0};
  int edge_collapses_count_{0};
  int multi_splits_count_{0};
  int splits_count_{0};
  bool hit_max_events_{false};

  explicit StraightSkeletonState(const StraightSkeletonInput &input)
      : epsilon(input.epsilon <= 0.0 ? 1e-8 : input.epsilon), input_(&input)
  {
  }

  void set_handle_edge_collapses(const bool val)
  {
    handle_edge_collapses_ = val;
  }

  void build_initial_kinetic_triangulation();
  bool process_next_event();
  bool process_edge_collapse_event(const TriangleEvent &ev);
  bool process_multi_split_event(const TriangleEvent &ev);
  bool process_split_event(const TriangleEvent &ev);
  bool process_flip_event(const TriangleEvent &ev);
  void handle_infinite_speed_vertex(int v_prime, int node_id, double event_time);
  void run_event_loop(int max_events);
};

void StraightSkeletonState::build_initial_kinetic_triangulation()
{
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
    const double2 d_in = normalize_or_zero(pos_v - pos_prev);
    const double2 n1(-d_in.y, d_in.x);

    /* Outgoing edge pos_v -> pos_next: inward normal is to the left (-dy, dx). */
    const double2 d_out = normalize_or_zero(pos_next - pos_v);
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
    const TriangleEvent ev = compute_triangle_collapse(ti, triangles, kinetic_verts, 0.0);
    if (ev.type != TriangleEventType::NONE && !std::isinf(ev.time)) {
      pq.insert_or_update(ti, ev);
    }
  }
}

void StraightSkeletonState::handle_infinite_speed_vertex(const int initial_v_prime,
                                                         const int initial_node_id,
                                                         const double event_time)
{
  int curr_v_prime = initial_v_prime;
  int curr_node_id = initial_node_id;

  for (int iter = 0; iter < 100; iter++) {
    if (curr_v_prime < 0 || curr_v_prime >= kinetic_verts.size() ||
        !kinetic_verts.is_active[curr_v_prime] || !kinetic_verts.is_infinitely_fast[curr_v_prime])
    {
      break;
    }

    int e_out_prime = -1;
    int e_in_prime = -1;
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
    if (e_out_prime == -1 || e_in_prime == -1) {
      break;
    }

    const int v_next = triangles.end_vert(e_out_prime);
    const int v_prev = triangles.start_vert(e_in_prime);
    const double2 p_next = kinetic_verts.position_at(v_next, event_time);
    const double2 p_prev = kinetic_verts.position_at(v_prev, event_time);
    const double2 p_prime = skeleton.nodes[curr_node_id].pos;

    const bool is_closed_loop = (v_prev == v_next) || [&]() {
      for (const int ei : IndexRange(triangles.edges_num())) {
        if (triangles.is_boundary(ei)) {
          if (triangles.start_vert(ei) == v_next && triangles.end_vert(ei) == v_prev) {
            return true;
          }
        }
      }
      return false;
    }();

    if (dist_2d(p_prev, p_next) < 1e-2 && is_closed_loop) {
      /* Both ends collide at the midpoint between p_prev and p_next at event_time. */
      const double2 p_final = 0.5 * (p_prev + p_next);
      int final_node = curr_node_id;
      if (dist_2d(p_prime, p_final) >= 1e-5) {
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

    /* Ridge propagation along parallel opposing boundary edges to closer endpoint. */
    const double len_prev = dist_2d(p_prev, p_prime);
    const double len_next = dist_2d(p_next, p_prime);
    const bool choose_next = (len_next <= len_prev);
    const int target_v = choose_next ? v_next : v_prev;
    const double2 target_pos = choose_next ? p_next : p_prev;
    const int target_e = choose_next ? e_out_prime : e_in_prime;

    int next_node = curr_node_id;
    if (dist_2d(p_prime, target_pos) > 1e-5) {
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

    /* In all surviving triangles containing curr_v_prime or target_v, replace with v_new. */
    VectorSet<int> modified_triangles;
    if (pair_next != -1) {
      modified_triangles.add(pair_next / 3);
    }
    if (pair_prev != -1) {
      modified_triangles.add(pair_prev / 3);
    }
    for (const int ti : IndexRange(triangles.triangles_num())) {
      if (ti == ti_deg || triangles.is_triangle_deleted(ti)) {
        continue;
      }
      bool has_target_or_prime = false;
      for (const int k : IndexRange(3)) {
        const int vert = triangles.start_vert(3 * ti + k);
        if (vert == curr_v_prime || vert == target_v) {
          has_target_or_prime = true;
          triangles.set_start_vert(3 * ti + k, v_new);
        }
      }
      if (has_target_or_prime) {
        modified_triangles.add(ti);
      }
    }

    for (const int mod_ti : modified_triangles) {
      if (mod_ti == ti_deg || triangles.is_triangle_deleted(mod_ti)) {
        continue;
      }
      const TriangleEvent new_ev = compute_triangle_collapse(
          mod_ti, triangles, kinetic_verts, event_time);
      if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
        pq.insert_or_update(mod_ti, new_ev);
      }
      else {
        pq.remove(mod_ti);
      }
    }

    curr_v_prime = v_new;
    curr_node_id = next_node;
  }
}

bool StraightSkeletonState::process_edge_collapse_event(const TriangleEvent &ev)
{
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

  const auto [vel_prime, is_inf_prime] = compute_bisector_vel(norm_in, norm_out);

  const int v_prime = kinetic_verts.append(
      ev.event_pos, vel_prime, n_in, n_out, ev.time, node_id, -1, -1, is_inf_prime);

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

  /* Replace v_start and v_end with v_prime in all surviving triangles. */
  VectorSet<int> modified_triangles;
  for (const int other_ti : IndexRange(triangles.triangles_num())) {
    if (other_ti == ti || triangles.is_triangle_deleted(other_ti)) {
      continue;
    }
    bool modified = false;
    for (const int k_edge : IndexRange(3)) {
      const int ei = 3 * other_ti + k_edge;
      if (triangles.start_vert(ei) == v_start || triangles.start_vert(ei) == v_end) {
        triangles.set_start_vert(ei, v_prime);
        modified = true;
      }
    }
    if (modified) {
      modified_triangles.add(other_ti);
    }
  }

  if (pe_before != -1) {
    modified_triangles.add(pe_before / 3);
  }
  if (pe_after != -1) {
    modified_triangles.add(pe_after / 3);
  }

  /* Recompute collapse events for all modified surviving triangles. */
  for (const int mod_ti : modified_triangles) {
    if (mod_ti == ti || triangles.is_triangle_deleted(mod_ti)) {
      continue;
    }
    const TriangleEvent new_ev = compute_triangle_collapse(
        mod_ti, triangles, kinetic_verts, current_time_);
    if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
      pq.insert_or_update(mod_ti, new_ev);
    }
    else {
      pq.remove(mod_ti);
    }
  }

  if (is_inf_prime) {
    handle_infinite_speed_vertex(v_prime, node_id, ev.time);
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
  if (dot_2d(d_uv, d_uv) < 1e-12) {
    d_uv = kinetic_verts.start_pos[v] - kinetic_verts.start_pos[u];
  }

  /* Cross product with d_uv to determine which boundary edges lie on side 1 (ti1, left of d_uv)
   * versus side 2 (ti2, right of d_uv). */
  const double c_in_u = cross_2d(d_uv, ccw_u);
  const double c_out_u = cross_2d(d_uv, cw_u);

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

  /* Stitch holes in triangulation left by deleting ti1 and ti2. */
  const int e1_next = next_triangle_edge(e1);
  const int e1_prev = prev_triangle_edge(e1);
  const int p1_next = triangles.paired_edge(e1_next);
  const int p1_prev = triangles.paired_edge(e1_prev);

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
  triangles.mark_triangle_deleted(ti1);
  pq.remove(ti1);

  const int e2_next = next_triangle_edge(e2);
  const int e2_prev = prev_triangle_edge(e2);
  const int p2_next = triangles.paired_edge(e2_next);
  const int p2_prev = triangles.paired_edge(e2_prev);

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
  triangles.mark_triangle_deleted(ti2);
  pq.remove(ti2);

  /* Replace u and v with v1_prime or v2_prime in all surviving triangles. */
  VectorSet<int> modified_triangles;
  for (const int other_ti : IndexRange(triangles.triangles_num())) {
    if (other_ti == ti1 || other_ti == ti2 || triangles.is_triangle_deleted(other_ti)) {
      continue;
    }
    bool has_u_or_v = false;
    for (const int k : IndexRange(3)) {
      const int vert = triangles.start_vert(3 * other_ti + k);
      if (vert == u || vert == v) {
        has_u_or_v = true;
        break;
      }
    }
    if (!has_u_or_v) {
      continue;
    }

    int ref_vert = -1;
    for (const int k : IndexRange(3)) {
      const int vert = triangles.start_vert(3 * other_ti + k);
      if (vert != u && vert != v) {
        ref_vert = vert;
        break;
      }
    }

    int replacement = v1_prime;
    if (ref_vert != -1) {
      const double2 p_ref = kinetic_verts.position_at(ref_vert, ev.time);
      const double c = cross_2d(d_uv, p_ref - ev.event_pos);
      if (c < -1e-9) {
        replacement = v2_prime;
      }
      else if (std::abs(c) <= 1e-9) {
        const double c_start = cross_2d(
            d_uv, kinetic_verts.start_pos[ref_vert] - kinetic_verts.start_pos[u]);
        if (c_start < 0.0) {
          replacement = v2_prime;
        }
      }
    }

    for (const int k : IndexRange(3)) {
      const int ei = 3 * other_ti + k;
      const int vert = triangles.start_vert(ei);
      if (vert == u || vert == v) {
        triangles.set_start_vert(ei, replacement);
      }
    }
    modified_triangles.add(other_ti);
  }

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

  /* Recompute collapse events for all modified triangles. */
  for (const int mod_ti : modified_triangles) {
    if (mod_ti == ti1 || mod_ti == ti2 || triangles.is_triangle_deleted(mod_ti)) {
      continue;
    }
    const TriangleEvent new_ev = compute_triangle_collapse(
        mod_ti, triangles, kinetic_verts, current_time_);
    if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
      pq.insert_or_update(mod_ti, new_ev);
    }
    else {
      pq.remove(mod_ti);
    }
  }

  if (is_inf_1) {
    handle_infinite_speed_vertex(v1_prime, node_id, ev.time);
  }
  if (is_inf_2) {
    handle_infinite_speed_vertex(v2_prime, node_id, ev.time);
  }

  multi_splits_count_++;
  return true;
}

bool StraightSkeletonState::process_split_event(const TriangleEvent &ev)
{
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
  const double2 d_hit = normalize_or_zero(pos_end - pos_start);
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
      modified_triangles.add(cur_ti);
      const int out_edge = next_triangle_edge(cur_edge);
      BLI_assert(triangles.start_vert(out_edge) == v_opp);
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
      modified_triangles.add(cur_ti);
      BLI_assert(triangles.start_vert(cur_edge) == v_opp);
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
        mod_ti, triangles, kinetic_verts, current_time_);
    if (new_ev.type != TriangleEventType::NONE && !std::isinf(new_ev.time)) {
      pq.insert_or_update(mod_ti, new_ev);
    }
    else {
      pq.remove(mod_ti);
    }
  }

  if (is_inf_a) {
    handle_infinite_speed_vertex(kv_a, node_id, ev.time);
  }
  if (is_inf_b) {
    handle_infinite_speed_vertex(kv_b, node_id, ev.time);
  }

  splits_count_++;
  return true;
}

bool StraightSkeletonState::process_flip_event(const TriangleEvent &ev)
{
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

  current_time_ = ev.time;

  if (!flip_edge(triangles, e1)) {
    return false;
  }

  flips_count_++;

  const TriangleEvent ev1 = compute_triangle_collapse(
      ti1, triangles, kinetic_verts, current_time_);
  const TriangleEvent ev2 = compute_triangle_collapse(
      ti2, triangles, kinetic_verts, current_time_);

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
  const int nodes_num = int(skel.nodes.size());
  if (nodes_num == 0) {
    return;
  }

  /* 1. Collect all planar graph edges: input boundary edges + skeleton edges. */
  Array<Vector<int>> adj(nodes_num);

  auto add_adj_edge = [&](const int u, const int v) {
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

    if (cycle.size() >= 4 && cycle.last() == u_start) {
      cycle.pop_last();

      /* Reject face cycles that contain duplicate vertices. */
      VectorSet<int> unique_verts;
      bool has_duplicates = false;
      for (const int vi : cycle) {
        if (!unique_verts.add(vi)) {
          has_duplicates = true;
          break;
        }
      }
      if (has_duplicates) {
        return;
      }

      /* Verify that face has positive (CCW) signed area. */
      double area = 0.0;
      const int m = int(cycle.size());
      for (const int i : IndexRange(m)) {
        const double2 p1 = skel.nodes[cycle[i]].pos;
        const double2 p2 = skel.nodes[cycle[(i + 1) % m]].pos;
        area += (p1.x * p2.y - p2.x * p1.y);
      }
      area *= 0.5;

      if (area > 1e-9) {
        /* Canonical representation: cyclically rotated so smallest vertex index is first. */
        int min_idx = 0;
        for (const int i : IndexRange(1, m - 1)) {
          if (cycle[i] < cycle[min_idx]) {
            min_idx = i;
          }
        }
        Vector<int> canonical(m);
        for (const int i : IndexRange(m)) {
          canonical[i] = cycle[(min_idx + i) % m];
        }

        if (seen_faces.add(canonical)) {
          face_indices.extend(cycle.as_span());
          face_lens.append(m);
          face_orig_list.append(orig_face_idx);
        }
      }
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
    if (node.time > max_collapse_time) {
      max_collapse_time = node.time;
    }
  }

  const bool is_full_skeleton = std::isinf(input.offset) ||
                                (input.offset >= max_collapse_time - 1e-9);
  const double effective_offset = is_full_skeleton ? max_collapse_time :
                                                     std::min(input.offset, max_collapse_time);
  const int segments = std::max(1, input.segments);

  /* Fast path: full skeleton, single segment — use the existing roof-face traversal. */
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

  const int orig_verts_num = nodes_num;
  Vector<double2> out_vert(output.vert.as_span());
  Vector<double> out_vert_time(output.vert_time.as_span());
  Vector<int> out_vert_orig(output.vert_orig.as_span());

  auto get_or_create_skel_vert = [&](const int u) -> int {
    BLI_assert(u >= 0 && u < orig_verts_num);
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
    const double alpha = (std::abs(dt) > 1e-14) ? std::clamp((t_cut - tu) / dt, 0.0, 1.0) : 0.5;
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

  const double eps = 1e-9;

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
        if (area > 1e-12) {
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
          if (area > 1e-12) {
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
/** \name Public API Implementation
 * \{ */

StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input)
{
  StraightSkeletonState state(input);
  state.build_initial_kinetic_triangulation();
  state.run_event_loop(1000);
  return build_straight_skeleton_output(state, input);
}

/** \} */

} /* namespace blender::straightskeleton */
