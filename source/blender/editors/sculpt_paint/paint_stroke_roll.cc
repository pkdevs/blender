/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Roll texture mapping for paint strokes.
 *
 * This file implements the arc-length-parameterized spline (RollSpline),
 * virtual extension points, the surface-interpolation grid (B-spline
 * subdivision, Laplacian smoothing, LUT rasterization), and the UV lookup used by
 * sculpt_apply_texture() when the brush map mode is MTEX_MAP_MODE_ROLL.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_utildefines.hh"

#include "BLI_length_parameterize.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "RNA_access.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "WM_api.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "ED_view3d.hh"

#include "paint_intern.hh"

#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/* Shared tuning constants for roll texture mapping.
 * MIN_ROLL_PRESSURE lives in paint_intern.hh — it is also used by
 * space_stroke() to floor the dab spacing. */
static constexpr float PRESSURE_RATIO_MIN = 0.5f;
static constexpr float PRESSURE_RATIO_MAX = 2.0f;
static constexpr int VIRTUAL_EXTENSION_POINTS = 6;
static constexpr int EVAL_ROW_MARGIN = 3;

/**
 * Extrapolate pen pressure geometrically for virtual extension points.
 * \param base_p: pressure at the anchor (real) point.
 * \param ratio: per-step multiplier (clamped from neighboring real points).
 * \param n_points: number of virtual points to generate.
 * \param farthest_first: if true, step count decreases with index (backward ext).
 * \param r_pressures: output vector to append to.
 */
static void extrapolate_pressures(
    float base_p, float ratio, int n_points, bool farthest_first, Vector<float> &r_pressures)
{
  for (int i = 0; i < n_points; i++) {
    const int steps = farthest_first ? (n_points - i) : (i + 1);
    r_pressures.append(std::clamp(base_p * powf(ratio, float(steps)), 0.01f, 1.0f));
  }
}

/* -------------------------------------------------------------------- */
/** \name RollSpline — polyline-based arc-length parameterized spline
 * \{ */

void RollSpline::clear()
{
  poly_2d.clear();
  poly_3d.clear();
  lengths_3d.clear();
  tangents_3d.clear();
  pressures.clear();
  normals.clear();
}

bool RollSpline::is_empty() const
{
  return poly_3d.size() < 2;
}

void RollSpline::update_lengths()
{
  const int n3d = int(poly_3d.size());
  if (n3d >= 2) {
    lengths_3d.reinitialize(length_parameterize::segments_num(n3d, false));
    length_parameterize::accumulate_lengths(poly_3d.as_span(), false, lengths_3d);

    /* Smooth tangents via central differences — gives C0 continuous tangent
     * field instead of the piecewise-constant direction per segment. */
    tangents_3d.reinitialize(n3d);
    tangents_3d[0] = math::normalize(poly_3d[1] - poly_3d[0]);
    for (int i = 1; i < n3d - 1; i++) {
      tangents_3d[i] = math::normalize(poly_3d[i + 1] - poly_3d[i - 1]);
    }
    tangents_3d[n3d - 1] = math::normalize(poly_3d[n3d - 1] - poly_3d[n3d - 2]);
  }
  else {
    lengths_3d.clear();
    tangents_3d.clear();
  }
}

float2 RollSpline::tangent_2d_at_index(int idx) const
{
  const int n = int(poly_2d.size());
  if (n < 2) {
    return float2(1, 0);
  }
  idx = std::clamp(idx, 0, n - 2);
  const float2 d = poly_2d[idx + 1] - poly_2d[idx];
  const float len = math::length(d);
  return (len > 1e-7f) ? d / len : float2(1, 0);
}

float RollSpline::closest_point_arc_length(const float3 &query) const
{
  float best_dist_sq = FLT_MAX;
  int best_seg = 0;
  float best_t = 0.0f;

  for (int i = 0; i < int(poly_3d.size()) - 1; i++) {
    const float3 ab = poly_3d[i + 1] - poly_3d[i];
    const float ab_dot = math::dot(ab, ab);
    const float t = (ab_dot > 1e-12f) ?
                        std::clamp(math::dot(query - poly_3d[i], ab) / ab_dot, 0.0f, 1.0f) :
                        0.0f;
    const float3 proj = math::interpolate(poly_3d[i], poly_3d[i + 1], t);
    const float dist_sq = math::distance_squared(query, proj);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best_seg = i;
      best_t = t;
    }
  }

  const float seg_start = (best_seg > 0) ? lengths_3d[best_seg - 1] : 0.0f;
  const float seg_end = lengths_3d[best_seg];
  return seg_start + best_t * (seg_end - seg_start);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Roll Texture Mapping Stroke Helpers
 * \{ */

int PaintStroke::roll_points_per_radius() const
{
  /* Knots are one dab spacing apart (2R * spacing). */
  const float s = std::max(spacing_raw_, 0.05f);
  return std::max(int(std::ceil(1.0f / s)) + 2, 5);
}

int PaintStroke::roll_max_points() const
{
  if (!need_roll_mapping_) {
    return 1;
  }
  /* About 20 brush radii of history, so turns and overlaps far back are
   * still available to the strip. */
  constexpr int ROLL_HISTORY_RADII = 20;
  const int budget = roll_points_per_radius() * ROLL_HISTORY_RADII;
  /* Leave room in the ring buffer for the virtual backward extension. */
  return std::min(budget, ROLL_MAX_STROKE_POINTS - VIRTUAL_EXTENSION_POINTS - 2);
}

bool PaintStroke::roll_texture_pressure_scaled() const
{
  if (!brush || !BKE_brush_use_size_pressure(brush)) {
    return false;
  }
  /* Read the option from whichever texture slot uses roll mapping. */
  const MTex &mt = (brush->mtex.brush_map_mode == MTEX_MAP_MODE_ROLL) ? brush->mtex :
                                                                        brush->mask_mtex;
  return mt.roll_pressure_scale != 0;
}

float3 PaintStroke::roll_sample_surface_normal(bContext *C, const float2 &mouse) const
{
  /* Sculpt mode: normal of the face under the pen.  Not the cursor's
   * sampled area normal: mid-stroke that one is computed around the
   * (deferred) dab rather than the pen and gives the wrong orientation.
   * Noise is smoothed out along the stroke later. */
  if (vc.obact && (vc.obact->mode & OB_MODE_SCULPT)) {
    if (const std::optional<CursorGeometryInfo> gi = cursor_geometry_info_update(C, mouse, false))
    {
      if (math::length_squared(gi->normal) > 1e-8f) {
        return math::normalize(gi->normal);
      }
    }
  }
  /* Fallback: the view direction in object space. */
  if (vc.obact && vc.rv3d) {
    const float3 n = math::transform_direction(
        vc.obact->world_to_object() * float4x4(vc.rv3d->viewinv), float3(0, 0, 1));
    if (math::length_squared(n) > 1e-8f) {
      return math::normalize(n);
    }
  }
  return float3(0, 0, 1);
}

void PaintStroke::add_roll_point(const float2 &mouse_in,
                                 const float2 &mouse_out,
                                 const float3 &loc,
                                 const float3 &surface_normal,
                                 float size,
                                 float pressure,
                                 bool pen_flip,
                                 float x_tilt,
                                 float y_tilt)
{
  constexpr int buf_cap = ROLL_MAX_STROKE_POINTS;

  if (need_roll_mapping_) {
    /* Budget: total knots (virtual backward + real) capped at
     * roll_max_points() + initial_backward_ext_count_.  When the budget is
     * exceeded we drop the oldest knot:
     *
     *  Phase 1 — Virtual knots still remain: drop the farthest backward
     *            extension point.  No real data is lost.
     *  Phase 2 — All virtual knots consumed: the ring buffer wraps, dropping
     *            the oldest real point.
     *
     * In both cases we accumulate the removed segment's arc length into
     * stroke_distance_world_ (raw) and stroke_distance_normalized_ (pressure-
     * weighted, for the pressure-scale feature).  These offsets are subtracted
     * in spline_uv() so the texture V coordinate stays continuous despite the
     * shrinking polyline. */
    const int budget = roll_max_points() + initial_backward_ext_count_;
    const int total_knots = int(backward_ext_2d_.size()) + num_points_;
    bool wrap_ring = false;

    if (total_knots >= budget) {
      if (!backward_ext_2d_.is_empty()) {
        /* Consume the oldest virtual knot. The ring buffer keeps growing;
         * no real stroke data is lost yet.
         * Accumulate the arc-length of the removed span so that
         * stroke_distance_world_ compensates and the texture stays put. */
        float seg_len = 0.0f;
        if (!roll_spline_.lengths_3d.is_empty()) {
          seg_len = roll_spline_.segment_length_3d(0);
        }
        else if (backward_ext_3d_.size() >= 2) {
          seg_len = math::distance(backward_ext_3d_[0], backward_ext_3d_[1]);
        }
        stroke_distance_world_ += seg_len;
        /* Normalized distance: use the average pressure of the consumed
         * segment's two endpoints, matching the grid V computation. */
        if (roll_initial_radius_ > 0.0f) {
          const float p0 = (!roll_spline_.pressures.is_empty()) ? roll_spline_.pressures[0] : 1.0f;
          const float p1 = (roll_spline_.pressures.size() > 1) ? roll_spline_.pressures[1] : p0;
          const float p_avg = std::max((p0 + p1) * 0.5f, 0.01f);
          stroke_distance_normalized_ += seg_len / (roll_initial_radius_ * p_avg);
        }
        backward_ext_2d_.remove(0);
        backward_ext_3d_.remove(0);
      }
      else {
        /* All virtual knots consumed — ring buffer wraps, oldest real point
         * is abandoned. Accumulate its distance so stroke_distance_world_
         * stays correct. */
        const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;
        const int next_idx = (oldest_idx + 1) % buf_cap;
        const float seg_len = math::distance(points_[oldest_idx].location,
                                             points_[next_idx].location);
        stroke_distance_world_ += seg_len;
        if (roll_initial_radius_ > 0.0f) {
          const float p_avg = std::max(
              (points_[oldest_idx].pressure + points_[next_idx].pressure) * 0.5f, 0.01f);
          stroke_distance_normalized_ += seg_len / (roll_initial_radius_ * p_avg);
        }
        wrap_ring = true;
      }
    }

    PaintStrokePoint *point = &points_[cur_point_];
    point->size = size;
    point->mouse_in = mouse_in;
    point->mouse_out = mouse_out;
    point->x_tilt = x_tilt;
    point->y_tilt = y_tilt;
    point->pen_flip = pen_flip;

    point->location = loc;
    point->surface_normal = (math::length_squared(surface_normal) > 1e-8f) ?
                                math::normalize(surface_normal) :
                                float3(0, 0, 1);
    /* Clamp pressure — pen touch-down and lift-off report near-zero values
     * that would create an extreme width spike in the roll strip. */
    point->pressure = std::max(pressure, MIN_ROLL_PRESSURE);

    cur_point_ = (cur_point_ + 1) % buf_cap;
    if (!wrap_ring) {
      num_points_++;
    }
  }
  else {
    /* Non-roll: simple ring buffer wrapping at ROLL_MAX_STROKE_POINTS. */
    PaintStrokePoint *point = &points_[cur_point_];
    point->size = size;
    point->mouse_in = mouse_in;
    point->mouse_out = mouse_out;
    point->x_tilt = x_tilt;
    point->y_tilt = y_tilt;
    point->pen_flip = pen_flip;

    point->location = loc;
    point->pressure = pressure;

    cur_point_ = (cur_point_ + 1) % buf_cap;
    if (num_points_ < buf_cap) {
      num_points_++;
    }
  }
}

/**
 * Stroke direction at one end (forward sense), measured to the first point
 * at least `min_len` away so one jittery sample cannot tilt it.
 * \a from_start selects the first point, otherwise the last.
 */
static bool end_direction(const Span<float2> pts_2d,
                          const Span<float3> pts_3d,
                          const bool from_start,
                          const float min_len_2d,
                          const float min_len_3d,
                          float2 &r_dir_2d,
                          float3 &r_dir_3d)
{
  const int n = int(pts_2d.size());
  if (n < 2) {
    return false;
  }
  const int end = from_start ? 0 : n - 1;
  const int step = from_start ? 1 : -1;
  float2 d2(0);
  float3 d3(0);
  for (int k = end + step; k >= 0 && k < n; k += step) {
    /* Forward sense: away from the start, or toward the end. */
    d2 = from_start ? pts_2d[k] - pts_2d[end] : pts_2d[end] - pts_2d[k];
    d3 = from_start ? pts_3d[k] - pts_3d[end] : pts_3d[end] - pts_3d[k];
    if (math::length(d2) >= min_len_2d && math::length(d3) >= min_len_3d) {
      break;
    }
  }
  const float l2 = math::length(d2);
  const float l3 = math::length(d3);
  if (l2 < 1e-7f || l3 < 1e-7f) {
    return false;
  }
  r_dir_2d = d2 / l2;
  r_dir_3d = d3 / l3;
  return true;
}

/**
 * Average turning rate near one end of the stroke, over about one dab
 * diameter (or the whole stroke when shorter).  3D turning is measured
 * about \a surface_normal so extensions stay on the surface.
 * Returns radians per unit length, 2D (screen) and 3D (object space).
 */
static void estimate_end_curvature(const Span<float2> pts_2d,
                                   const Span<float3> pts_3d,
                                   const bool from_start,
                                   const float3 &surface_normal,
                                   const float max_arc_2d,
                                   const float max_arc_3d,
                                   float &r_curv_2d,
                                   float &r_curv_3d)
{
  r_curv_2d = 0.0f;
  r_curv_3d = 0.0f;
  const int n = int(pts_2d.size());
  if (n < 3) {
    return;
  }
  float sum_a2 = 0.0f, sum_a3 = 0.0f, arc2 = 0.0f, arc3 = 0.0f;
  int used = 0;
  for (int step = 0; step < n - 2; step++) {
    /* Consecutive segments (a, b) in stroke order, walking inward. */
    const int k = from_start ? step : (n - 3 - step);
    const float2 a2 = pts_2d[k + 1] - pts_2d[k];
    const float2 b2 = pts_2d[k + 2] - pts_2d[k + 1];
    const float3 a3 = pts_3d[k + 1] - pts_3d[k];
    const float3 b3 = pts_3d[k + 2] - pts_3d[k + 1];
    const float la2 = math::length(a2), lb2 = math::length(b2);
    const float la3 = math::length(a3), lb3 = math::length(b3);
    if (la2 < 1e-7f || lb2 < 1e-7f || la3 < 1e-7f || lb3 < 1e-7f) {
      continue;
    }
    /* Always use the first pair; stop once the window exceeds the limit. */
    if (used > 0 && (arc3 + lb3 > max_arc_3d || arc2 + lb2 > max_arc_2d)) {
      break;
    }
    sum_a2 += atan2f(a2.x * b2.y - a2.y * b2.x, math::dot(a2, b2));
    sum_a3 += atan2f(math::dot(math::cross(a3, b3), surface_normal), math::dot(a3, b3));
    /* Each turn is attributed to the arc between the segment midpoints. */
    arc2 += 0.5f * (la2 + lb2);
    arc3 += 0.5f * (la3 + lb3);
    used++;
  }
  if (arc2 > 1e-7f) {
    r_curv_2d = sum_a2 / arc2;
  }
  if (arc3 > 1e-7f) {
    r_curv_3d = sum_a3 / arc3;
  }
}

/**
 * Continue the stroke beyond its start or end with virtual points that
 * follow its curvature, so the strip covers the full brush footprint there.
 *
 * \param sign: +1 forward, -1 backward.
 * \param r_ext_2d, r_ext_3d: Appended in walk order (nearest to the anchor
 *   first for forward, farthest first for backward).
 */
static void generate_virtual_extension(const float2 &anchor_2d,
                                       const float3 &anchor_3d,
                                       const float2 &dir_2d,
                                       const float3 &dir_3d,
                                       const float curv_2d,
                                       const float curv_3d,
                                       const float3 &rot_axis,
                                       const int n_points,
                                       const float step_2d,
                                       const float step_3d,
                                       const float sign,
                                       Vector<float2> &r_ext_2d,
                                       Vector<float3> &r_ext_3d)
{
  /* Per-step rotation continuing the measured curvature, clamped so the
   * total turn over the extension stays within 90°.  Without this, tight
   * turns can spiral into loops. */
  const float max_per_step = float(M_PI_2) / float(std::max(1, n_points));
  const float angle_2d = std::clamp(curv_2d * step_2d, -max_per_step, max_per_step);
  const float angle_3d = std::clamp(curv_3d * step_3d, -max_per_step, max_per_step);
  const bool has_rot_3d = math::length_squared(rot_axis) > 1e-8f && angle_3d != 0.0f;

  /* Walk from anchor, rotating direction each step. */
  float2 cur_2d = dir_2d;
  float3 cur_3d = dir_3d;
  float2 prev_2d = anchor_2d;
  float3 prev_3d = anchor_3d;

  /* For backward extensions we build into a temporary array and reverse,
   * so the output is ordered farthest→nearest (matching the expected
   * prepend order where index 0 is the farthest virtual knot). */
  const bool backward = (sign < 0.0f);
  const int old_size = int(r_ext_2d.size());

  for (int i = 0; i < n_points; i++) {
    /* Rotate direction before stepping. */
    const float a2 = sign * angle_2d;
    if (a2 != 0.0f) {
      const float c = cosf(a2);
      const float s = sinf(a2);
      cur_2d = float2(cur_2d.x * c - cur_2d.y * s, cur_2d.x * s + cur_2d.y * c);
    }
    if (has_rot_3d) {
      const float a3 = sign * angle_3d;
      const float c = cosf(a3);
      const float s = sinf(a3);
      const float3 kxv = math::cross(rot_axis, cur_3d);
      const float kdv = math::dot(rot_axis, cur_3d);
      cur_3d = cur_3d * c + kxv * s + rot_axis * kdv * (1.0f - c);
    }

    prev_2d = prev_2d + cur_2d * (sign * step_2d);
    prev_3d = prev_3d + cur_3d * (sign * step_3d);
    r_ext_2d.append(prev_2d);
    r_ext_3d.append(prev_3d);
  }

  if (backward) {
    /* Reverse so output goes farthest-from-anchor → nearest. */
    std::reverse(r_ext_2d.begin() + old_size, r_ext_2d.end());
    std::reverse(r_ext_3d.begin() + old_size, r_ext_3d.end());
  }
}

void PaintStroke::prepend_virtual_roll_points()
{
  if (num_points_ < 2) {
    return;
  }

  constexpr int cap = ROLL_MAX_STROKE_POINTS;
  const int oldest = (cur_point_ - num_points_ + cap) % cap;

  const float3 p_start = points_[oldest].location;
  const float2 m_start = points_[oldest].mouse_out;

  /* Use the full (unpressured) brush radius for step sizes so the virtual
   * extension isn't compressed when the initial pen touch has low pressure. */
  const float base_pixel_r = (paint && brush) ? BKE_brush_radius_get(paint, brush) : 50.0f;
  const float r_world = paint_calc_object_space_radius(vc, p_start, base_pixel_r);
  const float r_screen = base_pixel_r;

  /* Stroke points from the start inward, about one dab diameter's worth. */
  const int n_curv = std::min(num_points_, roll_points_per_radius() * 2 + 2);
  Vector<float2> c2d;
  Vector<float3> c3d;
  for (int i = 0; i < n_curv; i++) {
    const int idx = (oldest + i) % cap;
    c2d.append(points_[idx].mouse_out);
    c3d.append(points_[idx].location);
  }

  float2 dir2;
  float3 dir3;
  if (!end_direction(c2d, c3d, true, 0.25f * r_screen, 0.25f * r_world, dir2, dir3)) {
    return;
  }
  float curv_2d, curv_3d;
  estimate_end_curvature(c2d,
                         c3d,
                         true,
                         points_[oldest].surface_normal,
                         2.0f * r_screen,
                         2.0f * r_world,
                         curv_2d,
                         curv_3d);

  const int n_backward = VIRTUAL_EXTENSION_POINTS;
  const float step_3d = (r_world * 1.5f) / float(n_backward);
  const float step_2d = (r_screen * 1.5f) / float(n_backward);

  backward_ext_2d_.clear();
  backward_ext_3d_.clear();
  generate_virtual_extension(m_start,
                             p_start,
                             dir2,
                             dir3,
                             curv_2d,
                             curv_3d,
                             points_[oldest].surface_normal,
                             n_backward,
                             step_2d,
                             step_3d,
                             -1.0f,
                             backward_ext_2d_,
                             backward_ext_3d_);
  initial_backward_ext_count_ = n_backward;
}

/**
 * Smooth the per-vertex normals along the stroke.  Raw face normals on a
 * bumpy surface differ from knot to knot and make the borders zig-zag.
 * The kernel is about one brush radius of knots on each side.
 */
static void smooth_normals_along_stroke(MutableSpan<float3> normals, const int half_width)
{
  const int n = int(normals.size());
  if (n < 3 || half_width < 1) {
    return;
  }
  Vector<float3> src(normals);
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < n; i++) {
      const int lo = std::max(0, i - half_width);
      const int hi = std::min(n - 1, i + half_width);
      float3 sum(0);
      for (int k = lo; k <= hi; k++) {
        sum += src[k];
      }
      normals[i] = (math::length_squared(sum) > 1e-8f) ? math::normalize(sum) : src[i];
    }
    src.as_mutable_span().copy_from(normals);
  }
}

void PaintStroke::make_roll_spline(bContext * /*C*/)
{
  if (num_points_ < 4) {
    return;
  }

  if (!roll_virtual_prepended_) {
    roll_virtual_prepended_ = true;
    prepend_virtual_roll_points();
  }

  /* Collect real knots from ring buffer. */
  constexpr int buf_cap = ROLL_MAX_STROKE_POINTS;

  const int oldest = (cur_point_ - num_points_ + buf_cap) % buf_cap;

  /* Combine backward extension + real knots directly as polyline.
   * No curve evaluation — smoothing happens later on the strip grid
   * (B-spline subdivision + Laplacian in compute_roll_center()). */
  roll_spline_.clear();
  roll_spline_.poly_2d.extend(backward_ext_2d_);
  roll_spline_.poly_3d.extend(backward_ext_3d_);
  /* Extrapolate pressure trend for backward virtual extension.
   * Derive a geometric ratio from the 2nd/3rd real points (skipping the
   * 1st which may have an unreliable initial-touch pressure), then
   * multiply backward so the strip width transitions smoothly. */
  {
    const int n_back = int(backward_ext_2d_.size());
    float base_p = (num_points_ > 0) ? points_[oldest].pressure : 1.0f;
    float ratio = 1.0f;
    if (num_points_ >= 3) {
      const int i1 = (oldest + 1) % buf_cap;
      const int i2 = (oldest + 2) % buf_cap;
      const float p1 = points_[i1].pressure;
      const float p2 = points_[i2].pressure;
      if (p2 > 0.01f) {
        ratio = std::clamp(p1 / p2, PRESSURE_RATIO_MIN, PRESSURE_RATIO_MAX);
      }
    }
    extrapolate_pressures(base_p, ratio, n_back, true, roll_spline_.pressures);
    /* Virtual points reuse the first point's normal. */
    const float3 back_n = (num_points_ > 0) ? points_[oldest].surface_normal : float3(0, 0, 1);
    for (int i = 0; i < n_back; i++) {
      roll_spline_.normals.append(back_n);
    }
  }
  for (int i = 0; i < num_points_; i++) {
    const int idx = (oldest + i) % buf_cap;
    roll_spline_.poly_2d.append(points_[idx].mouse_out);
    roll_spline_.poly_3d.append(points_[idx].location);
    roll_spline_.pressures.append(points_[idx].pressure);
    roll_spline_.normals.append(points_[idx].surface_normal);
  }

  /* Knots are one dab spacing apart (2R * spacing), so a brush radius spans
   * about 0.5 / spacing knots. */
  const int normal_half_width = std::clamp(int(0.5f / std::max(spacing_raw_, 0.05f)), 1, 12);
  smooth_normals_along_stroke(roll_spline_.normals, normal_half_width);

  n_virtual_poly_points_ = int(backward_ext_2d_.size());

  roll_spline_.update_lengths();

  /* Arc length of virtual extension — subtracted from V so V=0 at mouse-down.
   * Computed only once (when the full virtual extension is first available).
   * As virtual points are consumed, stroke_distance_world_ compensates;
   * updating roll_virtual_length_ here would cause double-counting. */
  if (roll_virtual_length_ == 0.0f && n_virtual_poly_points_ > 0 &&
      n_virtual_poly_points_ - 1 < int(roll_spline_.lengths_3d.size()))
  {
    roll_virtual_length_ = roll_spline_.lengths_3d[n_virtual_poly_points_ - 1];
    /* Compute normalized virtual length for pressure-scaled V offset.
     * This is the pressure-weighted equivalent of roll_virtual_length_:
     * sum of seg_len / (R * p_avg) over the virtual extension.
     * It must use the same midpoint-pressure formula as the grid V coords
     * and as stroke_distance_normalized_ in add_roll_point(), so the
     * subtraction in spline_uv() cancels exactly. */
    if (roll_initial_radius_ > 0.0f) {
      const Span<float> pres = roll_spline_.pressures.as_span();
      float accum = 0.0f;
      for (int i = 1; i <= n_virtual_poly_points_ - 1 && i < int(roll_spline_.lengths_3d.size());
           i++)
      {
        const float seg = roll_spline_.segment_length_3d(i);
        const float p = (i < int(pres.size()) && i - 1 >= 0) ?
                            std::max((pres[i] + pres[i - 1]) * 0.5f, 0.01f) :
                            1.0f;
        accum += seg / (roll_initial_radius_ * p);
      }
      roll_virtual_length_normalized_ = accum;
    }
  }
}

void PaintStroke::finish_roll_stroke(bContext *C,
                                     wmOperator *op,
                                     const float2 &mouse_up,
                                     float pressure)
{
  if (!need_roll_mapping_ || num_points_ < 4) {
    return;
  }

  /* Clamp pen lift-off pressure (same minimum as add_roll_point) to avoid
   * a width spike on the last recorded point. */
  pressure = std::max(pressure, MIN_ROLL_PRESSURE);

  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const Brush &brush = *BKE_paint_brush_for_read(this->paint);
  bke::PaintRuntime *paint_runtime = this->paint->runtime;

  /* 1. Skip space_stroke at finish — with a low brush spacing setting and
   *    near-zero pen-lift pressure, it can spawn thousands of expensive
   *    roll dabs (each rebuilds spline, grid, LUT, and paints).  The
   *    mouse-up position is force-recorded directly in step 2 below and
   *    deferred dabs are flushed in step 4, so the stroke still ends at
   *    the correct position — only the per-dab fill between the last
   *    mouse-move and the lift is dropped. */

  /* 2. Force-record the exact mouse-up position as a roll point
   *    (even if it doesn't fall on a spacing boundary). */
  {
    float2 mouse_out = paint_stroke_jitter_pos(
        this->paint, mode, brush, pressure, stroke_mode_, zoom_2d_, mouse_up);
    float3 location;
    bool is_location_is_set;
    update(C, brush, mode, mouse_up, mouse_out, pressure, location, &is_location_is_set);
    const int last_idx = (cur_point_ - 1 + ROLL_MAX_STROKE_POINTS) % ROLL_MAX_STROKE_POINTS;

    if (is_location_is_set) {
      add_roll_point(mouse_up,
                     mouse_out,
                     location,
                     roll_sample_surface_normal(C, mouse_out),
                     paint_runtime->pixel_radius,
                     pressure,
                     pen_flip_,
                     tilt_.x,
                     tilt_.y);
    }
    else if (num_points_ > 0) {
      /* Fallback: use last known location so the spline still extends. */
      add_roll_point(mouse_up,
                     mouse_out,
                     points_[last_idx].location,
                     points_[last_idx].surface_normal,
                     points_[last_idx].size,
                     pressure,
                     pen_flip_,
                     tilt_.x,
                     tilt_.y);
    }
    make_roll_spline(C);
  }
  /* 3. Append a virtual forward extension so the last dab has spline
   *    coverage for the brush half that extends beyond the stroke end.
   *    Uses the same curvature-following logic as prepend_virtual_roll_points()
   *    but in the forward direction, appending directly to the polyline. */
  if (num_points_ >= 2 && !roll_spline_.is_empty()) {
    constexpr int cap = ROLL_MAX_STROKE_POINTS;
    const int newest = (cur_point_ - 1 + cap) % cap;
    const int prev_newest = (newest - 1 + cap) % cap;

    const float3 p_end = points_[newest].location;
    const float2 m_end = points_[newest].mouse_out;

    const float base_pixel_r = paint ? BKE_brush_radius_get(paint, &brush) : 50.0f;
    const float r_world = paint_calc_object_space_radius(vc, p_end, base_pixel_r);
    const float r_screen = base_pixel_r;

    /* Stroke points ending at the newest, about one dab diameter's worth. */
    const int n_curv = std::min(num_points_, roll_points_per_radius() * 2 + 2);
    Vector<float2> c2d;
    Vector<float3> c3d;
    for (int i = n_curv - 1; i >= 0; i--) {
      const int idx = (newest - i + cap) % cap;
      c2d.append(points_[idx].mouse_out);
      c3d.append(points_[idx].location);
    }

    float2 dir2;
    float3 dir3;
    if (end_direction(c2d, c3d, false, 0.25f * r_screen, 0.25f * r_world, dir2, dir3)) {
      float curv_2d, curv_3d;
      estimate_end_curvature(c2d,
                             c3d,
                             false,
                             points_[newest].surface_normal,
                             2.0f * r_screen,
                             2.0f * r_world,
                             curv_2d,
                             curv_3d);

      const int n_forward = VIRTUAL_EXTENSION_POINTS;
      const float step_3d = (r_world * 1.5f) / float(n_forward);
      const float step_2d = (r_screen * 1.5f) / float(n_forward);

      generate_virtual_extension(m_end,
                                 p_end,
                                 dir2,
                                 dir3,
                                 curv_2d,
                                 curv_3d,
                                 points_[newest].surface_normal,
                                 n_forward,
                                 step_2d,
                                 step_3d,
                                 +1.0f,
                                 roll_spline_.poly_2d,
                                 roll_spline_.poly_3d);
      /* Extrapolate pressure trend for forward virtual extension. */
      {
        const float p_new = points_[newest].pressure;
        const float p_prev = points_[prev_newest].pressure;
        float ratio = 1.0f;
        if (p_prev > 0.01f) {
          ratio = std::clamp(p_new / p_prev, PRESSURE_RATIO_MIN, PRESSURE_RATIO_MAX);
        }
        extrapolate_pressures(p_new, ratio, n_forward, false, roll_spline_.pressures);
      }
      /* Virtual points reuse the last point's normal. */
      for (int i = 0; i < n_forward; i++) {
        roll_spline_.normals.append(points_[newest].surface_normal);
      }

      roll_spline_.update_lengths();
    }
  }

  /* 4. Flush deferred dabs forward to the stroke endpoint.
   *
   * During normal painting, dabs lag behind the mouse by `half` points so
   * the spline has enough future context for smooth UV mapping.  At
   * mouse-up we flush all remaining deferred dabs from the last painted
   * position to the newest recorded point.
   *
   * flush_start comes from one of three cases:
   *  (a) last_painted_roll_idx_ exists — continue from there.
   *  (b) Very short stroke (num_points <= half) — start from oldest point.
   *  (c) Normal stroke, no dabs placed yet — start from the deferred
   *      position (cur_point_ - half). */
  constexpr int buf_cap = ROLL_MAX_STROKE_POINTS;
  const int half = roll_half_points();
  const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;

  int flush_start;
  if (last_painted_roll_idx_ >= 0) {
    flush_start = last_painted_roll_idx_;
  }
  else if (num_points_ <= half) {
    flush_start = oldest_idx;
  }
  else {
    flush_start = (cur_point_ - half + buf_cap) % buf_cap;
  }

  /* Advance one step at a time toward (but not including) the newest point.
   * The newest entry is the force-recorded mouse-up position which may sit
   * arbitrarily close to the previous point — including it would create an
   * overlapping dab at the stroke end. */
  const int newest = (cur_point_ - 1 + buf_cap) % buf_cap;
  int idx = (flush_start + 1) % buf_cap;

  /* Bounded by the number of recorded points as a safety net — a stale
   * flush_start must never make the walk loop around the ring buffer. */
  int remaining = num_points_;
  while (idx != newest && remaining-- > 0) {
    PaintStrokePoint *point = &points_[idx];

    PointerRNA itemptr;
    RNA_collection_add(op->ptr, "stroke", &itemptr);
    RNA_float_set(&itemptr, "size", point->size);
    RNA_float_set_array(&itemptr, "location", point->location);
    RNA_float_set_array(&itemptr, "mouse", point->mouse_out);
    RNA_float_set_array(&itemptr, "mouse_event", point->mouse_in);
    RNA_float_set(&itemptr, "pressure", point->pressure);
    RNA_float_set(&itemptr, "x_tilt", point->x_tilt);
    RNA_float_set(&itemptr, "y_tilt", point->y_tilt);

    this->update_step(op, &itemptr);
    RNA_collection_clear(op->ptr, "stroke");

    tot_samples_++;
    idx = (idx + 1) % buf_cap;
  }
}

/**
 * Subdivide the grid along the stroke direction (rows) with cubic B-spline
 * (Lane-Riesenfeld) refinement: inserted rows are midpoints, existing rows
 * are repositioned with the 1/8 vertex rule (prev + 6*self + next) / 8.
 * Repeated passes approach the cubic B-spline limit curve per column,
 * giving smooth borders and center between knots.  First/last rows stay
 * pinned, and runs of identical vertices (self-intersection collapses)
 * are invariant under the vertex rule, so collapse geometry is preserved.
 * Produces (2*rows-1) × cols output.
 */
static void bspline_subdivide_rows(Vector<float3> &grid_pos,
                                   Vector<float2> &grid_uv,
                                   int &rows,
                                   const int cols)
{
  const int oR = rows;
  const int nR = 2 * oR - 1;

  Vector<float3> np(nR * cols);
  Vector<float2> nu(nR * cols);

  for (int c = 0; c < cols; c++) {
    /* Inserted rows: midpoints. */
    for (int r = 0; r < oR - 1; r++) {
      np[(2 * r + 1) * cols + c] = 0.5f * (grid_pos[r * cols + c] + grid_pos[(r + 1) * cols + c]);
      nu[(2 * r + 1) * cols + c] = 0.5f * (grid_uv[r * cols + c] + grid_uv[(r + 1) * cols + c]);
    }
    /* Existing rows: 1/8 vertex rule, ends pinned. */
    np[c] = grid_pos[c];
    nu[c] = grid_uv[c];
    np[(nR - 1) * cols + c] = grid_pos[(oR - 1) * cols + c];
    nu[(nR - 1) * cols + c] = grid_uv[(oR - 1) * cols + c];
    for (int r = 1; r < oR - 1; r++) {
      np[(2 * r) * cols + c] = (1.0f / 8.0f) *
                               (grid_pos[(r - 1) * cols + c] + 6.0f * grid_pos[r * cols + c] +
                                grid_pos[(r + 1) * cols + c]);
      nu[(2 * r) * cols + c] = (1.0f / 8.0f) *
                               (grid_uv[(r - 1) * cols + c] + 6.0f * grid_uv[r * cols + c] +
                                grid_uv[(r + 1) * cols + c]);
    }
  }

  grid_pos = std::move(np);
  grid_uv = std::move(nu);
  rows = nR;
}

/**
 * Expand the strip across the stroke by linear interpolation between
 * adjacent columns: cols → 4*cols-3 (3 border/center/border → 9).
 * U coordinates and cross-stroke positions vary linearly across the strip;
 * the Laplacian pass afterwards bows the interior near collapse points.
 */
static void expand_grid_columns(Vector<float3> &grid_pos,
                                Vector<float2> &grid_uv,
                                const int rows,
                                int &cols)
{
  const int oC = cols;
  const int nC = 4 * oC - 3;

  Vector<float3> np(rows * nC);
  Vector<float2> nu(rows * nC);

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < nC; c++) {
      const float fc = float(c) * float(oC - 1) / float(nC - 1);
      const int i = std::min(int(fc), oC - 2);
      const float s = fc - float(i);
      np[r * nC + c] = math::interpolate(grid_pos[r * oC + i], grid_pos[r * oC + i + 1], s);
      nu[r * nC + c] = math::interpolate(grid_uv[r * oC + i], grid_uv[r * oC + i + 1], s);
    }
  }

  grid_pos = std::move(np);
  grid_uv = std::move(nu);
  cols = nC;
}

/* -------------------------------------------------------------------- */
/** \name Roll Strip Surface — per-dab pipeline stages
 *
 * compute_roll_center() runs these stages in order on a contiguous window
 * of the stroke polyline (currently the full polyline):
 *
 *   1. build_roll_borders(): left/right border curves at +-radius along
 *      per-vertex binormals.
 *   2. collapse_border_self_intersections(): collapse inner-border loops
 *      at sharp turns so the strip never overlaps itself.
 *   3. build_strip_grid(): 3-column grid (right border, center, left
 *      border) with per-row V texture coordinates.
 *   4. bspline_subdivide_rows() x2 + expand_grid_columns(): refine to a
 *      smooth (4N-3) x 9 grid.
 *   5. laplacian_smooth_grid(): bow the interior near collapse points.
 *   6. compute_lut_eval_rows() + rasterize_lut(): project the grid to 2D
 *      and rasterize the dab neighborhood into the UV lookup table that
 *      spline_uv() samples per vertex.
 * \{ */

/**
 * Left/right border curves: the center offset by ±R (or the pressure-scaled
 * radius) along each vertex's binormal.  Pass empty \a pressures for a
 * constant width.
 */
static void build_roll_borders(const Span<float3> poly,
                               const Span<float3> tangents,
                               const Span<float3> normals,
                               const Span<float> pressures,
                               const float R,
                               Vector<float3> &r_border_left,
                               Vector<float3> &r_border_right)
{
  const int count = int(poly.size());
  r_border_left.reinitialize(count);
  r_border_right.reinitialize(count);

  for (int k = 0; k < count; k++) {
    const float3 &T = tangents[k];
    /* Binormal in the vertex's own tangent plane. */
    float3 B = math::cross(T, normals[k]);
    const float blen = math::length(B);
    if (blen > 1e-7f) {
      B /= blen;
    }
    else {
      /* Degenerate: tangent parallel to the surface normal. */
      B = math::normalize(math::cross(T, float3(0, 0, 1)));
      if (math::length_squared(B) < 1e-12f) {
        B = math::normalize(math::cross(T, float3(1, 0, 0)));
      }
    }
    const float Rk = (!pressures.is_empty()) ? std::max(R * pressures[k], R * 0.01f) : R;
    r_border_left[k] = poly[k] + B * Rk;
    r_border_right[k] = poly[k] - B * Rk;
  }
}

/**
 * Collapse inner-border loops at turns tighter than the brush radius: where
 * the border crosses itself, the loop vertices are pinned to the crossing
 * point so the quads fan around it instead of overlapping.
 */
static void collapse_border_self_intersections(const Span<float3> poly,
                                               const Span<float3> normals,
                                               const float R,
                                               Vector<float3> &border)
{
  const int count = int(border.size());
  if (count < 4) {
    return;
  }

  Vector<float2> b2d(count);
  Vector<float3> orig(border);
  int skip_until = -1;

  for (int i = 0; i < count - 1; i++) {
    if (i <= skip_until) {
      continue;
    }

    /* 2D frame from the surface normal at `i`, so the crossing test does
     * not depend on the current dab. */
    float3 mx = math::cross(normals[i], float3(0, 0, 1));
    if (math::length_squared(mx) < 1e-6f) {
      mx = math::cross(normals[i], float3(1, 0, 0));
    }
    mx = math::normalize(mx);
    const float3 my = math::normalize(math::cross(normals[i], mx));
    for (int k = i; k < count; k++) {
      b2d[k] = float2(math::dot(orig[k], mx), math::dot(orig[k], my));
    }

    const float2 d1 = b2d[i + 1] - b2d[i];
    if (math::dot(d1, d1) < 1e-8f) {
      continue;
    }

    /* Nearest crossing first: points never move, so a partner found once
     * stays the same on later dabs. */
    const int j_max = count - 2;
    for (int j = i + 2; j <= j_max; j++) {
      const float2 d2 = b2d[j + 1] - b2d[j];
      if (math::dot(d2, d2) < 1e-8f) {
        continue;
      }

      const float2 ac = b2d[j] - b2d[i];
      const float denom = d1.x * d2.y - d1.y * d2.x;
      if (std::abs(denom) < 1e-8f) {
        continue;
      }
      const float s = (ac.x * d2.y - ac.y * d2.x) / denom;
      const float t = (ac.x * d1.y - ac.y * d1.x) / denom;

      if (s >= 0.0f && s <= 1.0f && t >= 0.0f && t <= 1.0f) {
        /* Merge point from the segment intersection. */
        const float3 pa = orig[i] + (orig[i + 1] - orig[i]) * s;
        const float3 pb = orig[j] + (orig[j + 1] - orig[j]) * t;
        const float3 cross_pt = (pa + pb) * 0.5f;

        /* Only collapse a real fold: walking both arms inward from the
         * crossing, they must stay within strip overlap (2R) until they
         * meet.  A stroke that leaves and comes back over itself (spiral,
         * figure 8) merely overlaps — nothing to merge. */
        {
          const float reach_sq = (R * 2.5f) * (R * 2.5f);
          int a = i + 1, b = j;
          float arc_a = 0.0f, arc_b = 0.0f;
          bool continuous = true;
          while (a < b) {
            if (math::distance_squared(poly[a], poly[b]) > reach_sq) {
              continuous = false;
              break;
            }
            /* Advance whichever arm has covered less arc. */
            if (arc_a <= arc_b) {
              arc_a += math::distance(poly[a], poly[a + 1]);
              a++;
            }
            else {
              arc_b += math::distance(poly[b], poly[b - 1]);
              b--;
            }
          }
          if (!continuous) {
            continue;
          }
        }

        bool all_inside = true;
        const int m_lo = std::max(0, i - 2);
        const int m_hi = std::min(count - 2, j + 2);
        const float R_sq = (R * 1.2f) * (R * 1.2f); /* 20% margin for inclined borders */
        /* Validate all loop vertices are within R of center.
         * Also find the apex = vertex with sharpest curvature on the
         * border (largest turning angle between consecutive segments). */
        int apex_idx = (i + 1 + j) / 2; /* default to midpoint */
        float max_turn_angle = -1.0f;
        for (int k = i + 1; k <= j; k++) {
          float min_dist_sq = FLT_MAX;
          for (int m = m_lo; m <= m_hi; m++) {
            const float3 ab = poly[m + 1] - poly[m];
            const float ab_dot = math::dot(ab, ab);
            const float tp = (ab_dot > 1e-12f) ?
                                 std::clamp(
                                     math::dot(orig[k] - poly[m], ab) / ab_dot, 0.0f, 1.0f) :
                                 0.0f;
            const float3 proj = math::interpolate(poly[m], poly[m + 1], tp);
            min_dist_sq = std::min(min_dist_sq, math::distance_squared(orig[k], proj));
          }
          if (min_dist_sq >= R_sq) {
            all_inside = false;
            break;
          }
          /* Curvature: angle between incoming and outgoing border segments
           * at vertex k (in 2D projection).  Higher = sharper turn. */
          if (k > i + 1 && k < j) {
            const float2 seg_prev = b2d[k] - b2d[k - 1];
            const float2 seg_next = b2d[k + 1] - b2d[k];
            const float lp = math::length(seg_prev);
            const float ln = math::length(seg_next);
            if (lp > 1e-7f && ln > 1e-7f) {
              const float cos_a = math::dot(seg_prev, seg_next) / (lp * ln);
              const float turn = 1.0f - std::clamp(cos_a, -1.0f, 1.0f);
              if (turn > max_turn_angle) {
                max_turn_angle = turn;
                apex_idx = k;
              }
            }
          }
        }
        if (!all_inside) {
          continue;
        }

        /* If the span is too long, only collapse the sharpest portion
         * (centered on the apex vertex).  This prevents long gentle
         * turns from collapsing the entire inner border.  The merge
         * point for partial collapses is the midpoint of the first
         * and last border vertices in the trimmed range. */
        int collapse_lo = i + 1;
        int collapse_hi = j;
        float3 merge_pt = cross_pt;

        /* If the intersection loop arc length exceeds 3R along the
         * center polyline, only collapse the sharpest portion centered
         * on the apex.  Remaining overlap vertices are linearly
         * interpolated between the intersection endpoints and the
         * merge point. */
        const float max_collapse_arc = 3.0f * R;
        float span_arc = 0.0f;
        for (int k = i; k < j; k++) {
          span_arc += math::distance(poly[k], poly[k + 1]);
        }
        const bool is_partial = span_arc > max_collapse_arc && (j - i) > 3;
        if (is_partial) {
          /* Center the window on the apex using arc-length balance:
           * grow outward from the apex in both directions, spending
           * half the budget on each side. */
          const float half_arc = max_collapse_arc * 0.5f;
          collapse_lo = apex_idx;
          collapse_hi = apex_idx;
          float arc_lo = 0.0f, arc_hi = 0.0f;
          while (collapse_lo > i + 1 || collapse_hi < j) {
            const bool can_lo = (collapse_lo > i + 1) && (arc_lo <= arc_hi);
            const bool can_hi = (collapse_hi < j) && (arc_hi <= arc_lo);
            if (can_lo) {
              const float seg = math::distance(poly[collapse_lo - 1], poly[collapse_lo]);
              if (arc_lo + seg > half_arc) {
                break;
              }
              collapse_lo--;
              arc_lo += seg;
            }
            else if (can_hi) {
              const float seg = math::distance(poly[collapse_hi], poly[collapse_hi + 1]);
              if (arc_hi + seg > half_arc) {
                break;
              }
              collapse_hi++;
              arc_hi += seg;
            }
            else {
              break;
            }
          }
          merge_pt = (orig[collapse_lo] + orig[collapse_hi]) * 0.5f;
        }

        for (int k = collapse_lo; k <= collapse_hi; k++) {
          border[k] = merge_pt;
        }
        if (is_partial) {
          /* Linearly interpolate border vertices between the real
           * intersection endpoints and the merge point.  This evenly
           * spaces the overlap zone instead of leaving it overlapping. */
          const int n_before = collapse_lo - (i + 1);
          for (int k = 0; k < n_before; k++) {
            const float t = float(k + 1) / float(n_before + 1);
            border[i + 1 + k] = math::interpolate(orig[i + 1], merge_pt, t);
          }
          const int n_after = j - collapse_hi;
          for (int k = 0; k < n_after; k++) {
            const float t = float(k + 1) / float(n_after + 1);
            border[collapse_hi + 1 + k] = math::interpolate(merge_pt, orig[j], t);
          }
        }
        /* Skip the entire original intersection span so the uncollapsed
         * outer portions are not re-tested by later iterations. */
        skip_until = j;
        break;
      }
    }
  }
}

/**
 * Initial 3-column grid (right border, center, left border) with V per row.
 * V is the arc length, or with \a normalize_v the arc length divided by the
 * local strip width, so the texture keeps its aspect ratio under pressure.
 * The continuity offsets in add_roll_point()/spline_uv() use the same
 * per-segment formula.
 * \param v_start: V of the first row.
 */
static void build_strip_grid(const Span<float3> poly,
                             const Span<float3> border_left,
                             const Span<float3> border_right,
                             const Span<float> pressures,
                             const float R,
                             const float v_start,
                             const bool normalize_v,
                             Vector<float3> &r_grid_pos,
                             Vector<float2> &r_grid_uv)
{
  const int rows = int(poly.size());
  r_grid_pos.reinitialize(rows * 3);
  r_grid_uv.reinitialize(rows * 3);

  Vector<float> v_coords(rows);
  v_coords[0] = v_start;
  for (int k = 1; k < rows; k++) {
    const float seg_len = math::distance(poly[k - 1], poly[k]);
    if (normalize_v && !pressures.is_empty()) {
      const float p_avg = std::max((pressures[k] + pressures[k - 1]) * 0.5f, 0.01f);
      v_coords[k] = v_coords[k - 1] + seg_len / (R * p_avg);
    }
    else {
      v_coords[k] = v_coords[k - 1] + seg_len;
    }
  }

  for (int k = 0; k < rows; k++) {
    /* Col 0 = right border, col 1 = center, col 2 = left border. */
    r_grid_pos[k * 3 + 0] = border_right[k];
    r_grid_pos[k * 3 + 1] = poly[k];
    r_grid_pos[k * 3 + 2] = border_left[k];
    /* ±1 when the texture scales with the strip, else ±(width / R). */
    const float u_border = (normalize_v || pressures.is_empty()) ? 1.0f :
                                                                   std::max(pressures[k], 0.01f);
    r_grid_uv[k * 3 + 0] = float2(-u_border, v_coords[k]);
    r_grid_uv[k * 3 + 1] = float2(0.0f, v_coords[k]);
    r_grid_uv[k * 3 + 2] = float2(u_border, v_coords[k]);
  }
}

/**
 * 2D Laplacian smoothing: pin border columns and first/last rows, smooth
 * everything between them (including the center column).  This curves the
 * cross-stroke segments near collapse points — the perpendicular lines
 * bow toward the merge point like a subdivision surface.
 * Uses double-buffering to avoid per-iteration allocation.
 */
static void laplacian_smooth_grid(Vector<float3> &grid_pos,
                                  Vector<float2> &grid_uv,
                                  const int rows,
                                  const int cols)
{
  constexpr int smooth_iters = 10;
  constexpr float mix = 0.5f;
  const int grid_total = rows * cols;
  Vector<float3> buf_p(grid_total);
  Vector<float2> buf_u(grid_total);
  float3 *src_p = grid_pos.data(), *dst_p = buf_p.data();
  float2 *src_u = grid_uv.data(), *dst_u = buf_u.data();
  for (int iter = 0; iter < smooth_iters; iter++) {
    memcpy(dst_p, src_p, sizeof(float3) * grid_total);
    memcpy(dst_u, src_u, sizeof(float2) * grid_total);
    for (int r = 1; r < rows - 1; r++) {
      for (int c = 1; c < cols - 1; c++) {
        const int idx = r * cols + c;
        float3 avg_p = 0.25f * (src_p[(r - 1) * cols + c] + src_p[(r + 1) * cols + c] +
                                src_p[r * cols + c - 1] + src_p[r * cols + c + 1]);
        float2 avg_u = 0.25f * (src_u[(r - 1) * cols + c] + src_u[(r + 1) * cols + c] +
                                src_u[r * cols + c - 1] + src_u[r * cols + c + 1]);
        dst_p[idx] = (1.0f - mix) * src_p[idx] + mix * avg_p;
        dst_u[idx] = (1.0f - mix) * src_u[idx] + mix * avg_u;
      }
    }
    std::swap(src_p, dst_p);
    std::swap(src_u, dst_u);
  }
  /* Ensure results end up in grid_pos/grid_uv. */
  if (src_p != grid_pos.data()) {
    memcpy(grid_pos.data(), src_p, sizeof(float3) * grid_total);
    memcpy(grid_uv.data(), src_u, sizeof(float2) * grid_total);
  }
}

/**
 * Compute the grid row range to rasterize into the LUT: intersect an
 * arc-length window around the dab with a spatial distance check.  The
 * arc-length constraint prevents old segments on the far side of a
 * self-crossing stroke from polluting the LUT.  The spatial check ensures
 * coverage on sharp curves where arc-length alone might be too tight.
 * Forward range is more generous to include the trailing segment and its
 * self-intersection merges.
 *
 * After two row-subdivision passes, polyline vertex `vi` maps to grid row
 * `(vi - win_lo) * 4`.
 *
 * \param lengths: Cumulative arc lengths of the FULL polyline.
 * \param win_lo: First polyline vertex covered by the grid.
 */
static void compute_lut_eval_rows(const Span<float2> grid_pos_2d,
                                  const int rows,
                                  const int cols,
                                  const Span<float> lengths,
                                  const float raw_s,
                                  const float R,
                                  const int win_lo,
                                  const float2 &dab_2d,
                                  int &r_row_lo,
                                  int &r_row_hi)
{
  const float eval_s_lo = std::max(0.0f, raw_s - R * 2.0f);
  const float eval_s_hi = std::min(lengths.last(), raw_s + R * 3.0f);

  int eval_seg_lo, eval_seg_hi;
  float fac_unused;
  length_parameterize::sample_at_length(lengths, eval_s_lo, eval_seg_lo, fac_unused);
  length_parameterize::sample_at_length(lengths, eval_s_hi, eval_seg_hi, fac_unused);

  const int arc_row_lo = std::max(0, (eval_seg_lo - win_lo) * 4 - EVAL_ROW_MARGIN);
  const int arc_row_hi = std::min(rows - 1, (eval_seg_hi - win_lo + 1) * 4 + EVAL_ROW_MARGIN);

  /* Within the arc-length window, keep rows spatially near the dab. */
  const float eval_r_sq = (R * 2.0f) * (R * 2.0f);
  const int center_c = cols / 2;

  int eval_lo = rows - 1, eval_hi = 0;
  for (int r = arc_row_lo; r <= arc_row_hi; r++) {
    if (math::distance_squared(dab_2d, grid_pos_2d[r * cols + center_c]) <= eval_r_sq) {
      eval_lo = std::min(eval_lo, r);
      eval_hi = std::max(eval_hi, r);
    }
  }
  r_row_lo = std::max(0, eval_lo - EVAL_ROW_MARGIN);
  r_row_hi = std::min(rows - 2, eval_hi + EVAL_ROW_MARGIN);
}

/**
 * Rasterize the eval-range grid quads into the UV lookup table so
 * per-vertex UV lookup is O(1) instead of O(rows x cols).  On high-poly
 * meshes this is the dominant cost — the LUT amortizes the bilinear
 * inverse across a fixed-size image.
 *
 * For each quad (P00, P10, P01, P11), and for each pixel in its bounding
 * box, we solve: "given a 2D point Q, find (u,v) such that
 * Q = (1-u)(1-v)P00 + u(1-v)P10 + (1-u)v*P01 + uv*P11".
 *
 * Rearranging gives a quadratic in v (A*v^2 + B*v + C = 0) using
 * cross-products of the quad's edge vectors.  Once v is found, u follows
 * from a linear solve.  If (u,v) is inside [0,1]^2 the pixel is inside
 * the quad, and we bilinearly interpolate the UV + tangent.
 *
 * When multiple quads overlap (at self-intersection collapses), the quad
 * whose bilinear solution is closest to the pixel center wins
 * (roll_lut_dist_sq comparison).
 */
static void rasterize_lut(const Span<float3> grid_pos,
                          const Span<float2> grid_pos_2d,
                          const Span<float2> grid_uv,
                          const int rows,
                          const int cols,
                          const int eval_row_lo,
                          const int eval_row_hi,
                          StrokeCache &cache)
{
  constexpr int RES = StrokeCache::ROLL_LUT_RES;

  /* 2D bounding box of eval-range grid. */
  float2 bb_min(FLT_MAX), bb_max(-FLT_MAX);
  for (int r = eval_row_lo; r <= std::min(eval_row_hi + 1, rows - 1); r++) {
    for (int c = 0; c < cols; c++) {
      bb_min = math::min(bb_min, grid_pos_2d[r * cols + c]);
      bb_max = math::max(bb_max, grid_pos_2d[r * cols + c]);
    }
  }
  const float2 extent = bb_max - bb_min;
  const float2 margin = extent * 0.1f;
  bb_min -= margin;
  bb_max += margin;
  const float2 ext = bb_max - bb_min;
  const float2 inv_ext(ext.x > 1e-10f ? float(RES) / ext.x : 0.0f,
                       ext.y > 1e-10f ? float(RES) / ext.y : 0.0f);

  const int lut_total = RES * RES;
  cache.roll_lut_uv.reinitialize(lut_total);
  cache.roll_lut_dist_sq.reinitialize(lut_total);
  cache.roll_lut_tan.reinitialize(lut_total);
  cache.roll_lut_row.reinitialize(lut_total);
  for (int i = 0; i < lut_total; i++) {
    cache.roll_lut_uv[i] = float2(FLT_MAX, 0.0f);
    cache.roll_lut_dist_sq[i] = FLT_MAX;
    cache.roll_lut_row[i] = -1;
  }

  auto cross2d = [](float2 a, float2 b) { return a.x * b.y - a.y * b.x; };

  for (int r = eval_row_lo; r <= eval_row_hi; r++) {
    for (int c = 0; c < cols - 1; c++) {
      const float2 P00 = grid_pos_2d[r * cols + c];
      const float2 P10 = grid_pos_2d[r * cols + c + 1];
      const float2 P01 = grid_pos_2d[(r + 1) * cols + c];
      const float2 P11 = grid_pos_2d[(r + 1) * cols + c + 1];

      /* Pixel bounding box of this quad. */
      float2 qmin = math::min(math::min(P00, P10), math::min(P01, P11));
      float2 qmax = math::max(math::max(P00, P10), math::max(P01, P11));
      int px_lo = std::max(0, int((qmin.x - bb_min.x) * inv_ext.x));
      int px_hi = std::min(RES - 1, int((qmax.x - bb_min.x) * inv_ext.x) + 1);
      int py_lo = std::max(0, int((qmin.y - bb_min.y) * inv_ext.y));
      int py_hi = std::min(RES - 1, int((qmax.y - bb_min.y) * inv_ext.y) + 1);

      /* Bilinear basis vectors for the quadratic solve. */
      const float2 a = P10 - P00;
      const float2 b = P01 - P00;
      const float2 tw = P11 - P10 - P01 + P00; /* twist term */
      const float A_coeff = cross2d(b, tw);

      for (int py = py_lo; py <= py_hi; py++) {
        for (int px = px_lo; px <= px_hi; px++) {
          const float2 query = bb_min + float2(px + 0.5f, py + 0.5f) / float(RES) * ext;
          const float2 d = query - P00;

          const float B_coeff = cross2d(b, a) - cross2d(d, tw);
          const float C_coeff = -cross2d(d, a);

          float v;
          if (std::abs(A_coeff) < 1e-8f) {
            v = (std::abs(B_coeff) > 1e-8f) ? -C_coeff / B_coeff : 0.5f;
          }
          else {
            const float disc = B_coeff * B_coeff - 4.0f * A_coeff * C_coeff;
            if (disc < 0.0f) {
              continue;
            }
            const float sq = sqrtf(disc);
            const float v1 = (-B_coeff + sq) / (2.0f * A_coeff);
            const float v2 = (-B_coeff - sq) / (2.0f * A_coeff);
            const float e1 = std::max(0.0f, std::max(-v1, v1 - 1.0f));
            const float e2 = std::max(0.0f, std::max(-v2, v2 - 1.0f));
            v = (e1 <= e2) ? v1 : v2;
          }
          if (v < -0.01f || v > 1.01f) {
            continue;
          }
          v = std::clamp(v, 0.0f, 1.0f);

          const float2 dv = a + v * tw;
          float u;
          if (std::abs(dv.x) > std::abs(dv.y)) {
            u = (std::abs(dv.x) > 1e-8f) ? (d.x - v * b.x) / dv.x : 0.5f;
          }
          else {
            u = (std::abs(dv.y) > 1e-8f) ? (d.y - v * b.y) / dv.y : 0.5f;
          }
          if (u < -0.01f || u > 1.01f) {
            continue;
          }
          u = std::clamp(u, 0.0f, 1.0f);

          const float2 pnt = (1 - u) * (1 - v) * P00 + u * (1 - v) * P10 + (1 - u) * v * P01 +
                             u * v * P11;
          const float dsq = math::distance_squared(query, pnt);

          const int li = py * RES + px;
          /* Prefer older rows on overlap to keep the LUT stable.
           *
           * The loop iterates rows low-to-high, so a non-empty pixel was
           * written by a row <= r.  Cases:
           *   - empty: write
           *   - same/adjacent row (within 2): use dsq (better fit wins)
           *   - far row (existing < r - 2): keep older (don't write)
           * This way overlapping branches at sharp turns get a stable
           * single stroke position rather than flickering between
           * competing branches based on tiny float distances. */
          const int existing_row = cache.roll_lut_row[li];
          bool write = false;
          if (existing_row < 0) {
            write = true;
          }
          else if (r - existing_row <= 2) {
            write = (dsq < cache.roll_lut_dist_sq[li]);
          }
          if (write) {
            cache.roll_lut_dist_sq[li] = dsq;
            cache.roll_lut_row[li] = r;
            /* Interpolate UV. */
            const float2 &UV00 = grid_uv[r * cols + c];
            const float2 &UV10 = grid_uv[r * cols + c + 1];
            const float2 &UV01 = grid_uv[(r + 1) * cols + c];
            const float2 &UV11 = grid_uv[(r + 1) * cols + c + 1];
            cache.roll_lut_uv[li] = (1 - u) * (1 - v) * UV00 + u * (1 - v) * UV10 +
                                    (1 - u) * v * UV01 + u * v * UV11;
            /* Interpolate tangent from 3D grid. */
            const float3 &Q00 = grid_pos[r * cols + c];
            const float3 &Q10 = grid_pos[r * cols + c + 1];
            const float3 &Q01 = grid_pos[(r + 1) * cols + c];
            const float3 &Q11 = grid_pos[(r + 1) * cols + c + 1];
            const float3 dPdv = (1 - u) * (Q01 - Q00) + u * (Q11 - Q10);
            cache.roll_lut_tan[li] = math::normalize(dPdv);
          }
        }
      }
    }
  }

  cache.roll_lut_min = bb_min;
  cache.roll_lut_inv_extent = inv_ext;
  cache.roll_lut_ready = true;
}

/** \} */

void PaintStroke::compute_roll_center(StrokeCache &cache)
{
  if (roll_spline_.is_empty()) {
    return;
  }
  /* Arc length of the dab center on the spline — anchors the LUT
   * evaluation window.  Full closest-point search, called once per dab on
   * the main thread. */
  const float raw_s = roll_spline_.closest_point_arc_length(cache.location);
  const Span<float> lengths = roll_spline_.lengths_3d.as_span();
  const Span<float3> poly_full = roll_spline_.poly_3d.as_span();
  const float R = cache.initial_radius;

  /* Capture initial_radius for normalized distance tracking. */
  if (roll_initial_radius_ == 0.0f && R > 0.0f) {
    roll_initial_radius_ = R;
  }

  /* Fallback normal, frozen on the first dab (sculpt or view normal),
   * for the rare dabs where sculpt_normal is degenerate. */
  if (math::length_squared(roll_proj_normal_) < 1e-8f) {
    roll_proj_normal_ = (math::length_squared(cache.sculpt_normal) > 1e-8f) ?
                            math::normalize(cache.sculpt_normal) :
                            math::normalize(cache.view_normal);
  }

  /* Per-vertex normals frozen at recording time.  Everything below derives
   * from them, so recorded segments never change between dabs. */
  const Span<float3> normals_full = roll_spline_.normals.as_span();
  BLI_assert(normals_full.size() == poly_full.size());

  /* Normal at the dab's position on the stroke, for the LUT projection. */
  float3 dab_normal = roll_proj_normal_;
  {
    int seg;
    float fac;
    length_parameterize::sample_at_length(lengths, raw_s, seg, fac);
    const float3 n = math::interpolate(normals_full[seg], normals_full[seg + 1], fac);
    if (math::length_squared(n) > 1e-8f) {
      dab_normal = math::normalize(n);
    }
  }

  /* The strip width follows the pressure-scaled dab radius; the Pressure
   * Scale option only decides whether the texture shrinks with it. */
  const bool use_pressure_width = brush && BKE_brush_use_size_pressure(brush);
  const bool normalize_v = roll_texture_pressure_scaled();
  const Span<float> pressures_full = (use_pressure_width &&
                                      roll_spline_.pressures.size() >= poly_full.size()) ?
                                         roll_spline_.pressures.as_span() :
                                         Span<float>();

  /* The grid covers the entire recorded polyline so that self-intersection
   * collapse anywhere on the stroke is detected; only the LUT rasterization
   * is restricted to the dab neighborhood (via the eval row range). */
  const int win_lo = 0;
  const int win_count = int(poly_full.size());
  const float v_start = 0.0f;

  const Span<float3> poly = poly_full.slice(win_lo, win_count);
  const Span<float3> tangents = roll_spline_.tangents_3d.as_span().slice(win_lo, win_count);
  const Span<float3> normals = normals_full.slice(win_lo, win_count);
  const Span<float> pressures = pressures_full.is_empty() ?
                                    pressures_full :
                                    pressures_full.slice(win_lo, win_count);

  cache.roll_surface_ready = false;

  /* 1-2. Border curves, then collapse their self-intersection loops. */
  build_roll_borders(
      poly, tangents, normals, pressures, R, cache.roll_border_left, cache.roll_border_right);

  collapse_border_self_intersections(poly, normals, R, cache.roll_border_left);
  collapse_border_self_intersections(poly, normals, R, cache.roll_border_right);

  /* 3. Initial 3-column grid with V coordinates. */
  Vector<float3> grid_pos;
  Vector<float2> grid_uv;
  build_strip_grid(poly,
                   cache.roll_border_left,
                   cache.roll_border_right,
                   pressures,
                   R,
                   v_start,
                   normalize_v,
                   grid_pos,
                   grid_uv);

  /* 4. Refine the strip: two cubic B-spline subdivision passes along the
   * stroke, then linear expansion across it.  N rows x 3 cols ->
   * (4N-3) rows x 9 cols.
   *
   * The B-spline vertex rule curves borders and center between knots —
   * along-stroke smoothing the Laplacian pass below cannot provide, since
   * it pins the border columns.  Across the strip only linear
   * interpolation is needed (U must map linearly to ±1 anyway). */
  int cur_rows = win_count;
  int cur_cols = 3;
  bspline_subdivide_rows(grid_pos, grid_uv, cur_rows, cur_cols);
  bspline_subdivide_rows(grid_pos, grid_uv, cur_rows, cur_cols);
  expand_grid_columns(grid_pos, grid_uv, cur_rows, cur_cols);

  /* 5. Bow the interior near collapse points. */
  laplacian_smooth_grid(grid_pos, grid_uv, cur_rows, cur_cols);

  /* 6. Project to 2D and rasterize the dab neighborhood into the LUT.
   * The projection uses the per-dab sculpt normal — the direction the
   * brush "stamps" from. */
  float3 view_x = math::cross(dab_normal, float3(0, 0, 1));
  if (math::length_squared(view_x) < 1e-6f) {
    view_x = math::cross(dab_normal, float3(1, 0, 0));
  }
  view_x = math::normalize(view_x);
  const float3 view_y = math::normalize(math::cross(dab_normal, view_x));

  const int total = cur_rows * cur_cols;
  Vector<float2> grid_pos_2d(total);
  for (int i = 0; i < total; i++) {
    grid_pos_2d[i] = float2(math::dot(grid_pos[i], view_x), math::dot(grid_pos[i], view_y));
  }

  const float2 dab_2d = float2(math::dot(cache.location, view_x),
                               math::dot(cache.location, view_y));
  compute_lut_eval_rows(grid_pos_2d,
                        cur_rows,
                        cur_cols,
                        lengths,
                        raw_s,
                        R,
                        win_lo,
                        dab_2d,
                        cache.roll_eval_row_lo,
                        cache.roll_eval_row_hi);

  cache.roll_subdiv_rows = cur_rows;
  cache.roll_subdiv_cols = cur_cols;

  rasterize_lut(grid_pos,
                grid_pos_2d,
                grid_uv,
                cur_rows,
                cur_cols,
                cache.roll_eval_row_lo,
                cache.roll_eval_row_hi,
                cache);

  /* 3D grid kept only for the debug overlay. */
  cache.roll_subdiv_pos = std::move(grid_pos);
  cache.roll_view_x = view_x;
  cache.roll_view_y = view_y;

  cache.roll_surface_ready = true;
}

void PaintStroke::spline_uv(const StrokeCache &cache,
                            const float co[3],
                            float r_out[3],
                            float r_tan[3]) const
{
  /* LUT-based UV lookup (O(1) per vertex).
   *
   * The grid's bilinear inverse is pre-rasterized into a 2D lookup table
   * once per dab.  Per-vertex evaluation is a simple 2D projection +
   * bilinear sample of the LUT — no per-quad search needed. */
  if (UNLIKELY(!cache.roll_lut_ready)) {
    r_out[0] = FLT_MAX;
    r_out[1] = r_out[2] = 0.0f;
    zero_v3(r_tan);
    return;
  }

  constexpr int RES = StrokeCache::ROLL_LUT_RES;
  const float2 query = float2(math::dot(float3(co), cache.roll_view_x),
                              math::dot(float3(co), cache.roll_view_y));

  const float2 fc = (query - cache.roll_lut_min) * cache.roll_lut_inv_extent;
  /* If the query is outside the LUT bounding box (or NaN), return a large
   * distance so this result loses the mirror-symmetry comparison.  Without
   * this, edge-clamped LUT values at mirrored positions can produce bogus
   * near-zero U that incorrectly wins. */
  if (UNLIKELY(!std::isfinite(fc.x) || !std::isfinite(fc.y) || fc.x < -0.5f ||
               fc.x > float(RES) - 0.5f || fc.y < -0.5f || fc.y > float(RES) - 0.5f))
  {
    r_out[0] = FLT_MAX;
    r_out[1] = r_out[2] = 0.0f;
    zero_v3(r_tan);
    return;
  }

  const float fx = std::clamp(fc.x - 0.5f, 0.0f, float(RES - 2));
  const float fy = std::clamp(fc.y - 0.5f, 0.0f, float(RES - 2));
  const int ix = std::min(int(fx), RES - 2);
  const int iy = std::min(int(fy), RES - 2);
  const float tx = fx - float(ix);
  const float ty = fy - float(iy);

  /* Bilinear interpolation of UV from 4 nearest LUT cells. */
  float2 uv00 = cache.roll_lut_uv[iy * RES + ix];
  float2 uv10 = cache.roll_lut_uv[iy * RES + ix + 1];
  float2 uv01 = cache.roll_lut_uv[(iy + 1) * RES + ix];
  float2 uv11 = cache.roll_lut_uv[(iy + 1) * RES + ix + 1];
  float3 t00 = cache.roll_lut_tan[iy * RES + ix];
  float3 t10 = cache.roll_lut_tan[iy * RES + ix + 1];
  float3 t01 = cache.roll_lut_tan[(iy + 1) * RES + ix];
  float3 t11 = cache.roll_lut_tan[(iy + 1) * RES + ix + 1];

  /* If any of the 4 LUT pixels was never rasterized (no grid quad covered
   * it), fill it from the nearest valid neighbor among the other 3 so the
   * bilinear sample produces a sensible UV instead of a V-near-zero value
   * that lands on tile boundaries under texture repeat. */
  constexpr float INVALID = FLT_MAX * 0.5f;
  const bool b00 = uv00.x >= INVALID;
  const bool b10 = uv10.x >= INVALID;
  const bool b01 = uv01.x >= INVALID;
  const bool b11 = uv11.x >= INVALID;
  if (UNLIKELY(b00 || b10 || b01 || b11)) {
    /* If ALL four are invalid, fall back to FLT_MAX so the symmetry
     * comparison in sculpt_apply_texture discards this sample. */
    if (b00 && b10 && b01 && b11) {
      r_out[0] = FLT_MAX;
      r_out[1] = r_out[2] = 0.0f;
      zero_v3(r_tan);
      return;
    }
    /* Find any valid neighbor to clone from. */
    const float2 fill_uv = !b00 ? uv00 : !b10 ? uv10 : !b01 ? uv01 : uv11;
    const float3 fill_t = !b00 ? t00 : !b10 ? t10 : !b01 ? t01 : t11;
    if (b00) {
      uv00 = fill_uv;
      t00 = fill_t;
    }
    if (b10) {
      uv10 = fill_uv;
      t10 = fill_t;
    }
    if (b01) {
      uv01 = fill_uv;
      t01 = fill_t;
    }
    if (b11) {
      uv11 = fill_uv;
      t11 = fill_t;
    }
  }

  /* Detect overlap discontinuity: at sharp turns, multiple grid quads
   * compete for the same LUT pixels.  Adjacent pixels may pick different
   * quads (tiny float-distance differences flip the winner), making
   * bilinear interpolation blend UVs from unrelated stroke positions.
   *
   * If the 4 V values vary too much, fall back to nearest-neighbor
   * (the pixel with the smallest dist_sq) — at least the result is a
   * single valid stroke position rather than a fabricated midpoint.
   *
   * Threshold is "half a brush-radius worth of arc length".  In pressure
   * mode V is already normalized (≈1 unit per radius), so 0.5 directly.
   * In non-pressure mode V is in world units, so 0.5 × radius. */
  const float v_min = std::min({uv00.y, uv10.y, uv01.y, uv11.y});
  const float v_max = std::max({uv00.y, uv10.y, uv01.y, uv11.y});
  const bool use_norm_v_threshold = roll_texture_pressure_scaled();
  const float v_threshold = use_norm_v_threshold ? 0.5f : 0.5f * cache.initial_radius;
  float2 uv_result;
  float3 t_result;
  if (UNLIKELY(v_max - v_min > v_threshold)) {
    /* Pick the LUT pixel with the smallest dist_sq among the 4 neighbors. */
    const float d00 = cache.roll_lut_dist_sq[iy * RES + ix];
    const float d10 = cache.roll_lut_dist_sq[iy * RES + ix + 1];
    const float d01 = cache.roll_lut_dist_sq[(iy + 1) * RES + ix];
    const float d11 = cache.roll_lut_dist_sq[(iy + 1) * RES + ix + 1];
    float best_d = d00;
    uv_result = uv00;
    t_result = t00;
    if (d10 < best_d) {
      best_d = d10;
      uv_result = uv10;
      t_result = t10;
    }
    if (d01 < best_d) {
      best_d = d01;
      uv_result = uv01;
      t_result = t01;
    }
    if (d11 < best_d) {
      uv_result = uv11;
      t_result = t11;
    }
  }
  else {
    uv_result = (1 - tx) * (1 - ty) * uv00 + tx * (1 - ty) * uv10 + (1 - tx) * ty * uv01 +
                tx * ty * uv11;
    t_result = (1 - tx) * (1 - ty) * t00 + tx * (1 - ty) * t10 + (1 - tx) * ty * t01 +
               tx * ty * t11;
  }

  r_out[0] = -uv_result.x;
  r_out[1] = uv_result.y;
  r_out[2] = 0.0f;

  copy_v3_v3(r_tan, math::normalize(t_result));

  /* Apply V offset to keep texture continuous as virtual knots are consumed.
   * Use normalized offset when pressure-scaled V is baked into the grid. */
  const bool use_norm_v = roll_texture_pressure_scaled();
  if (use_norm_v) {
    r_out[1] += stroke_distance_normalized_ - roll_virtual_length_normalized_;
  }
  else {
    r_out[1] += stroke_distance_world_ - roll_virtual_length_;
  }
}

void PaintStroke::draw_debug_roll(bContext *C) const
{
  if (!need_roll_mapping_ || roll_spline_.is_empty()) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  const float ox = float(region->winrct.xmin);
  const float oy = float(region->winrct.ymin);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);

  /* --- 2D overlay: center polyline + tick marks --- */
  {
    const uint pos_attr = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    const int n_pts = int(roll_spline_.poly_2d.size());

    /* Polyline: red = virtual extension, green = real. */
    GPU_line_width(3.0f);
    if (n_virtual_poly_points_ > 1) {
      immUniformColor4ub(255, 50, 50, 200);
      const int cnt = std::min(n_virtual_poly_points_ + 1, n_pts);
      immBegin(GPU_PRIM_LINE_STRIP, cnt);
      for (int i = 0; i < cnt; i++) {
        immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
      }
      immEnd();
    }
    if (n_virtual_poly_points_ < n_pts) {
      immUniformColor4ub(50, 255, 50, 200);
      const int cnt = n_pts - n_virtual_poly_points_;
      immBegin(GPU_PRIM_LINE_STRIP, cnt);
      for (int i = n_virtual_poly_points_; i < n_pts; i++) {
        immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
      }
      immEnd();
    }

    /* Tick marks at each knot point. */
    GPU_line_width(1.5f);
    const float tick_len = 10.0f;
    for (int i = 0; i < n_pts; i++) {
      const float2 tan = roll_spline_.tangent_2d_at_index(std::min(i, n_pts - 2));
      const float2 perp(-tan.y, tan.x);
      const float2 &pt = roll_spline_.poly_2d[i];
      immUniformColor4ub(200, 200, 200, 140);
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos_attr, pt.x + perp.x * tick_len + ox, pt.y + perp.y * tick_len + oy);
      immVertex2f(pos_attr, pt.x - perp.x * tick_len + ox, pt.y - perp.y * tick_len + oy);
      immEnd();
    }

    /* Yellow tick at virtual/real boundary. */
    if (n_virtual_poly_points_ > 0 && n_virtual_poly_points_ < n_pts) {
      GPU_line_width(2.5f);
      immUniformColor4ub(255, 255, 0, 255);
      const float2 tan = roll_spline_.tangent_2d_at_index(n_virtual_poly_points_);
      const float2 perp(-tan.y, tan.x);
      const float2 &pt = roll_spline_.poly_2d[n_virtual_poly_points_];
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos_attr, pt.x + perp.x * 25.0f + ox, pt.y + perp.y * 25.0f + oy);
      immVertex2f(pos_attr, pt.x - perp.x * 25.0f + ox, pt.y - perp.y * 25.0f + oy);
      immEnd();
    }

    immUnbindProgram();
  }

  /* --- 3D debug draw: poly-strip grid wireframe --- */

  Object *ob = CTX_data_active_object(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
  if (ob && ob->runtime && ob->runtime->sculpt_session && ob->runtime->sculpt_session->cache &&
      rv3d)
  {
    const StrokeCache &cache = *ob->runtime->sculpt_session->cache;
    if (cache.roll_surface_ready && cache.roll_subdiv_rows > 1) {
      const int rows = cache.roll_subdiv_rows;
      const int cols = cache.roll_subdiv_cols;
      const float3 *gpos = cache.roll_subdiv_pos.data();
      const int center_col = cols / 2;

      /* Save viewport state and set up 3D matrices. */
      int saved_viewport[4];
      GPU_viewport_size_get_i(saved_viewport);

      GPU_matrix_push();
      GPU_matrix_push_projection();
      wmViewport(&region->winrct);
      GPU_matrix_projection_set(rv3d->winmat);
      const float4x4 mv = float4x4(rv3d->viewmat) * ob->object_to_world();
      GPU_matrix_set(mv.ptr());

      const uint pos3d_attr = GPU_vertformat_attr_add(
          immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

      const int eval_lo = cache.roll_eval_row_lo;
      const int eval_hi = cache.roll_eval_row_hi;

      /* Full-grid column lines (dim) — shows the entire stored strip. */
      for (int c = 0; c < cols; c++) {
        GPU_line_width(1.0f);
        immUniformColor4ub(100, 100, 100, 60);
        immBegin(GPU_PRIM_LINE_STRIP, rows);
        for (int r = 0; r < rows; r++) {
          immVertex3fv(pos3d_attr, gpos[r * cols + c]);
        }
        immEnd();
      }

      /* Eval-range column lines (bright) — the portion used for this dab. */
      const int eval_rows = std::min(eval_hi + 2, rows) - eval_lo;
      if (eval_rows > 1) {
        for (int c = 0; c < cols; c++) {
          if (c == center_col) {
            GPU_line_width(2.5f);
            immUniformColor4ub(50, 255, 50, 220);
          }
          else if (c == 0 || c == cols - 1) {
            GPU_line_width(2.0f);
            immUniformColor4ub(255, 160, 0, 220);
          }
          else {
            GPU_line_width(1.0f);
            immUniformColor4ub(180, 180, 180, 140);
          }
          immBegin(GPU_PRIM_LINE_STRIP, eval_rows);
          for (int r = eval_lo; r < eval_lo + eval_rows; r++) {
            immVertex3fv(pos3d_attr, gpos[r * cols + c]);
          }
          immEnd();
        }
      }

      /* Row lines (across stroke) in eval range only. */
      GPU_line_width(1.0f);
      immUniformColor4ub(200, 200, 200, 80);
      const int row_stride = std::max(1, eval_rows / 30);
      for (int r = eval_lo; r < eval_lo + eval_rows; r += row_stride) {
        immBegin(GPU_PRIM_LINE_STRIP, cols);
        for (int c = 0; c < cols; c++) {
          immVertex3fv(pos3d_attr, gpos[r * cols + c]);
        }
        immEnd();
      }

      /* Points need the point shader on current GPU backends. */
      immUnbindProgram();
      GPU_point_size(10.0f);
      immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);

      /* Cyan markers at eval range boundaries. */
      immUniform1f("size", 10.0f);
      immUniformColor4ub(0, 255, 255, 255);
      for (int boundary_r : {eval_lo, std::min(eval_hi + 1, rows - 1)}) {
        immBegin(GPU_PRIM_POINTS, 1);
        immVertex3fv(pos3d_attr, gpos[boundary_r * cols + center_col]);
        immEnd();
      }

      /* Red dots at self-intersection collapses: the first vertex of each
       * run of consecutive border vertices sharing one position. */
      GPU_point_size(8.0f);
      immUniform1f("size", 8.0f);
      immUniformColor4ub(255, 0, 0, 255);
      for (int side = 0; side < 2; side++) {
        const Vector<float3> &border = (side == 0) ? cache.roll_border_left :
                                                     cache.roll_border_right;
        const int bcount = int(border.size());
        for (int k = 1; k < bcount - 1; k++) {
          if (math::distance_squared(border[k], border[k + 1]) < 1e-10f &&
              math::distance_squared(border[k - 1], border[k]) > 1e-10f)
          {
            immBegin(GPU_PRIM_POINTS, 1);
            immVertex3fv(pos3d_attr, border[k]);
            immEnd();
          }
        }
      }

      immUnbindProgram();
      GPU_matrix_pop_projection();
      GPU_matrix_pop();

      /* Restore viewport. */
      GPU_viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    }
  }

  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_roll_debug(bContext *C,
                                  const int2 & /*xy*/,
                                  const float2 & /*tilt*/,
                                  void *customdata)
{
  PaintStroke *stroke = static_cast<PaintStroke *>(customdata);
  stroke->draw_debug_roll(C);
}

void PaintStroke::draw_roll_preview(bContext *C) const
{
  if (!need_roll_mapping_ || roll_spline_.is_empty()) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  const int n_pts = int(roll_spline_.poly_2d.size());

  /* Determine which polyline index corresponds to the last-painted dab.
   * Points from painted_poly onward are "unflushed" and shown as preview. */
  constexpr int buf_cap = ROLL_MAX_STROKE_POINTS;
  const int half = roll_half_points();

  int painted_poly;
  if (last_painted_roll_idx_ < 0 || num_points_ < half) {
    painted_poly = n_virtual_poly_points_;
  }
  else {
    const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;
    const int dist = (last_painted_roll_idx_ - oldest_idx + buf_cap) % buf_cap;
    painted_poly = n_virtual_poly_points_ + dist;
    painted_poly = std::min(painted_poly, n_pts - 1);
  }

  if (painted_poly >= n_pts) {
    return;
  }

  const float ox = float(region->winrct.xmin);
  const float oy = float(region->winrct.ymin);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);

  const uint pos_attr = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Same color as the stabilize stroke line. */
  immUniformColor4ub(255, 100, 100, 128);

  GPU_line_width(2.0f);
  const int count = n_pts - painted_poly;
  if (count >= 2) {
    immBegin(GPU_PRIM_LINE_STRIP, count);
    for (int i = painted_poly; i < n_pts; i++) {
      immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
    }
    immEnd();
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_roll_preview(bContext *C,
                                    const int2 & /*xy*/,
                                    const float2 & /*tilt*/,
                                    void *customdata)
{
  PaintStroke *stroke = static_cast<PaintStroke *>(customdata);
  stroke->draw_roll_preview(C);
}

/** \} */

void PaintStroke::init_roll_cursors()
{
  /* Always-on preview of the unflushed portion of the roll spline. */
  roll_cursor_ = WM_paint_cursor_activate(
      SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_roll_preview, this);
  /* Register the roll spline debug overlay only when the developer
   * "Paint Debug" option is enabled (Preferences → Developer Extras). */
  if (U.experimental.use_paint_debug) {
    debug_cursor_ = WM_paint_cursor_activate(
        SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_roll_debug, this);
  }
}

}  // namespace blender::ed::sculpt_paint
