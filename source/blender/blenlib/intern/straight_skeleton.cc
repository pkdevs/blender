/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Straight Skeleton implementation.
 */

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include "BLI_array.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_heap.hh"
#include "BLI_map.hh"
#include "BLI_math_boolean.hh"
#include "BLI_set.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

namespace blender::straightskeleton {

bool debug_new_file = true;
const char *debug_html_filepath = "/tmp/skel_tri_debug.html";

void debug_reset_file()
{
  debug_new_file = true;
}

/** Return index of next edge after \a edge_index in #TriangleEdges. */
static inline int next_triangle_edge(int edge_index)
{
  edge_index += edge_index % 3 == 2 ? -2 : 1;
  return edge_index;
}

/** Return index of previous edge before \a edge_index in #TriangleEdges. */
[[maybe_unused]]
static inline int prev_triangle_edge(int edge_index)
{
  edge_index += edge_index % 3 == 0 ? 2 : 1;
  return edge_index;
}

/**
 * Represents a set of triangles by means of half-edges.
 * Each edge of a triangle has zero or one paired edge,
 * which is an edge in another triangle going in the opposite direction.
 * Each edge has an index, and given that index we can find
 * the start and end vertices, the paired edge, and the triangle it belongs to.
 * Vertices are represented by int indices; another structure will hold the coordinates.
 *
 * The implementation uses a concatenation of the three edges of each triangle,
 * one after another. We only need store the start vertex index for each edge, since
 * the end vertex of edge i is the start vertex of edge i+1 (modulo 3 within the triangle).
 *
 * If the start vertex of a triangle is -1, then the triangle is deleted.
 * The third vertex of an unbounded triangle is -1, representing the "ghost" vertex at
 * infinity (used in the straight skeleton algorithm).
 *
 * Convention for unbounded triangles:
 *   For interior hull half-edge ei going u->v, the unbounded triangle is stored as
 *   vertices (v, u, -1), giving three half-edges:
 *     base:  v->u  (index 3*ti+0) -- paired with ei.
 *     right: u->-1 (index 3*ti+1) -- boundary into infinity, no pair.
 *     left:  -1->v (index 3*ti+2) -- boundary into infinity, no pair.
 */
class TriangleEdges {
 public:
  TriangleEdges() = default;

  void reserve_triangles(int triangles_num)
  {
    start_.reserve(3 * triangles_num);
    paired_.reserve(3 * triangles_num);
  }

  void add_triangle(int v0, int v1, int v2)
  {
    start_.append(v0);
    start_.append(v1);
    start_.append(v2);
    paired_.append(-1);
    paired_.append(-1);
    paired_.append(-1);
  }

  void set_paired(int edge_index, int paired_index)
  {
    paired_[edge_index] = paired_index;
  }

  void set_start_vert(int edge_index, int vert)
  {
    start_[edge_index] = vert;
  }

  void set_triangle_verts(int ti, int v0, int v1, int v2)
  {
    start_[3 * ti + 0] = v0;
    start_[3 * ti + 1] = v1;
    start_[3 * ti + 2] = v2;
  }

  int start_vert(int edge_index) const
  {
    return start_[edge_index];
  }

  int end_vert(int edge_index) const
  {
    return start_[next_triangle_edge(edge_index)];
  }

  int paired_edge(int edge_index) const
  {
    return paired_[edge_index];
  }

  int triangle(int edge_index) const
  {
    return edge_index / 3;
  }

  int triangles_num() const
  {
    return int(start_.size()) / 3;
  }

  int edges_num() const
  {
    return int(start_.size());
  }

  void mark_triangle_deleted(int triangle_index)
  {
    start_[3 * triangle_index] = -1;
  }

  bool is_triangle_deleted(int triangle_index) const
  {
    return start_[3 * triangle_index] == -1;
  }

  /** Returns true if triangle \a ti is an unbounded (ghost) triangle. */
  bool is_triangle_unbounded(int triangle_index) const
  {
    /* Third vertex is the ghost vertex at infinity. */
    return start_[3 * triangle_index + 2] == -1;
  }

 private:
  Vector<int> start_;
  Vector<int> paired_;
};

/* -------------------------------------------------------------------- */
/** \name Contour edges and sequences
 * \{ */

/**
 * A directed edge on the wavefront contour.
 */
struct ContourEdge {
  /** Directed start vertex in [0, 2N - 1]. */
  int start{-1};
  /** Directed end vertex in [0, 2N - 1]. */
  int end{-1};
  /** Corresponding input edge index in input.edge, or -1 if none/cap. */
  int input_edge{-1};
  /** Corresponding input face index in input.face_offsets, or -1 if from input.edge or cap. */
  int input_face{-1};
  /** 0 for Side 0 (forward / inner), 1 for Side 1 (backward / outer), -1 for cap. */
  int side{-1};
  /** Next contour edge in the wavefront loop. */
  int next{-1};
  /** Previous contour edge in the wavefront loop. */
  int prev{-1};
};

struct ContourSequences {
  Vector<ContourEdge> edges;
  /** Map from directed vertex pair (start, end) to contour edge index. */
  Map<int2, int> edge_by_verts;
  /** Loops of contour edge indices. */
  Vector<Vector<int>> loops;
};

/** Print a description of \a contours to \a os. */
static void dump_contours(const ContourSequences &contours, std::ostream &os = std::cout)
{
  os << "ContourSequences: " << contours.edges.size() << " edges, " << contours.loops.size()
     << " loops\n";
  for (const int li : contours.loops.index_range()) {
    const Vector<int> &loop = contours.loops[li];
    os << "  loop[" << li << "]: ";
    for (const int cei : loop) {
      const ContourEdge &ce = contours.edges[cei];
      os << ce.start << "->" << ce.end;
      if (ce.side == -1) {
        os << "(cap) ";
      }
      else {
        os << "(s" << ce.side << ") ";
      }
    }
    os << "\n";
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug output
 * \{ */

/** Print a description of \a tri to \a os. */
static void dump_triangles(const TriangleEdges &tri, std::ostream &os = std::cout)
{
  const int triangles_num = tri.triangles_num();
  os << "TriangleEdges: " << triangles_num << " triangles, " << tri.edges_num() << " half-edges\n";
  for (const int ti : IndexRange(triangles_num)) {
    const int e0 = 3 * ti;
    const int e1 = e0 + 1;
    const int e2 = e0 + 2;
    const int v0 = tri.start_vert(e0);
    const int v1 = tri.start_vert(e1);
    const int v2 = tri.start_vert(e2);
    const bool is_unbounded = tri.is_triangle_unbounded(ti);
    const bool is_deleted = (v0 == -1);
    os << "  tri[" << ti << "]"
       << (is_deleted   ? " [DELETED]" :
           is_unbounded ? " [UNBOUNDED]" :
                          "")
       << ": verts=(" << v0 << "," << v1 << "," << v2 << ")"
       << "  edges=(" << e0 << "," << e1 << "," << e2 << ")"
       << "  paired=(" << tri.paired_edge(e0) << "," << tri.paired_edge(e1) << ","
       << tri.paired_edge(e2) << ")\n";
  }
}

int SkeletonGraph::add_node(const double2 &pos, double time, int orig_vert)
{
  const int id = int(nodes.size());
  SkeletonNode node;
  node.id = id;
  node.pos = pos;
  node.time = time;
  node.orig_vert = orig_vert;
  nodes.append(node);
  return id;
}

int SkeletonGraph::add_edge(int node1,
                            int node2,
                            int orig_edge,
                            int input_face,
                            int side,
                            double time_start,
                            double time_end,
                            bool is_input_boundary)
{
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

static void dump_skeleton_graph(const SkeletonGraph &graph, std::ostream &os = std::cout)
{
  os << "Skeleton Graph (" << graph.nodes.size() << " nodes, " << graph.edges.size()
     << " edges):\n";
  os << "  Nodes:\n";
  for (const SkeletonNode &node : graph.nodes) {
    os << "    n" << std::setw(2) << node.id << " | pos: (" << std::setw(8) << std::fixed
       << std::setprecision(4) << node.pos.x << ", " << std::setw(8) << std::fixed
       << std::setprecision(4) << node.pos.y << ")"
       << " | time: " << std::setw(8) << std::fixed << std::setprecision(4) << node.time
       << " | orig_vert: " << node.orig_vert << "\n";
  }
  os << "  Edges:\n";
  for (const SkeletonEdge &edge : graph.edges) {
    os << "    e" << std::setw(2) << edge.id << " | n" << edge.node1 << " -> n" << edge.node2
       << (edge.is_input_boundary ? " [INPUT]" : " [ARC]") << " | times: [" << std::fixed
       << std::setprecision(4) << edge.time_start << ", " << edge.time_end << "]"
       << " | orig_edge: " << edge.orig_edge << " | face: " << edge.input_face << " (s"
       << edge.side << ")\n";
  }
}

const char *triangle_event_type_name(TriangleEventType type)
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
    case TriangleEventType::UNBOUNDED_COLLAPSE:
      return "UNBOUNDED_COLLAPSE";
  }
  return "UNKNOWN";
}

/**
 * Priority queue of triangle collapse events ordered by collapse time.
 * Wraps Blender's BLI_heap (which uses float keys) while storing high-precision
 * double times inside TriangleEvent.
 */
class TrianglePriorityQueue {
  Heap *heap_{nullptr};
  Vector<TriangleEvent> events_;

 public:
  TrianglePriorityQueue() = default;

  explicit TrianglePriorityQueue(int max_triangles_num)
  {
    heap_ = BLI_heap_new_ex(max_triangles_num > 0 ? max_triangles_num : 16);
    events_.resize(max_triangles_num);
  }

  ~TrianglePriorityQueue()
  {
    if (heap_ != nullptr) {
      BLI_heap_free(heap_, nullptr);
      heap_ = nullptr;
    }
  }

  /* Non-copyable, movable */
  TrianglePriorityQueue(const TrianglePriorityQueue &) = delete;
  TrianglePriorityQueue &operator=(const TrianglePriorityQueue &) = delete;

  TrianglePriorityQueue(TrianglePriorityQueue &&other) noexcept
      : heap_(other.heap_), events_(std::move(other.events_))
  {
    other.heap_ = nullptr;
  }

  TrianglePriorityQueue &operator=(TrianglePriorityQueue &&other) noexcept
  {
    if (this != &other) {
      if (heap_ != nullptr) {
        BLI_heap_free(heap_, nullptr);
      }
      heap_ = other.heap_;
      events_ = std::move(other.events_);
      other.heap_ = nullptr;
    }
    return *this;
  }

  void reinitialize(int max_triangles_num)
  {
    if (heap_ != nullptr) {
      BLI_heap_free(heap_, nullptr);
    }
    heap_ = BLI_heap_new_ex(max_triangles_num > 0 ? max_triangles_num : 16);
    events_.clear();
    events_.resize(max_triangles_num);
  }

  int events_num() const
  {
    return heap_ != nullptr ? int(BLI_heap_len(heap_)) : 0;
  }

  bool is_empty() const
  {
    return heap_ == nullptr || BLI_heap_is_empty(heap_);
  }

  void insert_or_update(int ti, const TriangleEvent &event)
  {
    if (ti >= events_.size()) {
      events_.resize(std::max(ti + 1, int(events_.size()) * 2));
    }
    TriangleEvent &ev = events_[ti];
    HeapNode *prev_node = ev.heap_node;
    ev = event;
    ev.triangle_index = ti;
    ev.heap_node = prev_node;

    if (heap_ == nullptr) {
      heap_ = BLI_heap_new();
    }

    if (std::isinf(event.time) || event.time < 0.0 || event.type == TriangleEventType::NONE) {
      if (ev.heap_node != nullptr) {
        BLI_heap_remove(heap_, ev.heap_node);
        ev.heap_node = nullptr;
      }
      return;
    }

    const float key = float(event.time);
    if (ev.heap_node != nullptr) {
      BLI_heap_node_value_update(heap_, ev.heap_node, key);
    }
    else {
      ev.heap_node = BLI_heap_insert(heap_, key, POINTER_FROM_INT(ti));
    }
  }

  void remove(int ti)
  {
    if (ti < 0 || ti >= events_.size()) {
      return;
    }
    TriangleEvent &ev = events_[ti];
    if (ev.heap_node != nullptr && heap_ != nullptr) {
      BLI_heap_remove(heap_, ev.heap_node);
      ev.heap_node = nullptr;
    }
    ev.type = TriangleEventType::NONE;
    ev.time = std::numeric_limits<double>::infinity();
  }

  const TriangleEvent *top() const
  {
    if (is_empty()) {
      return nullptr;
    }
    HeapNode *node = BLI_heap_top(heap_);
    const int ti = POINTER_AS_INT(BLI_heap_node_ptr(node));
    return &events_[ti];
  }

  TriangleEvent pop_min()
  {
    BLI_assert(!is_empty());
    void *ptr = BLI_heap_pop_min(heap_);
    const int ti = POINTER_AS_INT(ptr);
    TriangleEvent ev = events_[ti];
    events_[ti].heap_node = nullptr;
    return ev;
  }

  const TriangleEvent &get_event(int ti) const
  {
    return events_[ti];
  }

  const Vector<TriangleEvent> &events() const
  {
    return events_;
  }
};

static void dump_priority_queue(const TrianglePriorityQueue &pq, std::ostream &os = std::cout)
{
  os << "Priority Queue (" << pq.events_num() << " active events):\n";
  Vector<TriangleEvent> active_events;
  for (const TriangleEvent &ev : pq.events()) {
    if (ev.heap_node != nullptr) {
      active_events.append(ev);
    }
  }
  std::sort(active_events.begin(),
            active_events.end(),
            [](const TriangleEvent &a, const TriangleEvent &b) {
              if (std::abs(a.time - b.time) > 1e-9) {
                return a.time < b.time;
              }
              return a.triangle_index < b.triangle_index;
            });

  for (const TriangleEvent &ev : active_events) {
    os << "  t" << std::setw(2) << ev.triangle_index << " | Type: " << std::setw(18) << std::left
       << triangle_event_type_name(ev.type) << " | Time: " << std::setw(10) << std::fixed
       << std::setprecision(6) << ev.time << " | Pos: (" << std::setw(8) << std::fixed
       << std::setprecision(4) << ev.event_pos.x << ", " << std::setw(8) << std::fixed
       << std::setprecision(4) << ev.event_pos.y << ")"
       << " | Edge: " << ev.edge_index << "\n";
  }
}

/**
 * Numerically stable quadratic solver for a * t^2 + b * t + c = 0.
 * Returns the smallest real root strictly greater than \a t_min.
 */
static bool solve_quadratic_smallest_future_root(
    const double a, const double b, const double c, const double t_min, double &r_t)
{
  constexpr double eps = 1e-14;
  if (std::abs(a) < eps) {
    if (std::abs(b) < eps) {
      return false;
    }
    const double t = -c / b;
    if (t > t_min) {
      r_t = t;
      return true;
    }
    return false;
  }

  double disc = b * b - 4.0 * a * c;
  if (disc < 0.0) {
    if (disc > -eps) {
      disc = 0.0;
    }
    else {
      return false;
    }
  }

  const double sqrt_disc = std::sqrt(disc);
  double t1, t2;
  if (std::abs(b) < eps) {
    t1 = -sqrt_disc / (2.0 * a);
    t2 = sqrt_disc / (2.0 * a);
  }
  else {
    const double q = -0.5 * (b + (b > 0.0 ? sqrt_disc : -sqrt_disc));
    t1 = q / a;
    t2 = c / q;
  }

  if (t1 > t2) {
    std::swap(t1, t2);
  }

  if (t1 > t_min) {
    r_t = t1;
    return true;
  }
  if (t2 > t_min) {
    r_t = t2;
    return true;
  }
  return false;
}

static Array<double2> compute_contour_edge_normals(const ContourSequences &contours,
                                                   const Span<double2> vert_pos,
                                                   const StraightSkeletonInput &input)
{
  const int orig_verts_num = int(input.vert.size());
  const int contour_edges_num = int(contours.edges.size());
  Array<double2> normals(contour_edges_num, double2(0.0, 0.0));

  for (const int cei : IndexRange(contour_edges_num)) {
    const ContourEdge &ce = contours.edges[cei];
    if (ce.side == -1) {
      /* 0-length cap edge. Outward normal is along or opposite input edge direction. */
      const int ei = ce.input_edge;
      const int u = input.edge[ei][0];
      const int v = input.edge[ei][1];
      double2 d = input.vert[v] - input.vert[u];
      const double len = std::hypot(d.x, d.y);
      if (len > 1e-14) {
        d /= len;
      }
      if ((ce.start % orig_verts_num) == v) {
        normals[cei] = d;
      }
      else {
        normals[cei] = -d;
      }
    }
    else {
      /* Directed edge is start -> end. Inward (left) normal is (-dy, dx) / length. */
      const double2 p_start = vert_pos[ce.start];
      const double2 p_end = vert_pos[ce.end];
      const double2 d = p_end - p_start;
      const double len = std::hypot(d.x, d.y);
      if (len > 1e-14) {
        normals[cei] = double2(-d.y / len, d.x / len);
      }
      else {
        normals[cei] = double2(0.0, 1.0);
      }
    }
  }
  return normals;
}

static Array<double2> compute_kinetic_velocities(const ContourSequences &contours,
                                                 const Span<double2> vert_pos,
                                                 const StraightSkeletonInput &input)
{
  const int total_verts_num = int(vert_pos.size());
  Array<double2> vert_vel(total_verts_num, double2(0.0, 0.0));
  const Array<double2> edge_normals = compute_contour_edge_normals(contours, vert_pos, input);

  Array<int> incoming_edge(total_verts_num, -1);
  Array<int> outgoing_edge(total_verts_num, -1);

  for (const int cei : contours.edges.index_range()) {
    const ContourEdge &ce = contours.edges[cei];
    if (ce.start >= 0 && ce.start < total_verts_num) {
      outgoing_edge[ce.start] = cei;
    }
    if (ce.end >= 0 && ce.end < total_verts_num) {
      incoming_edge[ce.end] = cei;
    }
  }

  for (const int vi : IndexRange(total_verts_num)) {
    const int in_ei = incoming_edge[vi];
    const int out_ei = outgoing_edge[vi];
    if (in_ei == -1 || out_ei == -1) {
      continue;
    }

    const double2 n1 = edge_normals[in_ei];
    const double2 n2 = edge_normals[out_ei];
    const double det = n1.x * n2.y - n1.y * n2.x;

    if (std::abs(det) > 1e-12) {
      vert_vel[vi] = double2((n2.y - n1.y) / det, (n1.x - n2.x) / det);
    }
    else {
      if (n1.x * n2.x + n1.y * n2.y > 0.0) {
        vert_vel[vi] = n1;
      }
      else {
        vert_vel[vi] = double2(0.0, 0.0);
      }
    }
  }

  return vert_vel;
}

static TriangleEvent compute_triangle_collapse(const int ti,
                                               const TriangleEdges &tri,
                                               const Span<double2> vert_pos,
                                               const Span<double2> vert_vel,
                                               const Span<double2> edge_normals,
                                               const ContourSequences &contours,
                                               [[maybe_unused]] const StraightSkeletonInput &input,
                                               const double current_time)
{
  TriangleEvent best_event;
  best_event.triangle_index = ti;
  best_event.time = std::numeric_limits<double>::infinity();
  best_event.type = TriangleEventType::NONE;

  if (tri.is_triangle_deleted(ti)) {
    return best_event;
  }

  const int total_verts_num = int(vert_pos.size());

  if (tri.is_triangle_unbounded(ti)) {
    const int e0 = 3 * ti;
    const int vi = tri.start_vert(e0);
    const int ui = tri.start_vert(e0 + 1);
    if (vi < 0 || vi >= total_verts_num || ui < 0 || ui >= total_verts_num) {
      return best_event;
    }

    const double2 p_v = vert_pos[vi];
    const double2 p_u = vert_pos[ui];
    const double2 v_v = vert_vel[vi];
    const double2 v_u = vert_vel[ui];

    /* 1. Base edge collapse. */
    const double2 dp0 = p_u - p_v;
    const double2 dv = v_u - v_v;
    const double dv_sq = dv.x * dv.x + dv.y * dv.y;
    if (dv_sq > 1e-14) {
      const double dt = -(dp0.x * dv.x + dp0.y * dv.y) / dv_sq;
      if (dt > 1e-9) {
        const double t = current_time + dt;
        const double2 p_at_t = dp0 + dt * dv;
        const double len_at_t = std::hypot(p_at_t.x, p_at_t.y);
        if (len_at_t < 1e-7) {
          const bool is_wf = contours.edge_by_verts.contains(int2(vi, ui));
          best_event.time = t;
          best_event.type = is_wf ? TriangleEventType::EDGE_COLLAPSE :
                                    TriangleEventType::UNBOUNDED_COLLAPSE;
          best_event.edge_index = e0;
          best_event.event_pos = p_v + dt * v_v;
        }
      }
    }

    /* 2. Collinear with origin:
     * Signed area of triangle formed by v(t), u(t), and (0,0) is zero.
     * v(t) x u(t) = a * dt^2 + b * dt + c = 0. */
    const double a = v_v.x * v_u.y - v_v.y * v_u.x;
    const double b = (p_v.x * v_u.y - p_v.y * v_u.x) + (v_v.x * p_u.y - v_v.y * p_u.x);
    const double c = p_v.x * p_u.y - p_v.y * p_u.x;
    double t_coll = 0.0;
    if (solve_quadratic_smallest_future_root(a, b, c, 1e-9, t_coll)) {
      const double t = current_time + t_coll;
      if (t < best_event.time) {
        best_event.time = t;
        best_event.type = TriangleEventType::UNBOUNDED_COLLAPSE;
        best_event.edge_index = e0;
        best_event.event_pos = 0.5 * ((p_v + t_coll * v_v) + (p_u + t_coll * v_u));
      }
    }
    return best_event;
  }

  /* Bounded triangle. */
  const int e0 = 3 * ti;
  const int v0 = tri.start_vert(e0);
  const int v1 = tri.start_vert(e0 + 1);
  const int v2 = tri.start_vert(e0 + 2);

  if (v0 < 0 || v0 >= total_verts_num || v1 < 0 || v1 >= total_verts_num || v2 < 0 ||
      v2 >= total_verts_num)
  {
    return best_event;
  }

  const int tri_v[3] = {v0, v1, v2};
  const double2 p[3] = {vert_pos[v0], vert_pos[v1], vert_pos[v2]};
  const double2 v[3] = {vert_vel[v0], vert_vel[v1], vert_vel[v2]};

  bool is_wf[3] = {false, false, false};
  int wf_edge_contour_idx[3] = {-1, -1, -1};
  int wf_count = 0;

  for (const int k : IndexRange(3)) {
    const int start_v = tri_v[k];
    const int end_v = tri_v[(k + 1) % 3];
    const int *cei = contours.edge_by_verts.lookup_ptr(int2(start_v, end_v));
    if (cei != nullptr) {
      is_wf[k] = true;
      wf_edge_contour_idx[k] = *cei;
      wf_count++;
    }
  }

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
    }
  };

  /* Method 2: Collapsing edges (pairwise distance derivative). */
  for (const int k : IndexRange(3)) {
    const int u_idx = tri_v[k];
    const double2 dp0 = p[(k + 1) % 3] - p[k];
    const double2 dv = v[(k + 1) % 3] - v[k];
    const double dv_sq = dv.x * dv.x + dv.y * dv.y;
    if (dv_sq > 1e-14) {
      const double dt = -(dp0.x * dv.x + dp0.y * dv.y) / dv_sq;
      if (dt > 1e-9) {
        const double2 p_at_t = dp0 + dt * dv;
        const double len_at_t = std::hypot(p_at_t.x, p_at_t.y);
        if (is_wf[k]) {
          if (len_at_t < 1e-7) {
            TriangleEvent cand;
            cand.triangle_index = ti;
            cand.time = current_time + dt;
            cand.type = TriangleEventType::EDGE_COLLAPSE;
            cand.edge_index = e0 + k;
            cand.vert_index = u_idx;
            cand.event_pos = p[k] + dt * v[k];
            consider_candidate(cand);
          }
        }
        else {
          if (len_at_t < 1e-7) {
            TriangleEvent cand;
            cand.triangle_index = ti;
            cand.time = current_time + dt;
            cand.type = TriangleEventType::MULTI_SPLIT;
            cand.edge_index = e0 + k;
            cand.vert_index = u_idx;
            cand.event_pos = p[k] + dt * v[k];
            consider_candidate(cand);
          }
        }
      }
    }
  }

  /* Method 3: Vertices moving into edges (Split / Flip / Multi-split).
   * Only occurs for triangles with exactly one wavefront edge. */
  if (wf_count == 1) {
    int k_wf = -1;
    for (const int k : IndexRange(3)) {
      if (is_wf[k]) {
        k_wf = k;
        break;
      }
    }
    BLI_assert(k_wf != -1);
    const int a_idx = k_wf;
    const int b_idx = (k_wf + 1) % 3;
    const int c_idx = (k_wf + 2) % 3;

    const int cei = wf_edge_contour_idx[k_wf];
    const double2 n = edge_normals[cei];

    /* Orthogonal distance from vertex c to edge (a -> b). */
    const double d_perp = (p[c_idx].x - p[a_idx].x) * n.x + (p[c_idx].y - p[a_idx].y) * n.y;
    /* Speed of approach: edge moves inward at unit speed, vertex moves with v[c_idx]. */
    const double s_approach = 1.0 - (v[c_idx].x * n.x + v[c_idx].y * n.y);

    if (s_approach > 1e-12 && d_perp > -1e-9) {
      const double dt = d_perp / s_approach;
      if (dt > 1e-9) {
        const double2 p_a_t = p[a_idx] + dt * v[a_idx];
        const double2 p_b_t = p[b_idx] + dt * v[b_idx];
        const double2 p_c_t = p[c_idx] + dt * v[c_idx];
        const double2 edge_vec = p_b_t - p_a_t;
        const double edge_len_sq = edge_vec.x * edge_vec.x + edge_vec.y * edge_vec.y;
        if (edge_len_sq > 1e-14) {
          const double lambda = ((p_c_t.x - p_a_t.x) * edge_vec.x +
                                 (p_c_t.y - p_a_t.y) * edge_vec.y) /
                                edge_len_sq;
          TriangleEventType type;
          if (std::abs(lambda) <= 1e-6 || std::abs(lambda - 1.0) <= 1e-6) {
            type = TriangleEventType::MULTI_SPLIT;
          }
          else if (lambda > 1e-6 && lambda < 1.0 - 1e-6) {
            type = TriangleEventType::SPLIT;
          }
          else {
            type = TriangleEventType::FLIP;
          }
          TriangleEvent cand;
          cand.triangle_index = ti;
          cand.time = current_time + dt;
          cand.type = type;
          cand.edge_index = e0 + k_wf;
          cand.vert_index = tri_v[c_idx];
          cand.event_pos = p_c_t;
          consider_candidate(cand);
        }
      }
    }
  }

  /* Method 4: Signed-area quadratic fallback for triangles with 0 wavefront edges. */
  if (wf_count == 0 && best_event.type == TriangleEventType::NONE) {
    const double2 dp1 = p[1] - p[0];
    const double2 dp2 = p[2] - p[0];
    const double2 dv1 = v[1] - v[0];
    const double2 dv2 = v[2] - v[0];

    const double a = dv1.x * dv2.y - dv1.y * dv2.x;
    const double b = (dp1.x * dv2.y - dp1.y * dv2.x) + (dv1.x * dp2.y - dv1.y * dp2.x);
    const double c = dp1.x * dp2.y - dp1.y * dp2.x;

    double dt = 0.0;
    if (solve_quadratic_smallest_future_root(a, b, c, 1e-9, dt)) {
      const double2 p0_t = p[0] + dt * v[0];
      const double2 p1_t = p[1] + dt * v[1];
      const double2 p2_t = p[2] + dt * v[2];
      const double d01 = std::hypot(p1_t.x - p0_t.x, p1_t.y - p0_t.y);
      const double d02 = std::hypot(p2_t.x - p0_t.x, p2_t.y - p0_t.y);

      TriangleEvent cand;
      cand.triangle_index = ti;
      cand.time = current_time + dt;
      if (d01 < 1e-7 && d02 < 1e-7) {
        cand.type = TriangleEventType::MULTI_SPLIT;
      }
      else {
        cand.type = TriangleEventType::FLIP;
      }
      cand.edge_index = e0;
      cand.vert_index = v0;
      cand.event_pos = (p0_t + p1_t + p2_t) / 3.0;
      consider_candidate(cand);
    }
  }

  return best_event;
}

/**
 * Write an SVG file at \a path showing the triangulation described by \a tri and \a vert_pos.
 *
 * Bounded triangles are filled light blue, labelled at the centroid.
 * Unbounded triangles are labelled outside the base edge.
 * Each geometric edge is drawn once; half-edge indices are shown on each side, offset by a
 * fixed perpendicular distance so labels on opposite sides never overlap the edge.
 * Wings of unbounded triangles are drawn as short dashed stubs along the exterior angle
 * bisector at each hull vertex.
 */
static Array<double2> compute_cosmetic_displacements(const Span<double2> vert_pos,
                                                     const ContourSequences &contours,
                                                     double scale)
{
  const int verts_num = int(vert_pos.size());
  Array<double2> accum(verts_num, double2(0.0, 0.0));

  for (const ContourEdge &ce : contours.edges) {
    if (ce.side == -1) {
      /* Skip 0-length cap edges (normal is undefined). */
      continue;
    }
    const int u = ce.start;
    const int v = ce.end;
    if (u < 0 || u >= verts_num || v < 0 || v >= verts_num) {
      continue;
    }
    /* Directed vector in SVG pixels (SVG y is flipped relative to math y). */
    const double dx_s = (vert_pos[v].x - vert_pos[u].x) * scale;
    const double dy_s = -(vert_pos[v].y - vert_pos[u].y) * scale;
    const double dist_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
    if (dist_s < 1e-6) {
      continue;
    }
    /* Left-perpendicular in SVG pixels: (dy_s, -dx_s) / dist_s.
     * Points to the left of the directed edge in math coordinates (into the triangulation area).
     */
    const double px = dy_s / dist_s;
    const double py = -dx_s / dist_s;
    accum[u] += double2(px, py);
    accum[v] += double2(px, py);
  }

  Array<double2> offsets(verts_num, double2(0.0, 0.0));
  const double disp_px = 7.0; /* cosmetic offset in SVG pixels */
  for (const int vi : IndexRange(verts_num)) {
    const double length = std::sqrt(accum[vi].x * accum[vi].x + accum[vi].y * accum[vi].y);
    if (length > 1e-6) {
      offsets[vi] = (accum[vi] / length) * disp_px;
    }
  }
  return offsets;
}

/**
 * Write an SVG file at \a path showing the triangulation described by \a tri and \a vert_pos.
 *
 * Bounded triangles are filled light blue, labelled at the centroid.
 * Unbounded triangles are labelled outside the base edge.
 * Each geometric edge is drawn once; half-edge indices are shown on each side, offset by a
 * fixed perpendicular distance so labels on opposite sides never overlap the edge.
 * Wings of unbounded triangles are drawn as short dashed stubs along the exterior angle
 * bisector at each hull vertex.
 */
static void draw_triangulation_svg(const TriangleEdges &tri,
                                   const Span<double2> vert_pos,
                                   const StraightSkeletonInput &input,
                                   const ContourSequences &contours,
                                   const Span<double2> vert_vel = {},
                                   const TrianglePriorityQueue *pq = nullptr,
                                   const SkeletonGraph *skeleton = nullptr,
                                   const char *path = nullptr)
{
  const int verts_num = int(vert_pos.size());
  const int triangles_num = tri.triangles_num();
  const int edges_num = tri.edges_num();

  /* Bounding box across all input vertices. */
  double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
  for (const double2 &v : input.vert) {
    xmin = std::min(xmin, v.x);
    xmax = std::max(xmax, v.x);
    ymin = std::min(ymin, v.y);
    ymax = std::max(ymax, v.y);
  }
  const double range = std::max(xmax - xmin, ymax - ymin);
  const double pad = range * 0.25 + 0.5;

  const double cx_mid = 0.5 * (xmin + xmax);
  const double cy_mid = 0.5 * (ymin + ymax);
  const double half_range = 0.5 * std::max(xmax - xmin, ymax - ymin) + pad;
  xmin = cx_mid - half_range;
  xmax = cx_mid + half_range;
  ymin = cy_mid - half_range;
  ymax = cy_mid + half_range;

  const double svgw = 700.0;
  const double svgh = 700.0;
  const double scale = svgw / (xmax - xmin);

  /* Map math coordinates to SVG coordinates (SVG y-axis is flipped). */
  auto tx = [&](double x) { return (x - xmin) * scale; };
  auto ty = [&](double y) { return svgh - (y - ymin) * scale; };

  /* Compute cosmetic displacements so duplicate vertices don't completely overlap. */
  const Array<double2> offsets = compute_cosmetic_displacements(vert_pos, contours, scale);
  Array<double2> vert_svg_pos(verts_num);
  for (const int vi : IndexRange(verts_num)) {
    vert_svg_pos[vi] = double2(tx(vert_pos[vi].x) + offsets[vi].x,
                               ty(vert_pos[vi].y) + offsets[vi].y);
  }

  /* Hull adjacency maps: for each hull vertex u: next_hull[u] = v means base went v -> u. */
  Map<int, int> next_hull;
  Map<int, int> prev_hull;
  for (const int ti : IndexRange(triangles_num)) {
    if (!tri.is_triangle_unbounded(ti) || tri.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v = tri.start_vert(e0);
    const int u = tri.start_vert(e0 + 1);
    if (u >= 0 && v >= 0) {
      next_hull.add_overwrite(u, v);
      prev_hull.add_overwrite(v, u);
    }
  }

  auto stub_dir_svg = [&](int u) -> std::pair<double, double> {
    const int *nxt = next_hull.lookup_ptr(u);
    const int *prv = prev_hull.lookup_ptr(u);
    double2 sum{0.0, 0.0};
    if (nxt && *nxt >= 0 && *nxt < verts_num) {
      const double2 d = vert_svg_pos[*nxt] - vert_svg_pos[u];
      const double dist = std::sqrt(d.x * d.x + d.y * d.y);
      if (dist > 1e-14) {
        sum.x += d.x / dist;
        sum.y += d.y / dist;
      }
    }
    if (prv && *prv >= 0 && *prv < verts_num) {
      const double2 d = vert_svg_pos[*prv] - vert_svg_pos[u];
      const double dist = std::sqrt(d.x * d.x + d.y * d.y);
      if (dist > 1e-14) {
        sum.x += d.x / dist;
        sum.y += d.y / dist;
      }
    }
    double bx = -sum.x;
    double by = -sum.y;
    const double bdist = std::sqrt(bx * bx + by * by);
    if (bdist < 1e-14) {
      if (nxt && *nxt >= 0 && *nxt < verts_num) {
        const double2 d = vert_svg_pos[*nxt] - vert_svg_pos[u];
        bx = d.y;
        by = -d.x;
      }
    }
    else {
      bx /= bdist;
      by /= bdist;
    }
    return {bx, by};
  };

  const double ray_length = 45.0; /* wing stub length in pixels */
  const double label_off_px = 12.0;

  auto tri_centroid = [&](int ti) -> double2 {
    const int e0 = 3 * ti;
    double2 c{0.0, 0.0};
    int verts_count = 0;
    for (const int k : IndexRange(3)) {
      const int v = tri.start_vert(e0 + k);
      if (v >= 0 && v < verts_num) {
        c += vert_svg_pos[v];
        verts_count++;
      }
    }
    return (verts_count > 0) ? c / double(verts_count) : double2{0.0, 0.0};
  };

  const char *out_path = path ? path : debug_html_filepath;

  static int unnamed_case_counter = 0;
  std::string case_label;
  if (input.label != nullptr && input.label[0] != '\0') {
    case_label = input.label;
  }
  else {
    case_label = "Case " + std::to_string(++unnamed_case_counter);
  }

  std::ofstream f;
  if (debug_new_file) {
    f.open(out_path, std::ios::trunc);
    f << "<!DOCTYPE html>\n"
      << "<html>\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\">\n"
      << "  <title>Straight Skeleton Triangulations</title>\n"
      << "  <style>\n"
      << "    body {\n"
      << "      font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, "
         "Helvetica, Arial, sans-serif;\n"
      << "      margin: 24px 40px;\n"
      << "      background: #eef2f7;\n"
      << "      color: #1a202c;\n"
      << "    }\n"
      << "    h1 {\n"
      << "      color: #1a365d;\n"
      << "      margin-bottom: 24px;\n"
      << "    }\n"
      << "    .test-case {\n"
      << "      background: #ffffff;\n"
      << "      border: 1px solid #cbd5e0;\n"
      << "      border-radius: 8px;\n"
      << "      padding: 20px 24px 24px 24px;\n"
      << "      margin-bottom: 36px;\n"
      << "      box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.07), 0 2px 4px -1px rgba(0, 0, "
         "0, 0.04);\n"
      << "    }\n"
      << "    .test-case h2 {\n"
      << "      margin-top: 0;\n"
      << "      margin-bottom: 12px;\n"
      << "      color: #2b6cb0;\n"
      << "      font-size: 20px;\n"
      << "      border-bottom: 2px solid #edf2f7;\n"
      << "      padding-bottom: 8px;\n"
      << "    }\n"
      << "    .case-meta {\n"
      << "      margin: 0 0 16px 0;\n"
      << "      color: #718096;\n"
      << "      font-size: 13px;\n"
      << "    }\n"
      << "    .svg-container {\n"
      << "      display: inline-block;\n"
      << "      border: 1px solid #e2e8f0;\n"
      << "      background: #f8fafc;\n"
      << "      border-radius: 4px;\n"
      << "      overflow: hidden;\n"
      << "    }\n"
      << "    svg text {\n"
      << "      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "
         "monospace;\n"
      << "      user-select: none;\n"
      << "    }\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "<h1>Straight Skeleton Triangulations</h1>\n";
    debug_new_file = false;
  }
  else {
    f.open(out_path, std::ios::app);
  }

  f << "<section class=\"test-case\" id=\"" << case_label << "\">\n";
  f << "<h2>" << case_label << "</h2>\n";
  f << "<p class=\"case-meta\">Input: " << input.vert.size() << " vertices, " << input.edge.size()
    << " edges, " << input.face_offsets.size() << " faces &bull; Triangles: " << triangles_num
    << "</p>\n";
  f << "<div class=\"svg-container\">\n";
  f << "<svg width=\"" << svgw << "\" height=\"" << svgh << "\" viewBox=\"0 0 " << svgw << " "
    << svgh << "\">\n";
  f << "<text x=\"15\" y=\"20\" font-size=\"11\" fill=\"#000088\" font-weight=\"bold\">― Input "
       "contour edges (bold)</text>\n";
  f << "<text x=\"15\" y=\"35\" font-size=\"11\" fill=\"#cc0088\" font-weight=\"bold\">╌ 0-length "
       "cap edges (magenta)</text>\n";
  f << "<text x=\"15\" y=\"50\" font-size=\"11\" fill=\"#6688aa\">╌ Delaunay edges "
       "(dashed)</text>\n";
  f << "<text x=\"15\" y=\"65\" font-size=\"11\" fill=\"#228822\" font-weight=\"bold\">➔ "
       "Wavefront "
       "velocities (green)</text>\n";
  f << "<text x=\"15\" y=\"80\" font-size=\"11\" fill=\"#111111\">― Static input "
       "edges &amp; ● vertices</text>\n";

  /* Fill bounded triangles. */
  for (const int ti : IndexRange(triangles_num)) {
    if (tri.is_triangle_unbounded(ti) || tri.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v0 = tri.start_vert(e0);
    const int v1 = tri.start_vert(e0 + 1);
    const int v2 = tri.start_vert(e0 + 2);
    if (v0 < 0 || v0 >= verts_num || v1 < 0 || v1 >= verts_num || v2 < 0 || v2 >= verts_num) {
      continue;
    }
    f << "<polygon points=\"" << vert_svg_pos[v0].x << "," << vert_svg_pos[v0].y << " "
      << vert_svg_pos[v1].x << "," << vert_svg_pos[v1].y << " " << vert_svg_pos[v2].x << ","
      << vert_svg_pos[v2].y << "\" fill=\"#ddeeff\" stroke=\"none\"/>\n";
  }

  /* Draw geometric edges and half-edge labels. */
  Set<int2> drawn_geom_edges;
  for (const int ei : IndexRange(edges_num)) {
    const int u = tri.start_vert(ei);
    const int v = tri.end_vert(ei);

    if (u < 0 || u >= verts_num || v < 0 || v >= verts_num) {
      continue;
    }

    const int paired = tri.paired_edge(ei);
    if (paired == -1 || ei < paired) {
      drawn_geom_edges.add(int2(std::min(u, v), std::max(u, v)));
      const int *ce_idx = contours.edge_by_verts.lookup_ptr(int2(u, v));
      if (ce_idx == nullptr) {
        ce_idx = contours.edge_by_verts.lookup_ptr(int2(v, u));
      }

      if (ce_idx != nullptr) {
        const ContourEdge &ce = contours.edges[*ce_idx];
        if (ce.side == -1) {
          /* 0-length cap edge. */
          f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
            << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
            << "\" stroke=\"#cc0088\" stroke-width=\"2.5\" stroke-dasharray=\"4,2\"/>\n";
        }
        else {
          /* Input contour edge. */
          f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
            << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
            << "\" stroke=\"#000088\" stroke-width=\"2.5\"/>\n";
        }
      }
      else {
        /* Non-contour Delaunay edge. */
        f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
          << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
          << "\" stroke=\"#6688aa\" stroke-width=\"1.2\" stroke-dasharray=\"5,4\"/>\n";
      }
    }

    /* Half-edge label offset into triangle interior. */
    const double dx_s = vert_svg_pos[v].x - vert_svg_pos[u].x;
    const double dy_s = vert_svg_pos[v].y - vert_svg_pos[u].y;
    const double dist_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
    if (dist_s > 1e-6) {
      const double px = dy_s / dist_s;
      const double py = -dx_s / dist_s;
      const double mid_sx = 0.5 * (vert_svg_pos[u].x + vert_svg_pos[v].x);
      const double mid_sy = 0.5 * (vert_svg_pos[u].y + vert_svg_pos[v].y);
      const double lx = mid_sx + px * label_off_px;
      const double ly = mid_sy + py * label_off_px;
      f << "<text x=\"" << lx << "\" y=\"" << ly
        << "\" font-size=\"10\" fill=\"#880000\" text-anchor=\"middle\">" << ei << "</text>\n";
    }
  }

  /* Draw wing stubs and label wing edges. */
  for (const int ti : IndexRange(triangles_num)) {
    if (!tri.is_triangle_unbounded(ti) || tri.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int vi = tri.start_vert(e0);
    const int ui = tri.start_vert(e0 + 1);

    if (ui >= 0 && ui < verts_num) {
      const auto [ndx, ndy] = stub_dir_svg(ui);
      const double x1 = vert_svg_pos[ui].x;
      const double y1 = vert_svg_pos[ui].y;
      const double x2 = x1 + ndx * ray_length;
      const double y2 = y1 + ndy * ray_length;
      f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
        << "\" stroke=\"#999999\" stroke-width=\"1\" stroke-dasharray=\"5,3\"/>\n";
      f << "<text x=\"" << (x1 + ndx * ray_length * 0.6 + ndy * 8.0) << "\" y=\""
        << (y1 + ndy * ray_length * 0.6 - ndx * 8.0)
        << "\" font-size=\"10\" fill=\"#aa6600\" text-anchor=\"middle\">" << (e0 + 1)
        << "</text>\n";
    }

    if (vi >= 0 && vi < verts_num) {
      const auto [ndx, ndy] = stub_dir_svg(vi);
      const double x1 = vert_svg_pos[vi].x;
      const double y1 = vert_svg_pos[vi].y;
      f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << (x1 + ndx * ray_length * 0.4)
        << "\" y2=\"" << (y1 + ndy * ray_length * 0.4)
        << "\" stroke=\"#bbbbbb\" stroke-width=\"1\" stroke-dasharray=\"3,3\"/>\n";
      f << "<text x=\"" << (x1 + ndx * ray_length * 0.25 - ndy * 8.0) << "\" y=\""
        << (y1 + ndy * ray_length * 0.25 + ndx * 8.0)
        << "\" font-size=\"10\" fill=\"#aa8800\" text-anchor=\"middle\">" << (e0 + 2)
        << "</text>\n";
    }
  }

  /* Triangle labels. */
  for (const int ti : IndexRange(triangles_num)) {
    if (tri.is_triangle_deleted(ti)) {
      continue;
    }
    if (tri.is_triangle_unbounded(ti)) {
      const int e0 = 3 * ti;
      const int vi = tri.start_vert(e0);
      const int ui = tri.start_vert(e0 + 1);
      if (vi < 0 || vi >= verts_num || ui < 0 || ui >= verts_num) {
        continue;
      }
      const double dx_s = vert_svg_pos[ui].x - vert_svg_pos[vi].x;
      const double dy_s = vert_svg_pos[ui].y - vert_svg_pos[vi].y;
      const double dist_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
      if (dist_s < 1e-6) {
        continue;
      }
      const double px = dy_s / dist_s;
      const double py = -dx_s / dist_s;
      const double mid_sx = 0.5 * (vert_svg_pos[vi].x + vert_svg_pos[ui].x);
      const double mid_sy = 0.5 * (vert_svg_pos[vi].y + vert_svg_pos[ui].y);
      const double lx = mid_sx + px * 26.0;
      const double ly = mid_sy + py * 26.0;
      f << "<text x=\"" << lx << "\" y=\"" << ly
        << "\" font-size=\"11\" fill=\"#886600\" text-anchor=\"middle\" "
        << "font-weight=\"bold\">t" << ti << "</text>\n";
    }
    else {
      const double2 cen = tri_centroid(ti);
      f << "<text x=\"" << cen.x << "\" y=\"" << cen.y
        << "\" font-size=\"11\" fill=\"#004488\" text-anchor=\"middle\" "
        << "font-weight=\"bold\">t" << ti << "</text>\n";
    }
  }

  /* Draw any contour edges (wavefronts and end caps) not already drawn as part of tri. */
  for (const ContourEdge &ce : contours.edges) {
    const int u = ce.start;
    const int v = ce.end;
    if (u < 0 || u >= verts_num || v < 0 || v >= verts_num) {
      continue;
    }
    const int2 geom_edge(std::min(u, v), std::max(u, v));
    if (drawn_geom_edges.add(geom_edge)) {
      if (ce.side == -1) {
        /* 0-length cap edge. */
        f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
          << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
          << "\" stroke=\"#cc0088\" stroke-width=\"2.5\" stroke-dasharray=\"4,2\"/>\n";
      }
      else {
        /* Wavefront contour edge. */
        f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
          << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
          << "\" stroke=\"#000088\" stroke-width=\"2.5\"/>\n";
      }
    }
  }

  /* Vertex circles and real labels (including +N duplicates). */
  for (const int vi : IndexRange(verts_num)) {
    const double cx = vert_svg_pos[vi].x;
    const double cy = vert_svg_pos[vi].y;
    f << "<circle cx=\"" << cx << "\" cy=\"" << cy
      << "\" r=\"5\" fill=\"#224488\" stroke=\"white\" stroke-width=\"1\"/>\n";
    f << "<text x=\"" << (cx + 7) << "\" y=\"" << (cy - 5)
      << "\" font-size=\"12\" fill=\"#112233\" font-weight=\"bold\">" << vi << "</text>\n";
  }

  /* Draw wavefront velocity arrows. */
  if (!vert_vel.is_empty()) {
    for (const int vi : IndexRange(verts_num)) {
      if (vi >= vert_vel.size()) {
        continue;
      }
      const double vx = vert_vel[vi].x;
      const double vy = vert_vel[vi].y;
      const double v_len = std::hypot(vx, vy);
      if (v_len > 1e-6) {
        const double arrow_px = 24.0;
        const double adx = (vx / v_len) * arrow_px;
        const double ady = (-vy / v_len) * arrow_px; /* Flip SVG y */
        const double x1 = vert_svg_pos[vi].x;
        const double y1 = vert_svg_pos[vi].y;
        const double x2 = x1 + adx;
        const double y2 = y1 + ady;
        f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
          << "\" stroke=\"#228822\" stroke-width=\"1.8\"/>\n";
        const double tip_len = 6.0;
        const double perp_x = -ady / arrow_px * tip_len * 0.6;
        const double perp_y = adx / arrow_px * tip_len * 0.6;
        const double back_x = x2 - (adx / arrow_px) * tip_len;
        const double back_y = y2 - (ady / arrow_px) * tip_len;
        f << "<polygon points=\"" << x2 << "," << y2 << " " << (back_x + perp_x) << ","
          << (back_y + perp_y) << " " << (back_x - perp_x) << "," << (back_y - perp_y)
          << "\" fill=\"#228822\" stroke=\"none\"/>\n";
      }
    }
  }

  /* Draw static original input edges (thin dashed line) inside the wavefront. */
  for (const int ei : input.edge.index_range()) {
    const int u = input.edge[ei][0];
    const int v = input.edge[ei][1];
    const double x1 = tx(input.vert[u].x);
    const double y1 = ty(input.vert[u].y);
    const double x2 = tx(input.vert[v].x);
    const double y2 = ty(input.vert[v].y);
    f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
      << "\" stroke=\"#111111\" stroke-width=\"1.5\" stroke-dasharray=\"3,2\"/>\n";
  }
  for (const int fi : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[fi];
    const int face_verts_num = int(r.size());
    for (const int k : IndexRange(face_verts_num)) {
      const int u = input.face_vert_indices[r[k]];
      const int v = input.face_vert_indices[r[(k + 1) % face_verts_num]];
      const double x1 = tx(input.vert[u].x);
      const double y1 = ty(input.vert[u].y);
      const double x2 = tx(input.vert[v].x);
      const double y2 = ty(input.vert[v].y);
      f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
        << "\" stroke=\"#111111\" stroke-width=\"1.5\" stroke-dasharray=\"3,2\"/>\n";
    }
  }

  /* Draw static original input vertices (small black dots at t=0). */
  for (const int vi : input.vert.index_range()) {
    const double cx = tx(input.vert[vi].x);
    const double cy = ty(input.vert[vi].y);
    f << "<circle cx=\"" << cx << "\" cy=\"" << cy
      << "\" r=\"3.5\" fill=\"#111111\" stroke=\"white\" stroke-width=\"1\"/>\n";
    f << "<text x=\"" << (cx - 12) << "\" y=\"" << (cy + 14)
      << "\" font-size=\"10\" fill=\"#444444\" font-weight=\"bold\">v" << vi << "</text>\n";
  }

  f << "</svg>\n";
  f << "</div>\n";

  /* Dump of Contours, Triangles, Priority Queue, and Skeleton Graph in readable HTML section. */
  std::stringstream ss_contours;
  dump_contours(contours, ss_contours);

  std::stringstream ss_triangles;
  dump_triangles(tri, ss_triangles);

  std::stringstream ss_pq;
  if (pq != nullptr) {
    dump_priority_queue(*pq, ss_pq);
  }

  std::stringstream ss_graph;
  if (skeleton != nullptr) {
    dump_skeleton_graph(*skeleton, ss_graph);
  }

  f << "<details open style=\"margin-top: 16px;\">\n";
  f << "  <summary style=\"font-weight: bold; cursor: pointer; color: #2b6cb0; margin-bottom: "
       "8px;\">Contours, Triangles, Priority Queue &amp; Skeleton Graph Dump</summary>\n";
  f << "  <div style=\"display: flex; gap: 20px; flex-wrap: wrap;\">\n";
  f << "    <div style=\"flex: 1; min-width: 260px;\">\n";
  f << "      <h4 style=\"margin: 4px 0 8px 0; color: #4a5568;\">Contour Sequences</h4>\n";
  f << "      <pre style=\"background: #f7fafc; border: 1px solid #e2e8f0; padding: 10px; "
       "border-radius: 4px; font-family: monospace; font-size: 11px; overflow-x: auto; "
       "max-height: 450px;\">"
    << ss_contours.str() << "</pre>\n";
  f << "    </div>\n";
  f << "    <div style=\"flex: 1; min-width: 260px;\">\n";
  f << "      <h4 style=\"margin: 4px 0 8px 0; color: #4a5568;\">Triangles</h4>\n";
  f << "      <pre style=\"background: #f7fafc; border: 1px solid #e2e8f0; padding: 10px; "
       "border-radius: 4px; font-family: monospace; font-size: 11px; overflow-x: auto; "
       "max-height: 450px;\">"
    << ss_triangles.str() << "</pre>\n";
  f << "    </div>\n";
  if (pq != nullptr) {
    f << "    <div style=\"flex: 1; min-width: 260px;\">\n";
    f << "      <h4 style=\"margin: 4px 0 8px 0; color: #4a5568;\">Priority Queue</h4>\n";
    f << "      <pre style=\"background: #f7fafc; border: 1px solid #e2e8f0; padding: 10px; "
         "border-radius: 4px; font-family: monospace; font-size: 11px; overflow-x: auto; "
         "max-height: 450px;\">"
      << ss_pq.str() << "</pre>\n";
    f << "    </div>\n";
  }
  if (skeleton != nullptr) {
    f << "    <div style=\"flex: 1; min-width: 260px;\">\n";
    f << "      <h4 style=\"margin: 4px 0 8px 0; color: #4a5568;\">Skeleton Graph</h4>\n";
    f << "      <pre style=\"background: #f7fafc; border: 1px solid #e2e8f0; padding: 10px; "
         "border-radius: 4px; font-family: monospace; font-size: 11px; overflow-x: auto; "
         "max-height: 450px;\">"
      << ss_graph.str() << "</pre>\n";
    f << "    </div>\n";
  }
  f << "  </div>\n";
  f << "</details>\n";

  f << "</section>\n";
  f.close();
  std::cout << "SVG section '" << case_label << "' written to " << out_path << "\n";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Triangulation construction and rewiring
 * \{ */

static void build_contour_sequences(const StraightSkeletonInput &input,
                                    ContourSequences &r_contours)
{
  const int orig_verts_num = int(input.vert.size());

  /* 1. Closed face boundaries. */
  const Span<int> fvi = input.face_vert_indices;
  for (const int fi : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[fi];
    const int face_verts_num = int(r.size());
    for (const int k : IndexRange(face_verts_num)) {
      const int u = fvi[r[k]];
      const int v = fvi[r[(k + 1) % face_verts_num]];

      ContourEdge ce0;
      ce0.start = u;
      ce0.end = v;
      ce0.input_face = fi;
      ce0.side = 0;
      const int idx0 = int(r_contours.edges.size());
      r_contours.edges.append(ce0);
      r_contours.edge_by_verts.add(int2(u, v), idx0);

      ContourEdge ce1;
      ce1.start = v + orig_verts_num;
      ce1.end = u + orig_verts_num;
      ce1.input_face = fi;
      ce1.side = 1;
      const int idx1 = int(r_contours.edges.size());
      r_contours.edges.append(ce1);
      r_contours.edge_by_verts.add(int2(ce1.start, ce1.end), idx1);
    }
  }

  /* 2. Explicit line segment edges. */
  for (const int ei : input.edge.index_range()) {
    const int u = input.edge[ei][0];
    const int v = input.edge[ei][1];

    ContourEdge ce0;
    ce0.start = u;
    ce0.end = v;
    ce0.input_edge = ei;
    ce0.side = 0;
    const int idx0 = int(r_contours.edges.size());
    r_contours.edges.append(ce0);
    r_contours.edge_by_verts.add(int2(u, v), idx0);

    ContourEdge ce1;
    ce1.start = v + orig_verts_num;
    ce1.end = u + orig_verts_num;
    ce1.input_edge = ei;
    ce1.side = 1;
    const int idx1 = int(r_contours.edges.size());
    r_contours.edges.append(ce1);
    r_contours.edge_by_verts.add(int2(ce1.start, ce1.end), idx1);
  }

  /* 3. 0-length cap edges at free ends. */
  Array<int> degree(orig_verts_num, 0);
  Array<int> last_edge(orig_verts_num, -1);
  for (const int ei : input.edge.index_range()) {
    const int u = input.edge[ei][0];
    const int v = input.edge[ei][1];
    degree[u]++;
    last_edge[u] = ei;
    degree[v]++;
    last_edge[v] = ei;
  }
  Array<bool> in_face(orig_verts_num, false);
  for (const int vi : input.face_vert_indices) {
    in_face[vi] = true;
  }

  for (const int vi : IndexRange(orig_verts_num)) {
    if (degree[vi] == 1 && !in_face[vi]) {
      const int ei = last_edge[vi];
      const int v = input.edge[ei][1];
      ContourEdge cap;
      cap.side = -1;
      cap.input_edge = ei;
      if (vi == v) {
        /* Edge arrives at vi: cap goes vi -> vi + N. */
        cap.start = vi;
        cap.end = vi + orig_verts_num;
      }
      else {
        /* Edge leaves vi: cap goes vi + N -> vi. */
        cap.start = vi + orig_verts_num;
        cap.end = vi;
      }
      const int cap_idx = int(r_contours.edges.size());
      r_contours.edges.append(cap);
      r_contours.edge_by_verts.add(int2(cap.start, cap.end), cap_idx);
    }
  }

  /* 4. Link contour edges into loops. */
  Map<int, Vector<int>> outgoing_map;
  for (const int cei : r_contours.edges.index_range()) {
    outgoing_map.lookup_or_add_default(r_contours.edges[cei].start).append(cei);
  }

  for (const int cei : r_contours.edges.index_range()) {
    const int end_v = r_contours.edges[cei].end;
    const Vector<int> *out = outgoing_map.lookup_ptr(end_v);
    if (out != nullptr && !out->is_empty()) {
      r_contours.edges[cei].next = (*out)[0];
      r_contours.edges[(*out)[0]].prev = cei;
    }
  }

  /* 5. Extract loops. */
  Array<bool> visited(r_contours.edges.size(), false);
  for (const int i : r_contours.edges.index_range()) {
    if (visited[i]) {
      continue;
    }
    Vector<int> loop;
    int curr = i;
    while (!visited[curr]) {
      visited[curr] = true;
      loop.append(curr);
      curr = r_contours.edges[curr].next;
      if (curr == -1) {
        break;
      }
    }
    r_contours.loops.append(std::move(loop));
  }
}

static void pair_unbounded_wings(TriangleEdges &triangles)
{
  const int triangles_num = triangles.triangles_num();
  Map<int, int> wing_out; /* vert -> edge index of vert -> -1 */
  Map<int, int> wing_in;  /* vert -> edge index of -1 -> vert */

  for (const int ti : IndexRange(triangles_num)) {
    if (!triangles.is_triangle_unbounded(ti) || triangles.is_triangle_deleted(ti)) {
      continue;
    }
    const int e_right = 3 * ti + 1; /* vert -> -1 */
    const int e_left = 3 * ti + 2;  /* -1 -> vert */
    const int u = triangles.start_vert(e_right);
    const int v = triangles.end_vert(e_left);
    if (u >= 0) {
      wing_out.add_overwrite(u, e_right);
    }
    if (v >= 0) {
      wing_in.add_overwrite(v, e_left);
    }
  }

  for (const auto item : wing_out.items()) {
    const int u = item.key;
    const int e_out = item.value;
    const int *e_in = wing_in.lookup_ptr(u);
    if (e_in != nullptr) {
      triangles.set_paired(e_out, *e_in);
      triangles.set_paired(*e_in, e_out);
    }
  }
}

static int choose_vertex_duplicate(const int v,
                                   const double2 &dir,
                                   const StraightSkeletonInput &input,
                                   const ContourSequences &contours)
{
  const int orig_verts_num = int(input.vert.size());
  const double2 pv = input.vert[v];

  /* Find incident Side 0 contour edges. */
  int w_out = -1;
  int w_in = -1;
  for (const ContourEdge &ce : contours.edges) {
    if (ce.side == 0) {
      if (ce.start == v) {
        w_out = ce.end;
      }
      if (ce.end == v) {
        w_in = ce.start;
      }
    }
  }

  const double2 p_test = pv + dir;

  if (w_out != -1 && w_in != -1) {
    /* Corner (closed face or interior chain vertex). */
    const double2 p_out = input.vert[w_out];
    const double2 p_in = input.vert[w_in];

    const int o_turn = orient2d(p_in, pv, p_out);
    const int o_out = orient2d(pv, p_out, p_test);
    const int o_in = orient2d(p_in, pv, p_test);

    bool is_side0 = false;
    if (o_turn == 0) {
      /* Straight line: Side 0 is left of outgoing edge. */
      is_side0 = (o_out >= 0);
    }
    else if (o_turn > 0) {
      /* Convex corner (interior angle < 180 deg). */
      is_side0 = (o_out >= 0 && o_in >= 0);
    }
    else {
      /* Reflex corner (interior angle > 180 deg). */
      is_side0 = (o_out >= 0 || o_in >= 0);
    }
    return is_side0 ? v : (v + orig_verts_num);
  }
  else if (w_out != -1) {
    /* Start of chain (free end): outgoing edge v -> w_out. */
    const double2 p_out = input.vert[w_out];
    return (orient2d(pv, p_out, p_test) >= 0) ? v : (v + orig_verts_num);
  }
  else if (w_in != -1) {
    /* End of chain (free end): incoming edge w_in -> v. */
    const double2 p_in = input.vert[w_in];
    return (orient2d(p_in, pv, p_test) >= 0) ? v : (v + orig_verts_num);
  }

  return v;
}

static void rewire_kinetic_triangulation(const StraightSkeletonInput &input,
                                         TriangleEdges &triangles,
                                         const ContourSequences &contours)
{
  const int orig_verts_num = int(input.vert.size());
  const int initial_triangles_num = triangles.triangles_num();

  /* 1. Build initial edge set to know which edges existed before rewiring. */
  Set<int2> initial_edges;
  for (const int ei : IndexRange(triangles.edges_num())) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    if (u >= 0 && v >= 0) {
      initial_edges.add(int2(std::min(u, v), std::max(u, v)));
    }
  }

  /* Identify convex hull vertices from initial unbounded triangles. */
  Set<int> hull_verts;
  for (const int ti : IndexRange(initial_triangles_num)) {
    if (triangles.is_triangle_unbounded(ti) && !triangles.is_triangle_deleted(ti)) {
      const int u = triangles.start_vert(3 * ti);
      const int v = triangles.start_vert(3 * ti + 1);
      if (u >= 0) {
        hull_verts.add(u % orig_verts_num);
      }
      if (v >= 0) {
        hull_verts.add(v % orig_verts_num);
      }
    }
  }

  /* 2. Rewire bounded triangles. */
  for (const int ti : IndexRange(initial_triangles_num)) {
    if (triangles.is_triangle_unbounded(ti) || triangles.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v0 = triangles.start_vert(e0) % orig_verts_num;
    const int v1 = triangles.start_vert(e0 + 1) % orig_verts_num;
    const int v2 = triangles.start_vert(e0 + 2) % orig_verts_num;

    const double2 p0 = input.vert[v0];
    const double2 p1 = input.vert[v1];
    const double2 p2 = input.vert[v2];
    const double2 cen = (p0 + p1 + p2) / 3.0;

    const int v0_new = choose_vertex_duplicate(v0, cen - p0, input, contours);
    const int v1_new = choose_vertex_duplicate(v1, cen - p1, input, contours);
    const int v2_new = choose_vertex_duplicate(v2, cen - p2, input, contours);

    triangles.set_triangle_verts(ti, v0_new, v1_new, v2_new);
  }

  /* Map from (u_orig, v_orig) to contour edge (start, end) for wavefront contour edges. */
  Map<int2, int2> contour_by_orig;
  for (const ContourEdge &ce : contours.edges) {
    if (ce.side >= 0) {
      contour_by_orig.add(int2(ce.start % orig_verts_num, ce.end % orig_verts_num),
                          int2(ce.start, ce.end));
    }
  }

  /* 3. Rewire unbounded hull triangles.
   * Each unbounded hull triangle's base edge is paired with a hull edge ei = (u, v)
   * of a bounded triangle. The base edge in CCW hull order goes v -> u.
   * If v -> u corresponds to an input wavefront edge, use the matching contour edge.
   * Otherwise (Delaunay edge between disconnected inputs), use (v_new, u_new) from
   * the rewired bounded Delaunay triangle. */
  for (const int ti : IndexRange(initial_triangles_num)) {
    if (!triangles.is_triangle_unbounded(ti) || triangles.is_triangle_deleted(ti)) {
      continue;
    }
    const int base_edge = 3 * ti;
    const int paired_ei = triangles.paired_edge(base_edge);
    if (paired_ei != -1) {
      const int u_orig = triangles.start_vert(paired_ei) % orig_verts_num;
      const int v_orig = triangles.end_vert(paired_ei) % orig_verts_num;
      const int2 *ce_verts = contour_by_orig.lookup_ptr(int2(v_orig, u_orig));
      if (ce_verts != nullptr) {
        triangles.set_triangle_verts(ti, (*ce_verts)[0], (*ce_verts)[1], -1);
      }
      else {
        const int u_new = triangles.start_vert(paired_ei);
        const int v_new = triangles.end_vert(paired_ei);
        triangles.set_triangle_verts(ti, v_new, u_new, -1);
      }
    }
  }

  /* 4. Handle facing free ends (e.g. verts 9 and 12).
   * For each pair of free ends connected by an initial Delaunay edge, add a quad. */
  Array<int> edge_deg(orig_verts_num, 0);
  for (const int2 &e : input.edge) {
    edge_deg[e[0]]++;
    edge_deg[e[1]]++;
  }

  Vector<int> free_ends;
  for (const int vi : IndexRange(orig_verts_num)) {
    if (edge_deg[vi] == 1) {
      free_ends.append(vi);
    }
  }

  for (int i = 0; i < free_ends.size(); i++) {
    for (int j = i + 1; j < free_ends.size(); j++) {
      const int u = free_ends[i];
      const int v = free_ends[j];
      if (!hull_verts.contains(u) && !hull_verts.contains(v)) {
        if (initial_edges.contains(int2(std::min(u, v), std::max(u, v)))) {
          const int u_dup = u + orig_verts_num;
          const int v_dup = v + orig_verts_num;
          triangles.add_triangle(u, u_dup, v);
          triangles.add_triangle(u, v, v_dup);
        }
      }
    }
  }

  /* 5. Handle free ends on the convex hull.
   * Add an unbounded triangle for the cap edge at each hull free end. */
  for (const int u : free_ends) {
    if (hull_verts.contains(u)) {
      int cap_start = -1;
      int cap_end = -1;
      for (const ContourEdge &ce : contours.edges) {
        if (ce.side == -1 && (ce.start == u || ce.end == u)) {
          cap_start = ce.start;
          cap_end = ce.end;
          break;
        }
      }
      if (cap_start != -1) {
        triangles.add_triangle(cap_start, cap_end, -1);
      }
    }
  }

  /* 6. Re-pair all half-edges. */
  const int total_edges_num = triangles.edges_num();
  Map<int2, int> new_edge_map;
  new_edge_map.reserve(total_edges_num);
  for (const int ei : IndexRange(total_edges_num)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    if (u >= 0 && v >= 0) {
      new_edge_map.add(int2(u, v), ei);
    }
  }
  for (const int ei : IndexRange(total_edges_num)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    if (u >= 0 && v >= 0) {
      const int *opp = new_edge_map.lookup_ptr(int2(v, u));
      if (opp != nullptr) {
        triangles.set_paired(ei, *opp);
      }
      else {
        triangles.set_paired(ei, -1);
      }
    }
    else {
      triangles.set_paired(ei, -1);
    }
  }

  /* 7. Pair unbounded wings around infinity. */
  pair_unbounded_wings(triangles);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main state
 * \{ */

/**
 * All working state for a straight skeleton computation.
 */
class StraightSkeletonState {
  /** The triangulation, stored as half-edges. */
  TriangleEdges triangles;
  /** Coincidence threshold used throughout the computation. */
  double epsilon{1e-8};
  /** 2D positions of all vertices (original [0, N-1] and duplicate [N, 2N-1]). */
  Array<double2> vert_pos;
  /** 2D velocities of all vertices. */
  Array<double2> vert_vel;
  /** Contour sequences (loops of wavefront edges). */
  ContourSequences contours;
  /** Priority queue of triangles ordered by collapse time. */
  TrianglePriorityQueue pq;
  /** Complete skeleton graph of nodes and edges. */
  SkeletonGraph skeleton;
  /** Active and completed kinetic vertices. */
  Vector<KineticVertex> kinetic_verts;

 public:
  StraightSkeletonState(const StraightSkeletonInput &input) : epsilon(input.epsilon)
  {
    const int orig_verts_num = int(input.vert.size());
    vert_pos.reinitialize(2 * orig_verts_num);
    for (const int i : IndexRange(orig_verts_num)) {
      vert_pos[i] = input.vert[i];
      vert_pos[i + orig_verts_num] = input.vert[i];
    }
  }

  const TrianglePriorityQueue &priority_queue() const
  {
    return pq;
  }

  Span<double2> velocities() const
  {
    return vert_vel;
  }

  const SkeletonGraph &skeleton_graph() const
  {
    return skeleton;
  }

  Span<KineticVertex> kinetic_vertices() const
  {
    return kinetic_verts.as_span();
  }

  /** Build the initial kinetic triangulation from \a input.
   * After this call #triangles contains the initial triangulation
   * with unbounded triangles added for exterior boundary edges.
   * \param dbg_level If > 0, dump the state to stdout and write an SVG to /tmp. */
  void build_initial_kinetic_triangulation(const StraightSkeletonInput &input, int dbg_level = 0);
};

void StraightSkeletonState::build_initial_kinetic_triangulation(const StraightSkeletonInput &input,
                                                                int dbg_level)
{
  const int orig_verts_num = int(input.vert.size());
  skeleton.nodes.clear();
  skeleton.edges.clear();
  skeleton.nodes.reserve(orig_verts_num * 2);

  /* Add all static input vertices at t = 0. */
  for (const int i : IndexRange(orig_verts_num)) {
    skeleton.add_node(input.vert[i], 0.0, i);
  }

  /* Add static input edges at t = 0. */
  for (const int ei : input.edge.index_range()) {
    const int u = input.edge[ei][0];
    const int v = input.edge[ei][1];
    skeleton.add_edge(u, v, ei, -1, 0, 0.0, 0.0, /*is_input_boundary=*/true);
  }

  /* Add face boundary edges at t = 0. */
  for (const int fi : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[fi];
    const int face_verts_num = int(r.size());
    for (const int k : IndexRange(face_verts_num)) {
      const int u = input.face_vert_indices[r[k]];
      const int v = input.face_vert_indices[r[(k + 1) % face_verts_num]];
      skeleton.add_edge(u, v, -1, fi, 0, 0.0, 0.0, /*is_input_boundary=*/true);
    }
  }

  /* 1. Build initial contour sequences from the input geometry (faces, edges, free-end caps). */
  build_contour_sequences(input, contours);

  /* 2. Compute constrained Delaunay triangulation (CDT_FULL covers the convex hull). */
  meshintersect::CDT_input<double> cdt_in;
  cdt_in.vert = input.vert;
  cdt_in.edge = input.edge;
  cdt_in.face_offsets = input.face_offsets;
  cdt_in.face_vert_indices = input.face_vert_indices;
  cdt_in.epsilon = epsilon;
  cdt_in.needed_ids = CDT_NO_ORIG_IDS;

  const meshintersect::CDT_result<double> cdt = meshintersect::delaunay_2d_calc(cdt_in, CDT_FULL);
  const int cdt_triangles_num = int(cdt.face.size());

  if (cdt_triangles_num == 0) {
    /* Degenerate 1D / collinear input (e.g. single line segment or collinear chains):
     * No 2D CDT faces exist because the input points have zero area in 2D.
     * Every contour edge in the wavefront loops lies on the exterior boundary.
     * Create an unbounded triangle with base v -> u for each contour edge u -> v,
     * then pair adjacent wings around infinity. */
    for (const Vector<int> &loop : contours.loops) {
      for (const int cei : loop) {
        const ContourEdge &ce = contours.edges[cei];
        triangles.add_triangle(ce.start, ce.end, -1);
      }
    }
    pair_unbounded_wings(triangles);
  }
  else {
    /* Load CDT triangles into TriangleEdges. */
    triangles.reserve_triangles(4 * cdt_triangles_num);
    for (const int fi : IndexRange(cdt_triangles_num)) {
      const Vector<int> &f = cdt.face[fi];
      BLI_assert(f.size() == 3);
      triangles.add_triangle(f[0], f[1], f[2]);
    }

    /* Pair up interior half-edges. */
    const int edges_num = 3 * cdt_triangles_num;
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
    }

    /* Add unbounded triangles for convex hull edges. */
    Vector<int> hull_edges;
    hull_edges.reserve(cdt_triangles_num);
    for (const int ei : IndexRange(edges_num)) {
      if (triangles.paired_edge(ei) == -1) {
        hull_edges.append(ei);
      }
    }
    for (const int ei : hull_edges) {
      const int u = triangles.start_vert(ei);
      const int v = triangles.end_vert(ei);
      const int new_tri = triangles.triangles_num();
      triangles.add_triangle(v, u, -1);
      const int base_edge = 3 * new_tri;
      triangles.set_paired(ei, base_edge);
      triangles.set_paired(base_edge, ei);
    }

    /* Rewire kinetic triangulation to attach triangles to correct duplicate vertices. */
    rewire_kinetic_triangulation(input, triangles, contours);
  }

  /* Compute vertex velocities for the initial wavefront. */
  vert_vel = compute_kinetic_velocities(contours, vert_pos, input);

  /* Initialize kinetic vertices from the wavefront. */
  const int total_verts_num = int(vert_pos.size());
  kinetic_verts.clear();
  kinetic_verts.resize(total_verts_num);
  for (const int k : IndexRange(total_verts_num)) {
    KineticVertex &kv = kinetic_verts[k];
    kv.id = k;
    kv.start_node = k % orig_verts_num;
    kv.start_time = 0.0;
    kv.start_pos = vert_pos[k];
    kv.velocity = vert_vel[k];
    kv.end_node = -1;
    kv.end_time = std::numeric_limits<double>::infinity();
    kv.is_active = true;
  }

  /* Compute initial collapse events and build the priority queue. */
  const Array<double2> edge_normals = compute_contour_edge_normals(contours, vert_pos, input);
  pq.reinitialize(triangles.triangles_num());
  for (const int ti : IndexRange(triangles.triangles_num())) {
    if (!triangles.is_triangle_deleted(ti)) {
      const TriangleEvent ev = compute_triangle_collapse(
          ti, triangles, vert_pos, vert_vel, edge_normals, contours, input, 0.0);
      if (ev.type != TriangleEventType::NONE && !std::isinf(ev.time)) {
        pq.insert_or_update(ti, ev);
      }
    }
  }

  if (dbg_level > 0) {
    dump_contours(contours);
    dump_triangles(triangles);
    dump_priority_queue(pq);
    dump_skeleton_graph(skeleton);
    draw_triangulation_svg(triangles, vert_pos, input, contours, vert_vel, &pq, &skeleton);
  }
}

/** \} */

Array<TriangleEvent> straight_skeleton_initial_events(const StraightSkeletonInput &input)
{
  StraightSkeletonState state(input);
  state.build_initial_kinetic_triangulation(input, /*dbg_level=*/0);
  const TrianglePriorityQueue &pq = state.priority_queue();

  Vector<TriangleEvent> active_events;
  for (const TriangleEvent &ev : pq.events()) {
    if (ev.heap_node != nullptr) {
      active_events.append(ev);
    }
  }
  std::sort(active_events.begin(),
            active_events.end(),
            [](const TriangleEvent &a, const TriangleEvent &b) {
              if (std::abs(a.time - b.time) > 1e-9) {
                return a.time < b.time;
              }
              return a.triangle_index < b.triangle_index;
            });
  return Array<TriangleEvent>(active_events.as_span());
}

SkeletonGraph straight_skeleton_initial_graph(const StraightSkeletonInput &input)
{
  StraightSkeletonState state(input);
  state.build_initial_kinetic_triangulation(input, /*dbg_level=*/0);
  return state.skeleton_graph();
}

StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input)
{
  StraightSkeletonState state(input);
  state.build_initial_kinetic_triangulation(input, /*dbg_level=*/1);
  /* TODO: run event loop. */
  return StraightSkeletonOutput{};
}

} /* namespace blender::straightskeleton */
