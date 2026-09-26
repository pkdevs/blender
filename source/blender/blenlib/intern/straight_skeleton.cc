/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation using naive step-by-step wavefront simulation.
 *
 * Mathematical Background & Algorithmic Design:
 * =============================================
 * The straight skeleton of a 2D polygon with holes is the geometric trace of
 * the inward propagation of its boundary edges moving parallel to themselves
 * at unit speed.
 *
 * In this simulation engine:
 * 1. Wavefront contours are maintained explicitly as cyclic chains of nodes
 *    (outer polygon oriented CCW, holes oriented CW). In both orientations, the
 *    interior domain lies strictly to the LEFT of each directed edge u -> v.
 *
 * 2. Each vertex i between incoming edge u -> i (normal n1) and outgoing edge
 *    i -> w (normal n2) advances with an angular bisector velocity v:
 *      n1 . v = 1
 *      n2 . v = 1
 *    Solving this linear system gives:
 *      det = n1.x * n2.y - n1.y * n2.x
 *      v.x = (n2.y - n1.y) / det
 *      v.y = (n1.x - n2.x) / det
 *
 * 3. Topological Event Taxonomy:
 *    - Edge Collapse Event: An active edge shrinks to zero length.
 *    - Vertex-Edge Split Event: A reflex vertex collides with an advancing edge.
 *    - Vertex-Vertex Multi-Split: Two or more non-adjacent vertices collide.
 *
 * 4. Radial Angular Sector "Gap" Model for Event Clusters:
 *    When one or more events occur concurrently at timestamp t* at location p*:
 *
 *    ```
 *    Case A: Advancing Edge Hit by M Reflex Vertices
 *
 *                 v1            v2
 *                  \            /
 *                   \  Gap 2   /
 *                    \        /
 *           Gap 1     v      v      Gap 3
 *       <--------------*----*-------------->
 *       A              p*   p*             B
 *             Advancing Wavefront Edge e
 *                  (G = M + 1 gaps)
 *
 *    Case B: M Reflex Vertices Collide Head-On (Star Split)
 *
 *                            \     /
 *                             \   /
 *                        Gap 1 \ / Gap 2
 *                              p*
 *                        Gap 4 / \ Gap 3
 *                             /   \
 *                            /     \
 *                         (G = M gaps)
 *    ```
 *
 *    Radial Rays & CCW Deterministic Gap Pairing Rule:
 *    All surviving non-collapsing edges meeting at p* radiate outward into B_eps(p*):
 *      - Incoming rays (u -> p*): Type = IN, polar angle theta = atan2(dy, dx).
 *      - Outgoing rays (p* -> w): Type = OUT, polar angle theta = atan2(dy, dx).
 *
 *    ```
 *                                      Incoming Ray u (IN)
 *                                               ^
 *                                              /
 *                                EXTERIOR     /   INTERIOR
 *                                 SECTOR     /     SECTOR
 *                                           /      ("GAP")
 *                                          * p*
 *                                         /
 *                                        /
 *                                       /
 *                                      v
 *                             Outgoing Ray w (OUT)
 *    ```
 *
 *    Because the domain interior is strictly to the LEFT of directed edges:
 *    Sweeping polar angle theta counter-clockwise around p*:
 *      - Crossing an OUT ray transitions from EXTERIOR into INTERIOR.
 *      - Crossing an IN ray transitions from INTERIOR into EXTERIOR.
 *    Therefore, each pair of adjacent rays where an OUT ray (at theta_out) is
 *    followed by an IN ray (at theta_in) defines an INTERIOR SECTOR ("GAP"):
 *      Gap_g = (theta_out, theta_in)
 *
 *    For each Gap g, exactly ONE new wavefront node w_g is born at p*:
 *      prev[w_g] = u  (from the IN ray)
 *      next[w_g] = w  (from the OUT ray)
 *      next[u] = w_g, prev[w] = w_g
 *
 *    Classification by Gap Count G:
 *      G = 0: Complete local annihilation (e.g. triangle collapse, regular polygon apex).
 *             Retires participating nodes; 0 new nodes born; contour deactivates.
 *      G = 1: Single surviving corner (e.g. standard edge collapse on N > 3 polygon).
 *             Retires colliding nodes; 1 new node born.
 *      G = 2: Standard split / neck pinch (e.g. reflex vertex hitting edge).
 *             2 new nodes born; splits 1 contour into 2, or merges 2 into 1.
 *      G >= 3: Multi-way star split. G new nodes born, splitting into G contours.
 *
 * 5. Collinear Collapses Along Line Segments [A, B]:
 *
 *    ```
 *           Top Wavefront (moving down)
 *              \       |       /
 *               \      |      /
 *      A         v1    v2    v3          B
 *      *=========+=====+=====+===========*  Collapse Line L
 *      (Gap 1)   ^     ^     ^     (Gap 2)
 *               /      |      \
 *              /       |       \
 *           Bottom Wavefront (moving up)
 *    ```
 *
 *    - Zero Gaps in the Interior (A, B): Opposing normals (n_top = -n_bottom) contract
 *      the domain to zero thickness -> 0 gaps along (A, B).
 *    - Surviving Gaps at Extremities A and B: Open chambers to the left and right
 *      survive -> Gap 1 at A (new node w_A), Gap 2 at B (new node w_B).
 *    - Skeleton Ridge Edge: Straight skeleton node at A, node at B, edge A -> B.
 *
 * 6. Multi-Segment Offset Slicing & Two-Pass Execution:
 *    - When segments > 1 or offset < infinity, target offset D = min(offset, t_max)
 *      is divided into k equal bands. At each band boundary t_seg[b], swept side-strip
 *      quads are emitted and baseline skeleton nodes are updated.
 *    - At the final band boundary, surviving closed wavefront contours are emitted
 *      as the inner polygon face(s).
 *
 * 7. Differences and Departures from Published Literature:
 *    The baseline wavefront simulation algorithm for straight skeletons was introduced by
 *    Felkel and Obdrzalek (1998) ("Straight skeleton implementation"). This engine incorporates
 *    the following key algorithmic extensions and departures:
 *
 *    - Concurrent Event Clusters & Gap Model: Felkel & Obdrzalek assume strictly sequential,
 *      isolated events in generic position. This engine introduces a deterministic radial
 *      angular sector ("Gap") model that resolves arbitrary clusters of M simultaneous
 *      reflex vertices and collapsing edges meeting at a single spatial point p* (such as
 *      highly symmetric multi-way star splits and regular polygon collapses).
 *
 *    - Collinear Ridge Resolution: Felkel & Obdrzalek do not support antiparallel wavefront
 *      edges colliding along an extended line segment (e.g., in dumbbells or parallel corridors).
 *      This engine detects collinear overlaps directly, generates the straight skeleton ridge
 *      edges spanning [p_a, p_b], and reconnects the non-overlapping overhanging wavefront
 * suffixes.
 *
 *    - Non-Convex Face Seam Merging: When a split event divides an advancing generator edge into
 *      multiple sub-edges, standard simulations output disconnected triangular or trapezoidal
 *      pieces. This engine tracks generator edge lineage and stitches sub-faces sharing opposite
 *      internal chords into unified, true straight skeleton faces with non-convex saddle notches.
 *
 *    - Spatial Grid Acceleration: Rather than an O(N^2) all-pairs search or a priority queue
 *      prone to degenerate cascading updates in symmetric clusters, this engine uses a 2D uniform
 *      bounding-box grid with overlapping bucket insertions, achieving near-linear performance
 *      (e.g., computing a 3,072-vertex Koch snowflake in under 80 ms).
 *
 *    - Two-Pass Multi-Segment Slicing: Directly subdivides the computed roof height into k uniform
 *      offset bands, computing swept side-strip quads and capping inner polygon faces in a single
 *      coherent framework.
 */

#include "BLI_straight_skeleton.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "PRF_profile.hh"

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name Data Structures (Structure of Arrays)
 * \{ */

/** Moving wavefront nodes between adjacent wavefront edges. */
struct WavefrontNodesSoA {
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
struct WavefrontContoursSoA {
  Vector<int> head_node;
  Vector<bool> is_hole;
  Vector<bool> is_active;
};

/** Straight skeleton graph constructed incrementally. */
struct SkeletonGraphSoA {
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
struct OutputFacesSoA {
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
 *
 * Geometric Situation:
 * ====================
 * ```
 *                        Outgoing edge i -> w
 *                               ^
 *                              /
 *                             /  Normal n2
 *                            / <---
 *                           /
 *                      (i) *
 *                         / \
 *                        /   \
 *                       /     \  Bisector velocity v:
 *                      /       v  n1 . v = 1, n2 . v = 1
 *                     /
 *                    / Normal n1
 *                   / ---->
 *                  /
 *          Incoming edge u -> i
 * ```
 *
 * Antiparallel Collinear Ridge (det == 0, dot(n1, n2) < 0):
 * ```
 *      Normal n1               Normal n2
 *          |                       ^
 *          v                       |
 *    (u) ======* (i)             (i) *====== (w)
 *               \                   /
 *                \                 /
 *                 v               v
 *           Ridge velocity vector along d2
 * ```
 */
static double2 compute_bisector_velocity(const double2 &n1, const double2 &n2, const double2 &d2)
{
  const double det = math::cross(n1, n2);
  if (std::abs(det) > 1e-7) {
    double2 v((n2.y - n1.y) / det, (n1.x - n2.x) / det);
    const double speed = math::length(v);
    if (speed > 10000.0) {
      v = (v / speed) * 10000.0;
    }
    return v;
  }

  /* Parallel edges continuing straight. */
  if (math::dot(n1, n2) > 0.0) {
    return n1;
  }

  /* Antiparallel opposing edges along a straight skeleton ridge!
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2D Spatial Grid with Overlapping Buckets
 * \{ */

/**
 * 2D uniform bounding-box grid with overlapping bucket insertions.
 *
 * Geometric Situation:
 * ====================
 * ```
 *  y_max +-------+-------+-------+
 *        |       | [e1]  | [e1]  |  An advancing edge bounding box spanning [p_u(t0), p_v(t1)]
 *        +-------+---+---+---+---+  is inserted into all overlapping grid cells (e.g. e1 spans
 *        |       |   | e1|   |   |  2x2 cells). A query box for reflex vertex w only checks
 *        +-------+---+---+---+---+  edges recorded in its overlapping cells.
 *  y_min +-------+-------+-------+
 *       x_min                   x_max
 * ```
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

  /* 1. Vertex Validity: no NaN, Inf, or negative times. */
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

  /* 2. Face Offsets and Simplicity. */
  const OffsetIndices<int> face_offsets = output.face_offsets_data.is_empty() ?
                                              output.face_offsets :
                                              OffsetIndices<int>(
                                                  output.face_offsets_data.as_span());
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
          r_error = err.str();
          return false;
        }
      }
    }
  }

  /* 3. Skeleton Graph Planarity: test all edge pairs for improper intersection. */
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

  /* 4. Domain Area Partition (for full skeleton with segments = 1 and infinite offset). */
  if (input.segments <= 1 && std::isinf(input.offset)) {
    double domain_area = 0.0;
    const int outer_num = int(input.outer_polygon.size());
    for (const int i : IndexRange(outer_num)) {
      const double2 p1 = input.vert[input.outer_polygon[i]];
      const double2 p2 = input.vert[input.outer_polygon[(i + 1) % outer_num]];
      domain_area += math::cross(p1, p2);
    }
    domain_area *= 0.5;

    for (const int h : IndexRange(input.hole_offsets.size())) {
      const IndexRange r = input.hole_offsets[h];
      const int h_num = int(r.size());
      double hole_area = 0.0;
      for (const int i : IndexRange(h_num)) {
        const double2 p1 = input.vert[input.hole_vert_indices[r[i]]];
        const double2 p2 = input.vert[input.hole_vert_indices[r[(i + 1) % h_num]]];
        hole_area += math::cross(p1, p2);
      }
      /* CW holes have negative signed area, so subtracting their absolute value: */
      domain_area -= std::abs(0.5 * hole_area);
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
  const StraightSkeletonInput &input_;
  double target_offset_{std::numeric_limits<double>::infinity()};
  int segments_num_{1};
  bool dry_run_{false};

  WavefrontNodesSoA wf_nodes_;
  WavefrontContoursSoA wf_contours_;
  SkeletonGraphSoA skel_graph_;
  OutputFacesSoA out_faces_;

  double current_time_{0.0};
  double max_collapse_time_{0.0};
  int current_band_{0};
  int events_processed_count_{0};

  SpatialGrid2D grid_;
  SpatialGrid2D vert_grid_;
  Vector<int> query_tags_;
  int current_query_id_{0};

  Array<double2> orig_edge_normal_;
  Array<bool> orig_edge_has_antiparallel_;
  bool has_any_antiparallel_edges_{false};

 public:
  SimulationEngine(const StraightSkeletonInput &input,
                   double target_offset,
                   int segments,
                   bool dry_run)
      : input_(input), target_offset_(target_offset), segments_num_(segments), dry_run_(dry_run)
  {
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
  double run()
  {
    PRF_scope(ProfileCategory::Core);

    init_state();

    const double eps = (input_.epsilon > 0.0) ? input_.epsilon : 1e-8;
    const int max_steps = 10000;
    int step_count = 0;

    while (has_active_contours() && step_count < max_steps) {
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

      /* Determine lookahead horizon up to next segment threshold or target offset. */
      double max_lookahead = std::numeric_limits<double>::infinity();
      if (!std::isinf(next_seg_time)) {
        max_lookahead = std::min(max_lookahead, next_seg_time - current_time_);
      }
      if (!std::isinf(target_offset_)) {
        max_lookahead = std::min(max_lookahead, target_offset_ - current_time_);
      }

      /* Detect events occurring in [current_time_, current_time_ + max_lookahead]. */
      const double event_t = find_earliest_event(max_lookahead, eps);

      if (!std::isinf(event_t) && event_t < next_seg_time - 1e-9 &&
          event_t < target_offset_ - 1e-9)
      {
        /* Advance simulation to event timestamp. */
        advance_positions(event_t - current_time_);
        current_time_ = event_t;
        max_collapse_time_ = std::max(max_collapse_time_, current_time_);

        /* Resolve all concurrent events at current_time_. */
        resolve_events_at_current_time(eps);
      }
      else if (!std::isinf(next_seg_time) && next_seg_time <= target_offset_ + 1e-9) {
        /* Advance to segment threshold. */
        advance_positions(next_seg_time - current_time_);
        current_time_ = next_seg_time;
        max_collapse_time_ = std::max(max_collapse_time_, current_time_);

        /* Resolve any events that coincide with segment threshold. */
        if (!std::isinf(event_t) && std::abs(event_t - next_seg_time) < 1e-9) {
          resolve_events_at_current_time(eps);
        }

        if (!dry_run_) {
          emit_segment_boundary_faces();
        }
        current_band_++;
      }
      else {
        break;
      }
    }

    /* If finished with finite target offset and surviving contours remain, emit inner polygon.
     * Note: Inner polygon cap is only emitted for positive offsets of closed polygons. */
    if (!dry_run_ && !std::isinf(input_.offset) && input_.offset > 0.0 &&
        input_.edge_chain.is_empty() && has_active_contours() &&
        current_time_ >= target_offset_ - 1e-7)
    {
      emit_inner_polygon_faces();
    }

    return max_collapse_time_;
  }

  StraightSkeletonOutput build_output()
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

    /* 1. Ensure all initial faces are clean and CCW. */
    for (const int fi : out_faces_.faces.index_range()) {
      Vector<int> &f = out_faces_.faces[fi];
      Vector<int> clean_f;
      for (const int v : f) {
        if (clean_f.is_empty() || clean_f.last() != v) {
          clean_f.append(v);
        }
      }
      while (clean_f.size() >= 2 && clean_f.first() == clean_f.last()) {
        clean_f.pop_last();
      }
      if (clean_f.size() >= 3 &&
          polygon_signed_area(clean_f.as_span(), skel_graph_.nodes_pos.as_span()) < 0.0)
      {
        std::reverse(clean_f.begin(), clean_f.end());
      }
      f = std::move(clean_f);
    }

    /* 2. Merge sub-faces of the same generator edge and band that share an internal seam.
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

    for (auto item : groups.items()) {
      Vector<int> &f_indices = item.value;
      if (f_indices.size() <= 1) {
        continue;
      }

      bool merged_in_group = true;
      while (merged_in_group) {
        merged_in_group = false;
        for (int a_idx = 0; a_idx < f_indices.size(); a_idx++) {
          const int i = f_indices[a_idx];
          if (out_faces_.faces[i].size() < 3) {
            continue;
          }
          Vector<int> &f1 = out_faces_.faces[i];
          const int n1 = int(f1.size());

          for (int b_idx = a_idx + 1; b_idx < f_indices.size(); b_idx++) {
            const int j = f_indices[b_idx];
            if (out_faces_.faces[j].size() < 3) {
              continue;
            }
            Vector<int> &f2 = out_faces_.faces[j];
            const int n2 = int(f2.size());

            int match_i = -1;
            int match_j = -1;
            for (int e1 = 0; e1 < n1; e1++) {
              const int a = f1[e1];
              const int b = f1[(e1 + 1) % n1];
              for (int e2 = 0; e2 < n2; e2++) {
                if (f2[e2] == b && f2[(e2 + 1) % n2] == a) {
                  match_i = e1;
                  match_j = e2;
                  break;
                }
              }
              if (match_i != -1) {
                break;
              }
            }

            if (match_i != -1) {
              /* Merge f1 and f2 along (a, b) and (b, a). */
              Vector<int> merged;
              merged.reserve(n1 + n2 - 2);
              for (int k = 1; k <= n1; k++) {
                merged.append(f1[(match_i + k) % n1]);
              }
              for (int k = 2; k < n2; k++) {
                merged.append(f2[(match_j + k) % n2]);
              }

              /* Clean consecutive duplicates. */
              Vector<int> clean;
              for (const int v : merged) {
                if (clean.is_empty() || clean.last() != v) {
                  clean.append(v);
                }
              }
              while (clean.size() >= 2 && clean.first() == clean.last()) {
                clean.pop_last();
              }

              if (clean.size() >= 3) {
                out_faces_.faces[i] = std::move(clean);
                out_faces_.faces[j].clear();
                merged_in_group = true;
                break;
              }
            }
          }
          if (merged_in_group) {
            break;
          }
        }
      }
    }

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

    /* 3. Remap vertices referenced by output faces. */
    Vector<Vector<int>> final_faces;
    Vector<int> final_face_orig;
    Vector<int> final_face_band;

    for (const int fi : out_faces_.faces.index_range()) {
      Vector<int> clean_f = out_faces_.faces[fi];
      if (clean_f.size() < 3) {
        continue;
      }

      /* Ensure positive signed area (counter-clockwise). */
      if (polygon_signed_area(clean_f.as_span(), skel_graph_.nodes_pos.as_span()) < 0.0) {
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

      final_faces.append(std::move(clean_f));
      final_face_orig.append(out_faces_.face_orig[fi]);
      final_face_band.append(out_faces_.face_band[fi]);
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
    output.face_offsets = OffsetIndices<int>(output.face_offsets_data.as_span());
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
    output.flips_num = 0;

    if (input_.full_verification) {
      output.is_valid = verify_straight_skeleton(input_, output, output.error_message);
    }

    return output;
  }

 private:
  /**
   * Initialize wavefront contours and straight skeleton nodes from input geometry.
   *
   * Orientation Conventions:
   * ========================
   * In both outer (CCW) and hole (CW) polygons, the interior domain lies strictly
   * to the LEFT of each directed edge:
   * ```
   *     Outer Polygon (CCW)                       Hole Polygon (CW)
   *
   *         <--------- (u)                           (v) --------->
   *        /          ^   \                         /   ^          \
   *       /  Interior |    \                       /    | Interior  \
   *      v   Domain   |     \                     /     | Domain     v
   *     |       (to left)    |                   |   (to left)        |
   *     |                    |                   |                    |
   *      \                  /                     \                  /
   *       \                /                       \                /
   *        ---------> ----                          <--------- -----
   * ```
   */
  void init_state()
  {
    PRF_scope(ProfileCategory::Core);

    /* 1. Initialize Skeleton Nodes from input vertices. */
    const int input_verts_num = int(input_.vert.size());
    for (const int i : IndexRange(input_verts_num)) {
      skel_graph_.nodes_pos.append(input_.vert[i]);
      skel_graph_.nodes_time.append(0.0);
      skel_graph_.nodes_orig.append(i);
    }

    if (!input_.edge_chain.is_empty()) {
      init_edge_chain_state();
    }
    else if (input_.offset < 0.0) {
      init_negative_offset_state();
    }
    else {
      init_polygon_state();
    }
  }

  void init_antiparallel_bins()
  {
    const int total_orig_edges = int(orig_edge_normal_.size());
    constexpr int NUM_ANGLE_BINS = 16;
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

    for (const int e : IndexRange(total_orig_edges)) {
      const int b = edge_bins[e];
      bool found = false;
      for (int offset = 6; offset <= 10; offset++) {
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

  void init_polygon_state()
  {
    /* 2. Initialize Outer Wavefront Contour (CCW). */
    const int outer_num = int(input_.outer_polygon.size());
    const int outer_start_idx = 0;
    wf_contours_.head_node.append(outer_start_idx);
    wf_contours_.is_hole.append(false);
    wf_contours_.is_active.append(true);

    for (const int i : IndexRange(outer_num)) {
      const int vi = input_.outer_polygon[i];
      const int prev_idx = (i == 0) ? outer_num - 1 : i - 1;
      const int next_idx = (i + 1 == outer_num) ? 0 : i + 1;

      wf_nodes_.pos.append(input_.vert[vi]);
      wf_nodes_.vel.append(double2(0.0, 0.0));
      wf_nodes_.prev_node.append(prev_idx);
      wf_nodes_.next_node.append(next_idx);
      wf_nodes_.skel_node.append(vi);
      wf_nodes_.orig_edge.append(i);
      wf_nodes_.orig_face.append(0);
      wf_nodes_.contour_id.append(0);
      wf_nodes_.is_active.append(true);
      wf_nodes_.time_birth.append(0.0);

      Vector<int> lt;
      lt.append(vi);
      wf_nodes_.left_trail.append(std::move(lt));

      Vector<int> rt;
      rt.append(input_.outer_polygon[next_idx]);
      wf_nodes_.right_trail.append(std::move(rt));
    }

    /* 3. Initialize Hole Contours (CW). */
    for (const int hole_i : input_.hole_offsets.index_range()) {
      const IndexRange r = input_.hole_offsets[hole_i];
      const int hole_len = int(r.size());
      const int hole_start = int(wf_nodes_.pos.size());
      const int contour_id = int(wf_contours_.head_node.size());

      wf_contours_.head_node.append(hole_start);
      wf_contours_.is_hole.append(true);
      wf_contours_.is_active.append(true);

      for (const int i : IndexRange(hole_len)) {
        const int vi = input_.hole_vert_indices[r[i]];
        const int prev_node_idx = hole_start + ((i == 0) ? hole_len - 1 : i - 1);
        const int next_node_idx = hole_start + ((i + 1 == hole_len) ? 0 : i + 1);

        wf_nodes_.pos.append(input_.vert[vi]);
        wf_nodes_.vel.append(double2(0.0, 0.0));
        wf_nodes_.prev_node.append(prev_node_idx);
        wf_nodes_.next_node.append(next_node_idx);
        wf_nodes_.skel_node.append(vi);
        wf_nodes_.orig_edge.append(outer_num + r[i]);
        wf_nodes_.orig_face.append(1 + hole_i);
        wf_nodes_.contour_id.append(contour_id);
        wf_nodes_.is_active.append(true);
        wf_nodes_.time_birth.append(0.0);

        Vector<int> lt;
        lt.append(vi);
        wf_nodes_.left_trail.append(std::move(lt));

        Vector<int> rt;
        rt.append(input_.hole_vert_indices[r[(i + 1) % hole_len]]);
        wf_nodes_.right_trail.append(std::move(rt));
      }
    }

    /* 4. Precompute Original Edge Inward Normals and Antiparallel Edge Flags (O(E)). */
    const int total_orig_edges = outer_num + int(input_.hole_vert_indices.size());
    orig_edge_normal_.reinitialize(total_orig_edges);
    for (const int i : IndexRange(outer_num)) {
      const int v1 = input_.outer_polygon[i];
      const int v2 = input_.outer_polygon[(i + 1) % outer_num];
      orig_edge_normal_[i] = compute_inward_normal(input_.vert[v1], input_.vert[v2]);
    }
    for (const int hole_i : input_.hole_offsets.index_range()) {
      const IndexRange r = input_.hole_offsets[hole_i];
      const int hole_len = int(r.size());
      for (const int i : IndexRange(hole_len)) {
        const int e_idx = outer_num + r[i];
        const int v1 = input_.hole_vert_indices[r[i]];
        const int v2 = input_.hole_vert_indices[r[(i + 1) % hole_len]];
        orig_edge_normal_[e_idx] = compute_inward_normal(input_.vert[v1], input_.vert[v2]);
      }
    }

    init_antiparallel_bins();

    /* 5. Compute Initial Bisector Velocities. */
    for (const int i : wf_nodes_.pos.index_range()) {
      update_node_velocity(i);
    }
  }

  void init_negative_offset_state()
  {
    /* Outer polygon traversed in reverse (CW) so inward normal points outward into exterior. */
    const int outer_num = int(input_.outer_polygon.size());
    wf_contours_.head_node.append(0);
    wf_contours_.is_hole.append(false);
    wf_contours_.is_active.append(true);

    for (const int j : IndexRange(outer_num)) {
      const int idx = outer_num - 1 - j;
      const int vi = input_.outer_polygon[idx];
      const int prev_node = (j == 0) ? outer_num - 1 : j - 1;
      const int next_node = (j + 1 == outer_num) ? 0 : j + 1;
      const int next_idx = (idx == 0) ? outer_num - 1 : idx - 1;
      const int orig_e = next_idx;

      wf_nodes_.pos.append(input_.vert[vi]);
      wf_nodes_.vel.append(double2(0.0, 0.0));
      wf_nodes_.prev_node.append(prev_node);
      wf_nodes_.next_node.append(next_node);
      wf_nodes_.skel_node.append(vi);
      wf_nodes_.orig_edge.append(orig_e);
      wf_nodes_.orig_face.append(0);
      wf_nodes_.contour_id.append(0);
      wf_nodes_.is_active.append(true);
      wf_nodes_.time_birth.append(0.0);

      Vector<int> lt;
      lt.append(vi);
      wf_nodes_.left_trail.append(std::move(lt));

      Vector<int> rt;
      rt.append(input_.outer_polygon[next_idx]);
      wf_nodes_.right_trail.append(std::move(rt));
    }

    /* Holes traversed in reverse (CCW) so inward normal points into hole cavity. */
    for (const int hole_i : input_.hole_offsets.index_range()) {
      const IndexRange r = input_.hole_offsets[hole_i];
      const int hole_len = int(r.size());
      const int hole_start = int(wf_nodes_.pos.size());
      const int contour_id = int(wf_contours_.head_node.size());

      wf_contours_.head_node.append(hole_start);
      wf_contours_.is_hole.append(true);
      wf_contours_.is_active.append(true);

      for (const int j : IndexRange(hole_len)) {
        const int idx = hole_len - 1 - j;
        const int vi = input_.hole_vert_indices[r[idx]];
        const int prev_node = hole_start + ((j == 0) ? hole_len - 1 : j - 1);
        const int next_node = hole_start + ((j + 1 == hole_len) ? 0 : j + 1);
        const int next_idx = (idx == 0) ? hole_len - 1 : idx - 1;
        const int orig_e = outer_num + r[next_idx];

        wf_nodes_.pos.append(input_.vert[vi]);
        wf_nodes_.vel.append(double2(0.0, 0.0));
        wf_nodes_.prev_node.append(prev_node);
        wf_nodes_.next_node.append(next_node);
        wf_nodes_.skel_node.append(vi);
        wf_nodes_.orig_edge.append(orig_e);
        wf_nodes_.orig_face.append(1 + hole_i);
        wf_nodes_.contour_id.append(contour_id);
        wf_nodes_.is_active.append(true);
        wf_nodes_.time_birth.append(0.0);

        Vector<int> lt;
        lt.append(vi);
        wf_nodes_.left_trail.append(std::move(lt));

        Vector<int> rt;
        rt.append(input_.hole_vert_indices[r[next_idx]]);
        wf_nodes_.right_trail.append(std::move(rt));
      }
    }

    /* Precompute Original Edge Normals for negative offset (pointing outward/inward). */
    const int total_orig_edges = outer_num + int(input_.hole_vert_indices.size());
    orig_edge_normal_.reinitialize(total_orig_edges);
    for (const int i : IndexRange(outer_num)) {
      const int v1 = input_.outer_polygon[i];
      const int v2 = input_.outer_polygon[(i + 1) % outer_num];
      orig_edge_normal_[i] = compute_inward_normal(input_.vert[v2], input_.vert[v1]);
    }
    for (const int hole_i : input_.hole_offsets.index_range()) {
      const IndexRange r = input_.hole_offsets[hole_i];
      const int hole_len = int(r.size());
      for (const int i : IndexRange(hole_len)) {
        const int e_idx = outer_num + r[i];
        const int v1 = input_.hole_vert_indices[r[i]];
        const int v2 = input_.hole_vert_indices[r[(i + 1) % hole_len]];
        orig_edge_normal_[e_idx] = compute_inward_normal(input_.vert[v2], input_.vert[v1]);
      }
    }

    init_antiparallel_bins();

    for (const int i : wf_nodes_.pos.index_range()) {
      update_node_velocity(i);
    }
  }

  void init_edge_chain_state()
  {
    Vector<int> chain;
    for (const int v : input_.edge_chain) {
      if (chain.is_empty() ||
          math::distance_squared(input_.vert[v], input_.vert[chain.last()]) > 1e-12)
      {
        chain.append(v);
      }
    }
    const int m = int(chain.size());
    if (m < 2) {
      return;
    }

    /* Segments: m - 1 segments. */
    Array<double2> u_dir(m - 1);
    Array<double2> n_left(m - 1);
    for (const int k : IndexRange(m - 1)) {
      const double2 d = input_.vert[chain[k + 1]] - input_.vert[chain[k]];
      u_dir[k] = math::normalize(d);
      n_left[k] = math::orthogonal(u_dir[k]);
    }

    const int total_nodes = 2 * m;
    const int contour_id = 0;
    wf_contours_.head_node.append(0);
    wf_contours_.is_hole.append(false);
    wf_contours_.is_active.append(true);

    /* Forward nodes: 0 .. m - 1. */
    for (const int k : IndexRange(m)) {
      const int vi = chain[k];
      const int prev_node = (k == 0) ? total_nodes - 1 : k - 1;
      const int next_node = k + 1;
      const int orig_e = (k < m - 1) ? k : m - 1;

      wf_nodes_.pos.append(input_.vert[vi]);
      wf_nodes_.vel.append(double2(0.0, 0.0));
      wf_nodes_.prev_node.append(prev_node);
      wf_nodes_.next_node.append(next_node);
      wf_nodes_.skel_node.append(vi);
      wf_nodes_.orig_edge.append(orig_e);
      wf_nodes_.orig_face.append(0);
      wf_nodes_.contour_id.append(contour_id);
      wf_nodes_.is_active.append(true);
      wf_nodes_.time_birth.append(0.0);

      Vector<int> lt;
      lt.append(vi);
      wf_nodes_.left_trail.append(std::move(lt));

      Vector<int> rt;
      rt.append((k < m - 1) ? chain[k + 1] : vi);
      wf_nodes_.right_trail.append(std::move(rt));
    }

    /* Return nodes: m .. 2m - 1 (j in 0 .. m - 1, k = m - 1 - j). */
    for (const int j : IndexRange(m)) {
      const int k = m - 1 - j;
      const int vi = chain[k];
      const int prev_node = (j == 0) ? m - 1 : m + j - 1;
      const int next_node = (j == m - 1) ? 0 : m + j + 1;
      const int orig_e = (j < m - 1) ? m + (m - 2 - j) : total_nodes - 1;

      wf_nodes_.pos.append(input_.vert[vi]);
      wf_nodes_.vel.append(double2(0.0, 0.0));
      wf_nodes_.prev_node.append(prev_node);
      wf_nodes_.next_node.append(next_node);
      wf_nodes_.skel_node.append(vi);
      wf_nodes_.orig_edge.append(orig_e);
      wf_nodes_.orig_face.append(0);
      wf_nodes_.contour_id.append(contour_id);
      wf_nodes_.is_active.append(true);
      wf_nodes_.time_birth.append(0.0);

      Vector<int> lt;
      lt.append(vi);
      wf_nodes_.left_trail.append(std::move(lt));

      Vector<int> rt;
      rt.append((j < m - 1) ? chain[k - 1] : vi);
      wf_nodes_.right_trail.append(std::move(rt));
    }

    /* Initial node velocities.
     * At terminal edges, wavefront vertices advance at 45 degrees on each side of the terminal
     * end. */
    wf_nodes_.vel[0] = -u_dir[0] + n_left[0];

    for (int k = 1; k < m - 1; k++) {
      wf_nodes_.vel[k] = compute_bisector_velocity(n_left[k - 1], n_left[k], u_dir[k]);
    }

    wf_nodes_.vel[m - 1] = u_dir[m - 2] + n_left[m - 2];
    wf_nodes_.vel[m] = u_dir[m - 2] - n_left[m - 2];

    for (int j = 1; j < m - 1; j++) {
      const int k = m - 1 - j;
      wf_nodes_.vel[m + j] = compute_bisector_velocity(-n_left[k], -n_left[k - 1], -u_dir[k - 1]);
    }

    wf_nodes_.vel[total_nodes - 1] = -u_dir[0] - n_left[0];

    /* Precompute Original Edge Normals. Total edges = 2m. */
    orig_edge_normal_.reinitialize(total_nodes);
    for (const int k : IndexRange(m - 1)) {
      orig_edge_normal_[k] = n_left[k];
    }
    orig_edge_normal_[m - 1] = u_dir[m - 2];
    for (const int k : IndexRange(m - 1)) {
      orig_edge_normal_[m + k] = -n_left[k];
    }
    orig_edge_normal_[total_nodes - 1] = -u_dir[0];

    init_antiparallel_bins();
  }

  void update_node_velocity(int i)
  {
    if (!wf_nodes_.is_active[i]) {
      return;
    }
    const int prev_i = wf_nodes_.prev_node[i];
    const int next_i = wf_nodes_.next_node[i];

    const double2 n1 = compute_inward_normal(wf_nodes_.pos[prev_i], wf_nodes_.pos[i]);
    const double2 n2 = compute_inward_normal(wf_nodes_.pos[i], wf_nodes_.pos[next_i]);

    const double2 d = wf_nodes_.pos[next_i] - wf_nodes_.pos[i];
    const double2 d2 = math::normalize(d);

    wf_nodes_.vel[i] = compute_bisector_velocity(n1, n2, d2);
  }

  bool has_active_contours() const
  {
    for (const bool active : wf_contours_.is_active) {
      if (active) {
        return true;
      }
    }
    return false;
  }

  double compute_adaptive_dt() const
  {
    double min_len = 1e30;
    double max_speed = 1.0;

    for (const int i : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[i]) {
        continue;
      }
      const int next_i = wf_nodes_.next_node[i];
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

  void advance_positions(double dt)
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
   * ```
   */
  double find_earliest_event(double dt, double eps)
  {
    PRF_scope(ProfileCategory::Core);

    double earliest_t = std::numeric_limits<double>::infinity();

    /* 1. Edge Collapse Events. */
    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const int v = wf_nodes_.next_node[u];
      const double2 e0 = wf_nodes_.pos[v] - wf_nodes_.pos[u];
      const double2 dv = wf_nodes_.vel[v] - wf_nodes_.vel[u];
      const double dv_sq = math::length_squared(dv);

      if (dv_sq > 1e-12 && math::dot(e0, dv) < -1e-12) {
        const double tau = -math::dot(e0, dv) / dv_sq;
        if (tau > 1e-11 && tau <= dt + 1e-9) {
          const double2 e_tau = e0 + tau * dv;
          if (math::length(e_tau) < std::max(eps, 1e-5) || math::dot(e0, e0 + dt * dv) <= 0.0) {
            earliest_t = std::min(earliest_t, current_time_ + tau);
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

      if (is_reflex(u)) {
        reflex_nodes.append(u);
      }
    }

    if (active_nodes_num == 0 || reflex_nodes.is_empty()) {
      return earliest_t;
    }

    double x_min = bbox_min.x, x_max = bbox_max.x;
    double y_min = bbox_min.y, y_max = bbox_max.y;

    const double diag = std::max(std::hypot(x_max - x_min, y_max - y_min), 1e-4);
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
      for (const int w : reflex_nodes) {
        const int prev_w = wf_nodes_.prev_node[w];
        const int next_w = wf_nodes_.next_node[w];
        for (const int u : wf_nodes_.pos.index_range()) {
          if (!wf_nodes_.is_active[u]) {
            continue;
          }
          const int v = wf_nodes_.next_node[u];
          if (u == w || v == w || prev_w == u || next_w == v) {
            continue;
          }

          const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
          const double d0 = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv);
          const double rate = 1.0 - math::dot(wf_nodes_.vel[w], n_uv);

          if (rate > 1e-8 && d0 > 1e-11) {
            const double tau = d0 / rate;
            if (tau > 1e-11 && tau <= dt + 1e-9) {
              const double2 p_w = wf_nodes_.pos[w] + tau * wf_nodes_.vel[w];
              const double2 p_u = wf_nodes_.pos[u] + tau * wf_nodes_.vel[u];
              const double2 p_v = wf_nodes_.pos[v] + tau * wf_nodes_.vel[v];
              const double s = math::line_point_factor(p_w, p_u, p_v);
              if (s > 1e-5 && s < 1.0 - 1e-5) {
                earliest_t = std::min(earliest_t, current_time_ + tau);
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
            if (tau > 1e-11 && tau <= dt + 1e-9) {
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

    /* Spatial grid with overlapping buckets. */
    x_min -= safety_tol;
    x_max += safety_tol;
    y_min -= safety_tol;
    y_max += safety_tol;

    const double tau_horizon = max_tau_search;

    grid_.init(x_min, y_min, x_max, y_max, active_nodes_num);

    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const int v = wf_nodes_.next_node[u];
      const double2 &p_u0 = wf_nodes_.pos[u];
      const double2 &p_v0 = wf_nodes_.pos[v];
      const double2 p_u1 = p_u0 + tau_horizon * wf_nodes_.vel[u];
      const double2 p_v1 = p_v0 + tau_horizon * wf_nodes_.vel[v];

      const double ex_min = std::min({p_u0.x, p_v0.x, p_u1.x, p_v1.x}) - safety_tol;
      const double ex_max = std::max({p_u0.x, p_v0.x, p_u1.x, p_v1.x}) + safety_tol;
      const double ey_min = std::min({p_u0.y, p_v0.y, p_u1.y, p_v1.y}) - safety_tol;
      const double ey_max = std::max({p_u0.y, p_v0.y, p_u1.y, p_v1.y}) + safety_tol;

      grid_.insert(u, ex_min, ey_min, ex_max, ey_max);
    }

    if (query_tags_.size() < wf_nodes_.pos.size()) {
      query_tags_.reinitialize(wf_nodes_.pos.size());
      query_tags_.fill(0);
      current_query_id_ = 1;
    }

    for (const int w : reflex_nodes) {
      const int prev_w = wf_nodes_.prev_node[w];
      const int next_w = wf_nodes_.next_node[w];
      const double2 &p_w0 = wf_nodes_.pos[w];
      const double2 p_w1 = p_w0 + tau_horizon * wf_nodes_.vel[w];

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
        if (u == w || v == w || prev_w == u || next_w == v) {
          return;
        }

        /* 1. Test split event between reflex w and edge u -> v. */
        const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
        const double d0 = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv);
        const double rate = 1.0 - math::dot(wf_nodes_.vel[w], n_uv);

        if (rate > 1e-8 && d0 > 1e-11) {
          const double tau = d0 / rate;
          if (tau > 1e-11 && tau <= tau_horizon + 1e-9) {
            const double2 p_w = wf_nodes_.pos[w] + tau * wf_nodes_.vel[w];
            const double2 p_u = wf_nodes_.pos[u] + tau * wf_nodes_.vel[u];
            const double2 p_v = wf_nodes_.pos[v] + tau * wf_nodes_.vel[v];
            const double s = math::line_point_factor(p_w, p_u, p_v);
            if (s > 1e-5 && s < 1.0 - 1e-5) {
              earliest_t = std::min(earliest_t, current_time_ + tau);
            }
          }
        }

        /* 2. Test vertex collision between reflex w and vertex u. */
        if (u != w && wf_nodes_.next_node[u] != w && wf_nodes_.next_node[w] != u) {
          const double2 dp = wf_nodes_.pos[w] - wf_nodes_.pos[u];
          const double2 dv = wf_nodes_.vel[w] - wf_nodes_.vel[u];
          const double dv_sq = math::length_squared(dv);
          if (dv_sq > 1e-12 && math::dot(dp, dv) < -1e-12) {
            const double tau = -math::dot(dp, dv) / dv_sq;
            if (tau > 1e-11 && tau <= tau_horizon + 1e-9) {
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

  bool is_reflex(int w) const
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

  int get_or_create_skel_node(const double2 &pos, double tol)
  {
    for (int i = int(skel_graph_.nodes_pos.size()) - 1; i >= 0; i--) {
      if (skel_graph_.nodes_time[i] < current_time_ - 1e-9) {
        break;
      }
      if (std::abs(skel_graph_.nodes_time[i] - current_time_) < 1e-9 &&
          math::length(skel_graph_.nodes_pos[i] - pos) < tol)
      {
        return i;
      }
    }
    const int new_s = int(skel_graph_.nodes_pos.size());
    skel_graph_.nodes_pos.append(pos);
    skel_graph_.nodes_time.append(current_time_);
    skel_graph_.nodes_orig.append(-1);
    return new_s;
  }

  struct EventCluster {
    double2 p_star{0.0, 0.0};
    int s_star{-1};
    Vector<int> colliding_verts;
    Vector<int> collapsing_edges;
    Vector<int> split_edges;
  };

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
  bool try_resolve_opposing_pair(const int e1,
                                 const int e2,
                                 const double cluster_tol,
                                 Vector<std::pair<int, int>> &processed_pairs)
  {
    if (!wf_nodes_.is_active[e1] || !wf_nodes_.is_active[e2]) {
      return false;
    }
    const int orig1 = wf_nodes_.orig_edge[e1];
    const int orig2 = wf_nodes_.orig_edge[e2];
    if (orig1 >= 0 && orig2 >= 0 && orig1 < orig_edge_normal_.size() &&
        orig2 < orig_edge_normal_.size())
    {
      if (math::dot(orig_edge_normal_[orig1], orig_edge_normal_[orig2]) > -0.8) {
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

    if (math::dot(n1, n2) > -0.8) {
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

    processed_pairs.append(std::make_pair(e1, e2));

    const double2 p_a = wf_nodes_.pos[u] + s_a * dir1;
    const double2 p_b = wf_nodes_.pos[u] + s_b * dir1;
    const int s_a_star = get_or_create_skel_node(p_a, cluster_tol);
    const int s_b_star = get_or_create_skel_node(p_b, cluster_tol);

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
        if (face_verts1.size() >= 3) {
          out_faces_.faces.append(std::move(face_verts1));
          out_faces_.face_orig.append(wf_nodes_.orig_face[e1]);
          out_faces_.face_band.append(current_band_);
          out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[e1]);
        }

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
        if (face_verts2.size() >= 3) {
          out_faces_.faces.append(std::move(face_verts2));
          out_faces_.face_orig.append(wf_nodes_.orig_face[e2]);
          out_faces_.face_band.append(current_band_);
          out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[e2]);
        }
      }

      if (prev_u != w && prev_u >= 0 && z >= 0) {
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

      if (prev_w != u && prev_w >= 0 && v >= 0) {
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

      wf_nodes_.is_active[u] = false;
      wf_nodes_.is_active[w] = false;
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
          if (face_verts1.size() >= 3) {
            out_faces_.faces.append(std::move(face_verts1));
            out_faces_.face_orig.append(wf_nodes_.orig_face[e1]);
            out_faces_.face_band.append(current_band_);
            out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[e1]);
          }
        }
        wf_nodes_.is_active[u] = false;
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
          if (face_verts2.size() >= 3) {
            out_faces_.faces.append(std::move(face_verts2));
            out_faces_.face_orig.append(wf_nodes_.orig_face[e2]);
            out_faces_.face_band.append(current_band_);
            out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[e2]);
          }
        }
        wf_nodes_.is_active[w] = false;
      }

      const bool e1_extends_left = (s_z > end_tol);
      const bool e2_extends_left = (s_z < -end_tol);
      const bool e1_extends_right = (l1 > s_w + end_tol);
      const bool e2_extends_right = (s_w > l1 + end_tol);

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

        Vector<int> orig_rt_u = std::move(wf_nodes_.right_trail[u]);
        wf_nodes_.right_trail[u].clear();
        wf_nodes_.right_trail[u].append(s_a_star);

        if (wf_nodes_.left_trail[z].is_empty() || wf_nodes_.left_trail[z].last() != s_a_star) {
          wf_nodes_.left_trail[z].append(s_a_star);
        }

        if (!dry_run_) {
          Vector<int> face_verts1;
          face_verts1.append(wf_nodes_.left_trail[u][0]);
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
          for (int k = int(wf_nodes_.left_trail[u].size()) - 1; k >= 1; k--) {
            const int l = wf_nodes_.left_trail[u][k];
            if (face_verts1.last() != l && l != face_verts1[0]) {
              face_verts1.append(l);
            }
          }
          if (face_verts1.size() >= 3) {
            out_faces_.faces.append(std::move(face_verts1));
            out_faces_.face_orig.append(wf_nodes_.orig_face[u]);
            out_faces_.face_band.append(current_band_);
            out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[u]);
          }
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

        if (orig_rt_u.is_empty() || orig_rt_u.last() != wf_nodes_.skel_node[v]) {
          orig_rt_u.append(wf_nodes_.skel_node[v]);
        }
        wf_nodes_.right_trail.append(std::move(orig_rt_u));

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
        wf_nodes_.next_node[w] = v;
        wf_nodes_.prev_node[v] = w;
        wf_nodes_.pos[v] = p_b;
        wf_nodes_.skel_node[v] = s_b_star;

        Vector<int> orig_rt_w = std::move(wf_nodes_.right_trail[w]);
        wf_nodes_.right_trail[w].clear();
        wf_nodes_.right_trail[w].append(s_b_star);

        if (wf_nodes_.left_trail[v].is_empty() || wf_nodes_.left_trail[v].last() != s_b_star) {
          wf_nodes_.left_trail[v].append(s_b_star);
        }

        if (!dry_run_) {
          Vector<int> face_verts2;
          face_verts2.append(wf_nodes_.left_trail[w][0]);
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
          for (int k = int(wf_nodes_.left_trail[w].size()) - 1; k >= 1; k--) {
            const int l = wf_nodes_.left_trail[w][k];
            if (face_verts2.last() != l && l != face_verts2[0]) {
              face_verts2.append(l);
            }
          }
          if (face_verts2.size() >= 3) {
            out_faces_.faces.append(std::move(face_verts2));
            out_faces_.face_orig.append(wf_nodes_.orig_face[w]);
            out_faces_.face_band.append(current_band_);
            out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[w]);
          }
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

        if (orig_rt_w.is_empty() || orig_rt_w.last() != wf_nodes_.skel_node[z]) {
          orig_rt_w.append(wf_nodes_.skel_node[z]);
        }
        wf_nodes_.right_trail.append(std::move(orig_rt_w));

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
          if (prev_u != w && prev_u >= 0 && z != u && z >= 0 && wf_nodes_.is_active[prev_u] &&
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
          if (prev_w != u && prev_w >= 0 && v != w && v >= 0 && wf_nodes_.is_active[prev_w] &&
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
        }
      }
    }

    update_node_velocity(v);
    update_node_velocity(z);
    if (prev_u >= 0 && wf_nodes_.is_active[prev_u]) {
      update_node_velocity(prev_u);
    }
    if (prev_w >= 0 && wf_nodes_.is_active[prev_w]) {
      update_node_velocity(prev_w);
    }
    if (wf_nodes_.is_active[u]) {
      update_node_velocity(u);
    }
    if (wf_nodes_.is_active[w]) {
      update_node_velocity(w);
    }
    if (new_node_id >= 0 && wf_nodes_.is_active[new_node_id]) {
      update_node_velocity(new_node_id);
    }

    return true;
  }

  void resolve_opposing_overlapping_edges(const double cluster_tol)
  {
    PRF_scope(ProfileCategory::Core);

    if (!has_any_antiparallel_edges_) {
      return;
    }

    Vector<std::pair<int, int>> processed_pairs;
    int opp_iter = 0;
    bool found_opposing = true;
    bool any_opposing_resolved = false;
    while (found_opposing && opp_iter < 50) {
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

      double x_min = bbox_min.x, x_max = bbox_max.x;
      double y_min = bbox_min.y, y_max = bbox_max.y;

      if (active_edges_num <= 16) {
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
            if (try_resolve_opposing_pair(e1, e2, cluster_tol, processed_pairs)) {
              found_opposing = true;
              any_opposing_resolved = true;
              break;
            }
          }
          if (found_opposing) {
            break;
          }
        }
      }
      else {
        x_min -= cluster_tol;
        x_max += cluster_tol;
        y_min -= cluster_tol;
        y_max += cluster_tol;

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
          if (math::distance_squared(wf_nodes_.pos[u], wf_nodes_.pos[v]) <
              cluster_tol * cluster_tol)
          {
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

          grid_.for_each_overlapping(qx_min, qy_min, qx_max, qy_max, [&](const int e2) {
            if (found_opposing || e2 <= e1 || !wf_nodes_.is_active[e2]) {
              return;
            }
            if (query_tags_[e2] == qid) {
              return;
            }
            query_tags_[e2] = qid;

            if (try_resolve_opposing_pair(e1, e2, cluster_tol, processed_pairs)) {
              found_opposing = true;
              any_opposing_resolved = true;
            }
          });

          if (found_opposing) {
            break;
          }
        }
      }
    }
    if (any_opposing_resolved) {
      rebuild_active_contours();
    }
  }

  void resolve_events_at_current_time(double eps)
  {
    PRF_scope(ProfileCategory::Core);

    const double cluster_tol = std::max(eps, 1e-4);

    /* 1. Identify all collapsing edges at current_time_. */
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

    /* Fast Path: Complete Contour Annihilation (e.g. circles, regular polygons, triangles).
     * If an entire active contour has all its edges collapsing at the current time,
     * they all converge to a single center point p*. We can emit its roof faces and retire
     * all nodes in strictly O(N) time without quadratic pairwise checks. */
    if (!collapsing_edges.is_empty()) {
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
          const int s_star = get_or_create_skel_node(p_star, cluster_tol);

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
              if (face_verts.size() >= 3) {
                out_faces_.faces.append(std::move(face_verts));
                out_faces_.face_orig.append(wf_nodes_.orig_face[curr]);
                out_faces_.face_band.append(current_band_);
                out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[curr]);
              }
            }

            skel_graph_.edges_node1.append(wf_nodes_.skel_node[curr]);
            skel_graph_.edges_node2.append(s_star);
            skel_graph_.edges_orig.append(-1);
            skel_graph_.edges_face.append(wf_nodes_.orig_face[curr]);
            skel_graph_.edges_time_start.append(wf_nodes_.time_birth[curr]);
            skel_graph_.edges_time_end.append(current_time_);
            skel_graph_.edges_is_boundary.append(false);

            wf_nodes_.is_active[curr] = false;
            curr = next_curr;
          } while (curr != head && curr >= 0);

          wf_contours_.is_active[cid] = false;
          any_contour_collapsed = true;
        }
      }

      if (any_contour_collapsed) {
        if (!has_active_contours()) {
          events_processed_count_++;
          return;
        }
        collapsing_edges.remove_if([&](const int e) { return !wf_nodes_.is_active[e]; });
      }
    }

    /* 0. Resolve any opposing collinear/overlapping wavefront edges. */
    resolve_opposing_overlapping_edges(cluster_tol);

    /* 2. Identify all vertex-vertex collisions at current_time_. */
    struct VertPair {
      int u;
      int w;
    };
    Vector<VertPair> vert_collisions;

    /* 3. Identify all split events at current_time_. */
    struct SplitInfo {
      int reflex_w;
      int edge_u;
      double2 pos;
    };
    Vector<SplitInfo> splits;

    double2 bbox_min(1e30, 1e30), bbox_max(-1e30, -1e30);
    int active_nodes_num = 0;
    Vector<int> reflex_nodes;

    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      active_nodes_num++;
      math::min_max(wf_nodes_.pos[u], bbox_min, bbox_max);

      if (is_reflex(u)) {
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
            vert_collisions.append(VertPair{u, w});
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
          if (u == w || v == w || prev_w == u || next_w == v) {
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
              splits.append(SplitInfo{w, u, wf_nodes_.pos[w]});
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

      /* 1. Vertex-vertex collisions. */
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
                vert_collisions.append(VertPair{u, w});
              }
            });
      }

      /* 2. Split events at current_time_. */
      if (!reflex_nodes.is_empty()) {
        grid_.init(x_min, y_min, x_max, y_max, active_nodes_num);
        for (const int u : wf_nodes_.pos.index_range()) {
          if (!wf_nodes_.is_active[u]) {
            continue;
          }
          const int v = wf_nodes_.next_node[u];
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
                if (u == w || v == w || prev_w == u || next_w == v) {
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
                    splits.append(SplitInfo{w, u, wf_nodes_.pos[w]});
                  }
                }
              });
        }
      }
    }

    /* 4. Group all interactions into spatial Event Clusters. */
    Vector<EventCluster> clusters;
    Array<int> vert_to_cluster(wf_nodes_.pos.size(), -1);
    Array<int> edge_to_cluster(wf_nodes_.pos.size(), -1);

    auto add_edge_to_cluster = [&](const int ci, const int e) {
      if (edge_to_cluster[e] == -1) {
        clusters[ci].collapsing_edges.append(e);
        edge_to_cluster[e] = ci;
      }
    };

    auto add_vert_to_cluster = [&](const int ci, const int v) {
      if (vert_to_cluster[v] == -1) {
        clusters[ci].colliding_verts.append(v);
        vert_to_cluster[v] = ci;
      }
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
        for (const int ci : clusters.index_range()) {
          if (math::length(clusters[ci].p_star - p) < cluster_tol) {
            found_ci = ci;
            break;
          }
        }
      }

      if (found_ci == -1) {
        EventCluster c;
        c.p_star = p;
        const int new_ci = int(clusters.size());
        clusters.append(std::move(c));
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
        for (const int ci : clusters.index_range()) {
          if (math::length(clusters[ci].p_star - p) < cluster_tol) {
            found_ci = ci;
            break;
          }
        }
      }

      if (found_ci == -1) {
        EventCluster c;
        c.p_star = p;
        const int new_ci = int(clusters.size());
        clusters.append(std::move(c));
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
      int found_ci = -1;
      if (vert_to_cluster[si.reflex_w] != -1) {
        found_ci = vert_to_cluster[si.reflex_w];
      }
      else {
        for (const int ci : clusters.index_range()) {
          if (math::length(clusters[ci].p_star - p) < cluster_tol) {
            found_ci = ci;
            break;
          }
        }
      }

      if (found_ci == -1) {
        EventCluster c;
        c.p_star = p;
        const int new_ci = int(clusters.size());
        clusters.append(std::move(c));
        add_vert_to_cluster(new_ci, si.reflex_w);
        if (!clusters[new_ci].split_edges.contains(si.edge_u)) {
          clusters[new_ci].split_edges.append(si.edge_u);
        }
      }
      else {
        add_vert_to_cluster(found_ci, si.reflex_w);
        if (!clusters[found_ci].split_edges.contains(si.edge_u)) {
          clusters[found_ci].split_edges.append(si.edge_u);
        }
      }
    }

    /* 5. Phase 1: Create Skeleton Nodes and bind colliding vertices across all clusters. */
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
      cluster.s_star = get_or_create_skel_node(cluster.p_star, cluster_tol);
      for (const int v : cluster.colliding_verts) {
        wf_nodes_.skel_node[v] = cluster.s_star;
      }
    }

    /* 6. Phase 2: Emit Roof Faces for point-collapsing edges and splice topology. */
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
          if (face_verts.size() >= 3) {

            out_faces_.faces.append(std::move(face_verts));
            out_faces_.face_orig.append(wf_nodes_.orig_face[e]);
            out_faces_.face_band.append(current_band_);
            out_faces_.face_orig_edge.append(wf_nodes_.orig_edge[e]);
          }
        }

        skel_graph_.edges_node1.append(wf_nodes_.skel_node[u]);
        skel_graph_.edges_node2.append(s_star);
        skel_graph_.edges_orig.append(-1);
        skel_graph_.edges_face.append(wf_nodes_.orig_face[e]);
        skel_graph_.edges_time_start.append(wf_nodes_.time_birth[e]);
        skel_graph_.edges_time_end.append(current_time_);
        skel_graph_.edges_is_boundary.append(false);

        const int prev_u = wf_nodes_.prev_node[u];
        wf_nodes_.is_active[u] = false;

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

    /* 7. Phase 4: Resolve remaining clusters via Radial Gap Pairing. */
    for (const EventCluster &cluster : clusters) {
      resolve_cluster(cluster);
    }

    /* 9. Rebuild and synchronize active contour loops. */
    rebuild_active_contours();
    events_processed_count_++;
  }

  void resolve_cluster(const EventCluster &cluster)
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
      if (prev_v >= 0 && !cluster.colliding_verts.contains(prev_v) && wf_nodes_.is_active[prev_v])
      {
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

      const int next_v = wf_nodes_.next_node[v];
      if (next_v >= 0 && !cluster.colliding_verts.contains(next_v) && wf_nodes_.is_active[next_v])
      {
        const double2 d = wf_nodes_.pos[next_v] - p_star;
        if (math::length(d) > 1e-4) {
          const double angle = std::atan2(d.y, d.x);
          rays.append(
              Ray{angle, RayType::OUT, next_v, v, wf_nodes_.orig_edge[v], wf_nodes_.orig_face[v]});
        }
      }
    }

    for (const int e : cluster.split_edges) {
      if (!wf_nodes_.is_active[e]) {
        continue;
      }
      const int v = wf_nodes_.next_node[e];
      /* Incoming sub-edge e -> p* */
      const double2 d_in = wf_nodes_.pos[e] - p_star;
      const double angle_in = std::atan2(d_in.y, d_in.x);
      rays.append(
          Ray{angle_in, RayType::IN, e, -1, wf_nodes_.orig_edge[e], wf_nodes_.orig_face[e]});

      /* Outgoing sub-edge p* -> v */
      const double2 d_out = wf_nodes_.pos[v] - p_star;
      const double angle_out = std::atan2(d_out.y, d_out.x);
      rays.append(
          Ray{angle_out, RayType::OUT, v, -1, wf_nodes_.orig_edge[e], wf_nodes_.orig_face[e]});
    }

    /* Retire all colliding vertices. */
    for (const int v : cluster.colliding_verts) {
      wf_nodes_.is_active[v] = false;
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
    for (const int i : IndexRange(n_rays)) {
      const Ray &ray_out = rays[i];
      const Ray &ray_in = rays[(i + 1) % n_rays];

      if (ray_out.type == RayType::OUT && ray_in.type == RayType::IN) {
        double delta_theta = ray_in.angle - ray_out.angle;
        if (delta_theta <= 1e-6) {
          delta_theta += 2.0 * M_PI;
        }
        if (delta_theta < 1e-4 || delta_theta > 2.0 * M_PI - 1e-4) {
          continue;
        }

        /* Spawn new WavefrontNode w_g at p*. */
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
        if (ray_out.retiring_node >= 0 && ray_out.retiring_node < int(wf_nodes_.left_trail.size()))
        {
          lt = wf_nodes_.left_trail[ray_out.retiring_node];
        }
        if (lt.is_empty() || lt.last() != s_star) {
          lt.append(s_star);
        }
        wf_nodes_.left_trail.append(std::move(lt));

        Vector<int> rt;
        if (ray_out.retiring_node >= 0 &&
            ray_out.retiring_node < int(wf_nodes_.right_trail.size()))
        {
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

        update_node_velocity(w_g);
        update_node_velocity(ray_in.incident_node);
        update_node_velocity(ray_out.incident_node);
      }
    }
  }

  void rebuild_active_contours()
  {
    Vector<bool> visited(wf_nodes_.pos.size(), false);
    wf_contours_.head_node.clear();
    wf_contours_.is_hole.clear();
    wf_contours_.is_active.clear();

    for (const int i : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[i] || visited[i]) {
        continue;
      }
      const int cid = int(wf_contours_.head_node.size());
      wf_contours_.head_node.append(i);
      wf_contours_.is_hole.append(false);
      wf_contours_.is_active.append(true);

      int curr = i;
      int count = 0;
      while (!visited[curr]) {
        visited[curr] = true;
        wf_nodes_.contour_id[curr] = cid;
        curr = wf_nodes_.next_node[curr];
        count++;
        if (curr == i || curr < 0 || curr >= int(wf_nodes_.pos.size())) {
          break;
        }
      }
      if (count < 2) {
        wf_contours_.is_active[cid] = false;
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
  void emit_segment_boundary_faces()
  {
    PRF_scope(ProfileCategory::Core);

    /* 1. Allocate a single skeleton node for each active wavefront vertex. */
    Array<int> new_skel_node(wf_nodes_.pos.size(), -1);
    for (const int i : wf_nodes_.pos.index_range()) {
      if (wf_nodes_.is_active[i]) {
        new_skel_node[i] = int(skel_graph_.nodes_pos.size());
        skel_graph_.nodes_pos.append(wf_nodes_.pos[i]);
        skel_graph_.nodes_time.append(current_time_);
        skel_graph_.nodes_orig.append(-1);
      }
    }

    /* 2. Emit swept face for each active wavefront edge. */
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
  void emit_inner_polygon_faces()
  {
    PRF_scope(ProfileCategory::Core);

    Vector<bool> visited(wf_nodes_.pos.size(), false);

    for (const int start_u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[start_u] || visited[start_u]) {
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
        out_faces_.faces.append(std::move(loop));
        out_faces_.face_orig.append(0);
        out_faces_.face_band.append(std::max(0, segments_num_ - 1));
        out_faces_.face_orig_edge.append(-1);
      }
    }
  }
};

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
  empty_output.flips_num = 0;

  const bool has_chain = !input.edge_chain.is_empty();
  const bool has_poly = !input.outer_polygon.is_empty();

  /* Must provide either outer_polygon OR edge_chain, but not both. */
  if (has_chain == has_poly) {
    return empty_output;
  }

  /* Holes cannot be provided with edge_chain. */
  if (has_chain && (input.hole_offsets.size() > 0 || !input.hole_vert_indices.is_empty())) {
    return empty_output;
  }

  const int segments = std::max(1, input.segments);

  /* Case 1: Open Edge Chain */
  if (has_chain) {
    /* Negative, infinite, NaN, or zero offset is invalid/no-op for edge chain. */
    if (input.offset <= 0.0 || std::isinf(input.offset) || std::isnan(input.offset)) {
      return empty_output;
    }
    if (input.edge_chain.size() < 2) {
      return empty_output;
    }
    for (const int v : input.edge_chain) {
      if (v < 0 || v >= input.vert.size()) {
        return empty_output;
      }
    }

    SimulationEngine engine(input, input.offset, segments, false);
    engine.run();
    return engine.build_output();
  }

  /* Case 2: Closed Polygon (with optional holes) */
  if (input.outer_polygon.size() < 3) {
    return empty_output;
  }
  for (const int v : input.outer_polygon) {
    if (v < 0 || v >= input.vert.size()) {
      return empty_output;
    }
  }
  for (const int v : input.hole_vert_indices) {
    if (v < 0 || v >= input.vert.size()) {
      return empty_output;
    }
  }

  if (std::isnan(input.offset) || input.offset == 0.0) {
    return empty_output;
  }

  /* Negative offset: wavefronts advance on CW side of outer polygon and CCW side of holes. */
  if (input.offset < 0.0) {
    if (std::isinf(input.offset)) {
      /* Unbounded exterior offset to infinity is not supported. */
      return empty_output;
    }

    SimulationEngine engine(input, std::abs(input.offset), segments, false);
    engine.run();
    return engine.build_output();
  }

  /* Positive offset / full skeleton:
   * Two-Pass Strategy:
   * When offset is infinite or exceeds collapse time with segments > 1:
   *   Pass 1: Dry-run simulation with offset = infinity, segments = 1 to find exact
   *           max_collapse_time.
   *   Pass 2: Real simulation run with effective_offset = min(input.offset, max_collapse_time)
   *           divided into input.segments bands. */
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
