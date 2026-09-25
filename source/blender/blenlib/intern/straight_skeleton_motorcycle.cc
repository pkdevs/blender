/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation using the Motorcycle Graph algorithm.
 * Phase 1: Motorcycle Graph generation with regular grid spatial hashing.
 *
 * References:
 * - S. Huber, M. Held (2011), "Motorcycle Graphs: Stochastic Properties Motivate
 *   an Efficient Yet Simple Implementation", ACM J. Exp. Algorithmics 16.
 * - S. Huber, M. Held (2012), "A Fast Straight-Skeleton Algorithm Based on
 *   Generalized Motorcycle Graphs", Int. J. Comput. Geom. Appl. 22(5).
 */

#include "straight_skeleton_motorcycle.hh"

#include "BLI_math_vector.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>

namespace blender::straightskeleton {

namespace {

constexpr double EPS = 1e-8;
constexpr double TIME_EPS = 1e-9;
constexpr double INF_TIME = std::numeric_limits<double>::infinity();

struct WallSegment {
  int id = -1;
  double2 a{0.0, 0.0};
  double2 b{0.0, 0.0};
  double2 dir{0.0, 0.0};    /* Unit direction vector from a to b. */
  double2 normal{0.0, 0.0}; /* Inward unit normal pointing into polygon interior. */
  double length = 0.0;
  int origin_vert = -1;
};

struct SpatialGrid {
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 1.0;
  double max_y = 1.0;
  int nx = 2;
  int ny = 2;
  double cell_w = 0.5;
  double cell_h = 0.5;

  /* Per-cell lists of wall IDs and completed motorcycle track IDs. */
  Vector<Vector<int>> cell_walls;
  Vector<Vector<int>> cell_crashed_tracks;
  Vector<Vector<int>> cell_active_motorcycles;

  void init(const double2 &bmin, const double2 &bmax, int elements_num)
  {
    const double span_x = bmax.x - bmin.x;
    const double span_y = bmax.y - bmin.y;
    const double margin = std::max({span_x, span_y, 1.0}) * 0.1;

    min_x = bmin.x - margin;
    min_y = bmin.y - margin;
    max_x = bmax.x + margin;
    max_y = bmax.y + margin;

    const int n_base = std::max(2, int(std::ceil(std::sqrt(double(elements_num)))));
    nx = std::clamp(n_base, 2, 64);
    ny = std::clamp(int(std::ceil(double(nx) * ((max_y - min_y) / (max_x - min_x)))), 2, 64);

    cell_w = (max_x - min_x) / double(nx);
    cell_h = (max_y - min_y) / double(ny);

    const int total_cells = nx * ny;
    cell_walls.resize(total_cells);
    cell_crashed_tracks.resize(total_cells);
    cell_active_motorcycles.resize(total_cells);
  }

  int cell_index(int cx, int cy) const
  {
    return cy * nx + cx;
  }

  int get_cx(double x) const
  {
    return std::clamp(int((x - min_x) / cell_w), 0, nx - 1);
  }

  int get_cy(double y) const
  {
    return std::clamp(int((y - min_y) / cell_h), 0, ny - 1);
  }

  bool is_valid_cell(int cx, int cy) const
  {
    return cx >= 0 && cx < nx && cy >= 0 && cy < ny;
  }

  /** Rasterize a line segment [p1, p2] into the grid cells it intersects. */
  void rasterize_segment(const double2 &p1, const double2 &p2, Vector<int> &r_cells) const
  {
    const int cx1 = get_cx(p1.x);
    const int cy1 = get_cy(p1.y);
    const int cx2 = get_cx(p2.x);
    const int cy2 = get_cy(p2.y);

    if (cx1 == cx2 && cy1 == cy2) {
      r_cells.append(cell_index(cx1, cy1));
      return;
    }

    /* 2D DDA grid traversal. */
    int cx = cx1;
    int cy = cy1;
    r_cells.append(cell_index(cx, cy));

    const double dx = p2.x - p1.x;
    const double dy = p2.y - p1.y;

    const int step_x = (dx > 0.0) ? 1 : (dx < 0.0 ? -1 : 0);
    const int step_y = (dy > 0.0) ? 1 : (dy < 0.0 ? -1 : 0);

    const double t_delta_x = (step_x != 0) ? std::abs(cell_w / dx) : INF_TIME;
    const double t_delta_y = (step_y != 0) ? std::abs(cell_h / dy) : INF_TIME;

    double t_max_x = INF_TIME;
    if (step_x > 0) {
      t_max_x = ((min_x + double(cx + 1) * cell_w) - p1.x) / dx;
    }
    else if (step_x < 0) {
      t_max_x = ((min_x + double(cx) * cell_w) - p1.x) / dx;
    }

    double t_max_y = INF_TIME;
    if (step_y > 0) {
      t_max_y = ((min_y + double(cy + 1) * cell_h) - p1.y) / dy;
    }
    else if (step_y < 0) {
      t_max_y = ((min_y + double(cy) * cell_h) - p1.y) / dy;
    }

    while (cx != cx2 || cy != cy2) {
      if (t_max_x < t_max_y) {
        t_max_x += t_delta_x;
        cx += step_x;
      }
      else {
        t_max_y += t_delta_y;
        cy += step_y;
      }
      if (is_valid_cell(cx, cy)) {
        r_cells.append(cell_index(cx, cy));
      }
      else {
        break;
      }
    }
  }
};

/* -------------------------------------------------------------------- */
/** \name Geometric Intersection Helpers
 * \{ */

/** Ray vs Segment intersection: ray p(t) = v + (t - t0) * s vs segment [A, B]. */
bool intersect_ray_segment(const double2 &v,
                           double t0,
                           const double2 &s,
                           const double2 &a,
                           const double2 &b,
                           double &r_time,
                           double2 &r_pos)
{
  const double2 d = b - a;
  const double det = -s.x * d.y + s.y * d.x;
  if (std::abs(det) < 1e-12) {
    return false;
  }

  const double diff_x = a.x - v.x;
  const double diff_y = a.y - v.y;

  const double dt = (diff_x * (-d.y) - diff_y * (-d.x)) / det;
  const double u = (s.x * diff_y - s.y * diff_x) / det;

  if (dt > TIME_EPS && u >= -1e-9 && u <= 1.0 + 1e-9) {
    r_time = t0 + dt;
    const double clamped_u = std::clamp(u, 0.0, 1.0);
    r_pos = a + clamped_u * d;
    return true;
  }
  return false;
}

/** Ray vs Ray intersection between two motorcycles m1 and m2. */
bool intersect_motorcycle_rays(const MotorcycleTrack &m1,
                               const MotorcycleTrack &m2,
                               double &r_time1,
                               double &r_time2,
                               double2 &r_pos)
{
  const double det = -m1.velocity.x * m2.velocity.y + m1.velocity.y * m2.velocity.x;
  if (std::abs(det) < 1e-12) {
    /* Check for collinear opposing rays (head-on collision). */
    const double dot_v = m1.velocity.x * m2.velocity.x + m1.velocity.y * m2.velocity.y;
    if (dot_v < -1e-12) {
      const double diff_x = m2.start_pos.x - m1.start_pos.x;
      const double diff_y = m2.start_pos.y - m1.start_pos.y;
      const double cross_diff = diff_x * m1.velocity.y - diff_y * m1.velocity.x;
      if (std::abs(cross_diff) < 1e-9) {
        /* They are on the exact same line! */
        const double v1_sq = m1.velocity.x * m1.velocity.x + m1.velocity.y * m1.velocity.y;
        const double v2_len = std::sqrt(m2.velocity.x * m2.velocity.x +
                                        m2.velocity.y * m2.velocity.y);
        const double v1_len = std::sqrt(v1_sq);
        const double lambda = v2_len / v1_len;
        const double dot_diff = (diff_x * m1.velocity.x + diff_y * m1.velocity.y) / v1_sq;
        const double dt1 = (dot_diff - lambda * (m1.start_time - m2.start_time)) / (1.0 + lambda);
        const double dt2 = dt1 + (m1.start_time - m2.start_time);
        if (dt1 > TIME_EPS && dt2 > TIME_EPS) {
          r_time1 = m1.start_time + dt1;
          r_time2 = m2.start_time + dt2;
          r_pos = m1.start_pos + dt1 * m1.velocity;
          return true;
        }
      }
    }
    return false;
  }

  const double diff_x = m2.start_pos.x - m1.start_pos.x;
  const double diff_y = m2.start_pos.y - m1.start_pos.y;

  const double dt1 = (diff_x * (-m2.velocity.y) - diff_y * (-m2.velocity.x)) / det;
  const double dt2 = (m1.velocity.x * diff_y - m1.velocity.y * diff_x) / det;

  if (dt1 > TIME_EPS && dt2 > TIME_EPS) {
    r_time1 = m1.start_time + dt1;
    r_time2 = m2.start_time + dt2;
    r_pos = m1.start_pos + dt1 * m1.velocity;
    return true;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Simulation Events & State
 * \{ */

enum class EventType {
  SWITCH,
  CRASH,
};

struct Event {
  EventType type = EventType::SWITCH;
  double time = 0.0;
  int mc_id = -1;
  int version = 0;

  /* For CRASH events: */
  MotorcycleCrashType crash_type = MotorcycleCrashType::NONE;
  int target_id = -1; /* Wall ID or target motorcycle ID. */
  double2 crash_pos{0.0, 0.0};

  /* For SWITCH events: */
  int next_cx = 0;
  int next_cy = 0;

  bool operator>(const Event &other) const
  {
    return time > other.time;
  }
};

struct MotorcycleSimulationState {
  Vector<WallSegment> walls;
  Vector<MotorcycleTrack> tracks;
  Vector<int> versions;
  Vector<int> current_cx;
  Vector<int> current_cy;
  Vector<double> cell_enter_time;
  SpatialGrid grid;
  std::priority_queue<Event, std::vector<Event>, std::greater<Event>> queue;

  /** Compute exit time and next cell from current cell. */
  double compute_cell_exit(
      int mc_id, int cx, int cy, double t_enter, int &r_next_cx, int &r_next_cy)
  {
    const MotorcycleTrack &m = tracks[mc_id];
    const double2 pos = m.start_pos + (t_enter - m.start_time) * m.velocity;

    const double x0 = grid.min_x + double(cx) * grid.cell_w;
    const double x1 = x0 + grid.cell_w;
    const double y0 = grid.min_y + double(cy) * grid.cell_h;
    const double y1 = y0 + grid.cell_h;

    double dt_x = INF_TIME;
    int step_x = 0;
    if (m.velocity.x > 1e-12) {
      dt_x = (x1 - pos.x) / m.velocity.x;
      step_x = 1;
    }
    else if (m.velocity.x < -1e-12) {
      dt_x = (x0 - pos.x) / m.velocity.x;
      step_x = -1;
    }

    double dt_y = INF_TIME;
    int step_y = 0;
    if (m.velocity.y > 1e-12) {
      dt_y = (y1 - pos.y) / m.velocity.y;
      step_y = 1;
    }
    else if (m.velocity.y < -1e-12) {
      dt_y = (y0 - pos.y) / m.velocity.y;
      step_y = -1;
    }

    const double dt = std::max(0.0, std::min(dt_x, dt_y));
    if (std::isinf(dt)) {
      r_next_cx = cx;
      r_next_cy = cy;
      return INF_TIME;
    }

    if (dt_x < dt_y) {
      r_next_cx = cx + step_x;
      r_next_cy = cy;
    }
    else if (dt_y < dt_x) {
      r_next_cx = cx;
      r_next_cy = cy + step_y;
    }
    else {
      r_next_cx = cx + step_x;
      r_next_cy = cy + step_y;
    }

    return t_enter + dt;
  }

  /** Find candidate crash events for motorcycle mc_id in cell (cx, cy). */
  void check_crashes_in_cell(int mc_id, int cx, int cy, double t_enter, double t_exit)
  {
    if (!grid.is_valid_cell(cx, cy)) {
      return;
    }
    const int c_idx = grid.cell_index(cx, cy);
    const MotorcycleTrack &m = tracks[mc_id];

    double earliest_crash_t = INF_TIME;
    double2 best_crash_pos{0.0, 0.0};
    MotorcycleCrashType best_type = MotorcycleCrashType::NONE;
    int best_target = -1;

    /* 1. Check collisions with static walls in this cell. */
    for (const int w_id : grid.cell_walls[c_idx]) {
      const WallSegment &w = walls[w_id];
      if (w.origin_vert == m.origin_vert) {
        continue; /* Avoid immediate self-intersection with incident wall. */
      }
      double t_hit = 0.0;
      double2 p_hit{0.0, 0.0};
      if (intersect_ray_segment(m.start_pos, m.start_time, m.velocity, w.a, w.b, t_hit, p_hit)) {
        if (t_hit >= t_enter - TIME_EPS && t_hit <= t_exit + TIME_EPS && t_hit < earliest_crash_t)
        {
          earliest_crash_t = t_hit;
          best_crash_pos = p_hit;
          best_type = MotorcycleCrashType::WALL;
          best_target = w_id;
        }
      }
    }

    /* 2. Check collisions with completed motorcycle tracks passing through this cell. */
    for (const int other_id : grid.cell_crashed_tracks[c_idx]) {
      if (other_id == mc_id) {
        continue;
      }
      const MotorcycleTrack &other = tracks[other_id];
      double t_hit = 0.0;
      double2 p_hit{0.0, 0.0};
      if (intersect_ray_segment(m.start_pos,
                                m.start_time,
                                m.velocity,
                                other.start_pos,
                                other.crash_pos,
                                t_hit,
                                p_hit))
      {
        /* Did other motorcycle reach p_hit before or at t_hit? */
        const double other_reach_t = other.start_time +
                                     std::sqrt(math::distance_squared(p_hit, other.start_pos)) /
                                         std::sqrt(math::length_squared(other.velocity));
        if (other_reach_t <= t_hit + TIME_EPS) {
          if (t_hit >= t_enter - TIME_EPS && t_hit <= t_exit + TIME_EPS &&
              t_hit < earliest_crash_t)
          {
            earliest_crash_t = t_hit;
            best_crash_pos = p_hit;
            best_type = MotorcycleCrashType::MOTORCYCLE_TRACE;
            best_target = other_id;
          }
        }
      }
    }

    /* 3. Check collisions with other active motorcycles in this cell. */
    for (const int other_id : grid.cell_active_motorcycles[c_idx]) {
      if (other_id == mc_id || tracks[other_id].crashed) {
        continue;
      }
      const MotorcycleTrack &other = tracks[other_id];
      double t1 = 0.0;
      double t2 = 0.0;
      double2 p_hit{0.0, 0.0};
      if (intersect_motorcycle_rays(m, other, t1, t2, p_hit)) {
        /* Check if m reaches p_hit later than or at the same time as other. */
        if (std::abs(t1 - t2) <= EPS) {
          /* Simultaneous collision! Both can crash here. */
          if (t1 >= t_enter - TIME_EPS && t1 <= t_exit + TIME_EPS && t1 < earliest_crash_t) {
            earliest_crash_t = t1;
            best_crash_pos = p_hit;
            best_type = MotorcycleCrashType::CONCURRENT_MOTORCYCLE;
            best_target = other_id;
          }
        }
        else if (t2 < t1) {
          /* Other arrived earlier; m hits other's trace. */
          if (t1 >= t_enter - TIME_EPS && t1 <= t_exit + TIME_EPS && t1 < earliest_crash_t) {
            earliest_crash_t = t1;
            best_crash_pos = p_hit;
            best_type = MotorcycleCrashType::MOTORCYCLE_TRACE;
            best_target = other_id;
          }
        }
        else {
          /* m arrived earlier than other; other hits m's trace!
           * Schedule crash event for other if earlier than other's known crash. */
          if (t2 >= cell_enter_time[other_id] - TIME_EPS) {
            Event ev;
            ev.type = EventType::CRASH;
            ev.time = t2;
            ev.mc_id = other_id;
            ev.version = ++versions[other_id];
            ev.crash_type = MotorcycleCrashType::MOTORCYCLE_TRACE;
            ev.target_id = mc_id;
            ev.crash_pos = p_hit;
            queue.push(ev);
          }
        }
      }
    }

    if (earliest_crash_t < INF_TIME) {
      Event ev;
      ev.type = EventType::CRASH;
      ev.time = earliest_crash_t;
      ev.mc_id = mc_id;
      ev.version = ++versions[mc_id];
      ev.crash_type = best_type;
      ev.target_id = best_target;
      ev.crash_pos = best_crash_pos;
      queue.push(ev);
    }
  }

  void insert_motorcycle(int mc_id)
  {
    MotorcycleTrack &m = tracks[mc_id];
    const int cx = grid.get_cx(m.start_pos.x);
    const int cy = grid.get_cy(m.start_pos.y);

    current_cx[mc_id] = cx;
    current_cy[mc_id] = cy;
    cell_enter_time[mc_id] = m.start_time;

    const int c_idx = grid.cell_index(cx, cy);
    grid.cell_active_motorcycles[c_idx].append(mc_id);

    int next_cx = cx;
    int next_cy = cy;
    const double t_exit = compute_cell_exit(mc_id, cx, cy, m.start_time, next_cx, next_cy);

    /* Check for crashes in this initial cell. */
    check_crashes_in_cell(mc_id, cx, cy, m.start_time, t_exit);

    /* Schedule cell switch event if not already at exit. */
    if (!std::isinf(t_exit)) {
      Event ev;
      ev.type = EventType::SWITCH;
      ev.time = t_exit;
      ev.mc_id = mc_id;
      ev.version = versions[mc_id];
      ev.next_cx = next_cx;
      ev.next_cy = next_cy;
      queue.push(ev);
    }
  }
};

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Public Functions
 * \{ */

MotorcycleGraph compute_motorcycle_graph(const StraightSkeletonInput &input)
{
  MotorcycleSimulationState state;

  /* 1. Extract walls and identify reflex vertices from outer polygon and holes. */
  double2 bbox_min{1e30, 1e30};
  double2 bbox_max{-1e30, -1e30};

  auto update_bbox = [&](const double2 &p) {
    bbox_min.x = std::min(bbox_min.x, p.x);
    bbox_min.y = std::min(bbox_min.y, p.y);
    bbox_max.x = std::max(bbox_max.x, p.x);
    bbox_max.y = std::max(bbox_max.y, p.y);
  };

  /* Helper to process a loop of vertices. */
  auto process_contour = [&](Span<int> contour) {
    const int n = int(contour.size());
    if (n < 3) {
      return;
    }

    const int wall_start_idx = int(state.walls.size());
    for (const int i : IndexRange(n)) {
      const int v_curr = contour[i];
      const int v_next = contour[(i + 1) % n];
      const double2 a = input.vert[v_curr];
      const double2 b = input.vert[v_next];
      update_bbox(a);
      update_bbox(b);

      const double2 diff = b - a;
      const double len = math::length(diff);
      const double2 dir = (len > 1e-12) ? (diff / len) : double2(1.0, 0.0);
      /* Inward normal for CCW outer (or CW hole, where interior is on the left). */
      const double2 normal(-dir.y, dir.x);

      WallSegment w;
      w.id = int(state.walls.size());
      w.a = a;
      w.b = b;
      w.dir = dir;
      w.normal = normal;
      w.length = len;
      w.origin_vert = v_curr;
      state.walls.append(w);
    }

    /* Identify reflex vertices. */
    for (const int i : IndexRange(n)) {
      const int v_curr = contour[i];
      const int v_prev = contour[(i + n - 1) % n];
      const int v_next = contour[(i + 1) % n];

      const double2 p_prev = input.vert[v_prev];
      const double2 p_curr = input.vert[v_curr];
      const double2 p_next = input.vert[v_next];

      const double2 d_in = math::normalize(p_curr - p_prev);
      const double2 d_out = math::normalize(p_next - p_curr);

      /* Right turn means reflex angle for interior on left. */
      const double cross = d_in.x * d_out.y - d_in.y * d_out.x;
      if (cross < -1e-9) {
        const double2 n_in(-d_in.y, d_in.x);
        const double2 n_out(-d_out.y, d_out.x);

        const double det = n_in.x * n_out.y - n_in.y * n_out.x;
        if (std::abs(det) > 1e-12) {
          const double sx = (n_out.y - n_in.y) / det;
          const double sy = (n_in.x - n_out.x) / det;

          MotorcycleTrack m;
          m.id = int(state.tracks.size());
          m.start_pos = p_curr;
          m.start_time = 0.0;
          m.velocity = double2(sx, sy);
          m.origin_vert = v_curr;
          m.left_wall = wall_start_idx + ((i + n - 1) % n);
          m.right_wall = wall_start_idx + i;

          state.tracks.append(m);
        }
      }
    }
  };

  process_contour(input.outer_polygon);
  for (const int h : IndexRange(input.hole_offsets.size())) {
    process_contour(input.hole_vert_indices.slice(input.hole_offsets[h]));
  }

  /* If no reflex vertices exist (e.g. convex polygon), motorcycle graph is empty. */
  if (state.tracks.is_empty()) {
    MotorcycleGraph mg;
    return mg;
  }

  /* 2. Initialize spatial grid and rasterize walls. */
  const int total_elements = int(state.walls.size() + state.tracks.size());
  state.grid.init(bbox_min, bbox_max, total_elements);

  for (const WallSegment &w : state.walls) {
    Vector<int> cells;
    state.grid.rasterize_segment(w.a, w.b, cells);
    for (const int c_idx : cells) {
      state.grid.cell_walls[c_idx].append(w.id);
    }
  }

  /* 3. Initialize motorcycle states and priority queue. */
  const int n_mc = int(state.tracks.size());
  state.versions.resize(n_mc, 0);
  state.current_cx.resize(n_mc, -1);
  state.current_cy.resize(n_mc, -1);
  state.cell_enter_time.resize(n_mc, 0.0);

  for (const int i : IndexRange(n_mc)) {
    state.insert_motorcycle(i);
  }

  /* 4. Main Event Loop. */
  int crashes_num = 0;
  int concurrent_crashes_num = 0;

  while (!state.queue.empty()) {
    const Event ev = state.queue.top();
    state.queue.pop();

    if (state.tracks[ev.mc_id].crashed) {
      continue;
    }
    if (ev.version != state.versions[ev.mc_id]) {
      continue; /* Outdated event. */
    }

    if (ev.type == EventType::SWITCH) {
      const int mc_id = ev.mc_id;
      const int old_cx = state.current_cx[mc_id];
      const int old_cy = state.current_cy[mc_id];

      /* Remove from old cell's active list. */
      if (state.grid.is_valid_cell(old_cx, old_cy)) {
        const int old_idx = state.grid.cell_index(old_cx, old_cy);
        state.grid.cell_active_motorcycles[old_idx].remove_first_occurrence_and_reorder(mc_id);
      }

      /* Check if next cell is outside grid boundary. */
      if (!state.grid.is_valid_cell(ev.next_cx, ev.next_cy)) {
        MotorcycleTrack &m = state.tracks[mc_id];
        m.crashed = true;
        m.crash_time = ev.time;
        m.crash_pos = m.start_pos + (ev.time - m.start_time) * m.velocity;
        m.crash_type = MotorcycleCrashType::BOUNDARY;
        crashes_num++;
        continue;
      }

      /* Enter new cell. */
      const int new_cx = ev.next_cx;
      const int new_cy = ev.next_cy;
      state.current_cx[mc_id] = new_cx;
      state.current_cy[mc_id] = new_cy;
      state.cell_enter_time[mc_id] = ev.time;

      const int new_idx = state.grid.cell_index(new_cx, new_cy);
      state.grid.cell_active_motorcycles[new_idx].append(mc_id);

      int next_cx = new_cx;
      int next_cy = new_cy;
      const double t_exit = state.compute_cell_exit(
          mc_id, new_cx, new_cy, ev.time, next_cx, next_cy);

      /* Check for crashes in this newly entered cell. */
      state.check_crashes_in_cell(mc_id, new_cx, new_cy, ev.time, t_exit);

      if (!std::isinf(t_exit)) {
        Event next_ev;
        next_ev.type = EventType::SWITCH;
        next_ev.time = t_exit;
        next_ev.mc_id = mc_id;
        next_ev.version = state.versions[mc_id];
        next_ev.next_cx = next_cx;
        next_ev.next_cy = next_cy;
        state.queue.push(next_ev);
      }
    }
    else if (ev.type == EventType::CRASH) {
      const int mc_id = ev.mc_id;
      MotorcycleTrack &m = state.tracks[mc_id];

      /* If target was a motorcycle that crashed earlier before reaching crash_pos,
       * then this crash cannot happen. Discard and re-evaluate. */
      if (ev.crash_type == MotorcycleCrashType::MOTORCYCLE_TRACE) {
        const MotorcycleTrack &target = state.tracks[ev.target_id];
        if (target.crashed) {
          const double dist_target_crash = math::distance(target.start_pos, target.crash_pos);
          const double dist_to_hit = math::distance(target.start_pos, ev.crash_pos);
          if (dist_to_hit > dist_target_crash + EPS * 10.0) {
            /* Target crashed before reaching this intersection! Discard event. */
            continue;
          }
        }
      }

      /* Valid crash! */
      m.crashed = true;
      m.crash_time = ev.time;
      m.crash_pos = ev.crash_pos;
      m.crash_type = ev.crash_type;
      m.crashed_into_id = ev.target_id;
      crashes_num++;

      /* Remove from active cell. */
      const int cx = state.current_cx[mc_id];
      const int cy = state.current_cy[mc_id];
      if (state.grid.is_valid_cell(cx, cy)) {
        const int c_idx = state.grid.cell_index(cx, cy);
        state.grid.cell_active_motorcycles[c_idx].remove_first_occurrence_and_reorder(mc_id);
      }

      /* Register completed track into spatial grid for other motorcycles to crash into. */
      Vector<int> track_cells;
      state.grid.rasterize_segment(m.start_pos, m.crash_pos, track_cells);
      for (const int c_idx : track_cells) {
        state.grid.cell_crashed_tracks[c_idx].append(mc_id);
      }

      /* Check for concurrent crash. */
      if (ev.crash_type == MotorcycleCrashType::CONCURRENT_MOTORCYCLE) {
        concurrent_crashes_num++;
        MotorcycleTrack &other = state.tracks[ev.target_id];
        if (!other.crashed) {
          other.crashed = true;
          other.crash_time = ev.time;
          other.crash_pos = ev.crash_pos;
          other.crash_type = MotorcycleCrashType::CONCURRENT_MOTORCYCLE;
          other.crashed_into_id = mc_id;
          crashes_num++;

          const int ocx = state.current_cx[ev.target_id];
          const int ocy = state.current_cy[ev.target_id];
          if (state.grid.is_valid_cell(ocx, ocy)) {
            const int oc_idx = state.grid.cell_index(ocx, ocy);
            state.grid.cell_active_motorcycles[oc_idx].remove_first_occurrence_and_reorder(
                ev.target_id);
          }

          Vector<int> other_track_cells;
          state.grid.rasterize_segment(other.start_pos, other.crash_pos, other_track_cells);
          for (const int c_idx : other_track_cells) {
            state.grid.cell_crashed_tracks[c_idx].append(ev.target_id);
          }

          /* Check if a child motorcycle should be launched (Huber & Held 2012, Case 2a).
           * When the left wall of m and the right wall of other span a reflex angle or parallel
           * advancing wavefront, launch a new motorcycle from the crash point. */
          if (m.left_wall >= 0 && m.left_wall < state.walls.size() && other.right_wall >= 0 &&
              other.right_wall < state.walls.size())
          {
            const WallSegment &w_left = state.walls[m.left_wall];
            const WallSegment &w_right = state.walls[other.right_wall];
            const double2 n_l = w_left.normal;
            const double2 n_r = w_right.normal;
            const double dot_n = math::dot(n_l, n_r);
            const double cross_n = n_l.x * n_r.y - n_l.y * n_r.x;

            double2 child_vel{0.0, 0.0};
            bool launch_child = false;

            if (dot_n > 0.999 && std::abs(cross_n) < 1e-4) {
              child_vel = n_l;
              launch_child = true;
            }
            else if (std::abs(cross_n) > 1e-6) {
              const double sx = (n_r.y - n_l.y) / cross_n;
              const double sy = (n_l.x - n_r.x) / cross_n;
              const double2 cand_vel(sx, sy);
              if (math::dot(cand_vel, m.velocity + other.velocity) > 0.0) {
                child_vel = cand_vel;
                launch_child = true;
              }
            }

            if (launch_child) {
              MotorcycleTrack child;
              child.id = int(state.tracks.size());
              child.start_pos = ev.crash_pos;
              child.start_time = ev.time;
              child.velocity = child_vel;
              child.origin_vert = -1;
              child.left_wall = m.left_wall;
              child.right_wall = other.right_wall;
              child.ancestor_ids.append(mc_id);
              child.ancestor_ids.append(ev.target_id);

              const int child_id = child.id;
              state.tracks.append(child);
              state.versions.append(0);
              state.current_cx.append(-1);
              state.current_cy.append(-1);
              state.cell_enter_time.append(0.0);
              state.insert_motorcycle(child_id);
            }
          }
        }
      }
    }
  }

  MotorcycleGraph mg;
  mg.tracks = std::move(state.tracks);
  mg.crashes_num = crashes_num;
  mg.concurrent_crashes_num = concurrent_crashes_num;

  return mg;
}

void dump_motorcycle_graph_to_svg(const StraightSkeletonInput &input,
                                  const MotorcycleGraph &mg,
                                  const char *filepath)
{
  FILE *f = fopen(filepath, "w");
  if (!f) {
    return;
  }

  /* Compute bounding box across all vertices, track origins, and crash positions. */
  double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
  for (const double2 &v : input.vert) {
    min_x = std::min(min_x, v.x);
    min_y = std::min(min_y, v.y);
    max_x = std::max(max_x, v.x);
    max_y = std::max(max_y, v.y);
  }
  for (const MotorcycleTrack &m : mg.tracks) {
    min_x = std::min(min_x, m.start_pos.x);
    min_y = std::min(min_y, m.start_pos.y);
    max_x = std::max(max_x, m.start_pos.x);
    max_y = std::max(max_y, m.start_pos.y);
    if (m.crashed) {
      min_x = std::min(min_x, m.crash_pos.x);
      min_y = std::min(min_y, m.crash_pos.y);
      max_x = std::max(max_x, m.crash_pos.x);
      max_y = std::max(max_y, m.crash_pos.y);
    }
  }

  double geom_w = max_x - min_x;
  double geom_h = max_y - min_y;
  if (geom_w < 1e-6) {
    geom_w = 1.0;
  }
  if (geom_h < 1e-6) {
    geom_h = 1.0;
  }

  /* Add 15% margin around the geometry. */
  const double pad_x = geom_w * 0.15;
  const double pad_y = geom_h * 0.15;
  min_x -= pad_x;
  max_x += pad_x;
  min_y -= pad_y;
  max_y += pad_y;
  geom_w = max_x - min_x;
  geom_h = max_y - min_y;

  const double svg_w = 960.0;
  const double banner_h = 80.0;
  const double side_pad = 40.0;

  const double avail_w = svg_w - 2.0 * side_pad;
  double scale = avail_w / geom_w;
  double geom_svg_h = geom_h * scale;
  if (geom_svg_h > 800.0) {
    geom_svg_h = 800.0;
    scale = geom_svg_h / geom_h;
  }
  const double svg_h = banner_h + geom_svg_h + 2.0 * side_pad;

  const double offset_x = side_pad + (avail_w - geom_w * scale) * 0.5;
  const double offset_y = banner_h + side_pad;

  auto tx = [&](double x) { return offset_x + (x - min_x) * scale; };
  auto ty = [&](double y) { return offset_y + (max_y - y) * scale; };

  fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  fprintf(f,
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.1f\" height=\"%.1f\" "
          "viewBox=\"0 0 %.1f %.1f\">\n",
          svg_w,
          svg_h,
          svg_w,
          svg_h);

  /* Modern background. */
  fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"#f8fafc\"/>\n");

  /* Top banner. */
  fprintf(f,
          "  <rect width=\"100%%\" height=\"%.1f\" fill=\"#ffffff\" stroke=\"#e2e8f0\" "
          "stroke-width=\"1.5\"/>\n",
          banner_h);

  /* Title and test label. */
  const char *title_str = (input.label && input.label[0] != '\0') ? input.label :
                                                                    "Motorcycle Graph";
  fprintf(f,
          "  <text x=\"30\" y=\"34\" font-family=\"system-ui, -apple-system, sans-serif\" "
          "font-size=\"18\" font-weight=\"700\" fill=\"#0f172a\">%s</text>\n",
          title_str);

  int wall_crashes = 0;
  int trace_crashes = 0;
  int concurrent_crashes = 0;
  for (const MotorcycleTrack &m : mg.tracks) {
    if (m.crashed) {
      if (m.crash_type == MotorcycleCrashType::WALL ||
          m.crash_type == MotorcycleCrashType::BOUNDARY)
      {
        wall_crashes++;
      }
      else if (m.crash_type == MotorcycleCrashType::MOTORCYCLE_TRACE) {
        trace_crashes++;
      }
      else if (m.crash_type == MotorcycleCrashType::CONCURRENT_MOTORCYCLE) {
        concurrent_crashes++;
      }
    }
  }

  fprintf(f,
          "  <text x=\"30\" y=\"58\" font-family=\"system-ui, -apple-system, sans-serif\" "
          "font-size=\"13\" fill=\"#64748b\">Motorcycles: %d  |  Crashes: %d (Wall: %d, Trace: "
          "%d, Head-On: %d)</text>\n",
          int(mg.tracks.size()),
          mg.crashes_num,
          wall_crashes,
          trace_crashes,
          concurrent_crashes);

  /* Legend on top right of banner. */
  fprintf(f, "  <g transform=\"translate(530, 18)\">\n");
  /* Row 1: Boundary, Track, Spawn. */
  fprintf(f,
          "    <line x1=\"0\" y1=\"10\" x2=\"22\" y2=\"10\" stroke=\"#2563eb\" "
          "stroke-width=\"2.5\"/>\n"
          "    <text x=\"28\" y=\"14\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Boundary</text>\n"
          "    <line x1=\"110\" y1=\"10\" x2=\"132\" y2=\"10\" stroke=\"#dc2626\" "
          "stroke-width=\"2.5\"/>\n"
          "    <polygon points=\"126,7 133,10 126,13\" fill=\"#dc2626\"/>\n"
          "    <text x=\"138\" y=\"14\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Track</text>\n"
          "    <circle cx=\"200\" cy=\"10\" r=\"4.5\" fill=\"#ef4444\" stroke=\"#ffffff\" "
          "stroke-width=\"1.5\"/>\n"
          "    <text x=\"210\" y=\"14\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Spawn</text>\n");
  /* Row 2: Wall Crash, Trace Crash, Head-On Crash. */
  fprintf(f,
          "    <circle cx=\"0\" cy=\"35\" r=\"4.5\" fill=\"#16a34a\" stroke=\"#ffffff\" "
          "stroke-width=\"1.5\"/>\n"
          "    <text x=\"10\" y=\"39\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Wall Crash</text>\n"
          "    <circle cx=\"110\" cy=\"35\" r=\"4.5\" fill=\"#9333ea\" stroke=\"#ffffff\" "
          "stroke-width=\"1.5\"/>\n"
          "    <text x=\"120\" y=\"39\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Trace Crash</text>\n"
          "    <circle cx=\"200\" cy=\"35\" r=\"4.5\" fill=\"#ea580c\" stroke=\"#ffffff\" "
          "stroke-width=\"1.5\"/>\n"
          "    <text x=\"210\" y=\"39\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Head-On Crash</text>\n");
  fprintf(f, "  </g>\n");

  /* Polygon body path using evenodd fill rule so holes are transparent cutouts. */
  fprintf(f,
          "  <path fill-rule=\"evenodd\" fill=\"#eff6ff\" stroke=\"#2563eb\" "
          "stroke-width=\"2.5\" stroke-linejoin=\"round\" d=\"");
  auto append_contour_to_path = [&](Span<int> contour) {
    if (contour.is_empty()) {
      return;
    }
    fprintf(f, "M %.2f %.2f ", tx(input.vert[contour[0]].x), ty(input.vert[contour[0]].y));
    for (int i = 1; i < contour.size(); i++) {
      fprintf(f, "L %.2f %.2f ", tx(input.vert[contour[i]].x), ty(input.vert[contour[i]].y));
    }
    fprintf(f, "Z ");
  };
  append_contour_to_path(input.outer_polygon);
  for (const int h : IndexRange(input.hole_offsets.size())) {
    append_contour_to_path(input.hole_vert_indices.slice(input.hole_offsets[h]));
  }
  fprintf(f, "\"/>\n");

  /* Boundary vertex dots and indices. */
  auto draw_vert_dots = [&](Span<int> contour) {
    for (const int v : contour) {
      const double vx = tx(input.vert[v].x);
      const double vy = ty(input.vert[v].y);
      fprintf(f, "  <circle cx=\"%.2f\" cy=\"%.2f\" r=\"3.0\" fill=\"#1e40af\"/>\n", vx, vy);
      fprintf(f,
              "  <text x=\"%.2f\" y=\"%.2f\" font-family=\"monospace\" font-size=\"10\" "
              "fill=\"#64748b\">v%d</text>\n",
              vx + 5.0,
              vy - 5.0,
              v);
    }
  };
  draw_vert_dots(input.outer_polygon);
  for (const int h : IndexRange(input.hole_offsets.size())) {
    draw_vert_dots(input.hole_vert_indices.slice(input.hole_offsets[h]));
  }

  /* Motorcycle tracks. */
  for (const MotorcycleTrack &m : mg.tracks) {
    const double2 p_end = m.crashed ? m.crash_pos : (m.start_pos + 10.0 * m.velocity);
    const double x1 = tx(m.start_pos.x);
    const double y1 = ty(m.start_pos.y);
    const double x2 = tx(p_end.x);
    const double y2 = ty(p_end.y);

    /* Track line. */
    fprintf(f,
            "  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#dc2626\" "
            "stroke-width=\"2.5\" stroke-dasharray=\"none\" stroke-linecap=\"round\"/>\n",
            x1,
            y1,
            x2,
            y2);

    /* Direction arrowhead at 60% along track. */
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double track_len = std::hypot(dx, dy);
    if (track_len > 18.0) {
      const double ux = dx / track_len;
      const double uy = dy / track_len;
      const double nx = -uy;
      const double ny = ux;
      const double mx = x1 + 0.60 * dx;
      const double my = y1 + 0.60 * dy;
      const double tip_x = mx + ux * 6.0;
      const double tip_y = my + uy * 6.0;
      const double left_x = mx - ux * 6.0 + nx * 4.5;
      const double left_y = my - uy * 6.0 + ny * 4.5;
      const double right_x = mx - ux * 6.0 - nx * 4.5;
      const double right_y = my - uy * 6.0 - ny * 4.5;
      fprintf(f,
              "  <polygon points=\"%.2f,%.2f %.2f,%.2f %.2f,%.2f\" fill=\"#dc2626\"/>\n",
              tip_x,
              tip_y,
              left_x,
              left_y,
              right_x,
              right_y);
    }

    /* Spawn vertex marker and label. */
    fprintf(f,
            "  <circle cx=\"%.2f\" cy=\"%.2f\" r=\"5.0\" fill=\"#ef4444\" stroke=\"#ffffff\" "
            "stroke-width=\"1.5\"/>\n",
            x1,
            y1);
    fprintf(f,
            "  <text x=\"%.2f\" y=\"%.2f\" font-family=\"monospace\" font-size=\"11\" "
            "font-weight=\"bold\" fill=\"#991b1b\">m%d</text>\n",
            x1 - 16.0,
            y1 - 7.0,
            m.id);

    /* Crash point marker and label. */
    if (m.crashed) {
      const char *crash_color = "#16a34a"; /* Default: WALL (green). */
      if (m.crash_type == MotorcycleCrashType::MOTORCYCLE_TRACE) {
        crash_color = "#9333ea"; /* TRACE (purple). */
      }
      else if (m.crash_type == MotorcycleCrashType::CONCURRENT_MOTORCYCLE) {
        crash_color = "#ea580c"; /* CONCURRENT (amber/orange). */
      }

      fprintf(f,
              "  <circle cx=\"%.2f\" cy=\"%.2f\" r=\"6.0\" fill=\"%s\" stroke=\"#ffffff\" "
              "stroke-width=\"2.0\"/>\n",
              x2,
              y2,
              crash_color);

      /* Badge for crash time placed along track ray to avoid overlapping other badges. */
      double badge_x = x2 + 12.0;
      double badge_y = y2 - 12.0;
      if (track_len > 30.0) {
        const double ux = dx / track_len;
        const double uy = dy / track_len;
        badge_x = x2 - ux * 28.0;
        badge_y = y2 - uy * 28.0;
      }

      fprintf(f,
              "  <rect x=\"%.2f\" y=\"%.2f\" width=\"48\" height=\"15\" rx=\"3\" "
              "fill=\"#ffffff\" fill-opacity=\"0.9\" stroke=\"#cbd5e1\" stroke-width=\"1\"/>\n",
              badge_x - 24.0,
              badge_y - 7.5);
      fprintf(f,
              "  <text x=\"%.2f\" y=\"%.2f\" font-family=\"monospace\" font-size=\"10\" "
              "font-weight=\"600\" fill=\"#1e293b\" text-anchor=\"middle\">t=%.2f</text>\n",
              badge_x,
              badge_y + 3.5,
              m.crash_time);
    }
  }

  fprintf(f, "</svg>\n");
  fclose(f);
}

/* -------------------------------------------------------------------- */
/** \name Phase 2: Extended Wavefront W*(0) Implementation
 * \{ */

/* Helper: 2D point-in-contour test using ray-casting. */
static bool is_point_in_contour_v2(const double2 &pt, Span<double2> contour)
{
  bool inside = false;
  const int n = int(contour.size());
  for (const int i : IndexRange(n)) {
    const double2 p1 = contour[i];
    const double2 p2 = contour[(i + 1) % n];
    if (((p1.y > pt.y) != (p2.y > pt.y)) &&
        (pt.x < (p2.x - p1.x) * (pt.y - p1.y) / (p2.y - p1.y) + p1.x))
    {
      inside = !inside;
    }
  }
  return inside;
}

ExtendedWavefront build_initial_extended_wavefront(const StraightSkeletonInput &input,
                                                   const MotorcycleGraph &mg)
{
  ExtendedWavefront ew;

  /* 1. Extract contours and boundary wall segments. */
  Vector<WallSegment> walls;
  auto process_contour_walls = [&](Span<int> contour) {
    const int n = int(contour.size());
    if (n < 3) {
      return;
    }
    for (const int i : IndexRange(n)) {
      const int v_curr = contour[i];
      const int v_next = contour[(i + 1) % n];
      const double2 a = input.vert[v_curr];
      const double2 b = input.vert[v_next];
      const double2 diff = b - a;
      const double len = math::length(diff);
      const double2 dir = (len > 1e-12) ? (diff / len) : double2(1.0, 0.0);
      const double2 normal(-dir.y, dir.x);

      WallSegment w;
      w.id = int(walls.size());
      w.a = a;
      w.b = b;
      w.dir = dir;
      w.normal = normal;
      w.length = len;
      w.origin_vert = v_curr;
      walls.append(w);
    }
  };

  process_contour_walls(input.outer_polygon);
  for (const int h : IndexRange(input.hole_offsets.size())) {
    process_contour_walls(input.hole_vert_indices.slice(input.hole_offsets[h]));
  }

  /* 2. Create vertices for original input boundary. */
  const int n_input_verts = int(input.vert.size());
  for (const int i : IndexRange(n_input_verts)) {
    ExtendedVertex ev;
    ev.id = i;
    ev.pos = input.vert[i];
    ev.origin_vert = i;
    ev.birth_time = 0.0;
    ev.is_active = true;
    ew.vertices.append(ev);
  }

  /* Classify original vertices as CONVEX or REFLEX, and compute their velocities. */
  auto classify_contour_verts = [&](Span<int> contour) {
    const int n = int(contour.size());
    for (const int i : IndexRange(n)) {
      const int v_curr = contour[i];
      const int v_prev = contour[(i + n - 1) % n];
      const int v_next = contour[(i + 1) % n];

      const double2 p_prev = input.vert[v_prev];
      const double2 p_curr = input.vert[v_curr];
      const double2 p_next = input.vert[v_next];

      const double2 d_in = math::normalize(p_curr - p_prev);
      const double2 d_out = math::normalize(p_next - p_curr);
      const double cross = d_in.x * d_out.y - d_in.y * d_out.x;

      const double2 n_in(-d_in.y, d_in.x);
      const double2 n_out(-d_out.y, d_out.x);
      const double det = n_in.x * n_out.y - n_in.y * n_out.x;

      double2 vel{0.0, 0.0};
      if (std::abs(det) > 1e-12) {
        vel.x = (n_out.y - n_in.y) / det;
        vel.y = (n_in.x - n_out.x) / det;
      }
      else {
        vel = n_in;
      }

      ExtendedVertex &v = ew.vertices[v_curr];
      v.velocity = vel;

      if (cross < -1e-9) {
        v.type = WavefrontVertexType::REFLEX;
        for (const MotorcycleTrack &m : mg.tracks) {
          if (m.origin_vert == v_curr) {
            v.motorcycle_id = m.id;
            v.velocity = m.velocity;
            break;
          }
        }
      }
      else {
        v.type = WavefrontVertexType::CONVEX;
      }
    }
  };

  classify_contour_verts(input.outer_polygon);
  for (const int h : IndexRange(input.hole_offsets.size())) {
    classify_contour_verts(input.hole_vert_indices.slice(input.hole_offsets[h]));
  }

  /* 3. Create Steiner vertices for motorcycle crash points. */
  Vector<int> track_crash_vert_ids;
  track_crash_vert_ids.resize(mg.tracks.size(), -1);

  for (const int m_idx : IndexRange(mg.tracks.size())) {
    const MotorcycleTrack &m = mg.tracks[m_idx];
    if (!m.crashed) {
      continue;
    }

    int existing_id = -1;
    for (const ExtendedVertex &ev : ew.vertices) {
      if (math::distance(ev.pos, m.crash_pos) < 1e-5) {
        existing_id = ev.id;
        break;
      }
    }

    if (existing_id != -1) {
      track_crash_vert_ids[m_idx] = existing_id;
      if (ew.vertices[existing_id].origin_vert == -1) {
        ew.vertices[existing_id].type = WavefrontVertexType::MULTI_STEINER;
      }
    }
    else {
      ExtendedVertex ev;
      ev.id = int(ew.vertices.size());
      ev.pos = m.crash_pos;
      ev.birth_time = 0.0;
      ev.origin_vert = -1;
      ev.motorcycle_id = m.id;
      ev.is_active = true;

      if (m.crash_type == MotorcycleCrashType::WALL ||
          m.crash_type == MotorcycleCrashType::BOUNDARY)
      {
        ev.type = WavefrontVertexType::MOVING_STEINER;
        ev.input_wall = m.crashed_into_id;
        if (m.crashed_into_id >= 0 && m.crashed_into_id < walls.size()) {
          const WallSegment &w = walls[m.crashed_into_id];
          const double dot_w = math::dot(math::normalize(m.velocity), w.normal);
          if (std::abs(dot_w) > 1e-6) {
            ev.velocity = math::normalize(m.velocity) / dot_w;
          }
        }
      }
      else if (m.crash_type == MotorcycleCrashType::MOTORCYCLE_TRACE) {
        ev.type = WavefrontVertexType::RESTING_STEINER;
        ev.velocity = double2(0.0, 0.0);
      }
      else if (m.crash_type == MotorcycleCrashType::CONCURRENT_MOTORCYCLE) {
        ev.type = WavefrontVertexType::MULTI_STEINER;
        ev.velocity = double2(0.0, 0.0);
      }

      track_crash_vert_ids[m_idx] = ev.id;
      ew.vertices.append(ev);
    }
  }

  /* 4. Build half-edge connectivity. */
  Vector<Vector<int>> vertex_outgoing_he;
  vertex_outgoing_he.resize(ew.vertices.size());

  auto add_halfedge_pair = [&](int u, int v, bool is_steiner, int wall_id) {
    const int h1_id = int(ew.halfedges.size());
    const int h2_id = h1_id + 1;

    ExtendedHalfEdge h1;
    h1.id = h1_id;
    h1.origin = u;
    h1.dest = v;
    h1.twin = h2_id;
    h1.is_steiner = is_steiner;
    h1.wall_id = wall_id;

    ExtendedHalfEdge h2;
    h2.id = h2_id;
    h2.origin = v;
    h2.dest = u;
    h2.twin = h1_id;
    h2.is_steiner = is_steiner;
    h2.wall_id = wall_id;

    ew.halfedges.append(h1);
    ew.halfedges.append(h2);

    vertex_outgoing_he[u].append(h1_id);
    vertex_outgoing_he[v].append(h2_id);
  };

  /* 4a. Slice wall edges by Steiner crash points. */
  for (const WallSegment &w : walls) {
    const int u_idx = w.origin_vert;
    const double2 a = w.a;
    const double2 b = w.b;
    const double2 diff = b - a;
    const double len_sq = math::length_squared(diff);

    struct SlicedPoint {
      double proj;
      int vert_id;
    };
    Vector<SlicedPoint> sliced;

    for (const int crash_vert_id : track_crash_vert_ids) {
      if (crash_vert_id == -1) {
        continue;
      }
      const double2 cp = ew.vertices[crash_vert_id].pos;
      if (math::distance(cp, a) < 1e-5 || math::distance(cp, b) < 1e-5) {
        continue;
      }
      const double proj = math::dot(cp - a, diff) / len_sq;
      if (proj > 1e-5 && proj < 1.0 - 1e-5) {
        const double2 perp = (a + proj * diff) - cp;
        if (math::length(perp) < 1e-4) {
          bool already = false;
          for (const SlicedPoint &sp : sliced) {
            if (sp.vert_id == crash_vert_id) {
              already = true;
              break;
            }
          }
          if (!already) {
            sliced.append({proj, crash_vert_id});
          }
        }
      }
    }

    std::sort(sliced.begin(), sliced.end(), [](const SlicedPoint &p1, const SlicedPoint &p2) {
      return p1.proj < p2.proj;
    });

    int v_dest = -1;
    for (const int vi : IndexRange(n_input_verts)) {
      if (vi != u_idx && math::distance(ew.vertices[vi].pos, b) < 1e-5) {
        v_dest = vi;
        break;
      }
    }
    if (v_dest == -1) {
      continue;
    }

    Vector<int> chain;
    chain.append(u_idx);
    for (const SlicedPoint &sp : sliced) {
      chain.append(sp.vert_id);
    }
    chain.append(v_dest);

    for (const int j : IndexRange(chain.size() - 1)) {
      add_halfedge_pair(chain[j], chain[j + 1], false, w.id);
    }
  }

  /* 4b. Add motorcycle tracks as Steiner edges. */
  for (const int m_idx : IndexRange(mg.tracks.size())) {
    const MotorcycleTrack &m = mg.tracks[m_idx];
    if (!m.crashed) {
      continue;
    }
    int start_id = m.origin_vert;
    if (start_id == -1) {
      for (const ExtendedVertex &ev : ew.vertices) {
        if (math::distance(ev.pos, m.start_pos) < 1e-5) {
          start_id = ev.id;
          break;
        }
      }
    }
    const int crash_id = track_crash_vert_ids[m_idx];
    if (start_id != -1 && crash_id != -1 && start_id != crash_id) {
      const double2 p_start = ew.vertices[start_id].pos;
      const double2 p_end = ew.vertices[crash_id].pos;
      const double2 track_diff = p_end - p_start;
      const double t_len_sq = math::length_squared(track_diff);

      struct TrackSlice {
        double proj;
        int vert_id;
      };
      Vector<TrackSlice> t_slices;

      for (const int other_crash_id : track_crash_vert_ids) {
        if (other_crash_id == -1 || other_crash_id == start_id || other_crash_id == crash_id) {
          continue;
        }
        const double2 cp = ew.vertices[other_crash_id].pos;
        const double proj = math::dot(cp - p_start, track_diff) / t_len_sq;
        if (proj > 1e-5 && proj < 1.0 - 1e-5) {
          const double2 perp = (p_start + proj * track_diff) - cp;
          if (math::length(perp) < 1e-4) {
            bool already = false;
            for (const TrackSlice &ts : t_slices) {
              if (ts.vert_id == other_crash_id) {
                already = true;
                break;
              }
            }
            if (!already) {
              t_slices.append({proj, other_crash_id});
            }
          }
        }
      }

      std::sort(t_slices.begin(), t_slices.end(), [](const TrackSlice &s1, const TrackSlice &s2) {
        return s1.proj < s2.proj;
      });

      Vector<int> t_chain;
      t_chain.append(start_id);
      for (const TrackSlice &ts : t_slices) {
        t_chain.append(ts.vert_id);
      }
      t_chain.append(crash_id);

      for (const int j : IndexRange(t_chain.size() - 1)) {
        add_halfedge_pair(t_chain[j], t_chain[j + 1], true, -1);
      }
    }
  }

  /* 5. Sort outgoing half-edges around every vertex in CCW polar order. */
  for (const int v_idx : IndexRange(ew.vertices.size())) {
    Vector<int> &out_hes = vertex_outgoing_he[v_idx];
    if (out_hes.is_empty()) {
      continue;
    }
    ew.vertices[v_idx].halfedge = out_hes[0];

    const double2 p_v = ew.vertices[v_idx].pos;
    std::sort(out_hes.begin(), out_hes.end(), [&](int h1, int h2) {
      const double2 dest1 = ew.vertices[ew.halfedges[h1].dest].pos;
      const double2 dest2 = ew.vertices[ew.halfedges[h2].dest].pos;
      const double a1 = std::atan2(dest1.y - p_v.y, dest1.x - p_v.x);
      const double a2 = std::atan2(dest2.y - p_v.y, dest2.x - p_v.x);
      return a1 < a2;
    });
  }

  /* 6. Link half-edge prev / next pointers according to the planar embedding. */
  for (const int h_idx : IndexRange(ew.halfedges.size())) {
    ExtendedHalfEdge &h = ew.halfedges[h_idx];
    const int dest_v = h.dest;
    const Vector<int> &dest_out = vertex_outgoing_he[dest_v];
    const int twin_id = h.twin;

    int twin_idx_in_dest = -1;
    for (const int i : IndexRange(dest_out.size())) {
      if (dest_out[i] == twin_id) {
        twin_idx_in_dest = i;
        break;
      }
    }

    if (twin_idx_in_dest != -1) {
      const int d = int(dest_out.size());
      const int next_he_id = dest_out[(twin_idx_in_dest - 1 + d) % d];
      h.next = next_he_id;
      ew.halfedges[next_he_id].prev = h_idx;
    }
  }

  /* 7. Extract faces and check convex cell invariants. */
  Array<bool> visited_he(ew.halfedges.size());
  visited_he.fill(false);

  Vector<double2> outer_pts;
  for (const int vi : input.outer_polygon) {
    outer_pts.append(input.vert[vi]);
  }
  Vector<Vector<double2>> holes_pts;
  for (const int h : IndexRange(input.hole_offsets.size())) {
    Vector<double2> h_pts;
    for (const int vi : input.hole_vert_indices.slice(input.hole_offsets[h])) {
      h_pts.append(input.vert[vi]);
    }
    holes_pts.append(h_pts);
  }

  ew.total_interior_area = 0.0;
  ew.all_faces_convex = true;

  for (const int h_idx : IndexRange(ew.halfedges.size())) {
    if (visited_he[h_idx]) {
      continue;
    }

    Vector<int> cycle_hes;
    Vector<int> cycle_verts;
    int curr = h_idx;
    while (!visited_he[curr]) {
      visited_he[curr] = true;
      cycle_hes.append(curr);
      cycle_verts.append(ew.halfedges[curr].origin);
      curr = ew.halfedges[curr].next;
      if (curr < 0 || curr >= ew.halfedges.size()) {
        break;
      }
    }

    if (cycle_verts.size() < 3) {
      continue;
    }

    double area = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    const int m = int(cycle_verts.size());
    for (const int j : IndexRange(m)) {
      const double2 p1 = ew.vertices[cycle_verts[j]].pos;
      const double2 p2 = ew.vertices[cycle_verts[(j + 1) % m]].pos;
      const double cross = (p1.x * p2.y - p1.y * p2.x);
      area += 0.5 * cross;
      cx += (p1.x + p2.x) * cross;
      cy += (p1.y + p2.y) * cross;
    }

    if (area > 1e-5) {
      cx /= (6.0 * area);
      cy /= (6.0 * area);
      const double2 centroid(cx, cy);

      const bool in_outer = is_point_in_contour_v2(centroid, outer_pts);
      bool in_hole = false;
      for (const Vector<double2> &hp : holes_pts) {
        if (is_point_in_contour_v2(centroid, hp)) {
          in_hole = true;
          break;
        }
      }

      if (in_outer && !in_hole) {
        ExtendedFace face;
        face.id = int(ew.faces.size());
        face.halfedge = cycle_hes[0];
        face.area = area;
        face.centroid = centroid;
        face.edge_count = m;
        face.vert_indices = cycle_verts;

        bool face_convex = true;
        for (const int j : IndexRange(m)) {
          const double2 p0 = ew.vertices[cycle_verts[(j + m - 1) % m]].pos;
          const double2 p1 = ew.vertices[cycle_verts[j]].pos;
          const double2 p2 = ew.vertices[cycle_verts[(j + 1) % m]].pos;
          const double2 d1 = p1 - p0;
          const double2 d2 = p2 - p1;
          const double turn = d1.x * d2.y - d1.y * d2.x;
          if (turn < -1e-5) {
            face_convex = false;
          }
        }
        face.is_convex = face_convex;
        if (!face_convex) {
          ew.all_faces_convex = false;
        }

        ew.total_interior_area += area;

        for (const int he_id : cycle_hes) {
          ew.halfedges[he_id].face = face.id;
        }
        ew.faces.append(face);
      }
    }
  }

  /* 8. Initialize Priority Queue of Collapse Events. */
  for (const int he_idx : IndexRange(ew.halfedges.size())) {
    ExtendedHalfEdge &he = ew.halfedges[he_idx];
    if (he.face == -1) {
      continue;
    }
    const ExtendedVertex &vu = ew.vertices[he.origin];
    const ExtendedVertex &vv = ew.vertices[he.dest];

    if (he.is_steiner) {
      for (const MotorcycleTrack &m : mg.tracks) {
        if (m.crashed && (m.origin_vert == vu.origin_vert || m.id == vu.motorcycle_id)) {
          WavefrontEvent ev;
          ev.time = m.crash_time;
          ev.edge_id = he.id;
          ev.u = he.origin;
          ev.v = he.dest;
          ev.pos = m.crash_pos;

          if (vv.type == WavefrontVertexType::MOVING_STEINER) {
            ev.type = WavefrontEventType::SPLIT;
          }
          else if (vv.type == WavefrontVertexType::MULTI_STEINER) {
            ev.type = WavefrontEventType::MULTI_SPLIT;
          }
          else {
            ev.type = WavefrontEventType::START;
          }

          he.collapse_time = ev.time;
          he.collapse_pos = ev.pos;
          ew.queue.push(ev);
          break;
        }
      }
    }
    else {
      if (vu.type == WavefrontVertexType::CONVEX && vv.type == WavefrontVertexType::CONVEX) {
        const double2 dp = vv.pos - vu.pos;
        const double2 ds = vu.velocity - vv.velocity;
        const double ds_len_sq = math::length_squared(ds);
        if (ds_len_sq > 1e-12) {
          const double t = math::dot(dp, ds) / ds_len_sq;
          if (t > 1e-6) {
            WavefrontEvent ev;
            ev.time = t;
            ev.type = WavefrontEventType::EDGE_COLLAPSE;
            ev.edge_id = he.id;
            ev.u = he.origin;
            ev.v = he.dest;
            ev.pos = vu.pos + t * vu.velocity;

            he.collapse_time = t;
            he.collapse_pos = ev.pos;
            ew.queue.push(ev);
          }
        }
      }
      else if ((vu.type == WavefrontVertexType::CONVEX &&
                vv.type == WavefrontVertexType::MOVING_STEINER) ||
               (vu.type == WavefrontVertexType::MOVING_STEINER &&
                vv.type == WavefrontVertexType::CONVEX))
      {
        const double2 dp = vv.pos - vu.pos;
        const double2 ds = vu.velocity - vv.velocity;
        const double ds_len_sq = math::length_squared(ds);
        if (ds_len_sq > 1e-12) {
          const double t = math::dot(dp, ds) / ds_len_sq;
          if (t > 1e-6) {
            WavefrontEvent ev;
            ev.time = t;
            ev.type = WavefrontEventType::SWITCH;
            ev.edge_id = he.id;
            ev.u = he.origin;
            ev.v = he.dest;
            ev.pos = vu.pos + t * vu.velocity;

            he.collapse_time = t;
            he.collapse_pos = ev.pos;
            ew.queue.push(ev);
          }
        }
      }
    }
  }

  ew.is_valid = !ew.faces.is_empty();
  return ew;
}

void dump_extended_wavefront_to_svg(const StraightSkeletonInput &input,
                                    const ExtendedWavefront &ew,
                                    const char *filepath)
{
  FILE *f = fopen(filepath, "w");
  if (!f) {
    return;
  }

  double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
  for (const ExtendedVertex &v : ew.vertices) {
    min_x = std::min(min_x, v.pos.x);
    min_y = std::min(min_y, v.pos.y);
    max_x = std::max(max_x, v.pos.x);
    max_y = std::max(max_y, v.pos.y);
  }

  double geom_w = max_x - min_x;
  double geom_h = max_y - min_y;
  if (geom_w < 1e-6) {
    geom_w = 1.0;
  }
  if (geom_h < 1e-6) {
    geom_h = 1.0;
  }

  const double pad_x = geom_w * 0.15;
  const double pad_y = geom_h * 0.15;
  min_x -= pad_x;
  max_x += pad_x;
  min_y -= pad_y;
  max_y += pad_y;
  geom_w = max_x - min_x;
  geom_h = max_y - min_y;

  const double svg_w = 960.0;
  const double banner_h = 80.0;
  const double side_pad = 40.0;

  const double avail_w = svg_w - 2.0 * side_pad;
  double scale = avail_w / geom_w;
  double geom_svg_h = geom_h * scale;
  if (geom_svg_h > 800.0) {
    geom_svg_h = 800.0;
    scale = geom_svg_h / geom_h;
  }
  const double svg_h = banner_h + geom_svg_h + 2.0 * side_pad;

  const double offset_x = side_pad + (avail_w - geom_w * scale) * 0.5;
  const double offset_y = banner_h + side_pad;

  auto tx = [&](double x) { return offset_x + (x - min_x) * scale; };
  auto ty = [&](double y) { return offset_y + (max_y - y) * scale; };

  fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  fprintf(f,
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.1f\" height=\"%.1f\" "
          "viewBox=\"0 0 %.1f %.1f\">\n",
          svg_w,
          svg_h,
          svg_w,
          svg_h);

  fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"#f8fafc\"/>\n");

  fprintf(f,
          "  <rect width=\"100%%\" height=\"%.1f\" fill=\"#ffffff\" stroke=\"#e2e8f0\" "
          "stroke-width=\"1.5\"/>\n",
          banner_h);

  const char *title_str = (input.label && input.label[0] != '\0') ? input.label :
                                                                    "Extended Wavefront W*(0)";
  fprintf(f,
          "  <text x=\"30\" y=\"34\" font-family=\"system-ui, -apple-system, sans-serif\" "
          "font-size=\"18\" font-weight=\"700\" fill=\"#0f172a\">W*(0): %s</text>\n",
          title_str);

  int convex_cnt = 0;
  int reflex_cnt = 0;
  int steiner_cnt = 0;
  for (const ExtendedVertex &v : ew.vertices) {
    if (v.type == WavefrontVertexType::CONVEX) {
      convex_cnt++;
    }
    else if (v.type == WavefrontVertexType::REFLEX) {
      reflex_cnt++;
    }
    else {
      steiner_cnt++;
    }
  }

  fprintf(f,
          "  <text x=\"30\" y=\"58\" font-family=\"system-ui, -apple-system, sans-serif\" "
          "font-size=\"13\" fill=\"#64748b\">Verts: %d (Convex: %d, Reflex: %d, Steiner: %d) | "
          "Faces: %d (All Convex: %s) | Events in Q: %d</text>\n",
          int(ew.vertices.size()),
          convex_cnt,
          reflex_cnt,
          steiner_cnt,
          int(ew.faces.size()),
          ew.all_faces_convex ? "Yes" : "No",
          int(ew.queue.size()));

  /* Legend. */
  fprintf(f, "  <g transform=\"translate(540, 16)\">\n");
  fprintf(f,
          "    <circle cx=\"0\" cy=\"10\" r=\"4.5\" fill=\"#2563eb\"/>\n"
          "    <text x=\"10\" y=\"14\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Convex</text>\n"
          "    <circle cx=\"80\" cy=\"10\" r=\"4.5\" fill=\"#dc2626\"/>\n"
          "    <text x=\"90\" y=\"14\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Reflex</text>\n"
          "    <circle cx=\"160\" cy=\"10\" r=\"4.5\" fill=\"#16a34a\"/>\n"
          "    <text x=\"170\" y=\"14\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Moving Steiner</text>\n");
  fprintf(f,
          "    <circle cx=\"0\" cy=\"35\" r=\"4.5\" fill=\"#9333ea\"/>\n"
          "    <text x=\"10\" y=\"39\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Resting Steiner</text>\n"
          "    <circle cx=\"100\" cy=\"35\" r=\"4.5\" fill=\"#ea580c\"/>\n"
          "    <text x=\"110\" y=\"39\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Multi Steiner</text>\n"
          "    <line x1=\"200\" y1=\"35\" x2=\"222\" y2=\"35\" stroke=\"#dc2626\" "
          "stroke-width=\"2\"/>\n"
          "    <text x=\"228\" y=\"39\" font-family=\"sans-serif\" font-size=\"11\" "
          "fill=\"#334155\">Track</text>\n");
  fprintf(f, "  </g>\n");

  /* 1. Draw convex interior faces with soft pastel fills. */
  const char *face_colors[] = {
      "#eff6ff", "#f0fdf4", "#fefce8", "#fdf2f8", "#f5f3ff", "#ecfeff", "#fff7ed", "#fafaf9"};
  for (const ExtendedFace &f_item : ew.faces) {
    fprintf(f, "  <polygon points=\"");
    for (const int vi : f_item.vert_indices) {
      fprintf(f, "%.2f,%.2f ", tx(ew.vertices[vi].pos.x), ty(ew.vertices[vi].pos.y));
    }
    fprintf(f,
            "\" fill=\"%s\" stroke=\"#cbd5e1\" stroke-width=\"1.0\" stroke-dasharray=\"3,3\"/>\n",
            face_colors[f_item.id % 8]);

    const double fcx = tx(f_item.centroid.x);
    const double fcy = ty(f_item.centroid.y);
    fprintf(f,
            "  <rect x=\"%.2f\" y=\"%.2f\" width=\"32\" height=\"16\" rx=\"4\" fill=\"#ffffff\" "
            "fill-opacity=\"0.85\" stroke=\"#94a3b8\" stroke-width=\"1\"/>\n",
            fcx - 16.0,
            fcy - 8.0);
    fprintf(f,
            "  <text x=\"%.2f\" y=\"%.2f\" font-family=\"sans-serif\" font-size=\"10\" "
            "font-weight=\"bold\" fill=\"#334155\" text-anchor=\"middle\">f%d</text>\n",
            fcx,
            fcy + 3.5,
            f_item.id);
  }

  /* 2. Draw half-edges (boundary in blue, steiner in crimson). */
  for (const ExtendedHalfEdge &he : ew.halfedges) {
    if (he.origin >= he.dest) {
      continue;
    }
    const double x1 = tx(ew.vertices[he.origin].pos.x);
    const double y1 = ty(ew.vertices[he.origin].pos.y);
    const double x2 = tx(ew.vertices[he.dest].pos.x);
    const double y2 = ty(ew.vertices[he.dest].pos.y);

    if (he.is_steiner) {
      fprintf(f,
              "  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#dc2626\" "
              "stroke-width=\"2.0\" stroke-dasharray=\"none\"/>\n",
              x1,
              y1,
              x2,
              y2);
    }
    else {
      fprintf(f,
              "  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#2563eb\" "
              "stroke-width=\"2.5\" stroke-linecap=\"round\"/>\n",
              x1,
              y1,
              x2,
              y2);
    }
  }

  /* 3. Draw classified vertices. */
  for (const ExtendedVertex &v : ew.vertices) {
    const double vx = tx(v.pos.x);
    const double vy = ty(v.pos.y);

    const char *v_color = "#2563eb";
    if (v.type == WavefrontVertexType::REFLEX) {
      v_color = "#dc2626";
    }
    else if (v.type == WavefrontVertexType::MOVING_STEINER) {
      v_color = "#16a34a";
    }
    else if (v.type == WavefrontVertexType::RESTING_STEINER) {
      v_color = "#9333ea";
    }
    else if (v.type == WavefrontVertexType::MULTI_STEINER) {
      v_color = "#ea580c";
    }

    fprintf(f,
            "  <circle cx=\"%.2f\" cy=\"%.2f\" r=\"5.0\" fill=\"%s\" stroke=\"#ffffff\" "
            "stroke-width=\"1.5\"/>\n",
            vx,
            vy,
            v_color);
    fprintf(f,
            "  <text x=\"%.2f\" y=\"%.2f\" font-family=\"monospace\" font-size=\"10\" "
            "fill=\"#475569\">v%d</text>\n",
            vx + 7.0,
            vy - 5.0,
            v.id);
  }

  fprintf(f, "</svg>\n");
  fclose(f);
}

/* -------------------------------------------------------------------- */
/** \name Phase 3: Wavefront Event Propagation
 * \{ */

struct CellActiveVertex {
  int id = -1;
  double2 pos{0.0, 0.0};
  double2 vel{0.0, 0.0};
  double birth_time = 0.0;
  int skel_node = -1;
  int in_wall = -1;
  int out_wall = -1;
  int prev = -1;
  int next = -1;
  bool is_active = true;
};

static double2 compute_bisector_velocity(const double2 &n_in, const double2 &n_out)
{
  const double det = n_in.x * n_out.y - n_in.y * n_out.x;
  if (std::abs(det) > 1e-12) {
    return double2((n_out.y - n_in.y) / det, (n_in.x - n_out.x) / det);
  }
  return n_in;
}

static int solve_convex_cell(const ExtendedFace &face,
                             const ExtendedWavefront &ew,
                             const Vector<WallSegment> &walls,
                             const Vector<int> &vert_in_wall,
                             const Vector<int> &vert_out_wall,
                             const double2 &bbox_min,
                             const double2 &bbox_max,
                             const double max_t,
                             SkeletonGraph &skel)
{
  const int m = int(face.vert_indices.size());
  if (m < 3) {
    return 0;
  }

  /* Collect face halfedges to identify wall associations. */
  Vector<int> face_halfedges;
  int curr_he = face.halfedge;
  do {
    face_halfedges.append(curr_he);
    curr_he = ew.halfedges[curr_he].next;
  } while (curr_he != face.halfedge && curr_he >= 0 && face_halfedges.size() < 2 * m);

  Vector<CellActiveVertex> nodes;
  for (const int i : IndexRange(m)) {
    const int vi = face.vert_indices[i];
    const ExtendedVertex &ev = ew.vertices[vi];

    int s_node = -1;
    if (ev.origin_vert >= 0) {
      s_node = ev.origin_vert;
    }

    int in_w = -1;
    int out_w = -1;
    for (const int h : face_halfedges) {
      if (ew.halfedges[h].dest == vi && !ew.halfedges[h].is_steiner) {
        in_w = ew.halfedges[h].wall_id;
      }
      if (ew.halfedges[h].origin == vi && !ew.halfedges[h].is_steiner) {
        out_w = ew.halfedges[h].wall_id;
      }
    }

    if (in_w == -1) {
      if (ev.origin_vert >= 0 && ev.origin_vert < vert_in_wall.size()) {
        in_w = vert_in_wall[ev.origin_vert];
      }
      else {
        in_w = ev.input_wall;
      }
    }

    if (out_w == -1) {
      if (ev.origin_vert >= 0 && ev.origin_vert < vert_out_wall.size()) {
        out_w = vert_out_wall[ev.origin_vert];
      }
      else {
        out_w = ev.input_wall;
      }
    }

    CellActiveVertex cav;
    cav.id = i;
    cav.pos = ev.pos;
    cav.vel = ev.velocity;
    cav.birth_time = 0.0;
    cav.skel_node = s_node;
    cav.in_wall = in_w;
    cav.out_wall = out_w;
    cav.prev = (i + m - 1) % m;
    cav.next = (i + 1) % m;
    cav.is_active = true;
    nodes.append(cav);
  }

  int active_count = m;
  int events_count = 0;

  while (active_count > 2) {
    double best_t = 1e30;
    int best_edge = -1;
    double2 best_pos{0.0, 0.0};

    for (const int i : IndexRange(nodes.size())) {
      if (!nodes[i].is_active) {
        continue;
      }
      const int j = nodes[i].next;
      const CellActiveVertex &u = nodes[i];
      const CellActiveVertex &v = nodes[j];

      const double2 dp = (v.pos - v.birth_time * v.vel) - (u.pos - u.birth_time * u.vel);
      const double2 dv = u.vel - v.vel;
      const double dv_sq = math::length_squared(dv);
      if (dv_sq > 1e-12) {
        const double t = math::dot(dp, dv) / dv_sq;
        if (t > std::max(u.birth_time, v.birth_time) - 1e-7 && t < max_t) {
          const double2 pos_u = u.pos + (t - u.birth_time) * u.vel;
          const double2 pos_v = v.pos + (t - v.birth_time) * v.vel;
          if (pos_u.x >= bbox_min.x && pos_u.x <= bbox_max.x && pos_u.y >= bbox_min.y &&
              pos_u.y <= bbox_max.y)
          {
            if (math::distance(pos_u, pos_v) < 1e-3) {
              if (t < best_t) {
                best_t = t;
                best_edge = i;
                best_pos = pos_u;
              }
            }
          }
        }
      }
    }

    if (best_edge == -1 || best_t > 1e20) {
      Vector<int> rem;
      for (const int i : IndexRange(nodes.size())) {
        if (nodes[i].is_active) {
          rem.append(i);
        }
      }
      if (rem.size() == 2) {
        if (nodes[rem[0]].skel_node >= 0 && nodes[rem[1]].skel_node >= 0) {
          skel.add_edge(nodes[rem[0]].skel_node, nodes[rem[1]].skel_node);
        }
      }
      break;
    }

    const double t_curr = best_t;
    const double2 pos_curr = best_pos;

    Vector<int> collapsing;
    collapsing.append(best_edge);
    collapsing.append(nodes[best_edge].next);

    /* Expand forward. */
    while (true) {
      const int nxt = nodes[collapsing.last()].next;
      if (collapsing.contains(nxt)) {
        break;
      }
      const double2 p_nxt = nodes[nxt].pos + (t_curr - nodes[nxt].birth_time) * nodes[nxt].vel;
      if (math::distance(p_nxt, pos_curr) < 1e-3) {
        collapsing.append(nxt);
      }
      else {
        break;
      }
    }

    /* Expand backward. */
    while (true) {
      const int prv = nodes[collapsing.first()].prev;
      if (collapsing.contains(prv)) {
        break;
      }
      const double2 p_prv = nodes[prv].pos + (t_curr - nodes[prv].birth_time) * nodes[prv].vel;
      if (math::distance(p_prv, pos_curr) < 1e-3) {
        collapsing.insert(0, prv);
      }
      else {
        break;
      }
    }

    const int s_star = skel.add_node(pos_curr, t_curr, -1);
    for (const int cid : collapsing) {
      if (nodes[cid].skel_node >= 0) {
        skel.add_edge(nodes[cid].skel_node, s_star);
      }
      nodes[cid].is_active = false;
    }

    events_count++;
    active_count -= collapsing.size();

    if (active_count == 0) {
      break;
    }
    if (active_count == 1) {
      for (const int i : IndexRange(nodes.size())) {
        if (nodes[i].is_active && nodes[i].skel_node >= 0) {
          skel.add_edge(nodes[i].skel_node, s_star);
          break;
        }
      }
      break;
    }

    /* Create merged vertex. */
    const int c_first = collapsing.first();
    const int c_last = collapsing.last();
    const int prev_node = nodes[c_first].prev;
    const int next_node = nodes[c_last].next;

    const int in_w = nodes[c_first].in_wall;
    const int out_w = nodes[c_last].out_wall;

    double2 new_v{0.0, 0.0};
    if (in_w >= 0 && in_w < walls.size() && out_w >= 0 && out_w < walls.size()) {
      new_v = compute_bisector_velocity(walls[in_w].normal, walls[out_w].normal);
    }
    else {
      const double2 d_in = math::normalize(pos_curr - nodes[prev_node].pos);
      const double2 d_out = math::normalize(nodes[next_node].pos - pos_curr);
      const double2 n_in(-d_in.y, d_in.x);
      const double2 n_out(-d_out.y, d_out.x);
      new_v = compute_bisector_velocity(n_in, n_out);
    }

    const int new_id = int(nodes.size());
    CellActiveVertex merged;
    merged.id = new_id;
    merged.pos = pos_curr;
    merged.vel = new_v;
    merged.birth_time = t_curr;
    merged.skel_node = s_star;
    merged.in_wall = in_w;
    merged.out_wall = out_w;
    merged.prev = prev_node;
    merged.next = next_node;
    merged.is_active = true;

    nodes.append(merged);
    nodes[prev_node].next = new_id;
    nodes[next_node].prev = new_id;
    active_count++;

    if (active_count == 2) {
      if (nodes[prev_node].skel_node >= 0) {
        skel.add_edge(nodes[prev_node].skel_node, s_star);
      }
      break;
    }
  }

  return events_count;
}

StraightSkeletonOutput straight_skeleton_calc_motorcycle(const StraightSkeletonInput &input)
{
  if (input.outer_polygon.size() < 3) {
    StraightSkeletonOutput empty_out;
    empty_out.hit_max_events = false;
    empty_out.events_processed_num = 0;
    empty_out.flips_num = 0;
    return empty_out;
  }

  /* Phase 1: Compute motorcycle graph. */
  const MotorcycleGraph mg = compute_motorcycle_graph(input);

  /* Phase 2: Build initial extended wavefront W*(0). */
  const ExtendedWavefront ew = build_initial_extended_wavefront(input, mg);

  /* Phase 3: Propagate extended wavefront W*(t) via convex cell simulation. */
  SkeletonGraph skel;
  for (const int i : IndexRange(input.vert.size())) {
    skel.add_node(input.vert[i], 0.0, i);
  }

  /* Extract wall segments. */
  Vector<WallSegment> walls;
  auto add_contour_walls = [&](Span<int> contour) {
    const int n = int(contour.size());
    for (const int i : IndexRange(n)) {
      const int v_curr = contour[i];
      const int v_next = contour[(i + 1) % n];
      const double2 p1 = input.vert[v_curr];
      const double2 p2 = input.vert[v_next];
      const double2 diff = p2 - p1;
      const double len = math::length(diff);
      const double2 norm = (len > 1e-12) ? double2(-diff.y, diff.x) / len : double2(0.0, 1.0);

      WallSegment w;
      w.id = int(walls.size());
      w.a = p1;
      w.b = p2;
      w.normal = norm;
      w.origin_vert = v_curr;
      walls.append(w);
    }
  };

  add_contour_walls(input.outer_polygon);
  for (const int h : IndexRange(input.hole_offsets.size())) {
    add_contour_walls(input.hole_vert_indices.slice(input.hole_offsets[h]));
  }

  Vector<int> vert_in_wall(input.vert.size(), -1);
  Vector<int> vert_out_wall(input.vert.size(), -1);
  for (const WallSegment &w : walls) {
    vert_out_wall[w.origin_vert] = w.id;
  }
  for (const int w_idx : IndexRange(walls.size())) {
    const WallSegment &w = walls[w_idx];
    for (const int vi : IndexRange(input.vert.size())) {
      if (math::distance(input.vert[vi], w.b) < 1e-5) {
        vert_in_wall[vi] = w_idx;
        break;
      }
    }
  }

  double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
  for (const double2 &p : input.vert) {
    min_x = std::min(min_x, p.x);
    min_y = std::min(min_y, p.y);
    max_x = std::max(max_x, p.x);
    max_y = std::max(max_y, p.y);
  }
  const double geom_dim = std::max(max_x - min_x, max_y - min_y);
  const double pad = std::max(2.0, 0.2 * geom_dim);
  const double2 bbox_min(min_x - pad, min_y - pad);
  const double2 bbox_max(max_x + pad, max_y + pad);
  const double max_t = std::max(10.0, 2.5 * geom_dim);

  int total_events = 0;
  for (const ExtendedFace &face : ew.faces) {
    total_events += solve_convex_cell(
        face, ew, walls, vert_in_wall, vert_out_wall, bbox_min, bbox_max, max_t, skel);
  }

  /* Build full StraightSkeletonOutput with face extraction and offset band clipping. */
  return build_straight_skeleton_output(input, skel, total_events, 0, false);
}

/** \} */

}  // namespace blender::straightskeleton
