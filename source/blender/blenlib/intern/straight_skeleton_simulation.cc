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
 */

#include "straight_skeleton_intern.hh"
#include "straight_skeleton_verify.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <vector>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_time.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

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
  const double2 u = d / len;
  return double2(-u.y, u.x);
}

/** Compute angular bisector velocity vector from incoming and outgoing edge normals. */
static double2 compute_bisector_velocity(const double2 &n1, const double2 &n2, const double2 &d2)
{
  const double det = n1.x * n2.y - n1.y * n2.x;
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
    area += (p1.x * p2.y - p2.x * p1.y);
  }
  return 0.5 * area;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2D Spatial Grid with Overlapping Buckets
 * \{ */

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

  bool use_priority_queue_{false};

  enum class SimEventType : uint8_t {
    EDGE_COLLAPSE,
    SPLIT,
    VERT_COLLISION,
  };

  struct SimEvent {
    double time;
    SimEventType type;
    int u;
    int v;
    int target_v;
    int version_u;
    int version_v;
    int version_target_v;

    bool operator>(const SimEvent &other) const
    {
      return time > other.time;
    }
  };

  std::priority_queue<SimEvent, std::vector<SimEvent>, std::greater<SimEvent>> pq_;
  Vector<int> node_version_;
  Vector<int> modified_nodes_;

  SpatialGrid2D grid_;
  SpatialGrid2D vert_grid_;
  Vector<int> query_tags_;
  int current_query_id_{0};

 public:
  SimulationEngine(const StraightSkeletonInput &input,
                   double target_offset,
                   int segments,
                   bool dry_run)
      : input_(input), target_offset_(target_offset), segments_num_(segments), dry_run_(dry_run)
  {
    use_priority_queue_ = input_.use_priority_queue;
    if (const char *env = std::getenv("STRAIGHT_SKELETON_USE_PQ")) {
      use_priority_queue_ = (std::string_view(env) == "1" || std::string_view(env) == "true");
    }
  }

  double run()
  {
    init_state();

    const double eps = (input_.epsilon > 0.0) ? input_.epsilon : 1e-8;
    if (use_priority_queue_) {
      init_pq(eps);
    }
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
      Vector<SimEvent> simultaneous_events;
      double event_t = std::numeric_limits<double>::infinity();
      if (use_priority_queue_) {
        event_t = find_earliest_event_pq(max_lookahead, eps, simultaneous_events);
      }
      else {
        event_t = find_earliest_event(max_lookahead, eps);
      }

      if (!std::isinf(event_t) && event_t <= next_seg_time + 1e-9 &&
          event_t <= target_offset_ + 1e-9)
      {
        /* Advance simulation to event timestamp. */
        advance_positions(event_t - current_time_);
        current_time_ = event_t;
        max_collapse_time_ = std::max(max_collapse_time_, current_time_);

        /* Resolve all concurrent events at current_time_. */
        resolve_events_at_current_time(eps, simultaneous_events);
        if (use_priority_queue_) {
          update_pq_for_modified_nodes(eps);
        }
      }
      else if (!std::isinf(next_seg_time) && next_seg_time <= target_offset_ + 1e-9) {
        /* Advance to segment threshold. */
        advance_positions(next_seg_time - current_time_);
        current_time_ = next_seg_time;
        max_collapse_time_ = std::max(max_collapse_time_, current_time_);

        Vector<SimEvent> seg_events;
        if (use_priority_queue_) {
          peek_events_at_current_time(eps, seg_events);
        }

        /* Resolve any events that coincide with segment threshold. */
        resolve_events_at_current_time(eps, seg_events);
        if (use_priority_queue_) {
          update_pq_for_modified_nodes(eps);
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

    /* If finished with finite target offset and surviving contours remain, emit inner polygon. */
    if (!dry_run_ && !std::isinf(input_.offset) && has_active_contours() &&
        current_time_ >= target_offset_ - 1e-7)
    {
      emit_inner_polygon_faces();
    }

    return max_collapse_time_;
  }

  StraightSkeletonOutput build_output()
  {
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
     * When an advancing wavefront edge is split by a reflex vertex into multiple sub-edges,
     * each sub-edge emits a sub-polygon upon collapse. These sub-polygons share an internal
     * seam with opposite orientations. Merging them eliminates internal triangulation chords
     * and produces the true, unified straight skeleton face with its V-notch at the saddle point.
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
  void init_state()
  {
    /* 1. Initialize Skeleton Nodes from input vertices. */
    const int input_verts_num = int(input_.vert.size());
    for (const int i : IndexRange(input_verts_num)) {
      skel_graph_.nodes_pos.append(input_.vert[i]);
      skel_graph_.nodes_time.append(0.0);
      skel_graph_.nodes_orig.append(i);
    }

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

    /* 4. Compute Initial Bisector Velocities. */
    for (const int i : wf_nodes_.pos.index_range()) {
      update_node_velocity(i);
    }
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
    const double len = math::length(d);
    const double2 d2 = (len > 1e-12) ? d / len : double2(0.0, 0.0);

    wf_nodes_.vel[i] = compute_bisector_velocity(n1, n2, d2);

    if (use_priority_queue_) {
      if (i < node_version_.size()) {
        node_version_[i]++;
      }
      modified_nodes_.append(i);
    }
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

  double find_earliest_event(double dt, double eps)
  {
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

    double x_min = 1e30, x_max = -1e30, y_min = 1e30, y_max = -1e30;
    int active_nodes_num = 0;
    Vector<int> reflex_nodes;

    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      active_nodes_num++;
      const double2 &p = wf_nodes_.pos[u];
      x_min = std::min(x_min, p.x);
      x_max = std::max(x_max, p.x);
      y_min = std::min(y_min, p.y);
      y_max = std::max(y_max, p.y);

      if (is_reflex(u)) {
        reflex_nodes.append(u);
      }
    }

    if (active_nodes_num == 0 || reflex_nodes.is_empty()) {
      return earliest_t;
    }

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
              const double2 e_uv = p_v - p_u;
              const double len_sq = math::length_squared(e_uv);

              if (len_sq > 1e-12) {
                const double s = math::dot(p_w - p_u, e_uv) / len_sq;
                if (s > 1e-5 && s < 1.0 - 1e-5) {
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
            const double2 e_uv = p_v - p_u;
            const double len_sq = math::length_squared(e_uv);

            if (len_sq > 1e-12) {
              const double s = math::dot(p_w - p_u, e_uv) / len_sq;
              if (s > 1e-5 && s < 1.0 - 1e-5) {
                earliest_t = std::min(earliest_t, current_time_ + tau);
              }
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

  void schedule_edge_collapse(int u, double eps)
  {
    if (!wf_nodes_.is_active[u]) {
      return;
    }
    const int v = wf_nodes_.next_node[u];
    if (v < 0 || !wf_nodes_.is_active[v]) {
      return;
    }
    const double2 e0 = wf_nodes_.pos[v] - wf_nodes_.pos[u];
    const double2 dv = wf_nodes_.vel[v] - wf_nodes_.vel[u];
    const double dv_sq = math::length_squared(dv);

    if (dv_sq > 1e-12 && math::dot(e0, dv) < -1e-12) {
      const double tau = -math::dot(e0, dv) / dv_sq;
      if (tau > 1e-11) {
        const double2 e_tau = e0 + tau * dv;
        if (math::length(e_tau) < std::max(eps, 1e-5)) {
          const double event_t = current_time_ + tau;
          pq_.push(SimEvent{event_t,
                            SimEventType::EDGE_COLLAPSE,
                            u,
                            v,
                            -1,
                            node_version_[u],
                            node_version_[v],
                            -1});
        }
      }
    }
  }

  void test_and_schedule_split(int w, int u)
  {
    if (!wf_nodes_.is_active[w] || !wf_nodes_.is_active[u]) {
      return;
    }
    const int prev_w = wf_nodes_.prev_node[w];
    const int next_w = wf_nodes_.next_node[w];
    const int v = wf_nodes_.next_node[u];
    if (v < 0 || !wf_nodes_.is_active[v]) {
      return;
    }
    if (u == w || v == w || prev_w == u || next_w == v) {
      return;
    }

    const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
    const double d0 = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv);
    const double rate = 1.0 - math::dot(wf_nodes_.vel[w], n_uv);

    if (rate > 1e-8 && d0 > 1e-11) {
      const double tau = d0 / rate;
      if (tau > 1e-11) {
        const double2 p_w = wf_nodes_.pos[w] + tau * wf_nodes_.vel[w];
        const double2 p_u = wf_nodes_.pos[u] + tau * wf_nodes_.vel[u];
        const double2 p_v = wf_nodes_.pos[v] + tau * wf_nodes_.vel[v];
        const double2 e_uv = p_v - p_u;
        const double len_sq = math::length_squared(e_uv);

        if (len_sq > 1e-12) {
          const double s = math::dot(p_w - p_u, e_uv) / len_sq;
          if (s > 1e-5 && s < 1.0 - 1e-5) {
            const double event_t = current_time_ + tau;
            pq_.push(SimEvent{event_t,
                              SimEventType::SPLIT,
                              w,
                              u,
                              v,
                              node_version_[w],
                              node_version_[u],
                              node_version_[v]});
          }
        }
      }
    }
  }

  void test_and_schedule_vert_collision(int u, int w, double eps)
  {
    if (!wf_nodes_.is_active[u] || !wf_nodes_.is_active[w]) {
      return;
    }
    if (wf_nodes_.next_node[u] == w || wf_nodes_.next_node[w] == u) {
      return;
    }

    const double2 dp = wf_nodes_.pos[w] - wf_nodes_.pos[u];
    const double2 dv = wf_nodes_.vel[w] - wf_nodes_.vel[u];
    const double dv_sq = math::length_squared(dv);

    if (dv_sq > 1e-12 && math::dot(dp, dv) < -1e-12) {
      const double tau = -math::dot(dp, dv) / dv_sq;
      if (tau > 1e-11) {
        const double dist_at_tau = math::length(dp + tau * dv);
        if (dist_at_tau < std::max(eps, 1e-5)) {
          const double event_t = current_time_ + tau;
          pq_.push(SimEvent{event_t,
                            SimEventType::VERT_COLLISION,
                            u,
                            w,
                            -1,
                            node_version_[u],
                            node_version_[w],
                            -1});
        }
      }
    }
  }

  bool is_event_valid(const SimEvent &ev) const
  {
    if (ev.type == SimEventType::EDGE_COLLAPSE) {
      const int u = ev.u;
      const int v = ev.v;
      if (u < 0 || u >= wf_nodes_.pos.size() || !wf_nodes_.is_active[u]) {
        return false;
      }
      if (v < 0 || v >= wf_nodes_.pos.size() || !wf_nodes_.is_active[v]) {
        return false;
      }
      if (wf_nodes_.next_node[u] != v) {
        return false;
      }
      if (node_version_[u] != ev.version_u || node_version_[v] != ev.version_v) {
        return false;
      }
      return true;
    }
    if (ev.type == SimEventType::SPLIT) {
      const int w = ev.u;
      const int u = ev.v;
      if (w < 0 || w >= wf_nodes_.pos.size() || !wf_nodes_.is_active[w]) {
        return false;
      }
      if (u < 0 || u >= wf_nodes_.pos.size() || !wf_nodes_.is_active[u]) {
        return false;
      }
      const int v = wf_nodes_.next_node[u];
      if (v < 0 || v >= wf_nodes_.pos.size() || !wf_nodes_.is_active[v]) {
        return false;
      }
      if (v != ev.target_v) {
        return false;
      }
      if (node_version_[w] != ev.version_u || node_version_[u] != ev.version_v ||
          node_version_[v] != ev.version_target_v)
      {
        return false;
      }
      return true;
    }
    if (ev.type == SimEventType::VERT_COLLISION) {
      const int u = ev.u;
      const int w = ev.v;
      if (u < 0 || u >= wf_nodes_.pos.size() || !wf_nodes_.is_active[u]) {
        return false;
      }
      if (w < 0 || w >= wf_nodes_.pos.size() || !wf_nodes_.is_active[w]) {
        return false;
      }
      if (node_version_[u] != ev.version_u || node_version_[w] != ev.version_v) {
        return false;
      }
      return true;
    }
    return false;
  }

  void init_pq(double eps)
  {
    node_version_.reinitialize(wf_nodes_.pos.size());
    node_version_.fill(0);
    modified_nodes_.clear();

    for (const int u : wf_nodes_.pos.index_range()) {
      schedule_edge_collapse(u, eps);
    }

    for (const int w : wf_nodes_.pos.index_range()) {
      if (is_reflex(w)) {
        for (const int u : wf_nodes_.pos.index_range()) {
          test_and_schedule_split(w, u);
        }
        for (const int u : wf_nodes_.pos.index_range()) {
          if (u < w) {
            test_and_schedule_vert_collision(u, w, eps);
          }
        }
      }
    }
  }

  void update_pq_for_modified_nodes(double eps)
  {
    if (!use_priority_queue_ || modified_nodes_.is_empty()) {
      modified_nodes_.clear();
      return;
    }

    std::sort(modified_nodes_.begin(), modified_nodes_.end());
    modified_nodes_.resize(std::unique(modified_nodes_.begin(), modified_nodes_.end()) -
                           modified_nodes_.begin());

    for (const int m : modified_nodes_) {
      if (!wf_nodes_.is_active[m]) {
        continue;
      }

      const int prev_m = wf_nodes_.prev_node[m];
      if (prev_m >= 0 && wf_nodes_.is_active[prev_m]) {
        schedule_edge_collapse(prev_m, eps);
      }
      schedule_edge_collapse(m, eps);

      const bool m_reflex = is_reflex(m);
      if (m_reflex) {
        for (const int u : wf_nodes_.pos.index_range()) {
          test_and_schedule_split(m, u);
        }
        for (const int u : wf_nodes_.pos.index_range()) {
          if (u != m) {
            test_and_schedule_vert_collision(std::min(u, m), std::max(u, m), eps);
          }
        }
      }

      for (const int r : wf_nodes_.pos.index_range()) {
        if (r != m && is_reflex(r)) {
          if (prev_m >= 0 && wf_nodes_.is_active[prev_m]) {
            test_and_schedule_split(r, prev_m);
          }
          test_and_schedule_split(r, m);
          if (!m_reflex) {
            test_and_schedule_vert_collision(std::min(r, m), std::max(r, m), eps);
          }
        }
      }
    }

    modified_nodes_.clear();
  }

  double find_earliest_event_pq(double max_lookahead,
                                double eps,
                                Vector<SimEvent> &r_simultaneous_events)
  {
    r_simultaneous_events.clear();

    while (!pq_.empty()) {
      const SimEvent top = pq_.top();
      if (!is_event_valid(top)) {
        pq_.pop();
        continue;
      }

      const double event_t = std::max(current_time_, top.time);
      if (event_t > current_time_ + max_lookahead + 1e-9) {
        return std::numeric_limits<double>::infinity();
      }

      /* Peek all events within time_tol of event_t. */
      const double time_tol = std::max(eps, 1e-4);
      while (!pq_.empty()) {
        const SimEvent ev = pq_.top();
        if (!is_event_valid(ev)) {
          pq_.pop();
          continue;
        }
        if (ev.time > event_t + time_tol) {
          break;
        }
        pq_.pop();
        r_simultaneous_events.append(ev);
      }

      return event_t;
    }

    return std::numeric_limits<double>::infinity();
  }

  void peek_events_at_current_time(double eps, Vector<SimEvent> &r_events)
  {
    const double time_tol = std::max(eps, 1e-4);
    while (!pq_.empty()) {
      const SimEvent top = pq_.top();
      if (!is_event_valid(top)) {
        pq_.pop();
        continue;
      }
      if (top.time > current_time_ + time_tol) {
        break;
      }
      r_events.append(top);
      pq_.pop();
    }
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

  bool try_resolve_opposing_pair(const int e1,
                                 const int e2,
                                 const double cluster_tol,
                                 Vector<std::pair<int, int>> &processed_pairs)
  {
    if (!wf_nodes_.is_active[e1] || !wf_nodes_.is_active[e2]) {
      return false;
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
    const double l1 = math::length(d1);
    if (l1 < cluster_tol) {
      return false;
    }
    const double2 dir1 = d1 / l1;
    const double2 n1 = double2(-dir1.y, dir1.x);

    const int w = e2;
    const int z = wf_nodes_.next_node[w];
    if (z < 0 || !wf_nodes_.is_active[z]) {
      return false;
    }
    const double2 d2 = wf_nodes_.pos[z] - wf_nodes_.pos[w];
    const double l2 = math::length(d2);
    if (l2 < cluster_tol) {
      return false;
    }
    const double2 dir2 = d2 / l2;
    const double2 n2 = double2(-dir2.y, dir2.x);

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
      if (use_priority_queue_) {
        if (u < node_version_.size()) {
          node_version_[u]++;
        }
        if (w < node_version_.size()) {
          node_version_[w]++;
        }
      }
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
        if (use_priority_queue_ && u < node_version_.size()) {
          node_version_[u]++;
        }
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
        if (use_priority_queue_ && w < node_version_.size()) {
          node_version_[w]++;
        }
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
        if (use_priority_queue_) {
          node_version_.append(0);
        }

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
        if (use_priority_queue_) {
          node_version_.append(0);
        }

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
    Vector<std::pair<int, int>> processed_pairs;
    int opp_iter = 0;
    bool found_opposing = true;
    while (found_opposing && opp_iter < 50) {
      opp_iter++;
      found_opposing = false;
      const int n = int(wf_nodes_.pos.size());

      int active_edges_num = 0;
      double x_min = 1e30, x_max = -1e30, y_min = 1e30, y_max = -1e30;
      for (int i = 0; i < n; i++) {
        if (!wf_nodes_.is_active[i]) {
          continue;
        }
        const int next_i = wf_nodes_.next_node[i];
        if (next_i < 0 || !wf_nodes_.is_active[next_i]) {
          continue;
        }
        active_edges_num++;
        const double2 &p = wf_nodes_.pos[i];
        x_min = std::min(x_min, p.x);
        x_max = std::max(x_max, p.x);
        y_min = std::min(y_min, p.y);
        y_max = std::max(y_max, p.y);
      }

      if (active_edges_num <= 1) {
        break;
      }

      if (active_edges_num <= 16) {
        for (int e1 = 0; e1 < n; e1++) {
          for (int e2 = e1 + 1; e2 < n; e2++) {
            if (try_resolve_opposing_pair(e1, e2, cluster_tol, processed_pairs)) {
              found_opposing = true;
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
            }
          });

          if (found_opposing) {
            break;
          }
        }
      }
    }
    rebuild_active_contours();
  }

  void resolve_events_at_current_time(double eps, Span<SimEvent> simultaneous_events = {})
  {
    const double cluster_tol = std::max(eps, 1e-4);

    /* 0. Resolve any opposing collinear/overlapping wavefront edges first. */
    resolve_opposing_overlapping_edges(cluster_tol);

    /* 1. Identify all collapsing edges at current_time_. */
    Vector<int> collapsing_edges;
    for (const int u : wf_nodes_.pos.index_range()) {
      if (!wf_nodes_.is_active[u]) {
        continue;
      }
      const int v = wf_nodes_.next_node[u];
      if (math::length(wf_nodes_.pos[v] - wf_nodes_.pos[u]) < cluster_tol) {
        collapsing_edges.append(u);
      }
    }

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

    if (use_priority_queue_) {
      for (const SimEvent &ev : simultaneous_events) {
        if (ev.type == SimEventType::EDGE_COLLAPSE) {
          if (wf_nodes_.is_active[ev.u] && !collapsing_edges.contains(ev.u)) {
            collapsing_edges.append(ev.u);
          }
        }
        else if (ev.type == SimEventType::VERT_COLLISION) {
          if (wf_nodes_.is_active[ev.u] && wf_nodes_.is_active[ev.v]) {
            if (math::length(wf_nodes_.pos[ev.v] - wf_nodes_.pos[ev.u]) < cluster_tol * 2.0) {
              vert_collisions.append(VertPair{ev.u, ev.v});
            }
          }
        }
        else if (ev.type == SimEventType::SPLIT) {
          const int w = ev.u;
          const int u = ev.v;
          if (wf_nodes_.is_active[w] && wf_nodes_.is_active[u]) {
            const int v = wf_nodes_.next_node[u];
            if (v >= 0 && wf_nodes_.is_active[v]) {
              const double2 n_uv = compute_inward_normal(wf_nodes_.pos[u], wf_nodes_.pos[v]);
              const double dist = std::abs(math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv));
              if (dist < cluster_tol * 2.0) {
                splits.append(SplitInfo{w, u, wf_nodes_.pos[w]});
              }
            }
          }
        }
      }
    }
    else {
      double x_min = 1e30, x_max = -1e30, y_min = 1e30, y_max = -1e30;
      int active_nodes_num = 0;
      Vector<int> reflex_nodes;

      for (const int u : wf_nodes_.pos.index_range()) {
        if (!wf_nodes_.is_active[u]) {
          continue;
        }
        active_nodes_num++;
        const double2 &p = wf_nodes_.pos[u];
        x_min = std::min(x_min, p.x);
        x_max = std::max(x_max, p.x);
        y_min = std::min(y_min, p.y);
        y_max = std::max(y_max, p.y);

        if (is_reflex(u)) {
          reflex_nodes.append(u);
        }
      }

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
              const double2 e_uv = wf_nodes_.pos[v] - wf_nodes_.pos[u];
              const double len_sq = math::length_squared(e_uv);
              if (len_sq > 1e-12) {
                const double s = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], e_uv) / len_sq;
                if (s > 1e-5 && s < 1.0 - 1e-5) {
                  splits.append(SplitInfo{w, u, wf_nodes_.pos[w]});
                }
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
                  const double dist = std::abs(
                      math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], n_uv));
                  if (dist < cluster_tol) {
                    const double2 e_uv = wf_nodes_.pos[v] - wf_nodes_.pos[u];
                    const double len_sq = math::length_squared(e_uv);
                    if (len_sq > 1e-12) {
                      const double s = math::dot(wf_nodes_.pos[w] - wf_nodes_.pos[u], e_uv) /
                                       len_sq;
                      if (s > 1e-5 && s < 1.0 - 1e-5) {
                        splits.append(SplitInfo{w, u, wf_nodes_.pos[w]});
                      }
                    }
                  }
                });
          }
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
      if (use_priority_queue_ && v < node_version_.size()) {
        node_version_[v]++;
      }
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

    /* Find all interior sectors (Gaps): each OUT ray followed by an IN ray. */
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
        if (use_priority_queue_) {
          node_version_.append(0);
        }

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

  void emit_segment_boundary_faces()
  {
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

  void emit_inner_polygon_faces()
  {
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

StraightSkeletonOutput straight_skeleton_calc_simulation(const StraightSkeletonInput &input)
{
  /* Validate input: polygon must have at least 3 vertices. */
  if (input.outer_polygon.size() < 3) {
    StraightSkeletonOutput output;
    output.hit_max_events = false;
    output.events_processed_num = 0;
    output.flips_num = 0;
    return output;
  }

  /* Two-Pass Strategy:
   * When offset is infinite or exceeds collapse time with segments > 1:
   *   Pass 1: Dry-run simulation with offset = infinity, segments = 1 to find exact
   *           max_collapse_time.
   *   Pass 2: Real simulation run with effective_offset = min(input.offset, max_collapse_time)
   *           divided into input.segments bands. */
  double target_offset = input.offset;
  const int segments = std::max(1, input.segments);

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

}  // namespace blender::straightskeleton
