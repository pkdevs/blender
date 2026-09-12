/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include <algorithm>
#include <functional>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_bounds.hh"
#include "BLI_lasso_2d.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_sort.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_curves.hh"
#include "BKE_grease_pencil_fills.hh"

#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

namespace blender::ed::greasepencil::segment::carver {

/* Calculate the winding order of a point when compared against a triangle. */
static float point_in_tri_winding(const float2 pt,
                                  const float2 v1,
                                  const float2 v2,
                                  const float2 v3)
{
  const float side12 = line_point_side_v2(v1, v2, pt);
  const float side23 = line_point_side_v2(v2, v3, pt);
  const float side31 = line_point_side_v2(v3, v1, pt);

  /* The point is on an edge. */
  if ((side12 == 0.0f && side23 >= 0.0f && side31 >= 0.0f) ||
      (side12 >= 0.0f && side23 == 0.0f && side31 >= 0.0f) ||
      (side12 >= 0.0f && side23 >= 0.0f && side31 == 0.0f))
  {
    return 0.5f;
  }
  if ((side12 == 0.0f && side23 <= 0.0f && side31 <= 0.0f) ||
      (side12 <= 0.0f && side23 == 0.0f && side31 <= 0.0f) ||
      (side12 <= 0.0f && side23 <= 0.0f && side31 == 0.0f))
  {
    return -0.5f;
  }

  /* The point is inside. */
  if (side12 >= 0.0f && side23 >= 0.0f && side31 >= 0.0f) {
    return 1.0f;
  }
  if (side12 <= 0.0f && side23 <= 0.0f && side31 <= 0.0f) {
    return -1.0f;
  }

  /* The point is outside. */
  return 0.0f;
}

/* Point must not be on a corner, but can be on an edge. */
static int point_in_polygon_winding_twice(const float2 &point, const Span<float2> poly)
{
  /* Double and store as a int to avoid float rounding. */
  int twice_winding = 0;
  const float2 &tri_p1 = poly[0];
  for (const int i : poly.index_range().drop_front(1).drop_back(1)) {
    const float2 &tri_p2 = poly[i];
    const float2 &tri_p3 = poly[i + 1];
    twice_winding += int(point_in_tri_winding(point, tri_p1, tri_p2, tri_p3) * 2);
  }
  return twice_winding;
}

/* Point must not be on a corner or edge. */
static int point_in_polygon_winding_int(const float2 &point, const Span<float2> poly)
{
  const int twice_winding = point_in_polygon_winding_twice(point, poly);
  return int(twice_winding / 2);
}

/**
 * The Winding State is a sparse way to store what curves a point is inside.
 */
class WindingState {
 private:
  /* Winding order of each curve. */
  Map<int, int> orders_per_curve_;

 public:
  void add_to_curve(const int curve_i, const int winding_i)
  {
    if (winding_i == 0) {
      return;
    }

    if (orders_per_curve_.contains(curve_i)) {
      orders_per_curve_.lookup(curve_i) += winding_i;

      /* Remove unneeded ids. */
      if (orders_per_curve_.lookup(curve_i) == 0) {
        orders_per_curve_.remove(curve_i);
      }
    }
    else {
      orders_per_curve_.add(curve_i, winding_i);
    }
  }

  bool is_in_shape(const int shape_id, const GroupedSpan<int> &shapes) const
  {
    int winding = 0;

    const Span<int> shape = shapes[shape_id];
    for (const int curve_i : shape) {
      if (orders_per_curve_.contains(curve_i)) {
        winding += orders_per_curve_.lookup(curve_i);
      }
    }
    /* Odd-Even fill rule. */
    return winding % 2 != 0;
  }

  bool is_in_shapes(const IndexMask &shape_mask, const GroupedSpan<int> &shapes) const
  {
    if (orders_per_curve_.is_empty() || shape_mask.is_empty()) {
      return false;
    }

    return threading::parallel_reduce(
        shape_mask.index_range(),
        4096,
        false,
        [&](const IndexRange range, bool value) {
          if (value) {
            return value;
          }
          shape_mask.slice(range).foreach_index([&](const int shape_i) {
            if (this->is_in_shape(shape_i, shapes)) {
              value = true;
              return;
            }
          });
          return value;
        },
        std::logical_or());
  }

  /* Returns true if the point exists for this boolean operation. */
  bool is_contributing(const CurveBooleanOpParameters op_params,
                       const GroupedSpan<int> &shapes,
                       const int subject_shape,
                       const IndexMask &clipping_shapes) const
  {
    const bool subj = this->is_in_shape(subject_shape, shapes);
    const bool clip = this->is_in_shapes(clipping_shapes, shapes);

    switch (op_params.boolean_mode) {
      case Operation::Intersect: {
        return subj && clip;
      }
      case Operation::Difference: {
        return subj && !clip;
      }
      case Operation::Union: {
        return subj || clip;
      }
      default:
        BLI_assert_unreachable();
        break;
    }

    return false;
  }
};

/* The distance the left and right points will be placed from the segment.
 * A to much smaller will cause floating point problems, but anything much larger can cause
 * inaccuracies. */
constexpr float epsilon_distance = 0.01f;
/* When the segment is vary small the #epsilon_distance can be to large, instead use a factor of
 * the segment's length. */
constexpr float epsilon_factor = 0.05f;
/* Factor along the segment where the left and right points are placed.
 * A value of 0.5 can cause failure, so instead use a random number between 0.0 and 1.0 */
constexpr float mid_point_factor = 0.35421f;

/* Calculate the winding states for left and right of the segment. */
static std::pair<WindingState, WindingState> LR_states_from_segment(
    const Segment &segment,
    const Span<float2> all_positions,
    const OffsetIndices<int> points_by_curve,
    const GroupedSpan<int> shapes,
    const IndexMask &mask_shapes,
    const VArray<bool> &is_fill)
{
  WindingState state_L;
  WindingState state_R;

  const int curve_i = segment.curve;

  float2 point1;
  float2 point2;

  if (segment.has_intersection(Side::Start)) {
    point1 = math::interpolate(all_positions[segment.edge(Side::Start)[0]],
                               all_positions[segment.edge(Side::Start)[1]],
                               segment.intersection_factor[Side::Start]);
  }
  else {
    point1 = all_positions[segment.src_points.first()];
  }

  if (segment.edge(Side::Start)[0] == segment.edge(Side::End)[0] &&
      (segment.points_num() == 0 ||
       (segment.points_num() == 1 &&
        (!segment.has_intersection(Side::Start) && segment.has_intersection(Side::End)))))
  {
    point2 = math::interpolate(all_positions[segment.edge(Side::End)[0]],
                               all_positions[segment.edge(Side::End)[1]],
                               segment.intersection_factor[Side::End]);
  }
  else {
    const int first_end_index = segment.edge(Side::Start)[1];
    int end_index = first_end_index;

    point2 = all_positions[end_index];

    /* If the points are on top of each other, go to the next point. */
    while (math::distance(point1, point2) < 0.001f) {
      end_index = segment.wrap_index(end_index + 1);
      point2 = all_positions[end_index];

      /* Prevent looping forever. */
      if (end_index == first_end_index) {
        break;
      }
    }
  }

  const float2 line_dir = math::normalize(point2 - point1);
  /* Direction to the left. */
  const float2 tan_dir = float2(-line_dir.y, line_dir.x);

  const float2 mid_point = math::interpolate(point1, point2, mid_point_factor);

  const float eps_dis = math::min(epsilon_factor * math::distance(point1, point2),
                                  epsilon_distance);

  const float2 l_point = mid_point + tan_dir * eps_dis;
  const float2 r_point = mid_point - tan_dir * eps_dis;

  mask_shapes.foreach_index([&](const int shape_j_index) {
    const Span<int> shape_j = shapes[shape_j_index];
    for (const int curve_j : shape_j) {
      if (curve_j == curve_i) {
        return;
      }

      const IndexRange points_j = points_by_curve[curve_j];

      const Span<float2> poly_j = all_positions.slice(points_j);

      const int l_winding_i = point_in_polygon_winding_int(l_point, poly_j);
      const int r_winding_i = point_in_polygon_winding_int(r_point, poly_j);

      state_L.add_to_curve(curve_j, l_winding_i);
      state_R.add_to_curve(curve_j, r_winding_i);
    }
  });

  /* Self check. */
  if (is_fill[curve_i]) {
    const IndexRange points_i = points_by_curve[curve_i];

    const Span<float2> poly_i = all_positions.slice(points_i);

    const int l_winding_i = point_in_polygon_winding_int(l_point, poly_i);
    const int r_winding_i = point_in_polygon_winding_int(r_point, poly_i);

    state_L.add_to_curve(curve_i, l_winding_i);
    state_R.add_to_curve(curve_i, r_winding_i);
  }

  return {state_L, state_R};
}

/* Check the left and right of a curve `k` */
void get_inside_states_for_segments(const CurveBooleanOpParameters &op_params,
                                    const int curve_k,
                                    const bool is_subj,
                                    const int subj_shape_i,
                                    const Span<float2> points,
                                    const GroupedSpan<int> shapes,
                                    const OffsetIndices<int> points_by_curve,
                                    const IndexMask &clipping_shapes,
                                    const Span<Segment> all_segments,
                                    const OffsetIndices<int> segments_by_curve,
                                    const VArray<bool> &is_fill,
                                    MutableSpan<bool> all_inside_left,
                                    MutableSpan<bool> all_inside_right)
{
  const IndexRange segments = segments_by_curve[curve_k];

  if (segments.is_empty()) {
    return;
  }

  const IndexMask &mask_shapes = is_subj ? clipping_shapes :
                                           (!is_fill[shapes[subj_shape_i].first()] ?
                                                IndexRange(0) :
                                                IndexRange::from_single(subj_shape_i));

  for (const int seg_i : segments) {
    const Segment &this_segment = all_segments[seg_i];

    auto [state_L, state_R] = LR_states_from_segment(
        this_segment, points, points_by_curve, shapes, mask_shapes, is_fill);

    if (is_fill[curve_k]) {
      all_inside_left[seg_i] = state_L.is_contributing(
          op_params, shapes, subj_shape_i, clipping_shapes);
      all_inside_right[seg_i] = state_R.is_contributing(
          op_params, shapes, subj_shape_i, clipping_shapes);
    }
    else {
      all_inside_left[seg_i] = state_L.is_in_shapes(clipping_shapes, shapes);
      all_inside_right[seg_i] = state_R.is_in_shapes(clipping_shapes, shapes);
    }
  }
}

/**
 * Separate disjoint fill islands.
 *
 * Note: Nested islands of 3 or more curves will not be split.
 *
 * If a curve as been changed and it is not a child then update it's and it's children's fill ids.
 *
 * Note: Previously unchanged curves may have their fill id changed, if their parent curve id is
 * changed.
 */
void separate_fill_islands(bke::CurvesGeometry &dst,
                           const Span<bool> unchanged_curves,
                           const ProjectionFunc project)
{
  const OffsetIndices<int> points_by_curve = dst.points_by_curve();
  bke::MutableAttributeAccessor attributes = dst.attributes_for_write();
  bke::SpanAttributeWriter<int> fill_ids = attributes.lookup_for_write_span<int>("fill_id");

  Array<float2> dst_positions_2d(dst.points_num());
  const Span<float3> positions = dst.positions();
  for (const int i : dst.points_range()) {
    dst_positions_2d[i] = project(positions[i]);
  }

  /* Returns if #curve_i in inside of #curve_j, only valid if the curves do not intersect. */
  auto is_curve_i_in_j = [&](const int curve_i, const int curve_j) {
    const IndexRange points_i = points_by_curve[curve_i];
    const IndexRange points_j = points_by_curve[curve_j];

    const Span<float2> poly_i = dst_positions_2d.as_span().slice(points_i);
    const Span<float2> poly_j = dst_positions_2d.as_span().slice(points_j);

    const int winding = point_in_polygon_winding_int(poly_i.first(), poly_j);

    return winding % 2 != 0;
  };

  Array<bool> is_curve_child(points_by_curve.size(), false);
  Array<Vector<int>> curve_children(points_by_curve.size());

  for (const int curve_i : points_by_curve.index_range()) {
    /* Skip curves without fill. */
    if (fill_ids.span[curve_i] == 0) {
      continue;
    }

    for (const int curve_j : points_by_curve.index_range()) {
      if (curve_j == curve_i) {
        continue;
      }

      if (fill_ids.span[curve_j] != fill_ids.span[curve_i]) {
        continue;
      }

      if (is_curve_i_in_j(curve_i, curve_j)) {
        is_curve_child[curve_i] = true;
        curve_children[curve_j].append(curve_i);
      }
    }
  }

  int new_fill_id = bke::greasepencil::get_next_available_fill_id(fill_ids.span.varray());

  for (const int curve_i : points_by_curve.index_range()) {
    if (is_curve_child[curve_i]) {
      continue;
    }

    if (fill_ids.span[curve_i] == 0 || unchanged_curves[curve_i]) {
      continue;
    }

    fill_ids.span[curve_i] = new_fill_id;

    const Span<int> curve_i_children = curve_children[curve_i];
    for (const int curve_j : curve_i_children) {
      fill_ids.span[curve_j] = new_fill_id;
    }

    new_fill_id++;
  }

  fill_ids.finish();
}

}  // namespace blender::ed::greasepencil::segment::carver
