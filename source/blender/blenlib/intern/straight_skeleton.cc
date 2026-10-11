/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation for Planar Straight Line Graphs (PSLGs).
 * The algorithm used here is a simulation of the wavefront propagation process,
 * looking for events that change the structure:
 *    - Edge Collapse Event: An active edge shrinks to zero length.
 *    - Vertex-Edge Split Event: A reflex vertex collides with an advancing edge.
 *    - Vertex-Vertex Multi-Split: Two or more non-adjacent vertices collide.
 *    - Antiparallel Edge-Edge Event: Opposing parallel wavefronts collide along a ridge.
 *
 * Several acceleration structures to speed it up, including:
 *   - Independent Component Partitioning with parallel evaluation.
 *   - Event Priority Queue
 *   - Spatial Grid for finding candidate edge-edge collisions.
 *   - Other optimizations like using the fact that antiparallel edge-edge
 *     collisions only happen in specific geometric configurations
 */

#include "BLI_straight_skeleton.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "PRF_profile.hh"

namespace blender::straightskeleton {

constexpr double DEFAULT_CLUSTER_TOL = 2.5e-4;

/* -------------------------------------------------------------------- */
/** \name Data Structures
 * \{ */

/** Moving wavefront nodes between adjacent wavefront edges. */
struct WavefrontNodes {
  Vector<double2> pos;
  Vector<double2> vel;
  Vector<int> prev_node;
  Vector<int> next_node;
  Vector<int> skel_node;
  Vector<int> orig_edge;
  Vector<int> orig_face;
  Vector<int> contour_id;
  Vector<bool> is_active;
  Vector<double> time_birth;

  Vector<Vector<int>> left_trail;
  Vector<Vector<int>> right_trail;
};

/** Wavefront contours (outer boundary CCW, holes CW). */
struct WavefrontContours {
  Vector<int> head_node;
  Vector<bool> is_hole;
  Vector<bool> is_active;
  Vector<bool> is_wire;
};

/** Straight skeleton graph constructed incrementally. */
struct SkeletonGraph {
  Vector<double2> nodes_pos;
  Vector<double> nodes_time;
  Vector<int> nodes_orig;

  Vector<int> edges_node1;
  Vector<int> edges_node2;
  Vector<int> edges_orig;
  Vector<int> edges_face;
  Vector<double> edges_time_start;
  Vector<double> edges_time_end;
  Vector<bool> edges_is_boundary;
};

/** Output faces accumulated during simulation. */
struct OutputFaces {
  Vector<Vector<int>> faces;
  Vector<int> face_orig;
  Vector<int> face_band;
  Vector<int> face_orig_edge;
};

/** Ordered undirected edge representation for hashing. */
struct OrderedEdge {
  int v_low;
  int v_high;

  OrderedEdge() : v_low(-1), v_high(-1) {}
  OrderedEdge(int a, int b) : v_low(std::min(a, b)), v_high(std::max(a, b)) {}

  uint64_t hash() const
  {
    return (uint64_t(v_low) * 73856093u) ^ (uint64_t(v_high) * 19349663u);
  }

  bool operator==(const OrderedEdge &other) const
  {
    return v_low == other.v_low && v_high == other.v_high;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geometric Helpers
 * \{ */

/** Compute the inward unit normal of directed edge u -> v (interior to the left). */
static double2 compute_inward_normal(const double2 &p_u, const double2 &p_v)
{
  const double2 d = p_v - p_u;
  const double len = math::length(d);
  if (len < 1e-12) {
    return double2(0.0, 0.0);
  }
  return math::orthogonal(d / len);
}

/**
 * Compute angular bisector velocity vector from incoming and outgoing edge normals.
 * n1 is the incoming edge's inward normal and n2 is the outgoing edge's inward normal.
 * d2 is the direction of the outgoing edge.
 */
static double2 compute_bisector_velocity(const double2 &n1, const double2 &n2, const double2 &d2)
{
  const double det = math::cross(n1, n2);
  const double dot = math::dot(n1, n2);

  if (std::abs(det) > 1e-7) {
    double2 v((n2.y - n1.y) / det, (n1.x - n2.x) / det);
    const double speed = math::length(v);
    if (speed > 1000.0) {
      v = (v / speed) * 1000.0;
    }
    return v;
  }

  /* Parallel edges continuing straight. */
  if (dot > 0.0) {
    return n1;
  }

  /* Antiparallel opposing edges along a straight skeleton ridge.
   * The surviving chamber lies in direction d2 (outgoing edge).
   * The ridge vertex propagates along this line with speed 1. */
  return d2;
}

/** Compute signed area of a 2D polygon cycle. */
static double polygon_signed_area(Span<int> cycle, Span<double2> positions)
{
  const int n = int(cycle.size());
  double area = 0.0;
  for (const int i : IndexRange(n)) {
    const double2 p1 = positions[cycle[i]];
    const double2 p2 = positions[cycle[(i + 1) % n]];
    area += math::cross(p1, p2);
  }
  return 0.5 * area;
}

/** Test if 2D triangle (a, b, c) is counter-clockwise. */
static bool is_ccw_v2(const double2 &a, const double2 &b, const double2 &c)
{
  return (c.y - a.y) * (b.x - a.x) > (b.y - a.y) * (c.x - a.x);
}

/** Test whether 2D segments (p1, p2) and (p3, p4) strictly cross each other. */
static bool segments_cross_v2(const double2 &p1,
                              const double2 &p2,
                              const double2 &p3,
                              const double2 &p4)
{
  return (is_ccw_v2(p1, p3, p4) != is_ccw_v2(p2, p3, p4)) &&
         (is_ccw_v2(p1, p2, p3) != is_ccw_v2(p1, p2, p4));
}

/** Test whether 2D segment (p1, p2) crosses or passes too close to (p3, p4). */
static bool segment_crosses_or_touches_v2(const double2 &p1,
                                          const double2 &p2,
                                          const double2 &p3,
                                          const double2 &p4,
                                          const double eps_sq = 1e-12)
{
  if (segments_cross_v2(p1, p2, p3, p4)) {
    return true;
  }
  if (math::dist_squared_to_line_segment(p3, p1, p2) < eps_sq ||
      math::dist_squared_to_line_segment(p4, p1, p2) < eps_sq ||
      math::dist_squared_to_line_segment(p1, p3, p4) < eps_sq ||
      math::dist_squared_to_line_segment(p2, p3, p4) < eps_sq)
  {
    return true;
  }
  return false;
}

/** Test whether point pt is strictly inside 2D polygon using ray casting. */
static bool isect_point_polygon_v2(const double2 &pt, Span<double2> poly)
{
  const int nr = int(poly.size());
  bool inside = false;
  for (int i = 0, j = nr - 1; i < nr; j = i++) {
    if (((poly[i].y > pt.y) != (poly[j].y > pt.y)) &&
        (pt.x < (poly[j].x - poly[i].x) * (pt.y - poly[i].y) / (poly[j].y - poly[i].y + 1e-20) +
                    poly[i].x))
    {
      inside = !inside;
    }
  }
  return inside;
}

/** Find a point strictly inside a 2D simple polygon by finding an ear triangle. */
static double2 find_polygon_interior_point(Span<double2> coords, const double signed_area)
{
  const int n = int(coords.size());
  if (n < 3) {
    return (n > 0) ? coords[0] : double2(0.0, 0.0);
  }
  if (n == 3) {
    return (coords[0] + coords[1] + coords[2]) / 3.0;
  }

  const bool is_ccw = (signed_area > 0.0);

  for (const int i : IndexRange(n)) {
    const int prev_i = (i == 0) ? n - 1 : i - 1;
    const int next_i = (i + 1 == n) ? 0 : i + 1;
    const double2 &p_prev = coords[prev_i];
    const double2 &p_curr = coords[i];
    const double2 &p_next = coords[next_i];

    const double cross = math::cross(p_curr - p_prev, p_next - p_curr);
    if ((is_ccw && cross <= 1e-12) || (!is_ccw && cross >= -1e-12)) {
      continue;
    }

    bool has_internal_vert = false;
    for (const int j : IndexRange(n)) {
      if (j == prev_i || j == i || j == next_i) {
        continue;
      }
      if (math::isect_point_tri(coords[j], p_prev, p_curr, p_next) != 0) {
        has_internal_vert = true;
        break;
      }
    }

    if (!has_internal_vert) {
      return (p_prev + p_curr + p_next) / 3.0;
    }
  }

  double2 centroid(0.0, 0.0);
  for (const double2 &p : coords) {
    centroid += p;
  }
  return centroid / double(n);
}

/** Face analysis record for nesting hierarchy discovery. */
struct AnalyzedFace {
  int orig_face_idx{-1};
  Vector<int> vert_indices;
  Vector<double2> coords;
  double signed_area{0.0};
  double2 min_co{0.0, 0.0};
  double2 max_co{0.0, 0.0};
  double2 interior_pt{0.0, 0.0};
  int parent{-1};
  int depth{0};
  bool is_hole{false};
};

/** Build containment tree and calculate even-odd nesting depths. */
static void build_containment_tree(MutableSpan<AnalyzedFace> faces)
{
  const int num_faces = int(faces.size());
  threading::parallel_for(IndexRange(num_faces), 64, [&](const IndexRange range) {
    for (const int b : range) {
      int best_parent = -1;
      double min_parent_area = std::numeric_limits<double>::infinity();

      for (int a = 0; a < num_faces; a++) {
        if (a == b) {
          continue;
        }
        if (faces[a].min_co.x > faces[b].min_co.x || faces[a].max_co.x < faces[b].max_co.x ||
            faces[a].min_co.y > faces[b].min_co.y || faces[a].max_co.y < faces[b].max_co.y)
        {
          continue;
        }
        if (std::abs(faces[a].signed_area) <= std::abs(faces[b].signed_area)) {
          continue;
        }

        if (isect_point_polygon_v2(faces[b].interior_pt, faces[a].coords.as_span())) {
          const double a_area = std::abs(faces[a].signed_area);
          if (a_area < min_parent_area) {
            min_parent_area = a_area;
            best_parent = a;
          }
        }
      }
      faces[b].parent = best_parent;
    }
  });

  for (int i = 0; i < num_faces; i++) {
    int d = 0;
    int curr = faces[i].parent;
    while (curr != -1) {
      d++;
      curr = faces[curr].parent;
    }
    faces[i].depth = d;
    faces[i].is_hole = (d % 2 != 0);
  }
}

/** Extract an arc of vertex indices from cyclic loop from start_idx to end_idx (inclusive). */
static Vector<int> get_cyclic_arc(Span<int> loop, const int start_idx, const int end_idx)
{
  Vector<int> arc;
  const int n = int(loop.size());
  int curr = start_idx;
  while (true) {
    arc.append(loop[curr]);
    if (curr == end_idx) {
      break;
    }
    curr = (curr + 1) % n;
  }
  return arc;
}

/**
 * Clean consecutive duplicate vertices and retraced spikes / antennas from a cyclic polygon.
 *
 * Geometric Situation:
 * ====================
 * ```
 *                  (b) Spike / Antenna (zero area)
 *                 / ^
 *                /   \
 *               v     \
 *             (a)     (a)
 *             /         \
 *           ...         ...
 * ```
 * When an edge goes out from (a) to (b) and immediately retraces from (b) back to (a),
 * the edge pair (a, b) and (b, a) encloses zero area and leaves duplicate vertex (a) in
 * the polygon loop. Such spikes can arise when merging sub-faces sharing multi-edge seams.
 * Removing the spike collapses [a, b, a] to [a], restoring a simple manifold face boundary.
 */
static Vector<int> clean_polygon_spikes_and_duplicates(Span<int> polygon, Span<double2> vert_pos)
{
  Vector<int> result(polygon);

  bool modified = true;
  while (modified) {
    modified = false;

    /* Remove consecutive duplicates (v == next_v or coincident positions). */
    Vector<int> clean_consecutive;
    for (const int v : result) {
      if (clean_consecutive.is_empty()) {
        clean_consecutive.append(v);
      }
      else {
        const int last_v = clean_consecutive.last();
        if (last_v != v) {
          if (math::distance_squared(vert_pos[last_v], vert_pos[v]) < 1e-12) {
            /* Coincident consecutive vertices. */
          }
          else {
            clean_consecutive.append(v);
          }
        }
      }
    }
    while (clean_consecutive.size() >= 2) {
      const int first_v = clean_consecutive.first();
      const int last_v = clean_consecutive.last();
      if (first_v == last_v || math::distance_squared(vert_pos[first_v], vert_pos[last_v]) < 1e-12)
      {
        clean_consecutive.pop_last();
      }
      else {
        break;
      }
    }
    if (clean_consecutive.size() != result.size()) {
      result = std::move(clean_consecutive);
      modified = true;
    }

    if (result.size() < 3) {
      break;
    }

    /* Remove spikes / antennas (prev_v == next_v). */
    const int n = int(result.size());
    for (int i = 0; i < n; i++) {
      const int prev_v = result[(i - 1 + n) % n];
      const int next_v = result[(i + 1) % n];
      if (prev_v == next_v) {
        result.remove(i);
        modified = true;
        break;
      }
    }
    if (modified) {
      continue;
    }

    /* Remove geometric 180-degree turnaround spikes. */
    for (int i = 0; i < int(result.size()); i++) {
      const int count = int(result.size());
      if (count < 3) {
        break;
      }
      const int prev_v = result[(i - 1 + count) % count];
      const int curr_v = result[i];
      const int next_v = result[(i + 1) % count];
      const double2 a = vert_pos[prev_v];
      const double2 b = vert_pos[curr_v];
      const double2 c = vert_pos[next_v];
      const double2 v1 = b - a;
      const double2 v2 = c - b;
      const double l1_sq = math::length_squared(v1);
      const double l2_sq = math::length_squared(v2);
      if (l1_sq > 1e-12 && l2_sq > 1e-12) {
        const double cross = math::cross(v1, v2);
        if (cross * cross <= 1e-8 * l1_sq * l2_sq) {
          const double dot = math::dot(v1, v2);
          if (dot < 0.0) {
            /* 180-degree turnaround: b is a spike. */
            result.remove(i);
            modified = true;
            break;
          }
        }
      }
    }
  }

  return result;
}

/* -------------------------------------------------------------------- */
/** \name 2D Spatial Grid with Overlapping Buckets
 * \{ */

/**
 * Uniform 2D bounding-box grid with overlapping bucket insertions.
 *
 * Maps items (such as edges or vertices) into grid cells covering their bounding boxes.
 * Querying a bounding box returns candidate items registered in all cells overlapping
 * the query region.
 */
struct SpatialGrid2D {
  double x_min{0.0};
  double y_min{0.0};
  double inv_cell_dx{1.0};
  double inv_cell_dy{1.0};
  int cols{1};
  int rows{1};

  Array<int> cell_heads;
  Vector<int> entry_next;
  Vector<int> entry_item;

  void init(const double domain_xmin,
            const double domain_ymin,
            const double domain_xmax,
            const double domain_ymax,
            const int num_items)
  {
    const double w = std::max(domain_xmax - domain_xmin, 1e-6);
    const double h = std::max(domain_ymax - domain_ymin, 1e-6);
    x_min = domain_xmin;
    y_min = domain_ymin;

    const double aspect = w / h;
    const int target_cells = std::clamp(num_items, 1, 65536);
    cols = std::clamp(int(std::round(std::sqrt(double(target_cells) * aspect))), 1, 256);
    rows = std::clamp(int(std::round(std::sqrt(double(target_cells) / aspect))), 1, 256);

    inv_cell_dx = double(cols) / w;
    inv_cell_dy = double(rows) / h;

    const int total_cells = cols * rows;
    cell_heads.reinitialize(total_cells);
    cell_heads.fill(-1);
    entry_next.clear();
    entry_item.clear();
  }

  void insert(const int item, const double x1, const double y1, const double x2, const double y2)
  {
    const int cx1 = std::clamp(int((x1 - x_min) * inv_cell_dx), 0, cols - 1);
    const int cx2 = std::clamp(int((x2 - x_min) * inv_cell_dx), 0, cols - 1);
    const int cy1 = std::clamp(int((y1 - y_min) * inv_cell_dy), 0, rows - 1);
    const int cy2 = std::clamp(int((y2 - y_min) * inv_cell_dy), 0, rows - 1);

    for (int cy = cy1; cy <= cy2; cy++) {
      const int row_offset = cy * cols;
      for (int cx = cx1; cx <= cx2; cx++) {
        const int cell_idx = row_offset + cx;
        const int entry_id = int(entry_item.size());
        entry_item.append(item);
        entry_next.append(cell_heads[cell_idx]);
        cell_heads[cell_idx] = entry_id;
      }
    }
  }

  template<typename Callback>
  void for_each_overlapping(const double x1,
                            const double y1,
                            const double x2,
                            const double y2,
                            const Callback &callback) const
  {
    const int cx1 = std::clamp(int((x1 - x_min) * inv_cell_dx), 0, cols - 1);
    const int cx2 = std::clamp(int((x2 - x_min) * inv_cell_dx), 0, cols - 1);
    const int cy1 = std::clamp(int((y1 - y_min) * inv_cell_dy), 0, rows - 1);
    const int cy2 = std::clamp(int((y2 - y_min) * inv_cell_dy), 0, rows - 1);

    for (int cy = cy1; cy <= cy2; cy++) {
      const int row_offset = cy * cols;
      for (int cx = cx1; cx <= cx2; cx++) {
        const int cell_idx = row_offset + cx;
        for (int e = cell_heads[cell_idx]; e != -1; e = entry_next[e]) {
          callback(entry_item[e]);
        }
      }
    }
  }
};

/** \} */

/**
 * Recursively split a polygon containing pinch (duplicate non-consecutive) vertices
 * into simple polygons.
 */
static void split_pinch_polygon(Span<int> polygon,
                                Vector<Vector<int>> &r_simple_faces,
                                Span<double2> vert_pos)
{
  Vector<int> cleaned = clean_polygon_spikes_and_duplicates(polygon, vert_pos);
  if (cleaned.size() < 3) {
    return;
  }

  /* Look for duplicate vertices. */
  Map<int, int> first_occurrence;
  int pinch_v = -1;
  int idx_first = -1;
  int idx_second = -1;

  for (int i = 0; i < cleaned.size(); i++) {
    const int v = cleaned[i];
    if (const int *first_idx = first_occurrence.lookup_ptr(v)) {
      pinch_v = v;
      idx_first = *first_idx;
      idx_second = i;
      break;
    }
    first_occurrence.add(v, i);
  }

  if (pinch_v == -1) {
    const int count = cleaned.size();
    if (count <= 32) {
      for (int i = 0; i < count; i++) {
        const int v1 = cleaned[i];
        for (int j = i + 2; j < count; j++) {
          if (i == 0 && j == count - 1) {
            continue;
          }
          const int v2 = cleaned[j];
          if (math::distance_squared(vert_pos[v1], vert_pos[v2]) < 1e-12) {
            pinch_v = v1;
            idx_first = i;
            idx_second = j;
            break;
          }
        }
        if (pinch_v != -1) {
          break;
        }
      }
    }
    else {
      double xmin = std::numeric_limits<double>::infinity();
      double ymin = std::numeric_limits<double>::infinity();
      double xmax = -std::numeric_limits<double>::infinity();
      double ymax = -std::numeric_limits<double>::infinity();

      for (const int v : cleaned) {
        const double2 p = vert_pos[v];
        xmin = std::min(xmin, p.x);
        ymin = std::min(ymin, p.y);
        xmax = std::max(xmax, p.x);
        ymax = std::max(ymax, p.y);
      }

      SpatialGrid2D grid;
      grid.init(xmin, ymin, xmax, ymax, count);

      constexpr double eps = 1e-6;
      for (int i = 0; i < count; i++) {
        const double2 p = vert_pos[cleaned[i]];
        grid.insert(i, p.x - eps, p.y - eps, p.x + eps, p.y + eps);
      }

      for (int i = 0; i < count; i++) {
        const double2 p1 = vert_pos[cleaned[i]];
        grid.for_each_overlapping(
            p1.x - eps, p1.y - eps, p1.x + eps, p1.y + eps, [&](const int j) {
              if (j <= i + 1 || (i == 0 && j == count - 1)) {
                return;
              }
              if (pinch_v != -1) {
                return;
              }
              const double2 p2 = vert_pos[cleaned[j]];
              if (math::distance_squared(p1, p2) < 1e-12) {
                pinch_v = cleaned[i];
                idx_first = i;
                idx_second = j;
              }
            });
        if (pinch_v != -1) {
          break;
        }
      }
    }
  }

  if (pinch_v == -1) {
    /* No duplicates. It is a simple polygon. */
    r_simple_faces.append(std::move(cleaned));
    return;
  }

  /* Polygon is pinched at pinch_v.
   * Split into two loops:
   * Loop 1: from idx_first to idx_second - 1 (starts with pinch_v)
   * Loop 2: from idx_second to end + from 0 to idx_first - 1 (starts with pinch_v) */
  Vector<int> loop1;
  for (int k = idx_first; k < idx_second; k++) {
    loop1.append(cleaned[k]);
  }

  Vector<int> loop2;
  for (int k = idx_second; k < cleaned.size(); k++) {
    loop2.append(cleaned[k]);
  }
  for (int k = 0; k < idx_first; k++) {
    loop2.append(cleaned[k]);
  }

  /* Recursively resolve both loops in case either still has duplicates. */
  split_pinch_polygon(loop1.as_span(), r_simple_faces, vert_pos);
  split_pinch_polygon(loop2.as_span(), r_simple_faces, vert_pos);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Full Verification
 *
 * Geometric predicates and graph planarity checkers for extensive validation.
 * \{ */

static double orient2d(const double2 &a, const double2 &b, const double2 &c)
{
  return math::cross(b - a, c - a);
}

static bool point_on_segment_interior(const double2 &p,
                                      const double2 &a,
                                      const double2 &b,
                                      const double tol = 1e-6)
{
  const double len_sq = math::distance_squared(a, b);
  if (len_sq < tol * tol) {
    return false;
  }
  const double fac = math::line_point_factor(p, a, b);
  const double tol_fac = tol / std::sqrt(len_sq);
  if (fac <= tol_fac || fac >= 1.0 - tol_fac) {
    return false;
  }
  return math::dist_squared_to_line_segment(p, a, b) <= tol * tol;
}

/**
 * Returns true if segment (a, b) and segment (c, d) intersect improperly.
 *
 * Geometric Cases:
 * ================
 * ```
 *  Case 1: Strict Interior Crossing (X-crossing)
 *         a           d
 *          \         /
 *           \   *   /
 *            \ / \ /
 *             X   X
 *            / \ / \
 *           c   *   b
 *
 *  Case 2: Endpoint on Segment Interior (T-junction)
 *         a ========= c ========= b
 *                     |
 *                     |
 *                     d
 *
 *  Case 3: Collinear Overlap
 *         a ========= c ========= b ========= d
 *                     [=========]  positive overlap length
 * ```
 */
static bool segments_intersect_improper(const double2 &a,
                                        const double2 &b,
                                        const double2 &c,
                                        const double2 &d,
                                        const double tol = 1e-6)
{
  /* Fast bounding-box rejection. */
  if (std::max(a.x, b.x) < std::min(c.x, d.x) - tol ||
      std::max(c.x, d.x) < std::min(a.x, b.x) - tol ||
      std::max(a.y, b.y) < std::min(c.y, d.y) - tol ||
      std::max(c.y, d.y) < std::min(a.y, b.y) - tol)
  {
    return false;
  }

  const double d1 = orient2d(c, d, a);
  const double d2 = orient2d(c, d, b);
  const double d3 = orient2d(a, b, c);
  const double d4 = orient2d(a, b, d);

  /* Strict crossing: endpoints lie on strictly opposite sides of each other. */
  if (((d1 > tol && d2 < -tol) || (d1 < -tol && d2 > tol)) &&
      ((d3 > tol && d4 < -tol) || (d3 < -tol && d4 > tol)))
  {
    return true;
  }

  /* Endpoint of one segment lying strictly inside the other segment. */
  if (point_on_segment_interior(a, c, d, tol) || point_on_segment_interior(b, c, d, tol) ||
      point_on_segment_interior(c, a, b, tol) || point_on_segment_interior(d, a, b, tol))
  {
    return true;
  }

  /* Collinear overlap check if endpoints match. */
  const bool a_c = math::distance(a, c) < tol;
  const bool a_d = math::distance(a, d) < tol;
  const bool b_c = math::distance(b, c) < tol;
  const bool b_d = math::distance(b, d) < tol;
  /* If segments share both endpoints (same segment in either direction), they are identical. */
  if ((a_c && b_d) || (a_d && b_c)) {
    return false;
  }

  if (a_c) {
    const double2 v1 = b - a;
    const double2 v2 = d - c;
    if (math::length_squared(v1) > tol && math::length_squared(v2) > tol) {
      if (math::dot(v1, v2) / (math::length(v1) * math::length(v2)) > 1.0 - tol) {
        return true;
      }
    }
  }
  if (a_d) {
    const double2 v1 = b - a;
    const double2 v2 = c - d;
    if (math::length_squared(v1) > tol && math::length_squared(v2) > tol) {
      if (math::dot(v1, v2) / (math::length(v1) * math::length(v2)) > 1.0 - tol) {
        return true;
      }
    }
  }
  if (b_c) {
    const double2 v1 = a - b;
    const double2 v2 = d - c;
    if (math::length_squared(v1) > tol && math::length_squared(v2) > tol) {
      if (math::dot(v1, v2) / (math::length(v1) * math::length(v2)) > 1.0 - tol) {
        return true;
      }
    }
  }
  if (b_d) {
    const double2 v1 = a - b;
    const double2 v2 = c - d;
    if (math::length_squared(v1) > tol && math::length_squared(v2) > tol) {
      if (math::dot(v1, v2) / (math::length(v1) * math::length(v2)) > 1.0 - tol) {
        return true;
      }
    }
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Full Verification Entry Point
 * \{ */

static bool verify_straight_skeleton(const StraightSkeletonInput &input,
                                     const StraightSkeletonOutput &output,
                                     std::string &r_error)
{
  PRF_scope(ProfileCategory::Core);

  std::ostringstream err;

  /* Vertex Validity: no NaN, Inf, or negative times. */
  for (const int i : output.vert.index_range()) {
    const double2 p = output.vert[i];
    if (std::isnan(p.x) || std::isnan(p.y) || std::isinf(p.x) || std::isinf(p.y)) {
      err << "Invalid vertex coordinates at v" << i << ": (" << p.x << ", " << p.y << ")\n";
      r_error = err.str();
      return false;
    }
    if (i < output.vert_time.size() && output.vert_time[i] < -1e-7) {
      err << "Negative vertex timestamp at v" << i << ": " << output.vert_time[i] << "\n";
      r_error = err.str();
      return false;
    }
  }

  /* Face Offsets and Simplicity. */
  const OffsetIndices<int> face_offsets = output.face_offsets();
  const Span<int> fvi = output.face_vert_indices;
  double total_face_area = 0.0;

  for (const int fi : face_offsets.index_range()) {
    const IndexRange r = face_offsets[fi];
    const int n = int(r.size());
    if (n < 3) {
      err << "Face f" << fi << " has fewer than 3 vertices (" << n << ")\n";
      r_error = err.str();
      return false;
    }

    /* Compute signed area. */
    double area = 0.0;
    for (const int i : IndexRange(n)) {
      const double2 p1 = output.vert[fvi[r[i]]];
      const double2 p2 = output.vert[fvi[r[(i + 1) % n]]];
      area += math::cross(p1, p2);
    }
    area *= 0.5;

    if (area <= 1e-8) {
      err << "Face f" << fi << " has non-positive area: " << area << "\n";
      r_error = err.str();
      return false;
    }
    total_face_area += area;

    /* Self-intersection check on face boundary. */
    for (int i = 0; i < n; i++) {
      const double2 a = output.vert[fvi[r[i]]];
      const double2 b = output.vert[fvi[r[(i + 1) % n]]];
      for (int j = i + 2; j < n; j++) {
        if (i == 0 && j == n - 1) {
          continue;
        }
        const double2 c = output.vert[fvi[r[j]]];
        const double2 d = output.vert[fvi[r[(j + 1) % n]]];
        if (segments_intersect_improper(a, b, c, d)) {
          err << "Face f" << fi << " boundary self-intersects between edge (" << fvi[r[i]] << ","
              << fvi[r[(i + 1) % n]] << ") and (" << fvi[r[j]] << "," << fvi[r[(j + 1) % n]]
              << ")\n";
          err << "  e1: (" << a.x << "," << a.y << ") to (" << b.x << "," << b.y << ")\n";
          err << "  e2: (" << c.x << "," << c.y << ") to (" << d.x << "," << d.y << ")\n";
          r_error = err.str();
          return false;
        }
      }
    }
  }

  /* Skeleton Graph Planarity: test all edge pairs for improper intersection. */
  const int edge_count = int(output.edge.size());
  for (int i = 0; i < edge_count; i++) {
    const int2 e1 = output.edge[i];
    const double2 a = output.vert[e1[0]];
    const double2 b = output.vert[e1[1]];
    if (math::length_squared(b - a) < 1e-12) {
      continue;
    }

    for (int j = i + 1; j < edge_count; j++) {
      const int2 e2 = output.edge[j];
      const double2 c = output.vert[e2[0]];
      const double2 d = output.vert[e2[1]];
      if (math::length_squared(d - c) < 1e-12) {
        continue;
      }

      if (segments_intersect_improper(a, b, c, d)) {
        err << "Graph Planarity Violation: edge " << i << " (v" << e1[0] << "-v" << e1[1]
            << ") improperly intersects edge " << j << " (v" << e2[0] << "-v" << e2[1] << ")\n";
        err << "  e1: (" << a.x << "," << a.y << ") to (" << b.x << "," << b.y << ")\n";
        err << "  e2: (" << c.x << "," << c.y << ") to (" << d.x << "," << d.y << ")\n";
        r_error = err.str();
        return false;
      }
    }
  }

  /* Domain Area Partition (for full skeleton with segments = 1 and infinite offset). */
  if (input.segments <= 1 && std::isinf(input.offset) &&
      input.direction == PropagationDirection::Inside)
  {
    double domain_area = 0.0;
    if (!input.face_offsets.is_empty()) {
      Vector<AnalyzedFace> faces;
      for (const int f_i : input.face_offsets.index_range()) {
        const IndexRange r = input.face_offsets[f_i];
        if (r.size() < 3) {
          continue;
        }
        AnalyzedFace f;
        f.orig_face_idx = f_i;
        for (const int idx : r) {
          const int v = input.face_vert_indices[idx];
          f.vert_indices.append(v);
          f.coords.append(input.vert[v]);
        }
        f.signed_area = math::area_poly_signed(f.coords.as_span());
        if (std::abs(f.signed_area) < 1e-10) {
          continue;
        }
        f.min_co = f.coords[0];
        f.max_co = f.coords[0];
        for (const double2 &p : f.coords) {
          f.min_co = math::min(f.min_co, p);
          f.max_co = math::max(f.max_co, p);
        }
        f.interior_pt = find_polygon_interior_point(f.coords.as_span(), f.signed_area);
        faces.append(std::move(f));
      }
      build_containment_tree(faces.as_mutable_span());
      for (const AnalyzedFace &f : faces) {
        if (!f.is_hole) {
          domain_area += std::abs(f.signed_area);
        }
        else {
          domain_area -= std::abs(f.signed_area);
        }
      }
    }

    if (std::abs(domain_area - total_face_area) > std::max(1e-2, 1e-4 * domain_area)) {
      err << "Area partition discrepancy: domain area = " << domain_area
          << ", total face area = " << total_face_area << "\n";
      r_error = err.str();
      return false;
    }
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Simulation Engine
 * \{ */

class SimulationEngine {
 private:
  struct EventCluster {
    double2 p_star{0.0, 0.0};
    int s_star{-1};
    Vector<int> colliding_verts;
    Vector<int> collapsing_edges;
    Vector<int> split_edges;
  };

  struct VertPair {
    int u;
    int w;
  };

  struct SplitInfo {
    int reflex_w;
    int edge_u;
    double2 pos;
  };

  struct Region {
    Vector<int> outer;
    Vector<Vector<int>> holes;
  };

  struct CandidateChord {
    int i_outer;
    int j_hole;
    double dist;
  };

  const StraightSkeletonInput &input_;
  double target_offset_{std::numeric_limits<double>::infinity()};
  int segments_num_{1};
  bool dry_run_{false};

  WavefrontNodes wf_nodes_;
  WavefrontContours wf_contours_;
  SkeletonGraph skel_graph_;
  OutputFaces out_faces_;

  double current_time_{0.0};
  double max_collapse_time_{0.0};
  int current_band_{0};
  int events_processed_count_{0};

  SpatialGrid2D grid_;
  SpatialGrid2D vert_grid_;
  /** Per-node timestamp tags for O(1) deduplication during spatial grid queries. */
  Vector<int> query_tags_;
  int current_query_id_{0};

  Array<double2> orig_edge_normal_;
  Array<bool> orig_edge_has_antiparallel_;
  bool has_any_antiparallel_edges_{false};
  Vector<bool> is_cap_node_;
  Vector<double2> cap_normal_;
  Vector<AnalyzedFace> analyzed_faces_;

  void emit_face(Vector<int> face_verts, int orig_face, int orig_edge);

  void init_state();
  void add_face_contour(Span<int> face_verts, bool is_hole, int orig_face_idx);
  void add_wire_edge_graph(Span<int2> wire_edges, int orig_face_idx);
  void init_pslg_state();
  void init_antiparallel_bins();

  void update_node_velocity(int i);
  void deactivate_node(int i);

  bool has_active_contours() const;
  bool has_active_outer_contours() const;

  double compute_adaptive_dt() const;
  void advance_positions(double dt);
  bool is_split_visible(const int w, const int u, const double2 &p_w) const;

  double find_earliest_reflex_event_brute_force(const double dt,
                                                const double eps,
                                                const Span<int> reflex_nodes);
  double query_reflex_events_grid(const double eps,
                                  const Span<int> reflex_nodes,
                                  const double tau_horizon,
                                  const double safety_tol);
  double find_earliest_event(double dt, double eps);

  bool is_reflex(int w) const;
  int get_or_create_skel_node(const double2 &pos, double tol);

  bool try_resolve_opposing_pair(const int e1,
                                 const int e2,
                                 const double cluster_tol,
                                 Set<std::pair<int, int>> &processed_pairs);
  bool find_and_resolve_opposing_brute_force(const double cluster_tol,
                                             Set<std::pair<int, int>> &processed_pairs);
  bool find_and_resolve_opposing_grid(const double cluster_tol,
                                      const double2 &bbox_min,
                                      const double2 &bbox_max,
                                      const int active_edges_num,
                                      Set<std::pair<int, int>> &processed_pairs);
  void resolve_opposing_overlapping_edges(const double cluster_tol);

  bool annihilate_collapsing_contours(const Span<bool> is_edge_collapsing,
                                      const double cluster_tol);
  void collect_simultaneous_events(const Vector<int> &collapsing_edges,
                                   const double cluster_tol,
                                   Vector<VertPair> &r_vert_collisions,
                                   Vector<SplitInfo> &r_splits);
  void group_events_into_clusters(const double cluster_tol,
                                  const Vector<int> &collapsing_edges,
                                  const Vector<VertPair> &vert_collisions,
                                  const Vector<SplitInfo> &splits,
                                  Vector<EventCluster> &r_clusters);
  void process_event_clusters(Vector<EventCluster> &clusters, const double cluster_tol);
  void resolve_events_at_current_time(double eps);
  void resolve_cluster(const EventCluster &cluster);

  void rebuild_active_contours();
  void emit_segment_boundary_faces();

  void extract_and_classify_loops(Vector<Vector<int>> &r_outer_loops,
                                  Vector<Vector<int>> &r_hole_loops);
  void associate_holes_with_regions(const Vector<Vector<int>> &outer_loops,
                                    Vector<Vector<int>> &hole_loops,
                                    Vector<Region> &r_regions);
  void find_valid_bridge_chords(const Span<double2> outer_coords,
                                const Span<double2> hole_coords,
                                const Span<Vector<double2>> other_holes_coords,
                                Vector<CandidateChord> &r_valid_chords);
  void find_best_opposing_chord_pair(const Vector<CandidateChord> &valid_chords,
                                     const Span<double2> outer_coords,
                                     const Span<double2> hole_coords,
                                     const double2 hole_centroid,
                                     const double max_extent,
                                     int &r_best_idx1,
                                     int &r_best_idx2);
  void bridge_hole_in_region(Region current, Vector<Region> &worklist);
  void emit_inner_polygon_faces();

 public:
  SimulationEngine(const StraightSkeletonInput &input,
                   double target_offset,
                   int segments,
                   bool dry_run)
      : input_(input), target_offset_(target_offset), segments_num_(segments), dry_run_(dry_run)
  {
  }

  double run();
  StraightSkeletonOutput build_output();
};

inline void SimulationEngine::emit_face(Vector<int> face_verts, int orig_face, int orig_edge)

{
  if (face_verts.size() >= 3) {
    out_faces_.faces.append(std::move(face_verts));
    out_faces_.face_orig.append(orig_face);
    out_faces_.face_band.append(current_band_);
    out_faces_.face_orig_edge.append(orig_edge);
  }
}

/**
 * Run wavefront simulation until all contours annihilate or target offset is reached.
 *
 * Geometric Subdivision into Offset Bands (segments = 3):
 * =======================================================
 * ```
 *  t = target_offset =====+=============+=============+===== Inner Polygon Face(s)
 *                         |   Band 2    |   Band 2    |
 *  t = 2/3 offset   ------+-------------+-------------+------
 *                         |   Band 1    |   Band 1    |      Side-strip quads
 *  t = 1/3 offset   ------+-------------+-------------+------
 *                         |   Band 0    |   Band 0    |
 *  t = 0 (boundary) =====*=============*=============*=====
 * ```
 */
inline double SimulationEngine::run()

{
  PRF_scope(ProfileCategory::Core);

  this->init_state();

  const double eps = (input_.epsilon > 0.0) ? input_.epsilon : 1e-8;
  const int max_steps = std::max(100000, 20 * int(wf_nodes_.pos.size()));
  int step_count = 0;
  while (this->has_active_contours() && step_count < max_steps) {
    step_count++;

    /* Determine target timestamp for next segment threshold. */
    double next_seg_time = std::numeric_limits<double>::infinity();
    if (!std::isinf(target_offset_) && segments_num_ > 1) {
      const double band_width = target_offset_ / double(segments_num_);
      next_seg_time = double(current_band_ + 1) * band_width;
    }
    else if (!std::isinf(target_offset_) && segments_num_ == 1) {
      next_seg_time = target_offset_;
    }

    /* Check if target offset or segment reached. */
    if (current_time_ >= target_offset_ - 1e-9) {
      break;
    }

    /* If in an inward simulation all outer contours have collapsed, the entire solid interior
     * domain has been completely roofed. Terminate the simulation. */
    if (input_.direction == PropagationDirection::Inside && !this->has_active_outer_contours()) {
      break;
    }

    /* Determine lookahead horizon up to next segment threshold or target offset. */
    double max_lookahead = std::numeric_limits<double>::infinity();
    if (!std::isinf(next_seg_time)) {
      max_lookahead = std::min(max_lookahead, next_seg_time - current_time_);
    }
    if (!std::isinf(target_offset_)) {
      max_lookahead = std::min(max_lookahead, target_offset_ - current_time_);
    }

    /* Detect events occurring in [current_time_, current_time_ + max_lookahead]. */
    const double event_t = this->find_earliest_event(max_lookahead, eps);

    if (!std::isinf(event_t) && event_t < next_seg_time - 1e-9 && event_t < target_offset_ - 1e-9)
    {
      /* Advance simulation to event timestamp. */
      this->advance_positions(event_t - current_time_);
      current_time_ = event_t;
      max_collapse_time_ = std::max(max_collapse_time_, current_time_);

      /* Resolve all concurrent events at current_time_. */
      this->resolve_events_at_current_time(eps);
    }
    else if (!std::isinf(next_seg_time) && next_seg_time <= target_offset_ + 1e-9) {
      /* Advance to segment threshold. */
      this->advance_positions(next_seg_time - current_time_);
      current_time_ = next_seg_time;
      max_collapse_time_ = std::max(max_collapse_time_, current_time_);

      /* Resolve any events that coincide with segment threshold. */
      if (!std::isinf(event_t) && std::abs(event_t - next_seg_time) < 1e-9) {
        this->resolve_events_at_current_time(eps);
      }

      if (!dry_run_) {
        this->emit_segment_boundary_faces();
      }
      current_band_++;
    }
    else {
      /* When no further events are detected before infinity in an unconstrained simulation,
       * remaining contours are terminal collapses. Collapse each to its centroid. */
      const double cluster_tol = std::max(eps, DEFAULT_CLUSTER_TOL);
      bool any_terminal_collapsed = false;
      for (const int cid : wf_contours_.is_active.index_range()) {
        if (!wf_contours_.is_active[cid]) {
          continue;
        }
        const int head = wf_contours_.head_node[cid];
        if (head < 0 || !wf_nodes_.is_active[head]) {
          continue;
        }
        Vector<int> contour_nodes;
        int curr = head;
        do {
          contour_nodes.append(curr);
          curr = wf_nodes_.next_node[curr];
        } while (curr != head && curr >= 0 && contour_nodes.size() < wf_nodes_.pos.size());

        if (!contour_nodes.is_empty() && (std::isinf(target_offset_) || contour_nodes.size() <= 3))
        {
          double2 p_sum(0.0, 0.0);
          for (const int node : contour_nodes) {
            p_sum += wf_nodes_.pos[node];
          }
          const double2 p_star = p_sum / double(contour_nodes.size());
          const int s_star = this->get_or_create_skel_node(p_star, cluster_tol);

          for (const int node : contour_nodes) {
            if (!dry_run_) {
              Vector<int> face_verts;
              face_verts.append(wf_nodes_.left_trail[node][0]);
              for (const int r : wf_nodes_.right_trail[node]) {
                if (face_verts.last() != r) {
                  face_verts.append(r);
                }
              }
              if (face_verts.last() != s_star) {
                face_verts.append(s_star);
              }
              for (int k = int(wf_nodes_.left_trail[node].size()) - 1; k >= 1; k--) {
                const int l = wf_nodes_.left_trail[node][k];
                if (face_verts.last() != l && l != face_verts[0]) {
                  face_verts.append(l);
                }
              }
              this->emit_face(
                  std::move(face_verts), wf_nodes_.orig_face[node], wf_nodes_.orig_edge[node]);
            }

            skel_graph_.edges_node1.append(wf_nodes_.skel_node[node]);
            skel_graph_.edges_node2.append(s_star);
            skel_graph_.edges_orig.append(-1);
            skel_graph_.edges_face.append(wf_nodes_.orig_face[node]);
            skel_graph_.edges_time_start.append(wf_nodes_.time_birth[node]);
            skel_graph_.edges_time_end.append(current_time_);
            skel_graph_.edges_is_boundary.append(false);

            this->deactivate_node(node);
          }
          wf_contours_.is_active[cid] = false;
          any_terminal_collapsed = true;
        }
      }
      if (any_terminal_collapsed) {
        this->rebuild_active_contours();
      }
      break;
    }
  }

  /* If finished with finite target offset and surviving contours remain, emit inner polygon.
   * Note: Inner polygon cap is only emitted for positive offsets of closed polygons. */
  if (!dry_run_ && !std::isinf(input_.offset) && input_.offset > 0.0 &&
      this->has_active_contours() && current_time_ >= target_offset_ - 1e-7)
  {
    this->emit_inner_polygon_faces();
  }

  return max_collapse_time_;
}

inline StraightSkeletonOutput SimulationEngine::build_output()

{
  PRF_scope(ProfileCategory::Core);

  StraightSkeletonOutput output;
  if (dry_run_) {
    return output;
  }

  const int input_verts_num = int(input_.vert.size());
  const int total_skel_nodes = int(skel_graph_.nodes_pos.size());

  Array<int> old_to_new(total_skel_nodes, -1);
  Vector<double2> compact_vert;
  Vector<double> compact_vert_time;
  Vector<int> compact_vert_orig;

  compact_vert.reserve(total_skel_nodes);
  compact_vert_time.reserve(total_skel_nodes);
  compact_vert_orig.reserve(total_skel_nodes);

  /* Preserve input vertices at 0 .. input_verts_num - 1. */
  for (const int i : IndexRange(input_verts_num)) {
    old_to_new[i] = i;
    compact_vert.append(skel_graph_.nodes_pos[i]);
    compact_vert_time.append(skel_graph_.nodes_time[i]);
    compact_vert_orig.append(skel_graph_.nodes_orig[i]);
  }

  /* Ensure all initial faces are clean and CCW. Only reverse if area is significantly
   * negative, because near-zero slivers (|area| <= 1e-6) have noisy signs and reversing them
   * breaks internal seam matching with adjacent generator sub-faces. */
  for (const int fi : out_faces_.faces.index_range()) {
    Vector<int> &f = out_faces_.faces[fi];
    f = clean_polygon_spikes_and_duplicates(f.as_span(), skel_graph_.nodes_pos.as_span());
    if (f.size() >= 3 && polygon_signed_area(f.as_span(), skel_graph_.nodes_pos.as_span()) < -1e-6)
    {
      std::reverse(f.begin(), f.end());
    }
  }

  /* Merge sub-faces of the same generator edge and band that share an internal seam.
   *
   * Geometric Situation:
   * ====================
   * Before Merge (sub-polygons f1 and f2 meeting along directed seam a -> b in f1 and b -> a in
   * f2):
   * ```
   *                    Saddle / Split Node
   *                           (b)
   *                          / | \
   *                         /  |  \
   *                        /   |   \
   *                       / f1 | f2 \
   *                      /     |     \
   *                     /      |      \
   *                   (v)      |      (w)
   *                    \       |       /
   *                     \      |      /
   *                      \     v     /
   *                      (u)---*---(z)
   *                           (a)
   *                Advancing Generator Edge e
   * ```
   *
   * After Merge along seam (a, b):
   * Internal chord removed; unified face (u, a, z, w, b, v) with true V-notch at saddle node b.
   */

  const int num_input_faces = int(out_faces_.faces.size());
  Array<OutputFaces> per_input_face(num_input_faces);

  threading::parallel_for(IndexRange(num_input_faces), 64, [&](const IndexRange range) {
    for (const int fi : range) {
      if (out_faces_.faces[fi].size() < 3) {
        continue;
      }
      Vector<Vector<int>> simple_faces;
      split_pinch_polygon(
          out_faces_.faces[fi].as_span(), simple_faces, skel_graph_.nodes_pos.as_span());
      for (Vector<int> &clean_f : simple_faces) {
        clean_f = clean_polygon_spikes_and_duplicates(clean_f.as_span(),
                                                      skel_graph_.nodes_pos.as_span());
        if (clean_f.size() >= 3) {
          if (polygon_signed_area(clean_f.as_span(), skel_graph_.nodes_pos.as_span()) < -1e-6) {
            std::reverse(clean_f.begin(), clean_f.end());
          }
          per_input_face[fi].faces.append(std::move(clean_f));
          per_input_face[fi].face_orig.append(out_faces_.face_orig[fi]);
          per_input_face[fi].face_band.append(out_faces_.face_band[fi]);
          per_input_face[fi].face_orig_edge.append(out_faces_.face_orig_edge[fi]);
        }
      }
    }
  });

  OutputFaces unpinched_faces;
  int total_unpinched = 0;
  for (const OutputFaces &of : per_input_face) {
    total_unpinched += int(of.faces.size());
  }
  unpinched_faces.faces.reserve(total_unpinched);
  unpinched_faces.face_orig.reserve(total_unpinched);
  unpinched_faces.face_band.reserve(total_unpinched);
  unpinched_faces.face_orig_edge.reserve(total_unpinched);

  for (OutputFaces &of : per_input_face) {
    for (int i = 0; i < of.faces.size(); i++) {
      unpinched_faces.faces.append(std::move(of.faces[i]));
      unpinched_faces.face_orig.append(of.face_orig[i]);
      unpinched_faces.face_band.append(of.face_band[i]);
      unpinched_faces.face_orig_edge.append(of.face_orig_edge[i]);
    }
  }
  out_faces_ = std::move(unpinched_faces);

  /* Group face indices by (orig, orig_edge, band). */
  Map<int64_t, Vector<int>> groups;
  for (const int i : out_faces_.faces.index_range()) {
    const int orig = out_faces_.face_orig[i];
    const int orig_edge = out_faces_.face_orig_edge[i];
    if (orig_edge < 0 || out_faces_.faces[i].size() < 3) {
      continue;
    }
    const int band = out_faces_.face_band[i];
    const int64_t key = (int64_t(orig) << 40) | (int64_t(orig_edge) << 16) | int64_t(band);
    groups.lookup_or_add_default(key).append(i);
  }

  /* Collect multi-face groups for parallel processing. */
  Vector<Vector<int>> multi_face_groups;
  for (auto item : groups.items()) {
    if (item.value.size() > 1) {
      multi_face_groups.append(std::move(item.value));
    }
  }

  threading::parallel_for(multi_face_groups.index_range(), 1, [&](const IndexRange range) {
    for (const int g_idx : range) {
      Vector<int> &f_indices = multi_face_groups[g_idx];
      bool merged_in_group = true;
      while (merged_in_group) {
        merged_in_group = false;

        struct EdgeRef {
          int face_idx;
          int edge_pos;
        };
        Map<int64_t, EdgeRef> edge_map;
        edge_map.reserve(f_indices.size() * 4);

        for (const int i : f_indices) {
          if (out_faces_.faces[i].size() < 3) {
            continue;
          }
          const Vector<int> &f1 = out_faces_.faces[i];
          const int n1 = int(f1.size());

          int match_i = -1;
          int match_j = -1;
          int j = -1;
          bool reverse_f2 = false;

          for (int e1 = 0; e1 < n1; e1++) {
            const int a = f1[e1];
            const int b = f1[(e1 + 1) % n1];
            const int u = std::min(a, b);
            const int v = std::max(a, b);
            const int64_t edge_key = (int64_t(u) << 32) | int64_t(v);

            if (const EdgeRef *ref = edge_map.lookup_ptr(edge_key)) {
              if (out_faces_.faces[ref->face_idx].size() >= 3) {
                j = ref->face_idx;
                match_i = e1;
                match_j = ref->edge_pos;
                const Vector<int> &f2 = out_faces_.faces[j];
                const int n2 = int(f2.size());
                reverse_f2 = (f2[match_j] == a && f2[(match_j + 1) % n2] == b);
                break;
              }
            }
            else {
              edge_map.add(edge_key, {i, e1});
            }
          }

          if (match_i != -1 && j != -1) {
            Vector<int> &f1_ref = out_faces_.faces[i];
            Vector<int> &f2_ref = out_faces_.faces[j];
            const int n1_val = int(f1_ref.size());
            int n2_val = int(f2_ref.size());

            if (reverse_f2) {
              std::reverse(f2_ref.begin(), f2_ref.end());
              const int a = f1_ref[match_i];
              const int b = f1_ref[(match_i + 1) % n1_val];
              for (int e2 = 0; e2 < n2_val; e2++) {
                if (f2_ref[e2] == b && f2_ref[(e2 + 1) % n2_val] == a) {
                  match_j = e2;
                  break;
                }
              }
            }

            /* Merge f1 and f2 along shared edge. */
            Vector<int> merged;
            merged.reserve(n1_val + n2_val - 2);
            for (int k = 1; k <= n1_val; k++) {
              merged.append(f1_ref[(match_i + k) % n1_val]);
            }
            for (int k = 2; k < n2_val; k++) {
              merged.append(f2_ref[(match_j + k) % n2_val]);
            }

            Vector<int> clean = clean_polygon_spikes_and_duplicates(
                merged.as_span(), skel_graph_.nodes_pos.as_span());

            if (clean.size() >= 3) {
              out_faces_.faces[i] = std::move(clean);
              out_faces_.faces[j].clear();
              merged_in_group = true;
              break;
            }
          }
        }
      }
    }
  });

  /* Compact out_faces_ by removing cleared faces. */
  int write_idx = 0;
  for (int i = 0; i < out_faces_.faces.size(); i++) {
    if (out_faces_.faces[i].size() >= 3) {
      if (write_idx != i) {
        out_faces_.faces[write_idx] = std::move(out_faces_.faces[i]);
        out_faces_.face_orig[write_idx] = out_faces_.face_orig[i];
        out_faces_.face_band[write_idx] = out_faces_.face_band[i];
        out_faces_.face_orig_edge[write_idx] = out_faces_.face_orig_edge[i];
      }
      write_idx++;
    }
  }
  out_faces_.faces.resize(write_idx);
  out_faces_.face_orig.resize(write_idx);
  out_faces_.face_band.resize(write_idx);
  out_faces_.face_orig_edge.resize(write_idx);

  /* Remap vertices referenced by output faces. */
  Vector<Vector<int>> final_faces;
  Vector<int> final_face_orig;
  Vector<int> final_face_band;
  Set<Vector<int>> seen_faces;

  for (const int fi : out_faces_.faces.index_range()) {
    if (out_faces_.faces[fi].size() < 3) {
      continue;
    }

    /* Clean spikes and split any pinch (figure-8) vertices into simple polygons. */
    Vector<Vector<int>> simple_faces;
    split_pinch_polygon(
        out_faces_.faces[fi].as_span(), simple_faces, skel_graph_.nodes_pos.as_span());

    for (Vector<int> &clean_f : simple_faces) {
      clean_f = clean_polygon_spikes_and_duplicates(clean_f.as_span(),
                                                    skel_graph_.nodes_pos.as_span());
      if (clean_f.size() < 3) {
        continue;
      }

      /* Ensure positive signed area (counter-clockwise) and skip degenerate zero-area faces. */
      const double a = polygon_signed_area(clean_f.as_span(), skel_graph_.nodes_pos.as_span());
      if (std::abs(a) <= 1e-8) {
        continue;
      }
      if (a < 0.0) {
        std::reverse(clean_f.begin(), clean_f.end());
      }

      for (int &v : clean_f) {
        if (old_to_new[v] == -1) {
          old_to_new[v] = int(compact_vert.size());
          compact_vert.append(skel_graph_.nodes_pos[v]);
          compact_vert_time.append(skel_graph_.nodes_time[v]);
          compact_vert_orig.append(skel_graph_.nodes_orig[v]);
        }
        v = old_to_new[v];
      }

      clean_f = clean_polygon_spikes_and_duplicates(clean_f.as_span(), compact_vert.as_span());
      if (clean_f.size() < 3) {
        continue;
      }

      /* Deduplicate identical faces (e.g. from opposing edge collapse). */
      const int m = int(clean_f.size());
      int min_idx = 0;
      for (int i = 1; i < m; i++) {
        if (clean_f[i] < clean_f[min_idx]) {
          min_idx = i;
        }
      }
      Vector<int> canonical_f;
      canonical_f.reserve(m);
      for (int i = 0; i < m; i++) {
        canonical_f.append(clean_f[(min_idx + i) % m]);
      }
      if (seen_faces.contains(canonical_f)) {
        continue;
      }
      seen_faces.add(canonical_f);

      final_faces.append(std::move(clean_f));
      final_face_orig.append(out_faces_.face_orig[fi]);
      final_face_band.append(out_faces_.face_band[fi]);
    }
  }

  const int total_faces = int(final_faces.size());
  Vector<int> face_indices;
  Vector<int> face_lens;
  for (const Vector<int> &f : final_faces) {
    face_lens.append(int(f.size()));
    face_indices.extend(f.as_span());
  }

  output.face_offsets_data.reinitialize(total_faces + 1);
  output.face_offsets_data[0] = 0;
  for (const int i : IndexRange(total_faces)) {
    output.face_offsets_data[i + 1] = output.face_offsets_data[i] + face_lens[i];
  }
  output.face_vert_indices = Array<int>(face_indices.as_span());
  output.face_orig = Array<int>(final_face_orig.as_span());
  output.face_band = Array<int>(final_face_band.as_span());

  output.vert = Array<double2>(compact_vert.as_span());
  output.vert_time = Array<double>(compact_vert_time.as_span());
  output.vert_orig = Array<int>(compact_vert_orig.as_span());

  /* Collect all unique face edges. */
  VectorSet<OrderedEdge> all_face_edges;
  for (const Vector<int> &f : final_faces) {
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

  output.hit_max_events = false;
  output.events_processed_num = events_processed_count_;

  if (input_.full_verification) {
    output.is_valid = verify_straight_skeleton(input_, output, output.error_message);
  }

  return output;
}

/**
 * Initialize wavefront contours and straight skeleton nodes from input geometry.
 * The orientation convention is that "inside" is to the left of the edges of a
 * polygon, so normal polygons go CCW and holes go CW.
 */
inline void SimulationEngine::init_state()

{
  PRF_scope(ProfileCategory::Core);

  const int input_verts_num = int(input_.vert.size());
  const int face_verts_num = int(input_.face_vert_indices.size());
  const int num_wire_edges = int(input_.edge.size());
  const int num_faces = int(input_.face_offsets.size());
  const bool is_both = (input_.direction == PropagationDirection::Both);
  const int dir_mult = is_both ? 2 : 1;
  const int num_segments = std::max(1, input_.segments);

  /* Initial wavefront nodes count across faces, wire edges, and bounding box. */
  const int init_wf_nodes = face_verts_num * dir_mult + num_wire_edges * 4 + 8;

  /* Estimate total skeleton nodes created by edge collapses, splits, and segment steps.
   * Standard straight skeleton of N vertices creates ~N internal nodes, plus intermediate
   * nodes generated at segment boundaries. */
  const int est_skel_nodes = (input_verts_num + init_wf_nodes * 2) * num_segments;

  skel_graph_.nodes_pos.reserve(est_skel_nodes);
  skel_graph_.nodes_time.reserve(est_skel_nodes);
  skel_graph_.nodes_orig.reserve(est_skel_nodes);

  /* Wavefront nodes can grow when split events create new contour vertices. */
  const int est_wf_nodes = init_wf_nodes * 2;
  wf_nodes_.pos.reserve(est_wf_nodes);
  wf_nodes_.vel.reserve(est_wf_nodes);
  wf_nodes_.prev_node.reserve(est_wf_nodes);
  wf_nodes_.next_node.reserve(est_wf_nodes);
  wf_nodes_.skel_node.reserve(est_wf_nodes);
  wf_nodes_.orig_edge.reserve(est_wf_nodes);
  wf_nodes_.orig_face.reserve(est_wf_nodes);
  wf_nodes_.contour_id.reserve(est_wf_nodes);
  wf_nodes_.is_active.reserve(est_wf_nodes);
  wf_nodes_.time_birth.reserve(est_wf_nodes);
  wf_nodes_.left_trail.reserve(est_wf_nodes);
  wf_nodes_.right_trail.reserve(est_wf_nodes);

  const int est_contours = (num_faces * dir_mult + num_wire_edges + 2) * 3;
  wf_contours_.head_node.reserve(est_contours);
  wf_contours_.is_hole.reserve(est_contours);
  wf_contours_.is_active.reserve(est_contours);
  wf_contours_.is_wire.reserve(est_contours);

  /* Trivalent skeleton graph has ~1.5x edges per skeleton node. */
  const int est_skel_edges = est_skel_nodes * 2;
  skel_graph_.edges_node1.reserve(est_skel_edges);
  skel_graph_.edges_node2.reserve(est_skel_edges);
  skel_graph_.edges_orig.reserve(est_skel_edges);
  skel_graph_.edges_face.reserve(est_skel_edges);
  skel_graph_.edges_time_start.reserve(est_skel_edges);
  skel_graph_.edges_time_end.reserve(est_skel_edges);
  skel_graph_.edges_is_boundary.reserve(est_skel_edges);

  /* Initialize Skeleton Nodes from input vertices. */
  for (const int i : IndexRange(input_verts_num)) {
    skel_graph_.nodes_pos.append(input_.vert[i]);
    skel_graph_.nodes_time.append(0.0);
    skel_graph_.nodes_orig.append(i);
  }

  this->init_pslg_state();
}

inline void SimulationEngine::add_face_contour(Span<int> face_verts,
                                               bool is_hole,
                                               int orig_face_idx)

{
  const int n = int(face_verts.size());
  if (n < 3) {
    return;
  }
  const int start_idx = int(wf_nodes_.pos.size());
  const int contour_id = int(wf_contours_.head_node.size());

  wf_contours_.head_node.append(start_idx);
  wf_contours_.is_hole.append(is_hole);
  wf_contours_.is_active.append(true);
  wf_contours_.is_wire.append(false);

  for (int i = 0; i < n; i++) {
    const int vi = face_verts[i];
    const int prev_idx = start_idx + ((i == 0) ? n - 1 : i - 1);
    const int next_idx = start_idx + ((i + 1 == n) ? 0 : i + 1);

    wf_nodes_.pos.append(skel_graph_.nodes_pos[vi]);
    wf_nodes_.vel.append(double2(0.0, 0.0));
    wf_nodes_.prev_node.append(prev_idx);
    wf_nodes_.next_node.append(next_idx);
    wf_nodes_.skel_node.append(vi);
    wf_nodes_.orig_edge.append(start_idx + i);
    wf_nodes_.orig_face.append(orig_face_idx);
    wf_nodes_.contour_id.append(contour_id);
    wf_nodes_.is_active.append(true);
    wf_nodes_.time_birth.append(0.0);

    Vector<int> lt;
    lt.append(vi);
    wf_nodes_.left_trail.append(std::move(lt));

    Vector<int> rt;
    rt.append(face_verts[(i + 1) % n]);
    wf_nodes_.right_trail.append(std::move(rt));
  }
}

inline void SimulationEngine::add_wire_edge_graph(Span<int2> wire_edges, int orig_face_idx)

{
  if (wire_edges.is_empty()) {
    return;
  }

  Map<int, Vector<int>> adj;
  Set<int64_t> unique_undirected;
  auto undir_key = [](int u, int v) -> int64_t {
    return (int64_t(math::min(u, v)) << 32) | int64_t(uint32_t(math::max(u, v)));
  };
  auto half_edge_key = [](int u, int v) -> int64_t {
    return (int64_t(u) << 32) | int64_t(uint32_t(v));
  };

  const int total_skel_nodes = int(skel_graph_.nodes_pos.size());
  for (const int2 &e : wire_edges) {
    const int u = e[0];
    const int v = e[1];
    if (u == v || u < 0 || v < 0 || u >= total_skel_nodes || v >= total_skel_nodes) {
      continue;
    }
    if (math::distance_squared(skel_graph_.nodes_pos[u], skel_graph_.nodes_pos[v]) < 1e-20) {
      continue;
    }
    if (!unique_undirected.add(undir_key(u, v))) {
      continue;
    }
    adj.lookup_or_add_default(u).append(v);
    adj.lookup_or_add_default(v).append(u);
  }

  if (adj.is_empty()) {
    return;
  }

  /* Sort outgoing neighbors counter-clockwise by polar angle. */
  for (auto item : adj.items()) {
    const int u = item.key;
    Vector<int> &nbrs = item.value;
    const double2 pu = skel_graph_.nodes_pos[u];
    std::stable_sort(nbrs.begin(), nbrs.end(), [&](const int a, const int b) -> bool {
      const double2 da = skel_graph_.nodes_pos[a] - pu;
      const double2 db = skel_graph_.nodes_pos[b] - pu;
      return std::atan2(da.y, da.x) < std::atan2(db.y, db.x);
    });
  }

  struct WfInitNode {
    int vert;
    bool is_cap{false};
    double2 cap_normal{0.0, 0.0};
  };

  Set<int64_t> visited_half_edges;
  for (const int64_t e_key : unique_undirected) {
    const int u0 = int(e_key >> 32);
    const int v0 = int(uint32_t(e_key & 0xffffffffLL));

    for (const auto [start_u, start_v] : {std::make_pair(u0, v0), std::make_pair(v0, u0)}) {
      if (visited_half_edges.contains(half_edge_key(start_u, start_v))) {
        continue;
      }

      Vector<std::pair<int, int>> loop_half_edges;
      int curr_u = start_u;
      int curr_v = start_v;
      while (!visited_half_edges.contains(half_edge_key(curr_u, curr_v))) {
        visited_half_edges.add(half_edge_key(curr_u, curr_v));
        loop_half_edges.append(std::make_pair(curr_u, curr_v));

        const Vector<int> *nbrs_ptr = adj.lookup_ptr(curr_v);
        if (!nbrs_ptr || nbrs_ptr->is_empty()) {
          break;
        }
        const Vector<int> &nbrs = *nbrs_ptr;
        const int deg = int(nbrs.size());
        int idx = -1;
        for (int k = 0; k < deg; k++) {
          if (nbrs[k] == curr_u) {
            idx = k;
            break;
          }
        }
        if (idx == -1) {
          break;
        }
        const int next_v = nbrs[(idx - 1 + deg) % deg];
        curr_u = curr_v;
        curr_v = next_v;
      }

      if (loop_half_edges.is_empty()) {
        continue;
      }

      Vector<WfInitNode> init_nodes;
      for (const auto &[u, v] : loop_half_edges) {
        const Vector<int> *deg_ptr = adj.lookup_ptr(v);
        const int deg_v = deg_ptr ? int(deg_ptr->size()) : 1;
        if (deg_v == 1) {
          /* Leaf: emit cap node, then leaf turnaround node. */
          WfInitNode cap_node;
          cap_node.vert = v;
          cap_node.is_cap = true;
          const double2 d = skel_graph_.nodes_pos[v] - skel_graph_.nodes_pos[u];
          const double len = math::length(d);
          cap_node.cap_normal = (len > 1e-12) ? (d / len) : double2(1.0, 0.0);
          init_nodes.append(cap_node);

          WfInitNode out_node;
          out_node.vert = v;
          out_node.is_cap = false;
          init_nodes.append(out_node);
        }
        else {
          /* Corner / junction: single node between sectors. */
          WfInitNode corner_node;
          corner_node.vert = v;
          corner_node.is_cap = false;
          init_nodes.append(corner_node);
        }
      }

      const int M = int(init_nodes.size());
      if (M < 3) {
        continue;
      }

      const int start_idx = int(wf_nodes_.pos.size());
      const int contour_id = int(wf_contours_.head_node.size());

      wf_contours_.head_node.append(start_idx);
      wf_contours_.is_hole.append(false);
      wf_contours_.is_active.append(true);
      wf_contours_.is_wire.append(true);

      for (int k = 0; k < M; k++) {
        const WfInitNode &in = init_nodes[k];
        const int prev_node = start_idx + ((k == 0) ? M - 1 : k - 1);
        const int next_node = start_idx + ((k + 1 == M) ? 0 : k + 1);

        wf_nodes_.pos.append(skel_graph_.nodes_pos[in.vert]);
        wf_nodes_.vel.append(double2(0.0, 0.0));
        wf_nodes_.prev_node.append(prev_node);
        wf_nodes_.next_node.append(next_node);
        wf_nodes_.skel_node.append(in.vert);
        wf_nodes_.orig_edge.append(start_idx + k);
        wf_nodes_.orig_face.append(orig_face_idx);
        wf_nodes_.contour_id.append(contour_id);
        wf_nodes_.is_active.append(true);
        wf_nodes_.time_birth.append(0.0);

        Vector<int> lt;
        lt.append(in.vert);
        wf_nodes_.left_trail.append(std::move(lt));

        Vector<int> rt;
        rt.append(init_nodes[(k + 1) % M].vert);
        wf_nodes_.right_trail.append(std::move(rt));
      }

      is_cap_node_.resize(wf_nodes_.pos.size(), false);
      cap_normal_.resize(wf_nodes_.pos.size(), double2(0.0, 0.0));

      for (int k = 0; k < M; k++) {
        if (init_nodes[k].is_cap) {
          is_cap_node_[start_idx + k] = true;
          cap_normal_[start_idx + k] = init_nodes[k].cap_normal;
        }
      }
    }
  }
}

inline void SimulationEngine::init_pslg_state()

{
  Vector<AnalyzedFace> faces;
  if (input_.face_offsets.size() > 0) {
    faces.reserve(input_.face_offsets.size() + 1);
    for (const int f_i : input_.face_offsets.index_range()) {
      const IndexRange r = input_.face_offsets[f_i];
      if (r.size() < 3) {
        continue;
      }
      AnalyzedFace f;
      f.orig_face_idx = f_i;
      f.vert_indices.reserve(r.size());
      f.coords.reserve(r.size());
      for (const int idx : r) {
        f.vert_indices.append(input_.face_vert_indices[idx]);
        f.coords.append(skel_graph_.nodes_pos[input_.face_vert_indices[idx]]);
      }
      f.signed_area = math::area_poly_signed(f.coords.as_span());
      if (std::abs(f.signed_area) < 1e-10) {
        continue;
      }
      f.min_co = f.coords[0];
      f.max_co = f.coords[0];
      for (const double2 &p : f.coords) {
        f.min_co = math::min(f.min_co, p);
        f.max_co = math::max(f.max_co, p);
      }
      f.interior_pt = find_polygon_interior_point(f.coords.as_span(), f.signed_area);
      faces.append(std::move(f));
    }
  }

  build_containment_tree(faces.as_mutable_span());

  Vector<int2> wire_edges;
  if (!input_.edge.is_empty()) {
    wire_edges.reserve(input_.edge.size());
    for (const int2 &e : input_.edge) {
      wire_edges.append(e);
    }
  }

  const bool is_outside = (input_.direction == PropagationDirection::Outside);
  const bool is_both = (input_.direction == PropagationDirection::Both);
  bool has_uncontained_wire = false;
  for (const int2 &e : wire_edges) {
    const double2 mid = 0.5 * (skel_graph_.nodes_pos[e[0]] + skel_graph_.nodes_pos[e[1]]);
    int containing_f = -1;
    double min_a = std::numeric_limits<double>::infinity();
    for (const int fi : faces.index_range()) {
      if (isect_point_polygon_v2(mid, faces[fi].coords.as_span())) {
        const double a = std::abs(faces[fi].signed_area);
        if (a < min_a) {
          min_a = a;
          containing_f = fi;
        }
      }
    }
    if (containing_f == -1 || faces[containing_f].is_hole) {
      has_uncontained_wire = true;
      break;
    }
  }

  const bool needs_bbox = is_outside || is_both || has_uncontained_wire ||
                          (faces.is_empty() && !wire_edges.is_empty());
  if (needs_bbox && !skel_graph_.nodes_pos.is_empty()) {
    double2 g_min = skel_graph_.nodes_pos[0];
    double2 g_max = skel_graph_.nodes_pos[0];
    for (const double2 &p : skel_graph_.nodes_pos) {
      g_min = math::min(g_min, p);
      g_max = math::max(g_max, p);
    }
    const double diag = math::max(g_max.x - g_min.x, g_max.y - g_min.y);
    const double pct = (input_.bbox_expansion_pct > 0.0) ? input_.bbox_expansion_pct : 10.0;
    double margin = math::max(1.0, diag * (pct / 100.0));
    if (!std::isinf(target_offset_) && target_offset_ > 0.0) {
      margin = math::max(margin, target_offset_ * 1.5);
    }

    const int b0 = int(skel_graph_.nodes_pos.size());
    skel_graph_.nodes_pos.append(double2(g_min.x - margin, g_min.y - margin));
    skel_graph_.nodes_time.append(0.0);
    skel_graph_.nodes_orig.append(-1);

    const int b1 = int(skel_graph_.nodes_pos.size());
    skel_graph_.nodes_pos.append(double2(g_max.x + margin, g_min.y - margin));
    skel_graph_.nodes_time.append(0.0);
    skel_graph_.nodes_orig.append(-1);

    const int b2 = int(skel_graph_.nodes_pos.size());
    skel_graph_.nodes_pos.append(double2(g_max.x + margin, g_max.y + margin));
    skel_graph_.nodes_time.append(0.0);
    skel_graph_.nodes_orig.append(-1);

    const int b3 = int(skel_graph_.nodes_pos.size());
    skel_graph_.nodes_pos.append(double2(g_min.x - margin, g_max.y + margin));
    skel_graph_.nodes_time.append(0.0);
    skel_graph_.nodes_orig.append(-1);

    if (is_outside || is_both || has_uncontained_wire) {
      AnalyzedFace bbox_face;
      bbox_face.orig_face_idx = -1;
      bbox_face.vert_indices = {b0, b1, b2, b3};
      bbox_face.coords = {skel_graph_.nodes_pos[b0],
                          skel_graph_.nodes_pos[b1],
                          skel_graph_.nodes_pos[b2],
                          skel_graph_.nodes_pos[b3]};
      bbox_face.signed_area = math::area_poly_signed(bbox_face.coords.as_span());
      bbox_face.min_co = double2(g_min.x - margin, g_min.y - margin);
      bbox_face.max_co = double2(g_max.x + margin, g_max.y + margin);
      bbox_face.interior_pt = 0.5 * (g_min + g_max);
      bbox_face.parent = -1;
      bbox_face.depth = 0;
      bbox_face.is_hole = false;

      for (AnalyzedFace &f : faces) {
        if (f.parent == -1) {
          f.parent = 0;
        }
        else {
          f.parent += 1;
        }
        f.depth += 1;
        f.is_hole = (f.depth % 2 != 0);
      }

      faces.insert(0, std::move(bbox_face));
    }
  }

  analyzed_faces_ = std::move(faces);

  for (const AnalyzedFace &f : analyzed_faces_) {
    if (input_.direction == PropagationDirection::Inside ||
        input_.direction == PropagationDirection::Outside)
    {
      if (!f.is_hole) {
        Vector<int> loop = f.vert_indices;
        if (f.signed_area < 0.0) {
          std::reverse(loop.begin(), loop.end());
        }
        this->add_face_contour(loop.as_span(), false, f.orig_face_idx);
      }
      else {
        Vector<int> loop = f.vert_indices;
        if (f.signed_area > 0.0) {
          std::reverse(loop.begin(), loop.end());
        }
        this->add_face_contour(loop.as_span(), true, f.orig_face_idx);
      }
    }
    else if (input_.direction == PropagationDirection::Both) {
      Vector<int> loop_ccw = f.vert_indices;
      if (f.signed_area < 0.0) {
        std::reverse(loop_ccw.begin(), loop_ccw.end());
      }
      this->add_face_contour(loop_ccw.as_span(), false, f.orig_face_idx);

      if (f.orig_face_idx != -1) {
        Vector<int> loop_cw = f.vert_indices;
        if (f.signed_area > 0.0) {
          std::reverse(loop_cw.begin(), loop_cw.end());
        }
        this->add_face_contour(loop_cw.as_span(), true, f.orig_face_idx);
      }
    }
  }

  this->add_wire_edge_graph(wire_edges.as_span(), -1);

  const int total_orig_edges = int(wf_nodes_.pos.size());
  orig_edge_normal_.reinitialize(total_orig_edges);
  for (const int i : IndexRange(total_orig_edges)) {
    if (i < is_cap_node_.size() && is_cap_node_[i]) {
      orig_edge_normal_[i] = cap_normal_[i];
    }
    else {
      const int next_i = wf_nodes_.next_node[i];
      orig_edge_normal_[i] = compute_inward_normal(wf_nodes_.pos[i], wf_nodes_.pos[next_i]);
    }
  }

  this->init_antiparallel_bins();

  for (const int i : wf_nodes_.pos.index_range()) {
    this->update_node_velocity(i);
  }
}

inline void SimulationEngine::init_antiparallel_bins()

{
  const int total_orig_edges = int(orig_edge_normal_.size());
  constexpr int NUM_ANGLE_BINS = 16;
  constexpr int OPPOSITE_BIN_OFFSET = NUM_ANGLE_BINS / 2;
  constexpr int BIN_WINDOW_RADIUS = NUM_ANGLE_BINS / 8; /* +/- 45 degrees. */

  Array<int> bin_counts(NUM_ANGLE_BINS, 0);
  Array<int> edge_bins(total_orig_edges);

  for (const int e : IndexRange(total_orig_edges)) {
    const double2 &n = orig_edge_normal_[e];
    double theta = std::atan2(n.y, n.x);
    if (theta < 0.0) {
      theta += 2.0 * M_PI;
    }
    const int bin = std::clamp(
        int((theta / (2.0 * M_PI)) * double(NUM_ANGLE_BINS)), 0, NUM_ANGLE_BINS - 1);
    edge_bins[e] = bin;
    bin_counts[bin]++;
  }

  orig_edge_has_antiparallel_.reinitialize(total_orig_edges);
  has_any_antiparallel_edges_ = false;

  constexpr int min_offset = OPPOSITE_BIN_OFFSET - BIN_WINDOW_RADIUS;
  constexpr int max_offset = OPPOSITE_BIN_OFFSET + BIN_WINDOW_RADIUS;

  for (const int e : IndexRange(total_orig_edges)) {
    const int b = edge_bins[e];
    bool found = false;
    /* Check for opposing edges within a 90-degree wedge (180 +/- 45 degrees) around the opposite
     * normal. */
    for (int offset = min_offset; offset <= max_offset; offset++) {
      const int opp_b = (b + offset) % NUM_ANGLE_BINS;
      if (bin_counts[opp_b] > 0) {
        found = true;
        break;
      }
    }
    orig_edge_has_antiparallel_[e] = found;
    if (found) {
      has_any_antiparallel_edges_ = true;
    }
  }
}

inline void SimulationEngine::update_node_velocity(int i)

{
  if (!wf_nodes_.is_active[i]) {
    return;
  }
  const int prev_i = wf_nodes_.prev_node[i];
  const int next_i = wf_nodes_.next_node[i];
  if (prev_i < 0 || !wf_nodes_.is_active[prev_i] || next_i < 0 || !wf_nodes_.is_active[next_i]) {
    return;
  }

  const double2 n1 = (prev_i < is_cap_node_.size() && is_cap_node_[prev_i]) ?
                         cap_normal_[prev_i] :
                         compute_inward_normal(wf_nodes_.pos[prev_i], wf_nodes_.pos[i]);
  const double2 n2 = (i < is_cap_node_.size() && is_cap_node_[i]) ?
                         cap_normal_[i] :
                         compute_inward_normal(wf_nodes_.pos[i], wf_nodes_.pos[next_i]);

  const double2 d = wf_nodes_.pos[next_i] - wf_nodes_.pos[i];
  const double2 d2 = (math::length_squared(d) > 1e-12) ? math::normalize(d) : n2;

  const double det = math::cross(n1, n2);
  const double dot = math::dot(n1, n2);
  if (dot < -0.999 && det <= 1e-4) {
    wf_nodes_.vel[i] = d2;
  }
  else {
    wf_nodes_.vel[i] = compute_bisector_velocity(n1, n2, d2);
  }
}

inline void SimulationEngine::deactivate_node(const int i)
{
  wf_nodes_.is_active[i] = false;
}

inline bool SimulationEngine::has_active_contours() const

{
  for (const bool active : wf_contours_.is_active) {
    if (active) {
      return true;
    }
  }
  return false;
}

inline bool SimulationEngine::has_active_outer_contours() const

{
  for (const int cid : wf_contours_.is_active.index_range()) {
    if (wf_contours_.is_active[cid] && !wf_contours_.is_hole[cid] && !wf_contours_.is_wire[cid]) {
      return true;
    }
  }
  return false;
}

inline double SimulationEngine::compute_adaptive_dt() const

{
  double min_len = 1e30;
  double max_speed = 1.0;

  for (const int i : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[i]) {
      continue;
    }
    const int next_i = wf_nodes_.next_node[i];
    if (next_i < 0 || !wf_nodes_.is_active[next_i]) {
      continue;
    }
    const double len = math::length(wf_nodes_.pos[next_i] - wf_nodes_.pos[i]);
    min_len = std::min(min_len, len);

    const double speed = math::length(wf_nodes_.vel[i]);
    max_speed = std::max(max_speed, speed);
  }

  if (min_len >= 1e29) {
    return 0.1;
  }

  const double dt = 0.25 * min_len / std::max(1.0, max_speed);
  return std::clamp(dt, 1e-5, 0.5);
}

inline void SimulationEngine::advance_positions(double dt)

{
  if (dt <= 0.0) {
    return;
  }
  for (const int i : wf_nodes_.pos.index_range()) {
    if (wf_nodes_.is_active[i]) {
      wf_nodes_.pos[i] += dt * wf_nodes_.vel[i];
    }
  }
}

/**
 * Find the earliest topological event occurring in [current_time_, current_time_ + dt].
 *
 * Event Taxonomy:
 * ===============
 * 1. Edge Collapse Event:
 * ```
 *       p_u(0)                    p_v(0)
 *         *------------------------*          Wavefront edge e = p_v - p_u at t0
 *          \                      /
 *           \  v_u          v_v  /
 *            v                  v
 *             *================*              Wavefront edge at t0 + tau
 *                     *                       Collapse at tau (length -> 0)
 *                    p*
 * ```
 *
 * 2. Vertex-Edge Split Event:
 * ```
 *               Reflex vertex w
 *                     *
 *                     | \
 *                     |  \ v_w
 *                     |   v
 *                  d0 |
 *                     |
 *                     v
 *         *========================*          Edge u -> v
 *        u          s in (0, 1)     v
 *
 *      Distance d(tau) closes at rate (1 - v_w . n_uv) until w contacts edge u -> v at p*.
 * ```
 *
 * 3. Vertex-Vertex Collision Event:
 * ```
 *        Node u               Node w
 *          *                   *
 *           \                 /
 *            \  v_u     v_w  /
 *             v             v
 *                    *                        Collision at p* when |p_u(tau) - p_w(tau)| -> 0
 *                   p*
/**
 * Test whether a candidate split event from reflex vertex `w` to collision point `p_w` on
 * edge `(u, v)` is geometrically valid and unobstructed (line-of-sight visibility test).
 *
 * Verifies that the motion trajectory from p_w0 to p_w stays strictly inside the valid domain,
 * does not cross domain/hole boundaries, and does not intersect any active wavefront edges.
 */
inline bool SimulationEngine::is_split_visible(const int w, const int u, const double2 &p_w) const

{
  const double2 p_w0 = wf_nodes_.pos[w];
  if (math::distance_squared(p_w0, p_w) < 1e-10) {
    return true;
  }
  const double2 p_start = p_w0 + 0.02 * (p_w - p_w0);
  const double2 p_end = p_w - 0.02 * (p_w - p_w0);
  const double2 mid = 0.5 * (p_w0 + p_w);

  /* 1. If input has face_offsets, verify containment and boundary crossing. */
  if (!analyzed_faces_.is_empty()) {
    const int orig_f = wf_nodes_.orig_face[w];
    int f_idx = -1;
    for (const int i : analyzed_faces_.index_range()) {
      if (analyzed_faces_[i].orig_face_idx == orig_f) {
        f_idx = i;
        break;
      }
    }
    if (f_idx >= 0 && f_idx < analyzed_faces_.size()) {
      const AnalyzedFace &face = analyzed_faces_[f_idx];
      const int region_idx = face.is_hole ? face.parent : f_idx;
      if (region_idx >= 0 && region_idx < analyzed_faces_.size()) {
        const AnalyzedFace &region = analyzed_faces_[region_idx];
        if (!isect_point_polygon_v2(mid, region.coords.as_span())) {
          return false;
        }
        for (int i = 0; i < region.coords.size(); i++) {
          const double2 &b1 = region.coords[i];
          const double2 &b2 = region.coords[(i + 1) % region.coords.size()];
          if (segments_cross_v2(p_start, p_end, b1, b2)) {
            return false;
          }
        }
        for (const int i : analyzed_faces_.index_range()) {
          if (analyzed_faces_[i].parent == region_idx) {
            const AnalyzedFace &h = analyzed_faces_[i];
            if (isect_point_polygon_v2(mid, h.coords.as_span())) {
              return false;
            }
            for (int j = 0; j < h.coords.size(); j++) {
              const double2 &b1 = h.coords[j];
              const double2 &b2 = h.coords[(j + 1) % h.coords.size()];
              if (segments_cross_v2(p_start, p_end, b1, b2)) {
                return false;
              }
            }
          }
        }
      }
    }
  }

  /* 2. Check if segment crosses any active wavefront edges. */
  const int prev_w = wf_nodes_.prev_node[w];
  const int next_w = wf_nodes_.next_node[w];
  const int v = wf_nodes_.next_node[u];

  if (!grid_.cell_heads.is_empty()) {
    const double sx_min = std::min(p_start.x, p_end.x);
    const double sx_max = std::max(p_start.x, p_end.x);
    const double sy_min = std::min(p_start.y, p_end.y);
    const double sy_max = std::max(p_start.y, p_end.y);

    bool crossed = false;
    grid_.for_each_overlapping(sx_min, sy_min, sx_max, sy_max, [&](const int k) {
      if (crossed) {
        return;
      }
      if (!wf_nodes_.is_active[k]) {
        return;
      }
      const int next_k = wf_nodes_.next_node[k];
      if (next_k < 0 || !wf_nodes_.is_active[next_k]) {
        return;
      }
      if (k == w || next_k == w || k == prev_w || next_k == prev_w || k == next_w ||
          next_k == next_w || k == u || next_k == u || k == v || next_k == v)
      {
        return;
      }
      if (segments_cross_v2(p_start, p_end, wf_nodes_.pos[k], wf_nodes_.pos[next_k])) {
        crossed = true;
      }
    });
    if (crossed) {
      return false;
    }
  }
  else {
    for (const int k : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[k]) {
        continue;
      }
      const int next_k = wf_nodes_.next_node[k];
      if (next_k < 0 || !wf_nodes_.is_active[next_k]) {
        continue;
      }
      if (k == w || next_k == w || k == prev_w || next_k == prev_w || k == next_w ||
          next_k == next_w || k == u || next_k == u || k == v || next_k == v)
      {
        continue;
      }
      if (segments_cross_v2(p_start, p_end, wf_nodes_.pos[k], wf_nodes_.pos[next_k])) {
        return false;
      }
    }
  }

  return true;
}

inline double SimulationEngine::find_earliest_reflex_event_brute_force(
    const double dt, const double eps, const Span<int> reflex_nodes)

{
  double earliest_t = std::numeric_limits<double>::infinity();

  for (const int w : reflex_nodes) {
    const int prev_w = wf_nodes_.prev_node[w];
    const int next_w = wf_nodes_.next_node[w];
    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const int v = wf_nodes_.next_node[u];
      if (v < 0 || !wf_nodes_.is_active[v]) {
        continue;
      }
      if (u == w || v == w || u == prev_w || v == prev_w || u == next_w || v == next_w) {
        continue;
      }

      const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
      const double d0 = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv);
      const double rate = 1.0 - math::dot(wf_nodes_.vel[w], n_uv);

      if (rate > 1e-8 && d0 > 1e-11) {
        const double tau = d0 / rate;
        if (tau > 1e-11 && tau <= dt + 1e-9 && current_time_ + tau < earliest_t) {
          const double2 p_w = wf_nodes_.pos[w] + tau * wf_nodes_.vel[w];
          const double2 p_u = wf_nodes_.pos[u] + tau * wf_nodes_.vel[u];
          const double2 p_v = wf_nodes_.pos[v] + tau * wf_nodes_.vel[v];
          const double s = math::line_point_factor(p_w, p_u, p_v);
          if (s > 1e-5 && s < 1.0 - 1e-5) {
            if (this->is_split_visible(w, u, p_w)) {
              earliest_t = std::min(earliest_t, current_time_ + tau);
            }
          }
        }
      }
    }
  }

  for (const int w : reflex_nodes) {
    for (const int u : wf_nodes_.pos.index_range()) {
      if (u >= w || !wf_nodes_.is_active[u]) {
        continue;
      }
      if (wf_nodes_.next_node[u] == w || wf_nodes_.next_node[w] == u) {
        continue;
      }

      const double2 dp = wf_nodes_.pos[w] - wf_nodes_.pos[u];
      const double2 dv = wf_nodes_.vel[w] - wf_nodes_.vel[u];
      const double dv_sq = math::length_squared(dv);

      if (dv_sq > 1e-12 && math::dot(dp, dv) < -1e-12) {
        const double tau = -math::dot(dp, dv) / dv_sq;
        if (tau > 1e-11 && tau <= dt + 1e-9 && current_time_ + tau < earliest_t) {
          const double dist_at_tau = math::length(dp + tau * dv);
          if (dist_at_tau < std::max(eps, 1e-5)) {
            earliest_t = std::min(earliest_t, current_time_ + tau);
          }
        }
      }
    }
  }
  return earliest_t;
}

inline double SimulationEngine::query_reflex_events_grid(const double eps,
                                                         const Span<int> reflex_nodes,
                                                         const double tau_horizon,
                                                         const double safety_tol)

{
  double earliest_t = std::numeric_limits<double>::infinity();

  for (const int w : reflex_nodes) {
    const double current_horizon = std::min(
        tau_horizon, !std::isinf(earliest_t) ? (earliest_t - current_time_) : tau_horizon);
    if (current_horizon <= 0.0) {
      break;
    }

    const int prev_w = wf_nodes_.prev_node[w];
    const int next_w = wf_nodes_.next_node[w];
    const double2 &p_w0 = wf_nodes_.pos[w];
    const double2 p_w1 = p_w0 + current_horizon * wf_nodes_.vel[w];

    const double wx_min = std::min(p_w0.x, p_w1.x) - safety_tol;
    const double wx_max = std::max(p_w0.x, p_w1.x) + safety_tol;
    const double wy_min = std::min(p_w0.y, p_w1.y) - safety_tol;
    const double wy_max = std::max(p_w0.y, p_w1.y) + safety_tol;

    current_query_id_++;
    if (current_query_id_ <= 0) {
      query_tags_.fill(0);
      current_query_id_ = 1;
    }
    const int qid = current_query_id_;

    grid_.for_each_overlapping(wx_min, wy_min, wx_max, wy_max, [&](const int u) {
      if (query_tags_[u] == qid) {
        return;
      }
      query_tags_[u] = qid;

      const int v = wf_nodes_.next_node[u];
      if (v < 0 || !wf_nodes_.is_active[v]) {
        return;
      }
      if (u == w || v == w || u == prev_w || v == prev_w || u == next_w || v == next_w) {
        return;
      }

      /* Test split event between reflex w and edge u -> v. */
      const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
      const double d0 = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv);
      const double rate = 1.0 - math::dot(wf_nodes_.vel[w], n_uv);

      if (rate > 1e-8 && d0 > 1e-11) {
        const double tau = d0 / rate;
        if (tau > 1e-11 && tau <= current_horizon + 1e-9 && current_time_ + tau < earliest_t) {
          const double2 p_w = wf_nodes_.pos[w] + tau * wf_nodes_.vel[w];
          const double2 p_u = wf_nodes_.pos[u] + tau * wf_nodes_.vel[u];
          const double2 p_v = wf_nodes_.pos[v] + tau * wf_nodes_.vel[v];
          const double s = math::line_point_factor(p_w, p_u, p_v);
          if (s > 1e-5 && s < 1.0 - 1e-5) {
            if (this->is_split_visible(w, u, p_w)) {
              earliest_t = std::min(earliest_t, current_time_ + tau);
            }
          }
        }
      }

      /* Test vertex collision between reflex w and vertex u. */
      if (u != w && wf_nodes_.next_node[u] != w && wf_nodes_.next_node[w] != u) {
        const double2 dp = wf_nodes_.pos[w] - wf_nodes_.pos[u];
        const double2 dv = wf_nodes_.vel[w] - wf_nodes_.vel[u];
        const double dv_sq = math::length_squared(dv);
        if (dv_sq > 1e-12 && math::dot(dp, dv) < -1e-12) {
          const double tau = -math::dot(dp, dv) / dv_sq;
          if (tau > 1e-11 && tau <= current_horizon + 1e-9 && current_time_ + tau < earliest_t) {
            const double dist_at_tau = math::length(dp + tau * dv);
            if (dist_at_tau < std::max(eps, 1e-5)) {
              earliest_t = std::min(earliest_t, current_time_ + tau);
            }
          }
        }
      }
    });
  }

  return earliest_t;
}

inline double SimulationEngine::find_earliest_event(double dt, double eps)

{
  PRF_scope(ProfileCategory::Core);

  double earliest_t = std::numeric_limits<double>::infinity();

  const double cluster_tol = std::max(eps, 1e-4);

  /* Already-collapsed edges at current_time_ (shrinking or degenerate). */
  for (const int u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[u]) {
      continue;
    }
    const int v = wf_nodes_.next_node[u];
    if (v >= 0 && wf_nodes_.is_active[v]) {
      const double2 e0 = wf_nodes_.pos[v] - wf_nodes_.pos[u];
      const double2 dv = wf_nodes_.vel[v] - wf_nodes_.vel[u];
      if (math::length_squared(e0) < cluster_tol * cluster_tol) {
        if (math::length_squared(e0) < 1e-12) {
          if (math::length_squared(dv) < 1e-12) {
            return current_time_;
          }
        }
        else if (math::dot(e0, dv) < -1e-12) {
          return current_time_;
        }
      }
    }
  }

  double2 bbox_min(1e30, 1e30), bbox_max(-1e30, -1e30);
  int active_nodes_num = 0;
  Vector<int> reflex_nodes;

  for (const int u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[u]) {
      continue;
    }
    active_nodes_num++;
    math::min_max(wf_nodes_.pos[u], bbox_min, bbox_max);

    if (this->is_reflex(u)) {
      reflex_nodes.append(u);
    }
  }

  if (active_nodes_num == 0) {
    return earliest_t;
  }

  const double diag = std::max(std::hypot(bbox_max.x - bbox_min.x, bbox_max.y - bbox_min.y), 1e-4);

  /* Edge Collapse Events. */
  for (const int u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[u]) {
      continue;
    }
    const int v = wf_nodes_.next_node[u];
    if (v < 0 || !wf_nodes_.is_active[v]) {
      continue;
    }
    const double2 e0 = wf_nodes_.pos[v] - wf_nodes_.pos[u];
    const double2 dv = wf_nodes_.vel[v] - wf_nodes_.vel[u];
    const double dv_sq = math::length_squared(dv);

    if (dv_sq > 1e-12 && math::dot(e0, dv) < -1e-12) {
      const double tau = -math::dot(e0, dv) / dv_sq;
      if (tau > 1e-11 && tau <= dt + 1e-9 && tau <= diag) {
        const double2 e_tau = e0 + tau * dv;
        if (math::length(e_tau) < std::max(eps, 1e-3) ||
            (!std::isinf(dt) && math::dot(e0, e0 + dt * dv) <= 0.0))
        {
          earliest_t = std::min(earliest_t, current_time_ + tau);
        }
      }
    }
  }
  const double safety_tol = std::max(eps, 1e-4);

  double max_tau_search = dt;
  if (!std::isinf(earliest_t)) {
    max_tau_search = std::min(max_tau_search, earliest_t - current_time_);
  }
  if (std::isinf(max_tau_search) || max_tau_search > diag) {
    max_tau_search = diag;
  }
  if (max_tau_search < 0.0) {
    max_tau_search = 0.0;
  }

  if (active_nodes_num <= 16) {
    /* Antiparallel Edge-Edge Events (brute force for small active node count). */
    if (has_any_antiparallel_edges_) {
      for (const int u : wf_nodes_.pos.index_range()) {
        if (!wf_nodes_.is_active[u]) {
          continue;
        }
        const int orig1 = wf_nodes_.orig_edge[u];
        if (orig1 >= 0 && orig1 < orig_edge_has_antiparallel_.size() &&
            !orig_edge_has_antiparallel_[orig1])
        {
          continue;
        }
        const int v = wf_nodes_.next_node[u];
        if (v < 0 || !wf_nodes_.is_active[v]) {
          continue;
        }
        double l1;
        const double2 dir1 = math::normalize_and_get_length(wf_nodes_.pos[v] - wf_nodes_.pos[u],
                                                            l1);
        if (l1 < cluster_tol) {
          continue;
        }
        const double2 n1 = math::orthogonal(dir1);
        for (const int w : wf_nodes_.pos.index_range()) {
          if (w <= u || !wf_nodes_.is_active[w]) {
            continue;
          }
          const int orig2 = wf_nodes_.orig_edge[w];
          if (orig2 >= 0 && orig2 < orig_edge_has_antiparallel_.size() &&
              !orig_edge_has_antiparallel_[orig2])
          {
            continue;
          }
          const int z = wf_nodes_.next_node[w];
          if (z < 0 || !wf_nodes_.is_active[z]) {
            continue;
          }
          double l2;
          const double2 dir2 = math::normalize_and_get_length(wf_nodes_.pos[z] - wf_nodes_.pos[w],
                                                              l2);
          if (l2 < cluster_tol) {
            continue;
          }
          if (math::dot(dir1, dir2) > -0.9999) {
            continue;
          }
          const double gap = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n1);
          if (gap <= cluster_tol) {
            continue;
          }
          const double tau = 0.5 * gap;
          if (tau > dt + 1e-9 || current_time_ + tau >= earliest_t) {
            continue;
          }
          const double2 pu = wf_nodes_.pos[u] + tau * wf_nodes_.vel[u];
          const double2 pv = wf_nodes_.pos[v] + tau * wf_nodes_.vel[v];
          const double2 pw = wf_nodes_.pos[w] + tau * wf_nodes_.vel[w];
          const double2 pz = wf_nodes_.pos[z] + tau * wf_nodes_.vel[z];
          const double s_v = math::dot(pv - pu, dir1);
          const double s_w = math::dot(pw - pu, dir1);
          const double s_z = math::dot(pz - pu, dir1);
          const double overlap = std::min(std::max(0.0, s_v), std::max(s_w, s_z)) -
                                 std::max(std::min(0.0, s_v), std::min(s_w, s_z));
          if (overlap > 10.0 * cluster_tol) {
            earliest_t = current_time_ + tau;
          }
        }
      }
    }

    if (!reflex_nodes.is_empty()) {
      const double reflex_t = this->find_earliest_reflex_event_brute_force(dt, eps, reflex_nodes);
      earliest_t = std::min(earliest_t, reflex_t);
    }
    return earliest_t;
  }

  /* Early exit if there are no reflex nodes and no antiparallel edges. */
  if (reflex_nodes.is_empty() && !has_any_antiparallel_edges_) {
    return earliest_t;
  }

  /* Spatial grid acceleration for > 16 nodes. */

  const double x_min = bbox_min.x - safety_tol;
  const double x_max = bbox_max.x + safety_tol;
  const double y_min = bbox_min.y - safety_tol;
  const double y_max = bbox_max.y + safety_tol;

  grid_.init(x_min, y_min, x_max, y_max, active_nodes_num);

  for (const int u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[u]) {
      continue;
    }
    const int v = wf_nodes_.next_node[u];
    if (v < 0 || !wf_nodes_.is_active[v]) {
      continue;
    }
    const double2 &p_u0 = wf_nodes_.pos[u];
    const double2 &p_v0 = wf_nodes_.pos[v];
    const double2 p_u1 = p_u0 + max_tau_search * wf_nodes_.vel[u];
    const double2 p_v1 = p_v0 + max_tau_search * wf_nodes_.vel[v];

    const double ex_min = std::min({p_u0.x, p_v0.x, p_u1.x, p_v1.x}) - safety_tol;
    const double ex_max = std::max({p_u0.x, p_v0.x, p_u1.x, p_v1.x}) + safety_tol;
    const double ey_min = std::min({p_u0.y, p_v0.y, p_u1.y, p_v1.y}) - safety_tol;
    const double ey_max = std::max({p_u0.y, p_v0.y, p_u1.y, p_v1.y}) + safety_tol;

    grid_.insert(u, ex_min, ey_min, ex_max, ey_max);
  }

  /* Timestamp tags for O(1) deduplication during spatial grid queries.
   * Because a node bounding box can span multiple grid cells, spatial queries can visit
   * the same node multiple times. Incrementing current_query_id_ avoids zeroing out
   * query_tags_ before every query. */
  if (query_tags_.size() < wf_nodes_.pos.size()) {
    query_tags_.reinitialize(wf_nodes_.pos.size());
    query_tags_.fill(0);
    current_query_id_ = 1;
  }

  /* Antiparallel Edge-Edge Events using spatial grid. */
  if (has_any_antiparallel_edges_) {
    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const int orig1 = wf_nodes_.orig_edge[u];
      if (orig1 >= 0 && orig1 < orig_edge_has_antiparallel_.size() &&
          !orig_edge_has_antiparallel_[orig1])
      {
        continue;
      }
      const int v = wf_nodes_.next_node[u];
      if (v < 0 || !wf_nodes_.is_active[v]) {
        continue;
      }
      double l1;
      const double2 dir1 = math::normalize_and_get_length(wf_nodes_.pos[v] - wf_nodes_.pos[u], l1);
      if (l1 < cluster_tol) {
        continue;
      }
      const double2 n1 = math::orthogonal(dir1);

      const double2 &p_u0 = wf_nodes_.pos[u];
      const double2 &p_v0 = wf_nodes_.pos[v];
      const double2 p_u1 = p_u0 + max_tau_search * wf_nodes_.vel[u];
      const double2 p_v1 = p_v0 + max_tau_search * wf_nodes_.vel[v];

      const double ex_min = std::min({p_u0.x, p_v0.x, p_u1.x, p_v1.x}) - safety_tol;
      const double ex_max = std::max({p_u0.x, p_v0.x, p_u1.x, p_v1.x}) + safety_tol;
      const double ey_min = std::min({p_u0.y, p_v0.y, p_u1.y, p_v1.y}) - safety_tol;
      const double ey_max = std::max({p_u0.y, p_v0.y, p_u1.y, p_v1.y}) + safety_tol;

      current_query_id_++;
      if (current_query_id_ <= 0) {
        query_tags_.fill(0);
        current_query_id_ = 1;
      }
      const int qid = current_query_id_;

      grid_.for_each_overlapping(ex_min, ey_min, ex_max, ey_max, [&](const int w) {
        if (w <= u) {
          return;
        }
        if (query_tags_[w] == qid) {
          return;
        }
        query_tags_[w] = qid;

        if (!wf_nodes_.is_active[w]) {
          return;
        }
        const int orig2 = wf_nodes_.orig_edge[w];
        if (orig2 >= 0 && orig2 < orig_edge_has_antiparallel_.size() &&
            !orig_edge_has_antiparallel_[orig2])
        {
          return;
        }
        const int z = wf_nodes_.next_node[w];
        if (z < 0 || !wf_nodes_.is_active[z]) {
          return;
        }
        double l2;
        const double2 dir2 = math::normalize_and_get_length(wf_nodes_.pos[z] - wf_nodes_.pos[w],
                                                            l2);
        if (l2 < cluster_tol) {
          return;
        }
        /* Facing, parallel edges only. */
        if (math::dot(dir1, dir2) > -0.9999) {
          return;
        }
        /* The other edge must lie on the interior side of this one. */
        const double gap = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n1);
        if (gap <= cluster_tol) {
          return;
        }
        const double tau = 0.5 * gap;
        if (tau > dt + 1e-9 || current_time_ + tau >= earliest_t) {
          return;
        }
        /* Require overlap along the edge direction at the meeting time. */
        const double2 pu = wf_nodes_.pos[u] + tau * wf_nodes_.vel[u];
        const double2 pv = wf_nodes_.pos[v] + tau * wf_nodes_.vel[v];
        const double2 pw = wf_nodes_.pos[w] + tau * wf_nodes_.vel[w];
        const double2 pz = wf_nodes_.pos[z] + tau * wf_nodes_.vel[z];
        const double s_v = math::dot(pv - pu, dir1);
        const double s_w = math::dot(pw - pu, dir1);
        const double s_z = math::dot(pz - pu, dir1);
        const double overlap = std::min(std::max(0.0, s_v), std::max(s_w, s_z)) -
                               std::max(std::min(0.0, s_v), std::min(s_w, s_z));
        if (overlap > 10.0 * cluster_tol) {
          earliest_t = current_time_ + tau;
          max_tau_search = std::min(max_tau_search, earliest_t - current_time_);
        }
      });
    }
  }

  if (!reflex_nodes.is_empty()) {
    const double reflex_tau_horizon = std::min(
        max_tau_search, !std::isinf(earliest_t) ? earliest_t - current_time_ : max_tau_search);
    const double reflex_t = this->query_reflex_events_grid(
        eps, reflex_nodes, reflex_tau_horizon, safety_tol);
    earliest_t = std::min(earliest_t, reflex_t);
  }

  return earliest_t;
}

inline bool SimulationEngine::is_reflex(int w) const

{
  if (!wf_nodes_.is_active[w]) {
    return false;
  }
  const int prev_w = wf_nodes_.prev_node[w];
  const int next_w = wf_nodes_.next_node[w];
  if (prev_w < 0 || next_w < 0 || !wf_nodes_.is_active[prev_w] || !wf_nodes_.is_active[next_w]) {
    return false;
  }
  const double2 n_in = compute_inward_normal(wf_nodes_.pos[prev_w], wf_nodes_.pos[w]);
  const double2 n_out = compute_inward_normal(wf_nodes_.pos[w], wf_nodes_.pos[next_w]);
  return (n_in.x * n_out.y - n_in.y * n_out.x <= -1e-8);
}

inline int SimulationEngine::get_or_create_skel_node(const double2 &pos, double tol)

{
  const double effective_tol = std::max(tol, DEFAULT_CLUSTER_TOL);
  for (int i = int(skel_graph_.nodes_pos.size()) - 1; i >= 0; i--) {
    if (skel_graph_.nodes_time[i] < current_time_ - 1e-9) {
      break;
    }
    if (std::abs(skel_graph_.nodes_time[i] - current_time_) < 1e-9 &&
        math::length(skel_graph_.nodes_pos[i] - pos) < effective_tol)
    {
      return i;
    }
  }

  /* Check if pos coincides with an input vertex (within tol) at t=0. */
  if (current_time_ <= 1e-6) {
    const int input_verts_num = int(input_.vert.size());
    for (int i = 0; i < input_verts_num; i++) {
      if (math::length(skel_graph_.nodes_pos[i] - pos) < effective_tol) {
        return i;
      }
    }
  }

  const int new_s = int(skel_graph_.nodes_pos.size());
  skel_graph_.nodes_pos.append(pos);
  skel_graph_.nodes_time.append(current_time_);
  skel_graph_.nodes_orig.append(-1);
  return new_s;
}

/**
 * Resolve a pair of collinear opposing wavefront edges meeting along a line segment.
 *
 * Geometric Situations:
 * =====================
 * Case A: Exact Coincident Match:
 * ```
 *             Wavefront Top: Edge e1 (u -> v moving down)
 *                      (u) -------------> (v)
 *                           |    |    |
 *                           v    v    v
 *               p_a (s_a*)  *============*  p_b (s_b*)   Skeleton Ridge
 *                           ^    ^    ^
 *                           |    |    |
 *                      (z) <------------- (w)
 *             Wavefront Bottom: Edge e2 (w -> z moving up)
 * ```
 *
 * Case B: Partial Overlap (overhanging suffixes continue advancing):
 * ```
 *                 (u) ----------------- p_a ============ p_b ----------------- (v)  Edge e1
 *                                       (z) <----------- (w)                         Edge e2
 *                                       [==================]                         Overlap
 * [p_a, p_b]
 * ```
 */
inline bool SimulationEngine::try_resolve_opposing_pair(const int e1,
                                                        const int e2,
                                                        const double cluster_tol,
                                                        Set<std::pair<int, int>> &processed_pairs)

{
  if (!wf_nodes_.is_active[e1] || !wf_nodes_.is_active[e2]) {
    return false;
  }
  const int orig1 = wf_nodes_.orig_edge[e1];
  const int orig2 = wf_nodes_.orig_edge[e2];
  if (orig1 >= 0 && orig2 >= 0 && orig1 < orig_edge_normal_.size() &&
      orig2 < orig_edge_normal_.size())
  {
    /* Require edge normals to be anti-parallel (opposing directions within ~8 degrees). */
    if (math::dot(orig_edge_normal_[orig1], orig_edge_normal_[orig2]) > -0.99) {
      return false;
    }
  }
  if (processed_pairs.contains(std::make_pair(e1, e2))) {
    return false;
  }
  const int u = e1;
  const int v = wf_nodes_.next_node[u];
  if (v < 0 || !wf_nodes_.is_active[v]) {
    return false;
  }
  const double2 d1 = wf_nodes_.pos[v] - wf_nodes_.pos[u];
  double l1;
  const double2 dir1 = math::normalize_and_get_length(d1, l1);
  if (l1 < cluster_tol) {
    return false;
  }
  const double2 n1 = math::orthogonal(dir1);

  const int w = e2;
  const int z = wf_nodes_.next_node[w];
  if (z < 0 || !wf_nodes_.is_active[z]) {
    return false;
  }
  const double2 d2 = wf_nodes_.pos[z] - wf_nodes_.pos[w];
  double l2;
  const double2 dir2 = math::normalize_and_get_length(d2, l2);
  if (l2 < cluster_tol) {
    return false;
  }
  const double2 n2 = math::orthogonal(dir2);

  if (math::dot(n1, n2) > -0.99) {
    return false;
  }

  if (std::abs(math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n1)) > cluster_tol ||
      std::abs(math::dot(wf_nodes_.pos[z] - wf_nodes_.pos[u], n1)) > cluster_tol)
  {
    return false;
  }

  const double s_w = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], dir1);
  const double s_z = math::dot(wf_nodes_.pos[z] - wf_nodes_.pos[u], dir1);
  const double s_a = std::max(0.0, std::min(s_w, s_z));
  const double s_b = std::min(l1, std::max(s_w, s_z));

  if (s_b - s_a < cluster_tol) {
    return false;
  }

  processed_pairs.add(std::make_pair(e1, e2));

  const double2 p_a = wf_nodes_.pos[u] + s_a * dir1;
  const double2 p_b = wf_nodes_.pos[u] + s_b * dir1;
  const int s_a_star = this->get_or_create_skel_node(p_a, cluster_tol);
  const int s_b_star = this->get_or_create_skel_node(p_b, cluster_tol);

  /* Add skeleton ridge edge between s_a_star and s_b_star. */
  skel_graph_.edges_node1.append(s_a_star);
  skel_graph_.edges_node2.append(s_b_star);
  skel_graph_.edges_orig.append(-1);
  skel_graph_.edges_face.append(wf_nodes_.orig_face[e1]);
  skel_graph_.edges_time_start.append(current_time_);
  skel_graph_.edges_time_end.append(current_time_);
  skel_graph_.edges_is_boundary.append(false);

  const int prev_u = wf_nodes_.prev_node[u];
  const int prev_w = wf_nodes_.prev_node[w];

  const double end_tol = std::max(cluster_tol, 0.002);
  const bool match_left = std::abs(std::min(s_w, s_z) - 0.0) < end_tol;
  const bool match_right = std::abs(std::max(s_w, s_z) - l1) < end_tol;
  int new_node_id = -1;

  if (match_left && match_right) {
    /* Case A: Exact coincident match - both edges completely collapse. */
    if (!dry_run_) {
      /* Emit face for e1 (u -> v). */
      Vector<int> face_verts1;
      face_verts1.append(wf_nodes_.left_trail[e1][0]);
      for (const int r : wf_nodes_.right_trail[e1]) {
        if (face_verts1.last() != r) {
          face_verts1.append(r);
        }
      }
      if (face_verts1.last() != s_b_star) {
        face_verts1.append(s_b_star);
      }
      if (face_verts1.last() != s_a_star) {
        face_verts1.append(s_a_star);
      }
      for (int k = int(wf_nodes_.left_trail[e1].size()) - 1; k >= 1; k--) {
        const int l = wf_nodes_.left_trail[e1][k];
        if (face_verts1.last() != l && l != face_verts1[0]) {
          face_verts1.append(l);
        }
      }
      this->emit_face(std::move(face_verts1), wf_nodes_.orig_face[e1], wf_nodes_.orig_edge[e1]);

      /* Emit face for e2 (w -> z). */
      Vector<int> face_verts2;
      face_verts2.append(wf_nodes_.left_trail[e2][0]);
      for (const int r : wf_nodes_.right_trail[e2]) {
        if (face_verts2.last() != r) {
          face_verts2.append(r);
        }
      }
      if (face_verts2.last() != s_a_star) {
        face_verts2.append(s_a_star);
      }
      if (face_verts2.last() != s_b_star) {
        face_verts2.append(s_b_star);
      }
      for (int k = int(wf_nodes_.left_trail[e2].size()) - 1; k >= 1; k--) {
        const int l = wf_nodes_.left_trail[e2][k];
        if (face_verts2.last() != l && l != face_verts2[0]) {
          face_verts2.append(l);
        }
      }
      this->emit_face(std::move(face_verts2), wf_nodes_.orig_face[e2], wf_nodes_.orig_edge[e2]);
    }

    if (prev_u == z && z >= 0 && wf_nodes_.is_active[z]) {
      if (!dry_run_) {
        Vector<int> face_verts;
        face_verts.append(wf_nodes_.left_trail[z][0]);
        for (const int r : wf_nodes_.right_trail[z]) {
          if (face_verts.last() != r) {
            face_verts.append(r);
          }
        }
        if (face_verts.last() != s_a_star) {
          face_verts.append(s_a_star);
        }
        for (int k = int(wf_nodes_.left_trail[z].size()) - 1; k >= 1; k--) {
          const int l = wf_nodes_.left_trail[z][k];
          if (face_verts.last() != l && l != face_verts[0]) {
            face_verts.append(l);
          }
        }
        this->emit_face(std::move(face_verts), wf_nodes_.orig_face[z], wf_nodes_.orig_edge[z]);
      }
      this->deactivate_node(z);
    }
    else if (prev_u != w && prev_u != z && prev_u >= 0 && z >= 0 && wf_nodes_.is_active[prev_u] &&
             wf_nodes_.is_active[z])
    {
      wf_nodes_.next_node[prev_u] = z;
      wf_nodes_.prev_node[z] = prev_u;
      if (wf_nodes_.right_trail[prev_u].is_empty() ||
          wf_nodes_.right_trail[prev_u].last() != s_a_star)
      {
        wf_nodes_.right_trail[prev_u].append(s_a_star);
      }
      if (wf_nodes_.left_trail[z].is_empty() || wf_nodes_.left_trail[z].last() != s_a_star) {
        wf_nodes_.left_trail[z].append(s_a_star);
      }
    }

    if (prev_w == v && v >= 0 && wf_nodes_.is_active[v]) {
      if (!dry_run_) {
        Vector<int> face_verts;
        face_verts.append(wf_nodes_.left_trail[v][0]);
        for (const int r : wf_nodes_.right_trail[v]) {
          if (face_verts.last() != r) {
            face_verts.append(r);
          }
        }
        if (face_verts.last() != s_b_star) {
          face_verts.append(s_b_star);
        }
        for (int k = int(wf_nodes_.left_trail[v].size()) - 1; k >= 1; k--) {
          const int l = wf_nodes_.left_trail[v][k];
          if (face_verts.last() != l && l != face_verts[0]) {
            face_verts.append(l);
          }
        }
        this->emit_face(std::move(face_verts), wf_nodes_.orig_face[v], wf_nodes_.orig_edge[v]);
      }
      this->deactivate_node(v);
    }
    else if (prev_w != u && prev_w != v && prev_w >= 0 && v >= 0 && wf_nodes_.is_active[prev_w] &&
             wf_nodes_.is_active[v])
    {
      wf_nodes_.next_node[prev_w] = v;
      wf_nodes_.prev_node[v] = prev_w;
      if (wf_nodes_.right_trail[prev_w].is_empty() ||
          wf_nodes_.right_trail[prev_w].last() != s_b_star)
      {
        wf_nodes_.right_trail[prev_w].append(s_b_star);
      }
      if (wf_nodes_.left_trail[v].is_empty() || wf_nodes_.left_trail[v].last() != s_b_star) {
        wf_nodes_.left_trail[v].append(s_b_star);
      }
    }

    this->deactivate_node(u);
    this->deactivate_node(w);
  }
  else {
    /* Case B: Partial overlap - if an edge is completely covered by the overlap,
     * it fully collapses and emits its roof face. Surviving suffixes continue. */
    const bool e1_completely_covered = (s_a < cluster_tol && s_b > l1 - cluster_tol);
    const double l2 = math::length(wf_nodes_.pos[z] - wf_nodes_.pos[w]);
    const bool e2_completely_covered = (s_b - s_a > l2 - cluster_tol);

    if (e1_completely_covered) {
      if (!dry_run_) {
        Vector<int> face_verts1;
        face_verts1.append(wf_nodes_.left_trail[e1][0]);
        for (const int r : wf_nodes_.right_trail[e1]) {
          if (face_verts1.last() != r) {
            face_verts1.append(r);
          }
        }
        if (face_verts1.last() != s_b_star) {
          face_verts1.append(s_b_star);
        }
        if (face_verts1.last() != s_a_star) {
          face_verts1.append(s_a_star);
        }
        for (int k = int(wf_nodes_.left_trail[e1].size()) - 1; k >= 1; k--) {
          const int l = wf_nodes_.left_trail[e1][k];
          if (face_verts1.last() != l && l != face_verts1[0]) {
            face_verts1.append(l);
          }
        }
        this->emit_face(std::move(face_verts1), wf_nodes_.orig_face[e1], wf_nodes_.orig_edge[e1]);
      }
      this->deactivate_node(u);
    }

    if (e2_completely_covered) {
      if (!dry_run_) {
        Vector<int> face_verts2;
        face_verts2.append(wf_nodes_.left_trail[e2][0]);
        for (const int r : wf_nodes_.right_trail[e2]) {
          if (face_verts2.last() != r) {
            face_verts2.append(r);
          }
        }
        if (face_verts2.last() != s_a_star) {
          face_verts2.append(s_a_star);
        }
        if (face_verts2.last() != s_b_star) {
          face_verts2.append(s_b_star);
        }
        for (int k = int(wf_nodes_.left_trail[e2].size()) - 1; k >= 1; k--) {
          const int l = wf_nodes_.left_trail[e2][k];
          if (face_verts2.last() != l && l != face_verts2[0]) {
            face_verts2.append(l);
          }
        }
        this->emit_face(std::move(face_verts2), wf_nodes_.orig_face[e2], wf_nodes_.orig_edge[e2]);
      }
      this->deactivate_node(w);
    }

    const bool e1_extends_left = (s_z > cluster_tol);
    const bool e2_extends_left = (s_z < -cluster_tol);
    const bool e1_extends_right = (l1 > s_w + cluster_tol);
    const bool e2_extends_right = (s_w > l1 + cluster_tol);

    if (e1_extends_left && e1_extends_right) {
      /* Case B1: Edge 1 extends beyond BOTH ends of Edge 2.
       * Edge 2 was completely covered and collapsed.
       * Edge 1 is split into two surviving pieces:
       *   Piece 1 (node u):  starts at u, ends at p_a -> connects to z.
       *   Piece 2 (new node u2): starts at p_b, ends at v -> connects from prev_w.
       */
      wf_nodes_.next_node[u] = z;
      wf_nodes_.prev_node[z] = u;
      wf_nodes_.pos[z] = p_a;
      wf_nodes_.skel_node[z] = s_a_star;

      Vector<int> orig_lt_u = std::move(wf_nodes_.left_trail[u]);
      Vector<int> orig_rt_u = std::move(wf_nodes_.right_trail[u]);

      if (!dry_run_) {
        Vector<int> face_verts1;
        face_verts1.append(orig_lt_u[0]);
        for (const int r : orig_rt_u) {
          if (face_verts1.last() != r) {
            face_verts1.append(r);
          }
        }
        if (face_verts1.last() != s_b_star) {
          face_verts1.append(s_b_star);
        }
        if (face_verts1.last() != s_a_star) {
          face_verts1.append(s_a_star);
        }
        for (int k = int(orig_lt_u.size()) - 1; k >= 1; k--) {
          const int l = orig_lt_u[k];
          if (face_verts1.last() != l && l != face_verts1[0]) {
            face_verts1.append(l);
          }
        }
        this->emit_face(std::move(face_verts1), wf_nodes_.orig_face[u], wf_nodes_.orig_edge[u]);
      }

      wf_nodes_.next_node[u] = z;
      wf_nodes_.prev_node[z] = u;
      wf_nodes_.pos[z] = p_a;
      wf_nodes_.skel_node[z] = s_a_star;
      wf_nodes_.left_trail[u].clear();
      wf_nodes_.left_trail[u].append(orig_lt_u.is_empty() ? wf_nodes_.skel_node[u] :
                                                            orig_lt_u.last());
      wf_nodes_.right_trail[u].clear();
      wf_nodes_.right_trail[u].append(s_a_star);

      if (wf_nodes_.left_trail[z].is_empty() || wf_nodes_.left_trail[z].last() != s_a_star) {
        wf_nodes_.left_trail[z].append(s_a_star);
      }

      const int u2 = int(wf_nodes_.pos.size());
      new_node_id = u2;
      wf_nodes_.pos.append(p_b);
      wf_nodes_.vel.append(wf_nodes_.vel[u]);
      wf_nodes_.prev_node.append(prev_w);
      wf_nodes_.next_node.append(v);
      wf_nodes_.skel_node.append(s_b_star);
      wf_nodes_.orig_edge.append(wf_nodes_.orig_edge[u]);
      wf_nodes_.orig_face.append(wf_nodes_.orig_face[u]);
      wf_nodes_.contour_id.append(wf_nodes_.contour_id[u]);
      wf_nodes_.is_active.append(true);
      wf_nodes_.time_birth.append(current_time_);

      Vector<int> lt2;
      lt2.append(s_b_star);
      wf_nodes_.left_trail.append(std::move(lt2));

      Vector<int> rt2;
      const int right_skel = orig_rt_u.is_empty() ? wf_nodes_.skel_node[v] : orig_rt_u.last();
      rt2.append(right_skel);
      wf_nodes_.right_trail.append(std::move(rt2));

      wf_nodes_.prev_node[v] = u2;
      wf_nodes_.next_node[prev_w] = u2;

      if (wf_nodes_.right_trail[prev_w].is_empty() ||
          wf_nodes_.right_trail[prev_w].last() != s_b_star)
      {
        wf_nodes_.right_trail[prev_w].append(s_b_star);
      }
    }
    else if (e2_extends_left && e2_extends_right) {
      /* Case B2: Edge 2 extends beyond BOTH ends of Edge 1.
       * Edge 1 was completely covered and collapsed.
       * Edge 2 is split into two surviving pieces:
       *   Piece 1 (new node w2): starts at p_a, ends at z -> connects from prev_u.
       *   Piece 2 (node w):  starts at w, ends at p_b -> connects to v.
       */
      Vector<int> orig_lt_w = std::move(wf_nodes_.left_trail[w]);
      Vector<int> orig_rt_w = std::move(wf_nodes_.right_trail[w]);

      if (!dry_run_) {
        Vector<int> face_verts2;
        face_verts2.append(orig_lt_w[0]);
        for (const int r : orig_rt_w) {
          if (face_verts2.last() != r) {
            face_verts2.append(r);
          }
        }
        if (face_verts2.last() != s_a_star) {
          face_verts2.append(s_a_star);
        }
        if (face_verts2.last() != s_b_star) {
          face_verts2.append(s_b_star);
        }
        for (int k = int(orig_lt_w.size()) - 1; k >= 1; k--) {
          const int l = orig_lt_w[k];
          if (face_verts2.last() != l && l != face_verts2[0]) {
            face_verts2.append(l);
          }
        }
        this->emit_face(std::move(face_verts2), wf_nodes_.orig_face[w], wf_nodes_.orig_edge[w]);
      }

      wf_nodes_.next_node[w] = v;
      wf_nodes_.prev_node[v] = w;
      wf_nodes_.pos[v] = p_b;
      wf_nodes_.skel_node[v] = s_b_star;
      wf_nodes_.left_trail[w].clear();
      wf_nodes_.left_trail[w].append(orig_lt_w.is_empty() ? wf_nodes_.skel_node[w] :
                                                            orig_lt_w.last());
      wf_nodes_.right_trail[w].clear();
      wf_nodes_.right_trail[w].append(s_b_star);

      if (wf_nodes_.left_trail[v].is_empty() || wf_nodes_.left_trail[v].last() != s_b_star) {
        wf_nodes_.left_trail[v].append(s_b_star);
      }

      const int w2 = int(wf_nodes_.pos.size());
      new_node_id = w2;
      wf_nodes_.pos.append(p_a);
      wf_nodes_.vel.append(wf_nodes_.vel[w]);
      wf_nodes_.prev_node.append(prev_u);
      wf_nodes_.next_node.append(z);
      wf_nodes_.skel_node.append(s_a_star);
      wf_nodes_.orig_edge.append(wf_nodes_.orig_edge[w]);
      wf_nodes_.orig_face.append(wf_nodes_.orig_face[w]);
      wf_nodes_.contour_id.append(wf_nodes_.contour_id[w]);
      wf_nodes_.is_active.append(true);
      wf_nodes_.time_birth.append(current_time_);

      Vector<int> lt2;
      lt2.append(s_a_star);
      wf_nodes_.left_trail.append(std::move(lt2));

      Vector<int> rt2;
      const int right_skel = orig_rt_w.is_empty() ? wf_nodes_.skel_node[z] : orig_rt_w.last();
      rt2.append(right_skel);
      wf_nodes_.right_trail.append(std::move(rt2));

      wf_nodes_.prev_node[z] = w2;
      wf_nodes_.next_node[prev_u] = w2;

      if (wf_nodes_.right_trail[prev_u].is_empty() ||
          wf_nodes_.right_trail[prev_u].last() != s_a_star)
      {
        wf_nodes_.right_trail[prev_u].append(s_a_star);
      }
    }
    else {
      /* Case B3: Neither edge extends on both ends. */
      /* Handle left endpoint p_a (s_a): */
      if (s_z < -cluster_tol) {
        /* Edge 2 extends to the left of s_a: surviving piece of Edge 2 starts at p_a, goes
         * to z. */
        wf_nodes_.pos[w] = p_a;
        wf_nodes_.skel_node[w] = s_a_star;
        wf_nodes_.next_node[w] = z;
        wf_nodes_.prev_node[z] = w;
        if (prev_u != w && prev_u >= 0) {
          wf_nodes_.prev_node[w] = prev_u;
          wf_nodes_.next_node[prev_u] = w;
        }
        if (wf_nodes_.right_trail[prev_u].is_empty() ||
            wf_nodes_.right_trail[prev_u].last() != s_a_star)
        {
          wf_nodes_.right_trail[prev_u].append(s_a_star);
        }
        if (wf_nodes_.left_trail[w].is_empty() || wf_nodes_.left_trail[w].last() != s_b_star) {
          wf_nodes_.left_trail[w].append(s_b_star);
        }
        if (wf_nodes_.left_trail[w].last() != s_a_star) {
          wf_nodes_.left_trail[w].append(s_a_star);
        }
      }
      else if (s_z > cluster_tol) {
        /* Edge 1 extends to the left of s_a: surviving piece of Edge 1 starts at u, ends
         * at p_a. */
        wf_nodes_.next_node[u] = z;
        wf_nodes_.prev_node[z] = u;
        wf_nodes_.pos[z] = p_a;
        wf_nodes_.skel_node[z] = s_a_star;
        if (wf_nodes_.right_trail[u].is_empty() || wf_nodes_.right_trail[u].last() != s_b_star) {
          wf_nodes_.right_trail[u].append(s_b_star);
        }
        if (wf_nodes_.right_trail[u].last() != s_a_star) {
          wf_nodes_.right_trail[u].append(s_a_star);
        }
        if (wf_nodes_.left_trail[z].is_empty() || wf_nodes_.left_trail[z].last() != s_a_star) {
          wf_nodes_.left_trail[z].append(s_a_star);
        }
      }
      else {
        /* Endpoints coincide at s_a. */
        if (prev_u == z && z >= 0 && wf_nodes_.is_active[z]) {
          if (!dry_run_) {
            Vector<int> face_verts;
            face_verts.append(wf_nodes_.left_trail[z][0]);
            for (const int r : wf_nodes_.right_trail[z]) {
              if (face_verts.last() != r) {
                face_verts.append(r);
              }
            }
            if (face_verts.last() != s_a_star) {
              face_verts.append(s_a_star);
            }
            for (int k = int(wf_nodes_.left_trail[z].size()) - 1; k >= 1; k--) {
              const int l = wf_nodes_.left_trail[z][k];
              if (face_verts.last() != l && l != face_verts[0]) {
                face_verts.append(l);
              }
            }
            this->emit_face(std::move(face_verts), wf_nodes_.orig_face[z], wf_nodes_.orig_edge[z]);
          }
          this->deactivate_node(z);
        }
        else if (prev_u != w && prev_u != z && prev_u >= 0 && z != u && z >= 0 &&
                 wf_nodes_.is_active[prev_u] && wf_nodes_.is_active[z])
        {
          wf_nodes_.next_node[prev_u] = z;
          wf_nodes_.prev_node[z] = prev_u;
          if (wf_nodes_.right_trail[prev_u].is_empty() ||
              wf_nodes_.right_trail[prev_u].last() != s_a_star)
          {
            wf_nodes_.right_trail[prev_u].append(s_a_star);
          }
          if (wf_nodes_.left_trail[z].is_empty() || wf_nodes_.left_trail[z].last() != s_a_star) {
            wf_nodes_.left_trail[z].append(s_a_star);
          }
        }
      }

      /* Handle right endpoint p_b (s_b): */
      if (l1 > s_w + cluster_tol) {
        /* Edge 1 extends to the right of s_b: surviving piece of Edge 1 starts at p_b,
         * goes to v. */
        wf_nodes_.pos[u] = p_b;
        wf_nodes_.skel_node[u] = s_b_star;
        wf_nodes_.next_node[u] = v;
        wf_nodes_.prev_node[v] = u;
        if (prev_w != u && prev_w >= 0 && wf_nodes_.is_active[prev_w]) {
          wf_nodes_.prev_node[u] = prev_w;
          wf_nodes_.next_node[prev_w] = u;
          if (wf_nodes_.right_trail[prev_w].is_empty() ||
              wf_nodes_.right_trail[prev_w].last() != s_b_star)
          {
            wf_nodes_.right_trail[prev_w].append(s_b_star);
          }
        }
        if (wf_nodes_.left_trail[u].is_empty() || wf_nodes_.left_trail[u].last() != s_a_star) {
          wf_nodes_.left_trail[u].append(s_a_star);
        }
        if (wf_nodes_.left_trail[u].last() != s_b_star) {
          wf_nodes_.left_trail[u].append(s_b_star);
        }
      }
      else if (s_w > l1 + cluster_tol) {
        /* Edge 2 extends to the right of s_b: surviving piece of Edge 2 starts at w, ends
         * at p_b. */
        wf_nodes_.next_node[w] = v;
        wf_nodes_.prev_node[v] = w;
        wf_nodes_.pos[v] = p_b;
        wf_nodes_.skel_node[v] = s_b_star;
        if (wf_nodes_.right_trail[w].is_empty() || wf_nodes_.right_trail[w].last() != s_a_star) {
          wf_nodes_.right_trail[w].append(s_a_star);
        }
        if (wf_nodes_.right_trail[w].last() != s_b_star) {
          wf_nodes_.right_trail[w].append(s_b_star);
        }
        if (wf_nodes_.left_trail[v].is_empty() || wf_nodes_.left_trail[v].last() != s_b_star) {
          wf_nodes_.left_trail[v].append(s_b_star);
        }
      }
      else {
        /* Endpoints coincide at s_b. */
        if (prev_w == v && v >= 0 && wf_nodes_.is_active[v]) {
          if (!dry_run_) {
            Vector<int> face_verts;
            face_verts.append(wf_nodes_.left_trail[v][0]);
            for (const int r : wf_nodes_.right_trail[v]) {
              if (face_verts.last() != r) {
                face_verts.append(r);
              }
            }
            if (face_verts.last() != s_b_star) {
              face_verts.append(s_b_star);
            }
            for (int k = int(wf_nodes_.left_trail[v].size()) - 1; k >= 1; k--) {
              const int l = wf_nodes_.left_trail[v][k];
              if (face_verts.last() != l && l != face_verts[0]) {
                face_verts.append(l);
              }
            }
            this->emit_face(std::move(face_verts), wf_nodes_.orig_face[v], wf_nodes_.orig_edge[v]);
          }
          this->deactivate_node(v);
        }
        else if (prev_w != u && prev_w != v && prev_w >= 0 && v != w && v >= 0 &&
                 wf_nodes_.is_active[prev_w] && wf_nodes_.is_active[v])
        {
          wf_nodes_.next_node[prev_w] = v;
          wf_nodes_.prev_node[v] = prev_w;
          if (wf_nodes_.right_trail[prev_w].is_empty() ||
              wf_nodes_.right_trail[prev_w].last() != s_b_star)
          {
            wf_nodes_.right_trail[prev_w].append(s_b_star);
          }
          if (wf_nodes_.left_trail[v].is_empty() || wf_nodes_.left_trail[v].last() != s_b_star) {
            wf_nodes_.left_trail[v].append(s_b_star);
          }
        }
      }
    }
  }

  this->update_node_velocity(v);
  this->update_node_velocity(z);
  if (prev_u >= 0 && wf_nodes_.is_active[prev_u]) {
    this->update_node_velocity(prev_u);
  }
  if (prev_w >= 0 && wf_nodes_.is_active[prev_w]) {
    this->update_node_velocity(prev_w);
  }
  if (wf_nodes_.is_active[u]) {
    this->update_node_velocity(u);
  }
  if (wf_nodes_.is_active[w]) {
    this->update_node_velocity(w);
  }
  if (new_node_id >= 0 && wf_nodes_.is_active[new_node_id]) {
    this->update_node_velocity(new_node_id);
  }

  return true;
}

inline bool SimulationEngine::find_and_resolve_opposing_brute_force(
    const double cluster_tol, Set<std::pair<int, int>> &processed_pairs)

{
  const int n = int(wf_nodes_.pos.size());
  for (int e1 = 0; e1 < n; e1++) {
    if (!wf_nodes_.is_active[e1]) {
      continue;
    }
    const int orig1 = wf_nodes_.orig_edge[e1];
    if (orig1 >= 0 && orig1 < orig_edge_has_antiparallel_.size() &&
        !orig_edge_has_antiparallel_[orig1])
    {
      continue;
    }
    for (int e2 = e1 + 1; e2 < n; e2++) {
      if (this->try_resolve_opposing_pair(e1, e2, cluster_tol, processed_pairs)) {
        return true;
      }
    }
  }
  return false;
}

inline bool SimulationEngine::find_and_resolve_opposing_grid(
    const double cluster_tol,
    const double2 &bbox_min,
    const double2 &bbox_max,
    const int active_edges_num,
    Set<std::pair<int, int>> &processed_pairs)

{
  const int n = int(wf_nodes_.pos.size());
  const double x_min = bbox_min.x - cluster_tol;
  const double x_max = bbox_max.x + cluster_tol;
  const double y_min = bbox_min.y - cluster_tol;
  const double y_max = bbox_max.y + cluster_tol;

  grid_.init(x_min, y_min, x_max, y_max, active_edges_num);
  for (int i = 0; i < n; i++) {
    if (!wf_nodes_.is_active[i]) {
      continue;
    }
    const int next_i = wf_nodes_.next_node[i];
    if (next_i < 0 || !wf_nodes_.is_active[next_i]) {
      continue;
    }
    const int orig_e = wf_nodes_.orig_edge[i];
    if (orig_e >= 0 && orig_e < orig_edge_has_antiparallel_.size() &&
        !orig_edge_has_antiparallel_[orig_e])
    {
      continue;
    }
    if (math::distance_squared(wf_nodes_.pos[i], wf_nodes_.pos[next_i]) <
        cluster_tol * cluster_tol)
    {
      continue;
    }
    const double2 &p_u = wf_nodes_.pos[i];
    const double2 &p_v = wf_nodes_.pos[next_i];
    const double ex_min = std::min(p_u.x, p_v.x) - cluster_tol;
    const double ex_max = std::max(p_u.x, p_v.x) + cluster_tol;
    const double ey_min = std::min(p_u.y, p_v.y) - cluster_tol;
    const double ey_max = std::max(p_u.y, p_v.y) + cluster_tol;
    grid_.insert(i, ex_min, ey_min, ex_max, ey_max);
  }

  if (query_tags_.size() < wf_nodes_.pos.size()) {
    query_tags_.reinitialize(wf_nodes_.pos.size());
    query_tags_.fill(0);
    current_query_id_ = 1;
  }

  for (int e1 = 0; e1 < n; e1++) {
    if (!wf_nodes_.is_active[e1]) {
      continue;
    }
    const int u = e1;
    const int v = wf_nodes_.next_node[u];
    if (v < 0 || !wf_nodes_.is_active[v]) {
      continue;
    }
    const int orig1 = wf_nodes_.orig_edge[e1];
    if (orig1 >= 0 && orig1 < orig_edge_has_antiparallel_.size() &&
        !orig_edge_has_antiparallel_[orig1])
    {
      continue;
    }
    if (math::distance_squared(wf_nodes_.pos[u], wf_nodes_.pos[v]) < cluster_tol * cluster_tol) {
      continue;
    }

    const double qx_min = std::min(wf_nodes_.pos[u].x, wf_nodes_.pos[v].x) - cluster_tol;
    const double qx_max = std::max(wf_nodes_.pos[u].x, wf_nodes_.pos[v].x) + cluster_tol;
    const double qy_min = std::min(wf_nodes_.pos[u].y, wf_nodes_.pos[v].y) - cluster_tol;
    const double qy_max = std::max(wf_nodes_.pos[u].y, wf_nodes_.pos[v].y) + cluster_tol;

    current_query_id_++;
    if (current_query_id_ <= 0) {
      query_tags_.fill(0);
      current_query_id_ = 1;
    }
    const int qid = current_query_id_;

    bool found_opposing = false;
    grid_.for_each_overlapping(qx_min, qy_min, qx_max, qy_max, [&](const int e2) {
      if (found_opposing || e2 <= e1 || !wf_nodes_.is_active[e2]) {
        return;
      }
      if (query_tags_[e2] == qid) {
        return;
      }
      query_tags_[e2] = qid;

      if (this->try_resolve_opposing_pair(e1, e2, cluster_tol, processed_pairs)) {
        found_opposing = true;
      }
    });

    if (found_opposing) {
      return true;
    }
  }
  return false;
}

inline void SimulationEngine::resolve_opposing_overlapping_edges(const double cluster_tol)

{
  PRF_scope(ProfileCategory::Core);

  if (!has_any_antiparallel_edges_) {
    return;
  }

  Set<std::pair<int, int>> processed_pairs;
  int opp_iter = 0;
  bool found_opposing = true;
  bool any_opposing_resolved = false;
  while (found_opposing && opp_iter < 500) {
    opp_iter++;
    found_opposing = false;
    const int n = int(wf_nodes_.pos.size());

    int active_edges_num = 0;
    double2 bbox_min(1e30, 1e30), bbox_max(-1e30, -1e30);
    for (int i = 0; i < n; i++) {
      if (!wf_nodes_.is_active[i]) {
        continue;
      }
      const int next_i = wf_nodes_.next_node[i];
      if (next_i < 0 || !wf_nodes_.is_active[next_i]) {
        continue;
      }
      const int orig_e = wf_nodes_.orig_edge[i];
      if (orig_e >= 0 && orig_e < orig_edge_has_antiparallel_.size() &&
          !orig_edge_has_antiparallel_[orig_e])
      {
        continue;
      }
      if (math::distance_squared(wf_nodes_.pos[i], wf_nodes_.pos[next_i]) <
          cluster_tol * cluster_tol)
      {
        continue;
      }
      active_edges_num++;
      math::min_max(wf_nodes_.pos[i], bbox_min, bbox_max);
      math::min_max(wf_nodes_.pos[next_i], bbox_min, bbox_max);
    }

    if (active_edges_num <= 1) {
      break;
    }

    if (active_edges_num <= 16) {
      found_opposing = this->find_and_resolve_opposing_brute_force(cluster_tol, processed_pairs);
    }
    else {
      found_opposing = this->find_and_resolve_opposing_grid(
          cluster_tol, bbox_min, bbox_max, active_edges_num, processed_pairs);
    }
    if (found_opposing) {
      any_opposing_resolved = true;
    }
  }
  if (any_opposing_resolved) {
    this->rebuild_active_contours();
  }
}

inline bool SimulationEngine::annihilate_collapsing_contours(const Span<bool> is_edge_collapsing,
                                                             const double cluster_tol)

{
  bool any_contour_collapsed = false;
  for (const int cid : wf_contours_.is_active.index_range()) {
    if (!wf_contours_.is_active[cid]) {
      continue;
    }
    const int head = wf_contours_.head_node[cid];
    if (head < 0 || !wf_nodes_.is_active[head]) {
      continue;
    }

    int curr = head;
    int active_count = 0;
    int collapsing_count = 0;
    double2 p_sum(0.0, 0.0);

    do {
      active_count++;
      p_sum += wf_nodes_.pos[curr];
      if (is_edge_collapsing[curr]) {
        collapsing_count++;
      }
      curr = wf_nodes_.next_node[curr];
    } while (curr != head && curr >= 0 && active_count < int(wf_nodes_.pos.size()));

    if (active_count >= 3 && collapsing_count == active_count) {
      const double2 p_star = p_sum / double(active_count);
      const int s_star = this->get_or_create_skel_node(p_star, cluster_tol);

      curr = head;
      do {
        const int next_curr = wf_nodes_.next_node[curr];
        if (!dry_run_) {
          Vector<int> face_verts;
          face_verts.append(wf_nodes_.left_trail[curr][0]);
          for (const int r : wf_nodes_.right_trail[curr]) {
            if (face_verts.last() != r) {
              face_verts.append(r);
            }
          }
          if (face_verts.last() != s_star) {
            face_verts.append(s_star);
          }
          for (int k = int(wf_nodes_.left_trail[curr].size()) - 1; k >= 1; k--) {
            const int l = wf_nodes_.left_trail[curr][k];
            if (face_verts.last() != l && l != face_verts[0]) {
              face_verts.append(l);
            }
          }
          this->emit_face(
              std::move(face_verts), wf_nodes_.orig_face[curr], wf_nodes_.orig_edge[curr]);
        }

        skel_graph_.edges_node1.append(wf_nodes_.skel_node[curr]);
        skel_graph_.edges_node2.append(s_star);
        skel_graph_.edges_orig.append(-1);
        skel_graph_.edges_face.append(wf_nodes_.orig_face[curr]);
        skel_graph_.edges_time_start.append(wf_nodes_.time_birth[curr]);
        skel_graph_.edges_time_end.append(current_time_);
        skel_graph_.edges_is_boundary.append(false);

        this->deactivate_node(curr);
        curr = next_curr;
      } while (curr != head && curr >= 0);

      wf_contours_.is_active[cid] = false;
      any_contour_collapsed = true;
    }
  }
  return any_contour_collapsed;
}

inline void SimulationEngine::collect_simultaneous_events(const Vector<int> &collapsing_edges,
                                                          const double cluster_tol,
                                                          Vector<VertPair> &r_vert_collisions,
                                                          Vector<SplitInfo> &r_splits)

{
  double2 bbox_min(1e30, 1e30), bbox_max(-1e30, -1e30);
  int active_nodes_num = 0;
  Vector<int> reflex_nodes;

  for (const int u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[u]) {
      continue;
    }
    active_nodes_num++;
    math::min_max(wf_nodes_.pos[u], bbox_min, bbox_max);

    if (this->is_reflex(u)) {
      reflex_nodes.append(u);
    }
  }

  double x_min = bbox_min.x, x_max = bbox_max.x;
  double y_min = bbox_min.y, y_max = bbox_max.y;

  if (active_nodes_num <= 16) {
    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      for (const int w : wf_nodes_.pos.index_range()) {
        if (w <= u || !wf_nodes_.is_active[w]) {
          continue;
        }
        if (wf_nodes_.next_node[u] == w || wf_nodes_.next_node[w] == u) {
          continue;
        }
        if (math::length(wf_nodes_.pos[w] - wf_nodes_.pos[u]) < cluster_tol) {
          r_vert_collisions.append(VertPair{u, w});
        }
      }
    }

    for (const int w : reflex_nodes) {
      const int prev_w = wf_nodes_.prev_node[w];
      const int next_w = wf_nodes_.next_node[w];
      for (const int u : wf_nodes_.pos.index_range()) {
        if (!wf_nodes_.is_active[u]) {
          continue;
        }
        const int v = wf_nodes_.next_node[u];
        if (v < 0 || !wf_nodes_.is_active[v]) {
          continue;
        }
        if (u == w || v == w || u == prev_w || v == prev_w || u == next_w || v == next_w) {
          continue;
        }
        if (collapsing_edges.contains(u)) {
          continue;
        }

        const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
        const double dist = std::abs(math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv));
        if (dist < cluster_tol) {
          const double s = math::line_point_factor(
              wf_nodes_.pos[w], wf_nodes_.pos[u], wf_nodes_.pos[v]);
          if (s > 1e-5 && s < 1.0 - 1e-5) {
            r_splits.append(SplitInfo{w, u, wf_nodes_.pos[w]});
          }
        }
      }
    }
  }
  else {
    x_min -= cluster_tol;
    x_max += cluster_tol;
    y_min -= cluster_tol;
    y_max += cluster_tol;

    vert_grid_.init(x_min, y_min, x_max, y_max, active_nodes_num);
    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const double2 &p = wf_nodes_.pos[u];
      vert_grid_.insert(
          u, p.x - cluster_tol, p.y - cluster_tol, p.x + cluster_tol, p.y + cluster_tol);
    }

    if (query_tags_.size() < wf_nodes_.pos.size()) {
      query_tags_.reinitialize(wf_nodes_.pos.size());
      query_tags_.fill(0);
      current_query_id_ = 1;
    }

    /* Vertex-vertex collisions. */
    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const double2 &p_u = wf_nodes_.pos[u];
      current_query_id_++;
      if (current_query_id_ <= 0) {
        query_tags_.fill(0);
        current_query_id_ = 1;
      }
      const int qid = current_query_id_;

      vert_grid_.for_each_overlapping(
          p_u.x - cluster_tol,
          p_u.y - cluster_tol,
          p_u.x + cluster_tol,
          p_u.y + cluster_tol,
          [&](const int w) {
            if (w <= u || !wf_nodes_.is_active[w]) {
              return;
            }
            if (query_tags_[w] == qid) {
              return;
            }
            query_tags_[w] = qid;

            if (wf_nodes_.next_node[u] == w || wf_nodes_.next_node[w] == u) {
              return;
            }
            if (math::length(wf_nodes_.pos[w] - wf_nodes_.pos[u]) < cluster_tol) {
              r_vert_collisions.append(VertPair{u, w});
            }
          });
    }

    /* Split events at current_time_. */
    if (!reflex_nodes.is_empty()) {
      grid_.init(x_min, y_min, x_max, y_max, active_nodes_num);
      for (const int u : wf_nodes_.pos.index_range()) {
        if (!wf_nodes_.is_active[u]) {
          continue;
        }
        const int v = wf_nodes_.next_node[u];
        if (v < 0 || !wf_nodes_.is_active[v]) {
          continue;
        }
        const double2 &p_u = wf_nodes_.pos[u];
        const double2 &p_v = wf_nodes_.pos[v];
        const double ex_min = std::min(p_u.x, p_v.x) - cluster_tol;
        const double ex_max = std::max(p_u.x, p_v.x) + cluster_tol;
        const double ey_min = std::min(p_u.y, p_v.y) - cluster_tol;
        const double ey_max = std::max(p_u.y, p_v.y) + cluster_tol;
        grid_.insert(u, ex_min, ey_min, ex_max, ey_max);
      }

      for (const int w : reflex_nodes) {
        const int prev_w = wf_nodes_.prev_node[w];
        const int next_w = wf_nodes_.next_node[w];
        const double2 &p_w = wf_nodes_.pos[w];

        current_query_id_++;
        if (current_query_id_ <= 0) {
          query_tags_.fill(0);
          current_query_id_ = 1;
        }
        const int qid = current_query_id_;

        grid_.for_each_overlapping(
            p_w.x - cluster_tol,
            p_w.y - cluster_tol,
            p_w.x + cluster_tol,
            p_w.y + cluster_tol,
            [&](const int u) {
              if (query_tags_[u] == qid) {
                return;
              }
              query_tags_[u] = qid;

              const int v = wf_nodes_.next_node[u];
              if (v < 0 || !wf_nodes_.is_active[v]) {
                return;
              }
              if (u == w || v == w || u == prev_w || v == prev_w || u == next_w || v == next_w) {
                return;
              }
              if (collapsing_edges.contains(u)) {
                return;
              }

              const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
              const double dist = std::abs(math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv));
              if (dist < cluster_tol) {
                const double s = math::line_point_factor(
                    wf_nodes_.pos[w], wf_nodes_.pos[u], wf_nodes_.pos[v]);
                if (s > 1e-5 && s < 1.0 - 1e-5) {
                  if (w == 59 || u == 59) {
                  }
                  r_splits.append(SplitInfo{w, u, wf_nodes_.pos[w]});
                }
              }
            });
      }
    }
  }
}

inline void SimulationEngine::group_events_into_clusters(const double cluster_tol,
                                                         const Vector<int> &collapsing_edges,
                                                         const Vector<VertPair> &vert_collisions,
                                                         const Vector<SplitInfo> &splits,
                                                         Vector<EventCluster> &r_clusters)

{
  Array<int> vert_to_cluster(wf_nodes_.pos.size(), -1);
  Array<int> edge_to_cluster(wf_nodes_.pos.size(), -1);

  auto add_edge_to_cluster = [&](const int ci, const int e) {
    if (edge_to_cluster[e] == -1) {
      r_clusters[ci].collapsing_edges.append(e);
      edge_to_cluster[e] = ci;
    }
  };

  auto add_vert_to_cluster = [&](const int ci, const int v) {
    if (vert_to_cluster[v] == -1) {
      r_clusters[ci].colliding_verts.append(v);
      vert_to_cluster[v] = ci;
    }
  };

  const double spatial_tol = std::max(cluster_tol, DEFAULT_CLUSTER_TOL);
  const double inv_spatial_tol = 1.0 / spatial_tol;
  Map<int64_t, Vector<int>> cluster_grid;

  auto find_nearby_cluster = [&](const double2 &p) -> int {
    const int64_t qx = int64_t(std::floor(p.x * inv_spatial_tol));
    const int64_t qy = int64_t(std::floor(p.y * inv_spatial_tol));
    for (int dx = -1; dx <= 1; dx++) {
      for (int dy = -1; dy <= 1; dy++) {
        const int64_t key = ((qx + dx) * 73856093) ^ ((qy + dy) * 19349663);
        if (const Vector<int> *cell = cluster_grid.lookup_ptr(key)) {
          for (const int ci : *cell) {
            if (math::length(r_clusters[ci].p_star - p) < spatial_tol) {
              return ci;
            }
          }
        }
      }
    }
    return -1;
  };

  auto register_cluster = [&](int ci, const double2 &p) {
    const int64_t qx = int64_t(std::floor(p.x * inv_spatial_tol));
    const int64_t qy = int64_t(std::floor(p.y * inv_spatial_tol));
    const int64_t key = (qx * 73856093) ^ (qy * 19349663);
    cluster_grid.lookup_or_add_default(key).append(ci);
  };

  for (const int e : collapsing_edges) {
    const int v = wf_nodes_.next_node[e];
    const double2 p = 0.5 * (wf_nodes_.pos[e] + wf_nodes_.pos[v]);
    int found_ci = -1;
    if (edge_to_cluster[e] != -1) {
      found_ci = edge_to_cluster[e];
    }
    else if (vert_to_cluster[e] != -1) {
      found_ci = vert_to_cluster[e];
    }
    else if (v >= 0 && vert_to_cluster[v] != -1) {
      found_ci = vert_to_cluster[v];
    }
    else {
      found_ci = find_nearby_cluster(p);
    }

    if (found_ci == -1) {
      EventCluster c;
      c.p_star = p;
      const int new_ci = int(r_clusters.size());
      r_clusters.append(std::move(c));
      register_cluster(new_ci, p);
      add_edge_to_cluster(new_ci, e);
      add_vert_to_cluster(new_ci, e);
      if (v >= 0) {
        add_vert_to_cluster(new_ci, v);
      }
    }
    else {
      add_edge_to_cluster(found_ci, e);
      add_vert_to_cluster(found_ci, e);
      if (v >= 0) {
        add_vert_to_cluster(found_ci, v);
      }
    }
  }

  for (const VertPair &vp : vert_collisions) {
    const double2 p = 0.5 * (wf_nodes_.pos[vp.u] + wf_nodes_.pos[vp.w]);
    int found_ci = -1;
    if (vert_to_cluster[vp.u] != -1) {
      found_ci = vert_to_cluster[vp.u];
    }
    else if (vert_to_cluster[vp.w] != -1) {
      found_ci = vert_to_cluster[vp.w];
    }
    else {
      found_ci = find_nearby_cluster(p);
    }

    if (found_ci == -1) {
      EventCluster c;
      c.p_star = p;
      const int new_ci = int(r_clusters.size());
      r_clusters.append(std::move(c));
      register_cluster(new_ci, p);
      add_vert_to_cluster(new_ci, vp.u);
      add_vert_to_cluster(new_ci, vp.w);
    }
    else {
      add_vert_to_cluster(found_ci, vp.u);
      add_vert_to_cluster(found_ci, vp.w);
    }
  }

  for (const SplitInfo &si : splits) {
    const double2 p = si.pos;
    const int u = si.edge_u;
    const int v = (u >= 0 && u < wf_nodes_.next_node.size()) ? wf_nodes_.next_node[u] : -1;
    int found_ci = -1;
    if (vert_to_cluster[si.reflex_w] != -1) {
      found_ci = vert_to_cluster[si.reflex_w];
    }
    else if (edge_to_cluster[u] != -1 &&
             math::length(r_clusters[edge_to_cluster[u]].p_star - p) < spatial_tol)
    {
      found_ci = edge_to_cluster[u];
    }
    else if (vert_to_cluster[u] != -1 && math::length(wf_nodes_.pos[u] - p) < spatial_tol) {
      found_ci = vert_to_cluster[u];
    }
    else if (v != -1 && vert_to_cluster[v] != -1 &&
             math::length(wf_nodes_.pos[v] - p) < spatial_tol)
    {
      found_ci = vert_to_cluster[v];
    }
    else {
      found_ci = find_nearby_cluster(p);
    }

    if (found_ci == -1) {
      EventCluster c;
      c.p_star = p;
      const int new_ci = int(r_clusters.size());
      r_clusters.append(std::move(c));
      register_cluster(new_ci, p);
      add_vert_to_cluster(new_ci, si.reflex_w);
      if (!r_clusters[new_ci].split_edges.contains(si.edge_u)) {
        r_clusters[new_ci].split_edges.append(si.edge_u);
      }
    }
    else {
      add_vert_to_cluster(found_ci, si.reflex_w);
      if (!r_clusters[found_ci].split_edges.contains(si.edge_u)) {
        r_clusters[found_ci].split_edges.append(si.edge_u);
      }
    }
  }
}

inline void SimulationEngine::process_event_clusters(Vector<EventCluster> &clusters,
                                                     const double cluster_tol)

{
  /* Create Skeleton Nodes and bind colliding vertices across all clusters. */
  for (EventCluster &cluster : clusters) {
    double2 p_sum(0.0, 0.0);
    int count = 0;
    for (const int v : cluster.colliding_verts) {
      p_sum += wf_nodes_.pos[v];
      count++;
    }
    if (count > 0) {
      cluster.p_star = p_sum / double(count);
    }
    cluster.s_star = this->get_or_create_skel_node(cluster.p_star, cluster_tol);
    for (const int v : cluster.colliding_verts) {
      wf_nodes_.skel_node[v] = cluster.s_star;
    }
  }

  /* Any edge whose BOTH endpoints are in the cluster has collapsed to p*. */
  for (EventCluster &cluster : clusters) {
    for (const int u : cluster.colliding_verts) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const int v = wf_nodes_.next_node[u];
      if (v >= 0 && cluster.colliding_verts.contains(v)) {
        if (!cluster.collapsing_edges.contains(u)) {
          cluster.collapsing_edges.append(u);
        }
      }
    }
  }

  /* Emit Roof Faces for point-collapsing edges and splice topology. */
  for (EventCluster &cluster : clusters) {
    const int s_star = cluster.s_star;
    for (const int e : cluster.collapsing_edges) {
      if (!wf_nodes_.is_active[e]) {
        continue;
      }
      const int u = e;
      const int v = wf_nodes_.next_node[u];

      if (!dry_run_) {
        Vector<int> face_verts;
        face_verts.append(wf_nodes_.left_trail[e][0]);
        for (const int r : wf_nodes_.right_trail[e]) {
          if (face_verts.last() != r) {
            face_verts.append(r);
          }
        }
        if (face_verts.last() != s_star) {
          face_verts.append(s_star);
        }
        for (int k = int(wf_nodes_.left_trail[e].size()) - 1; k >= 1; k--) {
          const int l = wf_nodes_.left_trail[e][k];
          if (face_verts.last() != l && l != face_verts[0]) {
            face_verts.append(l);
          }
        }
        this->emit_face(std::move(face_verts), wf_nodes_.orig_face[e], wf_nodes_.orig_edge[e]);
      }

      skel_graph_.edges_node1.append(wf_nodes_.skel_node[u]);
      skel_graph_.edges_node2.append(s_star);
      skel_graph_.edges_orig.append(-1);
      skel_graph_.edges_face.append(wf_nodes_.orig_face[e]);
      skel_graph_.edges_time_start.append(wf_nodes_.time_birth[e]);
      skel_graph_.edges_time_end.append(current_time_);
      skel_graph_.edges_is_boundary.append(false);

      const int prev_u = wf_nodes_.prev_node[u];
      this->deactivate_node(u);

      if (prev_u != u && prev_u != v && prev_u >= 0 && v >= 0) {
        wf_nodes_.next_node[prev_u] = v;
        wf_nodes_.prev_node[v] = prev_u;
        wf_nodes_.pos[v] = cluster.p_star;
        wf_nodes_.skel_node[v] = s_star;
        if (wf_nodes_.right_trail[prev_u].is_empty() ||
            wf_nodes_.right_trail[prev_u].last() != s_star)
        {
          wf_nodes_.right_trail[prev_u].append(s_star);
        }
        if (wf_nodes_.left_trail[v].is_empty() || wf_nodes_.left_trail[v].last() != s_star) {
          wf_nodes_.left_trail[v].append(s_star);
        }
      }
    }
  }

  /* Resolve remaining clusters via Radial Gap Pairing. */
  for (const EventCluster &cluster : clusters) {
    if (!cluster.split_edges.is_empty() || cluster.collapsing_edges.is_empty()) {
      this->resolve_cluster(cluster);
    }
  }
}

inline void SimulationEngine::resolve_events_at_current_time(double eps)

{
  PRF_scope(ProfileCategory::Core);

  const double cluster_tol = std::max(eps, 1e-4);

  /* Identify all collapsing edges at current_time_. */
  Array<bool> is_edge_collapsing(wf_nodes_.pos.size(), false);
  Vector<int> collapsing_edges;
  for (const int u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[u]) {
      continue;
    }
    const int v = wf_nodes_.next_node[u];
    if (math::distance_squared(wf_nodes_.pos[v], wf_nodes_.pos[u]) < cluster_tol * cluster_tol) {
      collapsing_edges.append(u);
      is_edge_collapsing[u] = true;
    }
  }

  if (!collapsing_edges.is_empty()) {
    if (this->annihilate_collapsing_contours(is_edge_collapsing.as_span(), cluster_tol)) {
      if (!this->has_active_contours()) {
        events_processed_count_++;
        return;
      }
      collapsing_edges.remove_if([&](const int e) { return !wf_nodes_.is_active[e]; });
    }
  }

  /* Resolve any opposing collinear/overlapping wavefront edges. */
  this->resolve_opposing_overlapping_edges(cluster_tol);

  Vector<VertPair> vert_collisions;
  Vector<SplitInfo> splits;
  this->collect_simultaneous_events(collapsing_edges, cluster_tol, vert_collisions, splits);

  /* Group all interactions into spatial Event Clusters. */
  Vector<EventCluster> clusters;
  this->group_events_into_clusters(
      cluster_tol, collapsing_edges, vert_collisions, splits, clusters);

  /* Process event clusters. */
  this->process_event_clusters(clusters, cluster_tol);

  /* Rebuild and synchronize active contour loops. */
  this->rebuild_active_contours();

  for (const int i : wf_nodes_.pos.index_range()) {
    if (wf_nodes_.is_active[i]) {
      this->update_node_velocity(i);
    }
  }

  events_processed_count_++;
}

inline void SimulationEngine::resolve_cluster(const EventCluster &cluster)

{
  PRF_scope(ProfileCategory::Core);

  const double2 p_star = cluster.p_star;
  const int s_star = cluster.s_star;

  /* Add skeleton edges for other colliding vertices. */
  for (const int v : cluster.colliding_verts) {
    if (!cluster.collapsing_edges.contains(v) &&
        !cluster.collapsing_edges.contains(wf_nodes_.prev_node[v]))
    {
      skel_graph_.edges_node1.append(wf_nodes_.skel_node[v]);
      skel_graph_.edges_node2.append(s_star);
      skel_graph_.edges_orig.append(-1);
      skel_graph_.edges_face.append(wf_nodes_.orig_face[v]);
      skel_graph_.edges_time_start.append(wf_nodes_.time_birth[v]);
      skel_graph_.edges_time_end.append(current_time_);
      skel_graph_.edges_is_boundary.append(false);
    }
  }

  /* Collect Surviving Non-Collapsing Rays incident to p*. */
  enum class RayType { IN, OUT };
  struct Ray {
    double angle;
    RayType type;
    int incident_node;
    int retiring_node;
    int orig_edge;
    int orig_face;
  };
  Vector<Ray> rays;

  for (const int v : cluster.colliding_verts) {
    const int prev_v = wf_nodes_.prev_node[v];
    if (prev_v >= 0 && !cluster.colliding_verts.contains(prev_v) && wf_nodes_.is_active[prev_v]) {
      bool already_have_in = false;
      for (const Ray &r : rays) {
        if (r.type == RayType::IN && r.incident_node == prev_v) {
          already_have_in = true;
          break;
        }
      }
      if (!already_have_in) {
        const double2 d = wf_nodes_.pos[prev_v] - p_star;
        if (math::length(d) > 1e-4) {
          const double angle = std::atan2(d.y, d.x);
          rays.append(Ray{angle,
                          RayType::IN,
                          prev_v,
                          v,
                          wf_nodes_.orig_edge[prev_v],
                          wf_nodes_.orig_face[prev_v]});
        }
      }
    }

    const int next_v = wf_nodes_.next_node[v];
    if (next_v >= 0 && !cluster.colliding_verts.contains(next_v) && wf_nodes_.is_active[next_v]) {
      bool already_have_out = false;
      for (const Ray &r : rays) {
        if (r.type == RayType::OUT && r.incident_node == next_v) {
          already_have_out = true;
          break;
        }
      }
      if (!already_have_out) {
        const double2 d = wf_nodes_.pos[next_v] - p_star;
        if (math::length(d) > 1e-4) {
          const double angle = std::atan2(d.y, d.x);
          rays.append(
              Ray{angle, RayType::OUT, next_v, v, wf_nodes_.orig_edge[v], wf_nodes_.orig_face[v]});
        }
      }
    }
  }

  const double cluster_tol = std::max(input_.epsilon, DEFAULT_CLUSTER_TOL);

  for (const int e_init : cluster.split_edges) {
    int e = e_init;
    const int target_orig = (e >= 0 && e < wf_nodes_.orig_edge.size()) ? wf_nodes_.orig_edge[e] :
                                                                         -1;

    auto edge_contains_pt = [&](const int cand) -> bool {
      if (cand < 0 || !wf_nodes_.is_active[cand]) {
        return false;
      }
      const int cand_v = wf_nodes_.next_node[cand];
      if (cand_v < 0 || !wf_nodes_.is_active[cand_v]) {
        return false;
      }
      const double2 &p_a = wf_nodes_.pos[cand];
      const double2 &p_b = wf_nodes_.pos[cand_v];
      const double len_sq = math::distance_squared(p_a, p_b);
      if (len_sq < 1e-10) {
        return false;
      }
      const double s = math::line_point_factor(p_star, p_a, p_b);
      if (s >= -1e-4 && s <= 1.0 + 1e-4) {
        const double2 n = math::orthogonal(math::normalize(p_b - p_a));
        if (std::abs(math::dot(p_star - p_a, n)) < std::max(cluster_tol, 1e-3)) {
          return true;
        }
      }
      return false;
    };

    if (!edge_contains_pt(e) && target_orig != -1) {
      int best_cand = -1;
      double best_dist = 1e30;
      for (const int cand : wf_nodes_.pos.index_range()) {
        if (!wf_nodes_.is_active[cand] || wf_nodes_.orig_edge[cand] != target_orig) {
          continue;
        }
        if (edge_contains_pt(cand)) {
          const int cand_v = wf_nodes_.next_node[cand];
          const double2 &p_a = wf_nodes_.pos[cand];
          const double2 &p_b = wf_nodes_.pos[cand_v];
          const double s = math::line_point_factor(p_star, p_a, p_b);
          const double2 proj = p_a + s * (p_b - p_a);
          const double d = math::distance_squared(proj, p_star);
          if (d < best_dist) {
            best_dist = d;
            best_cand = cand;
          }
        }
      }
      if (best_cand != -1) {
        e = best_cand;
      }
    }

    if (!wf_nodes_.is_active[e] || !edge_contains_pt(e)) {
      continue;
    }
    const int v = wf_nodes_.next_node[e];
    if (v < 0 || !wf_nodes_.is_active[v]) {
      continue;
    }
    if (cluster.colliding_verts.contains(e) || cluster.colliding_verts.contains(v)) {
      continue;
    }

    /* Incoming sub-edge e -> p* */
    bool already_have_in = false;
    for (const Ray &r : rays) {
      if (r.type == RayType::IN && r.incident_node == e) {
        already_have_in = true;
        break;
      }
    }
    if (!already_have_in) {
      const double2 d_in = wf_nodes_.pos[e] - p_star;
      const double angle_in = std::atan2(d_in.y, d_in.x);
      rays.append(
          Ray{angle_in, RayType::IN, e, -1, wf_nodes_.orig_edge[e], wf_nodes_.orig_face[e]});
    }

    /* Outgoing sub-edge p* -> v */
    bool already_have_out = false;
    for (const Ray &r : rays) {
      if (r.type == RayType::OUT && r.incident_node == v) {
        already_have_out = true;
        break;
      }
    }
    if (!already_have_out) {
      const double2 d_out = wf_nodes_.pos[v] - p_star;
      const double angle_out = std::atan2(d_out.y, d_out.x);
      rays.append(
          Ray{angle_out, RayType::OUT, v, -1, wf_nodes_.orig_edge[e], wf_nodes_.orig_face[e]});
    }
  }

  auto emit_retiring_face = [&](const int ret) {
    if (ret < 0 || dry_run_) {
      return;
    }
    Vector<int> face_verts;
    face_verts.append(wf_nodes_.left_trail[ret][0]);
    for (const int r : wf_nodes_.right_trail[ret]) {
      if (face_verts.last() != r) {
        face_verts.append(r);
      }
    }
    if (face_verts.last() != s_star) {
      face_verts.append(s_star);
    }
    for (int k = int(wf_nodes_.left_trail[ret].size()) - 1; k >= 1; k--) {
      const int l = wf_nodes_.left_trail[ret][k];
      if (face_verts.last() != l && l != face_verts[0]) {
        face_verts.append(l);
      }
    }
    this->emit_face(std::move(face_verts), wf_nodes_.orig_face[ret], wf_nodes_.orig_edge[ret]);
  };

  /* Cancel opposing coincident ray pairs that form zero-angle antennas. */
  bool found_opposing = true;
  while (found_opposing) {
    found_opposing = false;
    for (int a = 0; a < rays.size(); a++) {
      for (int b = a + 1; b < rays.size(); b++) {
        if (rays[a].type != rays[b].type && rays[a].incident_node == rays[b].incident_node) {
          double diff = std::abs(rays[a].angle - rays[b].angle);
          if (diff < 1e-4 || std::abs(diff - 2.0 * M_PI) < 1e-4) {
            const Ray &ray_in = (rays[a].type == RayType::IN) ? rays[a] : rays[b];
            const Ray &ray_out = (rays[a].type == RayType::OUT) ? rays[a] : rays[b];
            const int inc_node = ray_in.incident_node;

            emit_retiring_face(ray_in.incident_node);
            emit_retiring_face(ray_out.retiring_node);

            if (inc_node >= 0) {
              const int inc_skel = wf_nodes_.skel_node[inc_node];
              if (inc_skel != s_star) {
                skel_graph_.edges_node1.append(inc_skel);
                skel_graph_.edges_node2.append(s_star);
                skel_graph_.edges_orig.append(-1);
                skel_graph_.edges_face.append(rays[a].orig_face);
                skel_graph_.edges_time_start.append(current_time_);
                skel_graph_.edges_time_end.append(current_time_);
                skel_graph_.edges_is_boundary.append(false);
              }
              this->deactivate_node(inc_node);
            }

            rays.remove(b);
            rays.remove(a);
            found_opposing = true;
            break;
          }
        }
      }
      if (found_opposing) {
        break;
      }
    }
  }

  /* Retire all colliding vertices. */
  for (const int v : cluster.colliding_verts) {
    this->deactivate_node(v);
  }

  if (rays.size() < 2) {
    return;
  }

  /* Sort rays cyclically by polar angle.
   * When angles are equal, IN precedes OUT so that IN closes the preceding
   * clockwise sector before OUT opens the subsequent counter-clockwise sector. */
  std::sort(rays.begin(), rays.end(), [](const Ray &a, const Ray &b) {
    if (std::abs(a.angle - b.angle) > 1e-6) {
      return a.angle < b.angle;
    }
    if (a.type != b.type) {
      return a.type == RayType::IN;
    }
    return false;
  });

  /* Find all interior sectors (Gaps): each OUT ray followed by an IN ray.
   *
   * Cyclic Ray Sorting and Deterministic Gap Pairing:
   * ==================================================
   * ```
   *                             IN Ray (ray_in)
   *                                  ^
   *                                  |
   *                      EXTERIOR    |    INTERIOR SECTOR
   *                       SECTOR     |        ("GAP")
   *                                  * p*
   *                                 /
   *                                /
   *                               v
   *                         OUT Ray (ray_out)
   * ```
   * Sweeping CCW around p*:
   *   ray_out (angle theta_out) -> ray_in (angle theta_in) defines the interior gap.
   *   New node w_g is born at p*:
   *     prev[w_g] = ray_in.incident_node
   *     next[w_g] = ray_out.incident_node
   */
  const int n_rays = int(rays.size());
  auto spawn_node = [&](const Ray &ray_in, const Ray &ray_out) {
    const int w_g = int(wf_nodes_.pos.size());
    wf_nodes_.pos.append(p_star);
    wf_nodes_.vel.append(double2(0.0, 0.0));
    wf_nodes_.prev_node.append(ray_in.incident_node);
    wf_nodes_.next_node.append(ray_out.incident_node);
    wf_nodes_.skel_node.append(s_star);
    wf_nodes_.orig_edge.append(ray_out.orig_edge);
    wf_nodes_.orig_face.append(ray_out.orig_face);
    wf_nodes_.contour_id.append(wf_nodes_.contour_id[ray_in.incident_node]);
    wf_nodes_.is_active.append(true);
    wf_nodes_.time_birth.append(current_time_);

    Vector<int> lt;
    if (ray_out.retiring_node >= 0 && ray_out.retiring_node < int(wf_nodes_.left_trail.size())) {
      lt = wf_nodes_.left_trail[ray_out.retiring_node];
    }
    if (lt.is_empty() || lt.last() != s_star) {
      lt.append(s_star);
    }
    wf_nodes_.left_trail.append(std::move(lt));

    Vector<int> rt;
    if (ray_out.retiring_node >= 0 && ray_out.retiring_node < int(wf_nodes_.right_trail.size())) {
      rt = wf_nodes_.right_trail[ray_out.retiring_node];
    }
    if (rt.is_empty()) {
      rt.append(wf_nodes_.skel_node[ray_out.incident_node]);
    }
    wf_nodes_.right_trail.append(std::move(rt));

    /* Link topology. */
    wf_nodes_.next_node[ray_in.incident_node] = w_g;
    wf_nodes_.prev_node[ray_out.incident_node] = w_g;
    if (wf_nodes_.right_trail[ray_in.incident_node].is_empty() ||
        wf_nodes_.right_trail[ray_in.incident_node].last() != s_star)
    {
      wf_nodes_.right_trail[ray_in.incident_node].append(s_star);
    }

    this->update_node_velocity(w_g);
    this->update_node_velocity(ray_in.incident_node);
    this->update_node_velocity(ray_out.incident_node);
  };

  Array<bool> ray_used(n_rays, false);

  /* Pass 1: Standard CCW OUT -> IN gap pairing. */
  for (const int i : IndexRange(n_rays)) {
    const Ray &ray_out = rays[i];
    const Ray &ray_in = rays[(i + 1) % n_rays];

    if (ray_out.type == RayType::OUT && ray_in.type == RayType::IN) {
      double delta_theta = ray_in.angle - ray_out.angle;
      if (delta_theta <= 1e-6) {
        delta_theta += 2.0 * M_PI;
      }
      if (delta_theta < 1e-4 || delta_theta > 2.0 * M_PI - 1e-4 || delta_theta > M_PI + 1e-4) {
        continue;
      }
      if (ray_used[i] || ray_used[(i + 1) % n_rays]) {
        continue;
      }
      spawn_node(ray_in, ray_out);
      ray_used[i] = true;
      ray_used[(i + 1) % n_rays] = true;
    }
  }

  /* Pass 2: Pair any adjacent unused rays with opposite types (e.g. collinear turnaround). */
  for (const int i : IndexRange(n_rays)) {
    const int next_i = (i + 1) % n_rays;
    if (ray_used[i] || ray_used[next_i]) {
      continue;
    }
    const Ray &r1 = rays[i];
    const Ray &r2 = rays[next_i];
    if (r1.type != r2.type) {
      double delta_theta = std::abs(r2.angle - r1.angle);
      if (delta_theta > M_PI) {
        delta_theta = 2.0 * M_PI - delta_theta;
      }
      if (delta_theta > M_PI - 1e-4) {
        continue;
      }
      const Ray &ray_in = (r1.type == RayType::IN) ? r1 : r2;
      const Ray &ray_out = (r1.type == RayType::OUT) ? r1 : r2;
      spawn_node(ray_in, ray_out);
      ray_used[i] = true;
      ray_used[next_i] = true;
    }
  }

  /* Pass 3: Pair any remaining unused rays among the unused set (handles collinear / split
   * edges). */
  bool paired_any = true;
  while (paired_any) {
    paired_any = false;
    Vector<int> unused;
    for (const int i : IndexRange(n_rays)) {
      if (!ray_used[i]) {
        unused.append(i);
      }
    }
    if (unused.size() < 2) {
      break;
    }
    const int num_u = int(unused.size());
    if (num_u == 2) {
      const int i0 = unused[0];
      const int i1 = unused[1];
      if (rays[i0].type != rays[i1].type) {
        const int out_idx = (rays[i0].type == RayType::OUT) ? i0 : i1;
        const int in_idx = (rays[i0].type == RayType::IN) ? i0 : i1;
        spawn_node(rays[in_idx], rays[out_idx]);
        ray_used[i0] = true;
        ray_used[i1] = true;
        paired_any = true;
        break;
      }
    }
    for (const int u_i : IndexRange(num_u)) {
      const int idx_out = unused[u_i];
      const int idx_in = unused[(u_i + 1) % num_u];
      if (rays[idx_out].type == RayType::OUT && rays[idx_in].type == RayType::IN) {
        spawn_node(rays[idx_in], rays[idx_out]);
        ray_used[idx_out] = true;
        ray_used[idx_in] = true;
        paired_any = true;
        break;
      }
    }
  }
}

inline void SimulationEngine::rebuild_active_contours()

{
  Vector<bool> visited(wf_nodes_.pos.size(), false);
  const Vector<bool> old_is_hole = wf_contours_.is_hole;
  const Vector<bool> old_is_wire = wf_contours_.is_wire;
  wf_contours_.head_node.clear();
  wf_contours_.is_hole.clear();
  wf_contours_.is_active.clear();
  wf_contours_.is_wire.clear();

  struct CandidateContour {
    int head_node;
    Vector<int> nodes;
    bool closed;
    bool is_wire;
    bool is_hole;
    double poly_area;
    double oriented_area;
  };
  Vector<CandidateContour> candidates;

  for (const int i : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[i] || visited[i]) {
      continue;
    }
    Vector<int> contour_nodes;
    int curr = i;
    bool closed = false;
    while (curr >= 0 && curr < int(wf_nodes_.pos.size()) && wf_nodes_.is_active[curr]) {
      if (visited[curr]) {
        if (curr == i) {
          closed = true;
        }
        break;
      }
      visited[curr] = true;
      contour_nodes.append(curr);
      curr = wf_nodes_.next_node[curr];
    }

    bool is_wire_loop = !contour_nodes.is_empty();
    for (const int node : contour_nodes) {
      const int old_cid = wf_nodes_.contour_id[node];
      if (old_cid < 0 || old_cid >= old_is_wire.size() || !old_is_wire[old_cid]) {
        is_wire_loop = false;
        break;
      }
    }

    /* A contour is a hole wavefront (clockwise, negative signed area) only if every node
     * still belongs to a contour that was a hole. Once a hole merges with an outer contour
     * the result is a regular counter-clockwise contour. */
    bool is_hole_loop = !contour_nodes.is_empty();
    for (const int node : contour_nodes) {
      const int old_cid = wf_nodes_.contour_id[node];
      if (old_cid < 0 || old_cid >= old_is_hole.size() || !old_is_hole[old_cid]) {
        is_hole_loop = false;
        break;
      }
    }

    const double poly_area = polygon_signed_area(contour_nodes.as_span(), wf_nodes_.pos.as_span());
    /* If a loop has positive signed area (CCW), it encloses an inward-propagating solid region.
     * Even if its nodes originally came from holes (e.g. multiple expanding holes collided and
     * pinched off an island of solid domain), this loop is an active outer contour of that
     * solid island, not an expanding hole. */
    if (poly_area > 1e-6) {
      is_hole_loop = false;
    }
    const double expected_sign = is_hole_loop ? -1.0 : 1.0;
    const double oriented_area = expected_sign * poly_area;

    candidates.append({i,
                       std::move(contour_nodes),
                       closed,
                       is_wire_loop,
                       is_hole_loop,
                       poly_area,
                       oriented_area});
  }

  bool has_active_outer = false;
  for (const CandidateContour &cand : candidates) {
    if (cand.closed && cand.nodes.size() >= 3 && !cand.is_hole && !cand.is_wire &&
        cand.oriented_area > 1e-6)
    {
      has_active_outer = true;
      break;
    }
  }

  for (CandidateContour &cand : candidates) {
    const bool wrong_orientation =
        !cand.is_wire &&
        ((cand.nodes.size() <= 32 && cand.oriented_area <= 1e-6) || (cand.oriented_area < -1e-4) ||
         (cand.is_hole &&
          ((current_time_ > 0.0 && cand.nodes.size() <= 3) ||
           (input_.direction == PropagationDirection::Inside && !has_active_outer))));
    if (!cand.closed || cand.nodes.size() < 3 || wrong_orientation) {
      if (!cand.nodes.is_empty()) {
        double2 p_sum(0.0, 0.0);
        for (const int node : cand.nodes) {
          p_sum += wf_nodes_.pos[node];
        }
        const double2 p_star = p_sum / double(cand.nodes.size());
        const int s_star = this->get_or_create_skel_node(p_star, DEFAULT_CLUSTER_TOL);

        for (const int node : cand.nodes) {
          if (!dry_run_) {
            Vector<int> face_verts;
            face_verts.append(wf_nodes_.left_trail[node][0]);
            for (const int r : wf_nodes_.right_trail[node]) {
              if (face_verts.last() != r) {
                face_verts.append(r);
              }
            }
            if (face_verts.last() != s_star) {
              face_verts.append(s_star);
            }
            for (int k = int(wf_nodes_.left_trail[node].size()) - 1; k >= 1; k--) {
              const int l = wf_nodes_.left_trail[node][k];
              if (face_verts.last() != l && l != face_verts[0]) {
                face_verts.append(l);
              }
            }
            this->emit_face(
                std::move(face_verts), wf_nodes_.orig_face[node], wf_nodes_.orig_edge[node]);
          }

          skel_graph_.edges_node1.append(wf_nodes_.skel_node[node]);
          skel_graph_.edges_node2.append(s_star);
          skel_graph_.edges_orig.append(-1);
          skel_graph_.edges_face.append(wf_nodes_.orig_face[node]);
          skel_graph_.edges_time_start.append(wf_nodes_.time_birth[node]);
          skel_graph_.edges_time_end.append(current_time_);
          skel_graph_.edges_is_boundary.append(false);

          this->deactivate_node(node);
        }
      }
      continue;
    }

    const int cid = int(wf_contours_.head_node.size());
    wf_contours_.head_node.append(cand.head_node);
    wf_contours_.is_hole.append(cand.is_hole);
    wf_contours_.is_active.append(true);
    wf_contours_.is_wire.append(cand.is_wire);

    for (const int node : cand.nodes) {
      wf_nodes_.contour_id[node] = cid;
    }
  }
}

/**
 * Emit swept side-strip faces between current band boundary and next band boundary.
 *
 * Geometric Situation:
 * ====================
 * ```
 *               su_new (node u at t_next)         sv_new (node v at t_next)
 *                         *-----------------------------*
 *                         |                             |
 *                         |      Swept Quad Face        |
 *                         |                             |
 *                         *-----------------------------*
 *                skel_node[u] (at t_curr)       skel_node[v] (at t_curr)
 * ```
 */
inline void SimulationEngine::emit_segment_boundary_faces()

{
  PRF_scope(ProfileCategory::Core);

  /* Allocate a single skeleton node for each active wavefront vertex. */
  Array<int> new_skel_node(wf_nodes_.pos.size(), -1);
  for (const int i : wf_nodes_.pos.index_range()) {
    if (wf_nodes_.is_active[i]) {
      new_skel_node[i] = int(skel_graph_.nodes_pos.size());
      skel_graph_.nodes_pos.append(wf_nodes_.pos[i]);
      skel_graph_.nodes_time.append(current_time_);
      skel_graph_.nodes_orig.append(-1);
    }
  }

  /* Emit swept face for each active wavefront edge. */
  for (const int u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[u]) {
      continue;
    }
    const int v = wf_nodes_.next_node[u];
    if (v < 0 || v >= int(wf_nodes_.pos.size()) || !wf_nodes_.is_active[v]) {
      continue;
    }

    const int su_new = new_skel_node[u];
    const int sv_new = new_skel_node[v];

    /* Emit swept quad for band. */
    Vector<int> quad;
    quad.append(wf_nodes_.left_trail[u][0]);
    for (const int r : wf_nodes_.right_trail[u]) {
      if (quad.last() != r) {
        quad.append(r);
      }
    }
    if (quad.last() != sv_new) {
      quad.append(sv_new);
    }
    if (quad.last() != su_new) {
      quad.append(su_new);
    }
    for (int k = int(wf_nodes_.left_trail[u].size()) - 1; k >= 1; k--) {
      const int l = wf_nodes_.left_trail[u][k];
      if (quad.last() != l && l != quad[0]) {
        quad.append(l);
      }
    }

    if (quad.size() >= 3) {
      out_faces_.faces.append(std::move(quad));
      out_faces_.face_orig.append(wf_nodes_.orig_face[u]);
      out_faces_.face_band.append(current_band_);
      out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[u]);
    }

    /* Update baseline skeleton nodes for next band. */
    wf_nodes_.skel_node[u] = su_new;
    wf_nodes_.left_trail[u].clear();
    wf_nodes_.left_trail[u].append(su_new);
    wf_nodes_.right_trail[u].clear();
    wf_nodes_.right_trail[u].append(sv_new);
  }
}

/**
 * Emit closed inner polygon face(s) at effective offset threshold.
 *
 * Geometric Situation:
 * ====================
 * ```
 *              v1 *-------------------* v2
 *                /                     \
 *               /     Inner Polygon     \
 *              /      Closed Cap        \
 *             *--------------------------*
 *            v4                          v3
 * ```
 */
/**
 * Emit closed inner polygon face(s) at effective offset threshold.
 *
 * When inner holes are present and wavefronts from the outer boundary and inner holes
 * do not meet, an annular region is formed. Because Blender meshes cannot represent
 * faces with holes or repeating vertices, annular regions are split into simple disk
 * faces by adding non-crossing bridge chords approximately on opposite sides.
 */
inline void SimulationEngine::extract_and_classify_loops(Vector<Vector<int>> &r_outer_loops,
                                                         Vector<Vector<int>> &r_hole_loops)

{
  Vector<bool> visited(wf_nodes_.pos.size(), false);
  Vector<Vector<int>> all_loops;

  for (const int start_u : wf_nodes_.pos.index_range()) {
    if (!wf_nodes_.is_active[start_u] || visited[start_u]) {
      continue;
    }
    const int cid = wf_nodes_.contour_id[start_u];
    if (cid >= 0 && cid < wf_contours_.is_wire.size() && wf_contours_.is_wire[cid]) {
      continue;
    }
    Vector<int> loop;
    int curr = start_u;
    while (!visited[curr]) {
      visited[curr] = true;
      loop.append(wf_nodes_.skel_node[curr]);
      curr = wf_nodes_.next_node[curr];
      if (curr == start_u || curr < 0 || curr >= int(wf_nodes_.pos.size())) {
        break;
      }
    }
    if (loop.size() >= 3) {
      all_loops.append(std::move(loop));
    }
  }

  for (Vector<int> &loop : all_loops) {
    const double area = polygon_signed_area(loop.as_span(), skel_graph_.nodes_pos.as_span());
    if (area > 1e-9) {
      r_outer_loops.append(std::move(loop));
    }
    else if (area < -1e-9) {
      r_hole_loops.append(std::move(loop));
    }
  }
}

inline void SimulationEngine::associate_holes_with_regions(const Vector<Vector<int>> &outer_loops,
                                                           Vector<Vector<int>> &hole_loops,
                                                           Vector<Region> &r_regions)

{
  r_regions.reserve(outer_loops.size());
  for (const Vector<int> &outer : outer_loops) {
    r_regions.append(Region{outer, {}});
  }

  for (Vector<int> &hole : hole_loops) {
    const double2 test_pt = skel_graph_.nodes_pos[hole[0]];
    int best_region = -1;
    for (const int r_idx : r_regions.index_range()) {
      Vector<double2> outer_coords;
      outer_coords.reserve(r_regions[r_idx].outer.size());
      for (const int v : r_regions[r_idx].outer) {
        outer_coords.append(skel_graph_.nodes_pos[v]);
      }
      if (isect_point_polygon_v2(test_pt, outer_coords.as_span())) {
        best_region = r_idx;
        break;
      }
    }
    if (best_region != -1) {
      r_regions[best_region].holes.append(std::move(hole));
    }
    else if (!r_regions.is_empty()) {
      r_regions[0].holes.append(std::move(hole));
    }
  }
}

inline void SimulationEngine::find_valid_bridge_chords(
    const Span<double2> outer_coords,
    const Span<double2> hole_coords,
    const Span<Vector<double2>> other_holes_coords,
    Vector<CandidateChord> &r_valid_chords)

{
  const int n_outer = int(outer_coords.size());
  const int n_hole = int(hole_coords.size());

  auto is_in_annular_domain = [&](const double2 &pt) -> bool {
    if (!isect_point_polygon_v2(pt, outer_coords)) {
      return false;
    }
    if (isect_point_polygon_v2(pt, hole_coords)) {
      return false;
    }
    for (const Vector<double2> &h_coords : other_holes_coords) {
      if (isect_point_polygon_v2(pt, h_coords.as_span())) {
        return false;
      }
    }
    return true;
  };

  for (const int i : IndexRange(n_outer)) {
    const double2 &u = outer_coords[i];
    for (const int j : IndexRange(n_hole)) {
      const double2 &v = hole_coords[j];

      /* Midpoint and near-endpoints containment. */
      const double2 mid = 0.5 * (u + v);
      if (!is_in_annular_domain(mid)) {
        continue;
      }
      const double2 p_near_u = u + 0.05 * (v - u);
      const double2 p_near_v = v - 0.05 * (v - u);
      if (!is_in_annular_domain(p_near_u) || !is_in_annular_domain(p_near_v)) {
        continue;
      }

      /* Check crossing non-incident edges of outer loop. */
      bool crosses = false;
      for (const int k : IndexRange(n_outer)) {
        if (k == i || (k + 1) % n_outer == i) {
          continue;
        }
        if (segment_crosses_or_touches_v2(u, v, outer_coords[k], outer_coords[(k + 1) % n_outer]))
        {
          crosses = true;
          break;
        }
      }
      if (crosses) {
        continue;
      }

      /* Check crossing non-incident edges of hole loop. */
      for (const int k : IndexRange(n_hole)) {
        if (k == j || (k + 1) % n_hole == j) {
          continue;
        }
        if (segment_crosses_or_touches_v2(u, v, hole_coords[k], hole_coords[(k + 1) % n_hole])) {
          crosses = true;
          break;
        }
      }
      if (crosses) {
        continue;
      }

      /* Check crossing edges of any other holes. */
      for (const Vector<double2> &h_coords : other_holes_coords) {
        const int m = int(h_coords.size());
        for (const int k : IndexRange(m)) {
          if (segment_crosses_or_touches_v2(u, v, h_coords[k], h_coords[(k + 1) % m])) {
            crosses = true;
            break;
          }
        }
        if (crosses) {
          break;
        }
      }
      if (crosses) {
        continue;
      }

      r_valid_chords.append(CandidateChord{i, j, math::distance(u, v)});
    }
  }
}

inline void SimulationEngine::find_best_opposing_chord_pair(
    const Vector<CandidateChord> &valid_chords,
    const Span<double2> outer_coords,
    const Span<double2> hole_coords,
    const double2 hole_centroid,
    const double max_extent,
    int &r_best_idx1,
    int &r_best_idx2)

{
  r_best_idx1 = -1;
  r_best_idx2 = -1;
  double best_score = std::numeric_limits<double>::infinity();

  const int num_chords = int(valid_chords.size());
  for (const int idx1 : IndexRange(num_chords)) {
    const CandidateChord &c1 = valid_chords[idx1];
    const double2 &u1 = outer_coords[c1.i_outer];
    const double2 &v1 = hole_coords[c1.j_hole];
    const double2 d1 = u1 - hole_centroid;
    const double len_d1 = math::length(d1);
    if (len_d1 < 1e-12) {
      continue;
    }
    const double2 dir1 = d1 / len_d1;

    for (int idx2 = idx1 + 1; idx2 < num_chords; idx2++) {
      const CandidateChord &c2 = valid_chords[idx2];
      if (c1.i_outer == c2.i_outer || c1.j_hole == c2.j_hole) {
        continue;
      }
      const double2 &u2 = outer_coords[c2.i_outer];
      const double2 &v2 = hole_coords[c2.j_hole];

      /* Chords must not cross each other. */
      if (segment_crosses_or_touches_v2(u1, v1, u2, v2)) {
        continue;
      }

      const double2 d2 = u2 - hole_centroid;
      const double len_d2 = math::length(d2);
      if (len_d2 < 1e-12) {
        continue;
      }
      const double2 dir2 = d2 / len_d2;

      /* Dot product is -1 for diametrically opposite chords. */
      const double dot = math::dot(dir1, dir2);
      const double score = dot + 0.05 * (c1.dist + c2.dist) / max_extent;

      if (score < best_score) {
        best_score = score;
        r_best_idx1 = idx1;
        r_best_idx2 = idx2;
      }
    }
  }
}

inline void SimulationEngine::bridge_hole_in_region(Region current, Vector<Region> &worklist)

{
  /* Pick the first hole to bridge. */
  Vector<int> hole = current.holes.pop_last();

  const Span<int> outer_indices = current.outer.as_span();
  const Span<int> hole_indices = hole.as_span();
  const int n_outer = int(outer_indices.size());
  const int n_hole = int(hole_indices.size());

  Vector<double2> outer_coords;
  outer_coords.reserve(n_outer);
  for (const int v : outer_indices) {
    outer_coords.append(skel_graph_.nodes_pos[v]);
  }

  Vector<double2> hole_coords;
  hole_coords.reserve(n_hole);
  for (const int v : hole_indices) {
    hole_coords.append(skel_graph_.nodes_pos[v]);
  }

  Vector<Vector<double2>> other_holes_coords;
  for (const Vector<int> &h : current.holes) {
    Vector<double2> coords;
    coords.reserve(h.size());
    for (const int v : h) {
      coords.append(skel_graph_.nodes_pos[v]);
    }
    other_holes_coords.append(std::move(coords));
  }

  /* Compute hole centroid and bounding box scale for normalization. */
  double2 hole_centroid(0.0, 0.0);
  for (const double2 &p : hole_coords) {
    hole_centroid += p;
  }
  hole_centroid /= double(n_hole);

  double max_extent = 1.0;
  for (const double2 &p : outer_coords) {
    max_extent = math::max(max_extent, math::distance(p, hole_centroid));
  }

  /* Find all valid candidate bridge chords u_i -> v_j. */
  Vector<CandidateChord> valid_chords;
  this->find_valid_bridge_chords(
      outer_coords.as_span(), hole_coords.as_span(), other_holes_coords.as_span(), valid_chords);

  /* Select the best non-crossing chord pair on approximately opposite sides. */
  int best_idx1 = -1;
  int best_idx2 = -1;
  this->find_best_opposing_chord_pair(valid_chords,
                                      outer_coords.as_span(),
                                      hole_coords.as_span(),
                                      hole_centroid,
                                      max_extent,
                                      best_idx1,
                                      best_idx2);

  /* Fallback if no valid chord pair was found. */
  if (best_idx1 == -1 || best_idx2 == -1) {
    out_faces_.faces.append(std::move(current.outer));
    out_faces_.face_orig.append(0);
    out_faces_.face_band.append(math::max(0, segments_num_ - 1));
    out_faces_.face_orig_edge.append(-1);
    return;
  }

  const CandidateChord &c_a = valid_chords[best_idx1];
  const CandidateChord &c_b = valid_chords[best_idx2];
  const int i_a = c_a.i_outer;
  const int j_p = c_a.j_hole;
  const int i_b = c_b.i_outer;
  const int j_q = c_b.j_hole;

  /* Construct the two split faces. */
  Vector<int> outer_arc_1 = get_cyclic_arc(outer_indices, i_a, i_b);
  Vector<int> outer_arc_2 = get_cyclic_arc(outer_indices, i_b, i_a);

  Vector<int> hole_arc_1 = get_cyclic_arc(hole_indices, j_q, j_p);
  Vector<int> hole_arc_2 = get_cyclic_arc(hole_indices, j_p, j_q);

  Vector<int> face1 = outer_arc_1;
  face1.extend(hole_arc_1.as_span());

  Vector<int> face2 = outer_arc_2;
  face2.extend(hole_arc_2.as_span());

  /* Check signed areas; if negative, swap the hole arc endpoints. */
  const double area1 = polygon_signed_area(face1.as_span(), skel_graph_.nodes_pos.as_span());
  const double area2 = polygon_signed_area(face2.as_span(), skel_graph_.nodes_pos.as_span());
  if (area1 < 0.0 || area2 < 0.0) {
    face1 = outer_arc_1;
    face1.extend(hole_arc_2.as_span());

    face2 = outer_arc_2;
    face2.extend(hole_arc_1.as_span());
  }

  /* Partition remaining holes between face1 and face2. */
  Vector<Region> new_regions;
  new_regions.append(Region{std::move(face1), {}});
  new_regions.append(Region{std::move(face2), {}});

  for (Vector<int> &rem_hole : current.holes) {
    const double2 rem_test_pt = skel_graph_.nodes_pos[rem_hole[0]];
    Vector<double2> f1_coords;
    f1_coords.reserve(new_regions[0].outer.size());
    for (const int v : new_regions[0].outer) {
      f1_coords.append(skel_graph_.nodes_pos[v]);
    }
    if (isect_point_polygon_v2(rem_test_pt, f1_coords.as_span())) {
      new_regions[0].holes.append(std::move(rem_hole));
    }
    else {
      new_regions[1].holes.append(std::move(rem_hole));
    }
  }

  worklist.append(std::move(new_regions[0]));
  worklist.append(std::move(new_regions[1]));
}

inline void SimulationEngine::emit_inner_polygon_faces()

{
  PRF_scope(ProfileCategory::Core);

  Vector<Vector<int>> outer_loops;
  Vector<Vector<int>> hole_loops;
  this->extract_and_classify_loops(outer_loops, hole_loops);

  if (outer_loops.is_empty()) {
    return;
  }

  /* If there are no holes, emit outer loops directly as simple polygon caps. */
  if (hole_loops.is_empty()) {
    for (Vector<int> &loop : outer_loops) {
      out_faces_.faces.append(std::move(loop));
      out_faces_.face_orig.append(0);
      out_faces_.face_band.append(math::max(0, segments_num_ - 1));
      out_faces_.face_orig_edge.append(-1);
    }
    return;
  }

  Vector<Region> regions;
  this->associate_holes_with_regions(outer_loops, hole_loops, regions);

  Vector<Region> worklist = std::move(regions);
  while (!worklist.is_empty()) {
    Region current = worklist.pop_last();
    if (current.holes.is_empty()) {
      out_faces_.faces.append(std::move(current.outer));
      out_faces_.face_orig.append(0);
      out_faces_.face_band.append(math::max(0, segments_num_ - 1));
      out_faces_.face_orig_edge.append(-1);
      continue;
    }

    this->bridge_hole_in_region(std::move(current), worklist);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PSLG Independent Component Partitioning
 * \{ */

struct ComponentBuffers {
  Vector<double2> verts;
  Vector<int> orig_verts;
  Vector<int> face_offsets_data;
  Vector<int> face_vert_indices;
  Vector<int> orig_faces;
  Vector<int2> edges;
  Vector<int> orig_edges;
  StraightSkeletonInput input;
};

static std::optional<StraightSkeletonOutput> try_partition_independent_components(
    const StraightSkeletonInput &input)
{
  const bool is_inside = (input.direction == PropagationDirection::Inside);
  const bool is_both = (input.direction == PropagationDirection::Both);
  if ((!is_inside && !is_both) || input.face_offsets.is_empty()) {
    return std::nullopt;
  }

  Vector<AnalyzedFace> faces;
  for (const int f_i : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[f_i];
    if (r.size() < 3) {
      continue;
    }
    AnalyzedFace f;
    f.orig_face_idx = f_i;
    for (const int idx : r) {
      const int v = input.face_vert_indices[idx];
      f.vert_indices.append(v);
      f.coords.append(input.vert[v]);
    }
    f.signed_area = math::area_poly_signed(f.coords.as_span());
    if (std::abs(f.signed_area) < 1e-10) {
      continue;
    }
    f.min_co = f.coords[0];
    f.max_co = f.coords[0];
    for (const double2 &p : f.coords) {
      f.min_co = math::min(f.min_co, p);
      f.max_co = math::max(f.max_co, p);
    }
    f.interior_pt = find_polygon_interior_point(f.coords.as_span(), f.signed_area);
    faces.append(std::move(f));
  }

  if (faces.is_empty()) {
    return std::nullopt;
  }

  build_containment_tree(faces.as_mutable_span());

  Vector<int> wire_edge_containing_face(input.edge.size(), -1);
  for (const int e_i : input.edge.index_range()) {
    const double2 mid = 0.5 * (input.vert[input.edge[e_i][0]] + input.vert[input.edge[e_i][1]]);
    int containing_f = -1;
    double min_a = std::numeric_limits<double>::infinity();
    for (const int fi : faces.index_range()) {
      if (isect_point_polygon_v2(mid, faces[fi].coords.as_span())) {
        const double a = std::abs(faces[fi].signed_area);
        if (a < min_a) {
          min_a = a;
          containing_f = fi;
        }
      }
    }
    if (containing_f == -1 || faces[containing_f].is_hole) {
      if (is_inside) {
        /* Uncontained wire edge with Inside direction requires bounding box, do not partition. */
        return std::nullopt;
      }
    }
    wire_edge_containing_face[e_i] = containing_f;
  }

  Vector<int> root_indices;
  Array<int> face_to_comp(faces.size(), -1);
  for (const int fi : faces.index_range()) {
    int curr = fi;
    while (faces[curr].parent != -1) {
      curr = faces[curr].parent;
    }
    int comp = root_indices.first_index_of_try(curr);
    if (comp == -1) {
      comp = int(root_indices.size());
      root_indices.append(curr);
    }
    face_to_comp[fi] = comp;
  }

  const int num_inward = int(root_indices.size());
  if (is_inside && num_inward <= 1) {
    return std::nullopt;
  }

  Vector<int> hole_indices;
  Array<int> hole_to_comp(faces.size(), -1);
  if (is_both) {
    for (const int fi : faces.index_range()) {
      if (faces[fi].is_hole) {
        hole_to_comp[fi] = num_inward + 1 + int(hole_indices.size());
        hole_indices.append(fi);
      }
    }
  }

  const int num_components = is_both ? (num_inward + 1 + int(hole_indices.size())) : num_inward;
  const int ext_comp_idx = is_both ? num_inward : -1;

  Array<ComponentBuffers> comps(num_components);
  for (int k = 0; k < num_components; k++) {
    comps[k].face_offsets_data.append(0);
  }

  Array<Array<int>> global_to_local_verts(num_components);
  for (int k = 0; k < num_components; k++) {
    global_to_local_verts[k].reinitialize(input.vert.size());
    global_to_local_verts[k].fill(-1);
  }

  auto get_or_add_local_vert = [&](int comp_idx, int global_v) {
    int &local_v = global_to_local_verts[comp_idx][global_v];
    if (local_v == -1) {
      local_v = int(comps[comp_idx].verts.size());
      comps[comp_idx].verts.append(input.vert[global_v]);
      comps[comp_idx].orig_verts.append(global_v);
    }
    return local_v;
  };

  /* Add faces to inward components. */
  for (const int fi : faces.index_range()) {
    const int comp_idx = face_to_comp[fi];
    for (const int gv : faces[fi].vert_indices) {
      const int lv = get_or_add_local_vert(comp_idx, gv);
      comps[comp_idx].face_vert_indices.append(lv);
    }
    comps[comp_idx].face_offsets_data.append(int(comps[comp_idx].face_vert_indices.size()));
    comps[comp_idx].orig_faces.append(faces[fi].orig_face_idx);
  }

  /* Add root outer faces to exterior component (for Both). */
  if (is_both) {
    for (const int r_fi : root_indices) {
      for (const int gv : faces[r_fi].vert_indices) {
        const int lv = get_or_add_local_vert(ext_comp_idx, gv);
        comps[ext_comp_idx].face_vert_indices.append(lv);
      }
      comps[ext_comp_idx].face_offsets_data.append(
          int(comps[ext_comp_idx].face_vert_indices.size()));
      comps[ext_comp_idx].orig_faces.append(faces[r_fi].orig_face_idx);
    }

    /* Add hole interior components (for Both). */
    for (const int h_fi : hole_indices) {
      const int comp_idx = hole_to_comp[h_fi];
      /* Add hole boundary as outer face of this hole component. */
      for (const int gv : faces[h_fi].vert_indices) {
        const int lv = get_or_add_local_vert(comp_idx, gv);
        comps[comp_idx].face_vert_indices.append(lv);
      }
      comps[comp_idx].face_offsets_data.append(int(comps[comp_idx].face_vert_indices.size()));
      comps[comp_idx].orig_faces.append(faces[h_fi].orig_face_idx);

      /* Add any direct child faces (nested islands) as holes in this component. */
      for (const int c_fi : faces.index_range()) {
        if (faces[c_fi].parent == h_fi) {
          for (const int gv : faces[c_fi].vert_indices) {
            const int lv = get_or_add_local_vert(comp_idx, gv);
            comps[comp_idx].face_vert_indices.append(lv);
          }
          comps[comp_idx].face_offsets_data.append(int(comps[comp_idx].face_vert_indices.size()));
          comps[comp_idx].orig_faces.append(faces[c_fi].orig_face_idx);
        }
      }
    }
  }

  /* Add wire edges. */
  for (const int e_i : input.edge.index_range()) {
    const int cont_f = wire_edge_containing_face[e_i];
    int comp_idx = -1;
    if (cont_f == -1) {
      comp_idx = ext_comp_idx;
    }
    else if (faces[cont_f].is_hole) {
      comp_idx = is_both ? hole_to_comp[cont_f] : -1;
    }
    else {
      comp_idx = face_to_comp[cont_f];
    }
    if (comp_idx != -1) {
      const int lv0 = get_or_add_local_vert(comp_idx, input.edge[e_i][0]);
      const int lv1 = get_or_add_local_vert(comp_idx, input.edge[e_i][1]);
      comps[comp_idx].edges.append(int2(lv0, lv1));
      comps[comp_idx].orig_edges.append(e_i);
    }
  }

  for (int k = 0; k < num_components; k++) {
    comps[k].input.vert = comps[k].verts.as_span();
    comps[k].input.face_offsets = OffsetIndices<int>(comps[k].face_offsets_data.as_span());
    comps[k].input.face_vert_indices = comps[k].face_vert_indices.as_span();
    comps[k].input.edge = comps[k].edges.as_span();
    comps[k].input.direction = (k == ext_comp_idx) ? PropagationDirection::Outside :
                                                     PropagationDirection::Inside;
    comps[k].input.bbox_expansion_pct = input.bbox_expansion_pct;
    comps[k].input.offset = input.offset;
    comps[k].input.segments = input.segments;
    comps[k].input.epsilon = input.epsilon;
    comps[k].input.full_verification = input.full_verification;
  }

  Array<StraightSkeletonOutput> comp_outputs(num_components);
  threading::parallel_for(IndexRange(num_components), 1, [&](IndexRange range) {
    for (const int k : range) {
      comp_outputs[k] = straight_skeleton_calc(comps[k].input);
    }
  });

  Vector<double2> out_verts;
  Vector<double> out_vert_time;
  Vector<int> out_vert_orig;
  Vector<int2> out_edges;
  Vector<int> out_edge_orig;
  Vector<int> out_face_offsets_data;
  Vector<int> out_face_vert_indices;
  Vector<int> out_face_orig;
  Vector<int> out_face_band;

  out_face_offsets_data.append(0);
  bool hit_max_events = false;
  int64_t total_events_processed = 0;
  bool all_valid = true;
  std::string combined_error;

  /* Initialize out_verts with original input vertices. */
  out_verts.reserve(input.vert.size() + 2048);
  out_vert_time.reserve(input.vert.size() + 2048);
  out_vert_orig.reserve(input.vert.size() + 2048);
  for (const int i : input.vert.index_range()) {
    out_verts.append(input.vert[i]);
    out_vert_time.append(0.0);
    out_vert_orig.append(i);
  }

  for (int k = 0; k < num_components; k++) {
    const StraightSkeletonOutput &sub_out = comp_outputs[k];
    if (!sub_out.is_valid) {
      all_valid = false;
      if (combined_error.empty() && !sub_out.error_message.empty()) {
        combined_error = sub_out.error_message;
      }
    }

    Array<int> local_to_global(sub_out.vert.size(), -1);
    for (const int lv : sub_out.vert.index_range()) {
      if (lv < comps[k].orig_verts.size()) {
        local_to_global[lv] = comps[k].orig_verts[lv];
      }
      else {
        local_to_global[lv] = int(out_verts.size());
        out_verts.append(sub_out.vert[lv]);
        out_vert_time.append(sub_out.vert_time[lv]);
        const int vo = sub_out.vert_orig[lv];
        const int gvo = (vo >= 0 && vo < comps[k].orig_verts.size()) ? comps[k].orig_verts[vo] :
                                                                       -1;
        out_vert_orig.append(gvo);
      }
    }

    for (const int2 &e : sub_out.edge) {
      out_edges.append(int2(local_to_global[e[0]], local_to_global[e[1]]));
    }
    for (const int eo : sub_out.edge_orig) {
      const int geo = (eo >= 0 && eo < comps[k].orig_edges.size()) ? comps[k].orig_edges[eo] : -1;
      out_edge_orig.append(geo);
    }
    const OffsetIndices<int> sub_faces = sub_out.face_offsets();
    for (const int fi : sub_faces.index_range()) {
      for (const int idx : sub_faces[fi]) {
        out_face_vert_indices.append(local_to_global[sub_out.face_vert_indices[idx]]);
      }
      out_face_offsets_data.append(int(out_face_vert_indices.size()));
      const int sfo = sub_out.face_orig[fi];
      const int gfo = (sfo >= 0 && sfo < comps[k].orig_faces.size()) ? comps[k].orig_faces[sfo] :
                                                                       -1;
      out_face_orig.append(gfo);
      out_face_band.append(sub_out.face_band[fi]);
    }

    if (sub_out.hit_max_events) {
      hit_max_events = true;
    }
    total_events_processed += sub_out.events_processed_num;
  }

  StraightSkeletonOutput output;
  output.vert = Array<double2>(out_verts.as_span());
  output.vert_time = Array<double>(out_vert_time.as_span());
  output.vert_orig = Array<int>(out_vert_orig.as_span());
  output.edge = Array<int2>(out_edges.as_span());
  output.edge_orig = Array<int>(out_edge_orig.as_span());
  output.face_offsets_data = Array<int>(out_face_offsets_data.as_span());
  output.face_vert_indices = Array<int>(out_face_vert_indices.as_span());
  output.face_orig = Array<int>(out_face_orig.as_span());
  output.face_band = Array<int>(out_face_band.as_span());
  output.hit_max_events = hit_max_events;
  output.events_processed_num = total_events_processed;
  output.is_valid = all_valid;
  output.error_message = combined_error;
  return output;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Entry Point
 * \{ */

StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input)
{
  PRF_scope(ProfileCategory::Core);

  StraightSkeletonOutput empty_output;
  empty_output.hit_max_events = false;
  empty_output.events_processed_num = 0;

  if (input.face_offsets.is_empty() && input.edge.is_empty()) {
    return empty_output;
  }
  if (std::isnan(input.offset) || input.offset <= 0.0) {
    return empty_output;
  }
  for (const int v : input.face_vert_indices) {
    if (v < 0 || v >= input.vert.size()) {
      return empty_output;
    }
  }
  for (const int2 &e : input.edge) {
    if (e[0] < 0 || e[0] >= input.vert.size() || e[1] < 0 || e[1] >= input.vert.size()) {
      return empty_output;
    }
  }

  const int segments = std::max(1, input.segments);

  if (std::optional<StraightSkeletonOutput> partitioned = try_partition_independent_components(
          input))
  {
    return *partitioned;
  }

  double target_offset = input.offset;
  if (segments > 1 || !std::isinf(input.offset)) {
    SimulationEngine dry_engine(input, std::numeric_limits<double>::infinity(), 1, true);
    const double max_collapse_time = dry_engine.run();
    target_offset = std::min(target_offset, max_collapse_time);
  }

  SimulationEngine engine(input, target_offset, segments, false);
  engine.run();
  return engine.build_output();
}

/** \} */

} /* namespace blender::straightskeleton */
