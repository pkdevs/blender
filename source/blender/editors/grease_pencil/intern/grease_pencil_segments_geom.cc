/* SPDX-FileCopyrightText: 2025 Blender Authors
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

namespace blender::ed::greasepencil {

enum Side : uint8_t { Start = 0, End = 1 };

/* When looking for intersections, we need a little padding, otherwise we could miss curves
 * that intersect for the eye, but not in hard numbers. */
static constexpr int BBOX_PADDING = 2;

/**
 * Structure describing a curve segment (a point range in a curve) with end intersection points.
 * A Segment can go past the end of the source curve and loop back to the start.
 */
class Segment {
 public:
  /* Curve index. */
  int curve = -1;

  /* The start and end of the original curve is stored, because this segment may go past the end
   * and have to loop. */
  IndexRange src_points;

  /* Point range of the segment: Starting point and end point. Matches the point offsets
   * in a CurvesGeometry. */
  int points[2] = {-1, -1};

  /* If this segment is a full cyclical segment, note that the segment can start and end at some
   * intersection point. */
  bool full_wrap_loop = false;

  /* The normalized distance where the trim segment is intersected by another curve.
   * For the outer ends of the trim segment the intersection distance is given between:
   * - [start point] and [start point + 1]
   * - [end point] and [end point + 1]
   */
  float intersection_factor[2] = {0.0f, 0.0f};

  int intersection_index[2] = {-1, -1};

  constexpr Segment() = default;

  bool is_loop() const
  {
    return full_wrap_loop;
  }

  bool has_intersection(const Side side) const
  {
    return intersection_index[side] != -1;
  }

  int2 edge(const Side side) const
  {
    return int2(points[side], this->wrap_index(points[side] + 1));
  }

  int wrap_index(const int i) const
  {
    return math::mod_periodic(i - src_points.first(), src_points.size()) + src_points.first();
  }

  IndexRange point_range() const
  {
    if (this->is_loop()) {
      return src_points;
    }

    if (!this->has_intersection(Side::Start) && this->has_intersection(Side::End)) {
      return IndexRange::from_begin_end_inclusive(src_points.first(), points[Side::End]);
    }

    if (!this->has_intersection(Side::Start) && !this->has_intersection(Side::End)) {
      return src_points;
    }

    /* If both intersection points are on the same edge, there's ether no points between or
     * all of the points are. */
    if (points[Side::Start] == points[Side::End]) {

      /* If both intersections points are the same, either the segment as nothing or the full
       * range. */
      if (intersection_factor[Side::Start] == intersection_factor[Side::End]) {
        if (this->is_loop()) {
          return src_points.shift(points[Side::Start] - src_points.first() + 1);
        }
        return IndexRange(0);
      }

      if (intersection_factor[Side::Start] > intersection_factor[Side::End]) {
        return src_points.shift(points[Side::Start] - src_points.first() + 1);
      }
      return IndexRange(0);
    }

    if (points[Side::Start] > points[Side::End]) {
      return IndexRange::from_begin_end_inclusive(points[Side::Start] + 1,
                                                  points[Side::End] + src_points.size());
    }

    return IndexRange::from_begin_end_inclusive(points[Side::Start] + 1, points[Side::End]);
  }

  int points_num() const
  {
    return this->point_range().size();
  }

  template<typename Fn> void foreach_point(Fn &&fn) const
  {
    const IndexRange point_range = this->point_range();

    for (const int64_t pos : point_range.index_range()) {
      const int i = this->wrap_index(point_range[pos]);

      if constexpr (std::is_invocable_r_v<void, Fn, int64_t, int64_t>) {
        fn(i, pos);
      }
      else {
        fn(i);
      }
    }
  }

  constexpr static Segment from_curve(const int curve_i,
                                      const IndexRange points,
                                      const bool cyclic)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.src_points = points;

    segment.points[Side::Start] = points.first();
    segment.points[Side::End] = points.last();

    segment.intersection_factor[Side::Start] = 0.0f;
    segment.intersection_factor[Side::End] = cyclic ? 1.0f : 0.0f;

    segment.full_wrap_loop = cyclic;

    return segment;
  }

  static Segment from_intersections(const int curve_i,
                                    const IndexRange points,
                                    const std::optional<int> point_start,
                                    const std::optional<int> point_end,
                                    const std::optional<float> factor_start,
                                    const std::optional<float> factor_end,
                                    const std::optional<int> inter_index_start,
                                    const std::optional<int> inter_index_end)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.src_points = points;

    if (point_start) {
      segment.points[Side::Start] = *point_start;
      segment.intersection_factor[Side::Start] = *factor_start;
    }
    else {
      segment.points[Side::Start] = points.first();
      segment.intersection_factor[Side::Start] = 0.0f;
    }

    if (inter_index_start) {
      segment.intersection_index[Side::Start] = *inter_index_start;
    }

    if (point_end) {
      segment.points[Side::End] = *point_end;
      segment.intersection_factor[Side::End] = *factor_end;
    }
    else {
      segment.points[Side::End] = points.last();
      segment.intersection_factor[Side::End] = 0.0f;
    }

    if (inter_index_end) {
      segment.intersection_index[Side::End] = *inter_index_end;
    }

    BLI_assert(points.contains(segment.points[Side::Start]));
    BLI_assert(points.contains(segment.points[Side::End]));

    return segment;
  }
};

/**
 * Get the intersection distance of two line segments a-b and c-d.
 * The intersection distance is defined as the normalized distance (0..1)
 * from point a to the intersection point of a-b and c-d.
 */
static float get_intersection_distance_of_segments(const float2 &co_a,
                                                   const float2 &co_b,
                                                   const float2 &co_c,
                                                   const float2 &co_d)
{
  /* Get intersection point. */
  const float a1 = co_b[1] - co_a[1];
  const float b1 = co_a[0] - co_b[0];
  const float c1 = a1 * co_a[0] + b1 * co_a[1];

  const float a2 = co_d[1] - co_c[1];
  const float b2 = co_c[0] - co_d[0];
  const float c2 = a2 * co_c[0] + b2 * co_c[1];

  const float det = (a1 * b2 - a2 * b1);
  if (det == 0.0f) {
    return 0.0f;
  }

  const float2 isect((b2 * c1 - b1 * c2) / det, (a1 * c2 - a2 * c1) / det);

  /* Get normalized distance from point a to intersection point. */
  const float length_ab = math::distance(co_b, co_a);
  const float distance = math::safe_divide(math::distance(isect, co_a), length_ab);

  /* Snap to the ends if very close. */
  if (math::abs(distance) < 0.0001f) {
    return 0.0f;
  }
  if (math::abs(distance - 1.0f) < 0.0001f) {
    return 1.0f;
  }

  return distance;
}

static bke::CurvesGeometry create_curves_from_segments(const bke::CurvesGeometry &src,
                                                       const Span<Segment> segments,
                                                       const Span<bool> segment_reversed,
                                                       const Span<bool> cyclic,
                                                       const OffsetIndices<int> segment_offsets)
{
  struct InterpolatePoint {
    int src_point_1;
    int src_point_2;
    float factor;
  };

  Array<int> point_offsets(segment_offsets.size() + 1);
  Vector<InterpolatePoint> point_to_interpolate;

  for (const int curve_i : segment_offsets.index_range()) {
    point_offsets[curve_i] = point_to_interpolate.size();

    const IndexRange segment_range = segment_offsets[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = segments[seg_i];
      const bool reversed = segment_reversed[seg_i];
      const Side start_side = reversed ? Side::End : Side::Start;
      const Side end_side = reversed ? Side::Start : Side::End;

      if (segment.has_intersection(start_side) && !segment.is_loop()) {
        const float start_factor = segment.intersection_factor[start_side];
        const int2 start_edge = segment.edge(start_side);

        point_to_interpolate.append({start_edge.x, start_edge.y, start_factor});
      }

      segment.foreach_point(
          [&](const int index) { point_to_interpolate.append({index, index, 0.0f}); });

      if (reversed) {
        point_to_interpolate.as_mutable_span().take_back(segment.points_num()).reverse();
      }

      if (seg_i == segment_range.last() && segment.has_intersection(end_side) && !cyclic[curve_i])
      {
        const float end_factor = segment.intersection_factor[end_side];
        const int2 end_edge = segment.edge(end_side);

        point_to_interpolate.append({end_edge.x, end_edge.y, end_factor});
      }
    }
  }

  point_offsets.last() = point_to_interpolate.size();
  const OffsetIndices<int> dst_points_by_curve = OffsetIndices<int>(point_offsets);

  if (dst_points_by_curve.total_size() == 0) {
    return bke::CurvesGeometry();
  }

  bke::CurvesGeometry dst_curves(dst_points_by_curve.total_size(), dst_points_by_curve.size());
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  dst_curves.offsets_for_write().copy_from(dst_points_by_curve.data());
  dst_curves.cyclic_for_write().copy_from(cyclic);

  Array<int> old_by_new_map(dst_points_by_curve.size());

  threading::parallel_for(dst_points_by_curve.index_range(), 4096, [&](const IndexRange points) {
    for (const int i : points) {
      const IndexRange segment_range = segment_offsets[i];
      old_by_new_map[i] = segments[segment_range.first()].curve;
    }
  });

  const bke::AttributeAccessor src_attributes = src.attributes();
  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         old_by_new_map,
                         dst_attributes);

  /* Copy/Interpolate point attributes. */
  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes, dst_attributes, {bke::AttrDomain::Point}, {}))
  {
    bke::attribute_math::to_static_type(attribute.dst.span.type(), [&]<typename T>() {
      if constexpr (!std::is_same_v<T, std::string>) {
        const Span<T> src_attr = attribute.src.typed<T>();
        MutableSpan<T> dst_attr = attribute.dst.span.typed<T>();

        threading::parallel_for(
            point_to_interpolate.index_range(), 4096, [&](const IndexRange points) {
              for (const int i : points) {
                const InterpolatePoint &int_point = point_to_interpolate[i];

                if (int_point.factor == 0.0f) {
                  dst_attr[i] = src_attr[int_point.src_point_1];
                }
                else if (int_point.factor == 1.0f) {
                  dst_attr[i] = src_attr[int_point.src_point_2];
                }
                else {
                  dst_attr[i] = bke::attribute_math::mix2<T>(int_point.factor,
                                                             src_attr[int_point.src_point_1],
                                                             src_attr[int_point.src_point_2]);
                }
              }
            });
      }
    });

    attribute.dst.finish();
  }

  return dst_curves;
}

struct IntersectionPoint {
  int point_i = -1;
  int point_j = -1;
  float factor_i = -1.0f;
  float factor_j = -1.0f;
  int curve_i = -1;
  int curve_j = -1;

  int segment_index_i[2] = {-1, -1};
  int segment_index_j[2] = {-1, -1};

  constexpr IntersectionPoint() = default;

  float point_for_curve(const int curve) const
  {
    BLI_assert(curve == curve_i || curve == curve_j);
    return curve == curve_i ? point_i : point_j;
  }

  float factor_for_curve(const int curve) const
  {
    BLI_assert(curve == curve_i || curve == curve_j);
    return curve == curve_i ? factor_i : factor_j;
  }

  float parameter_for_curve(const int curve) const
  {
    return this->point_for_curve(curve) + this->factor_for_curve(curve);
  }

  int other_curve(const int curve) const
  {
    BLI_assert(curve == curve_i || curve == curve_j);
    return curve == curve_i ? curve_j : curve_i;
  }
};

static IntersectionPoint create_intersection(const int point_i,
                                             const int point_j,
                                             const float factor_i,
                                             const float factor_j,
                                             const int curve_i,
                                             const int curve_j)
{
  IntersectionPoint inter_point;
  inter_point.point_i = point_i;
  inter_point.point_j = point_j;
  inter_point.factor_i = factor_i;
  inter_point.factor_j = factor_j;
  inter_point.curve_i = curve_i;
  inter_point.curve_j = curve_j;

  return inter_point;
}

static void find_intersections_between_curve_and_curves(
    const Span<float2> screen_space_positions,
    const Span<Bounds<float2>> screen_space_bbox,
    const OffsetIndices<int> points_by_curve,
    const VArray<bool> &cyclic,
    const IndexMask &visible_curves,
    const int curve_i,
    Array<Vector<int>> &r_inters_per_curves,
    Vector<IntersectionPoint> &r_intersections)
{
  const bool cyclic_i = cyclic[curve_i];
  const IndexRange curve_points_i = points_by_curve[curve_i];

  for (const int i : curve_points_i.index_range().drop_back(cyclic_i ? 0 : 1)) {
    const int point_i1 = curve_points_i[i];
    const int point_i2 = curve_points_i[(i + 1) % curve_points_i.size()];

    const float2 co_i1 = screen_space_positions[point_i1];
    const float2 co_i2 = screen_space_positions[point_i2];

    Bounds<float2> bbox_i{math::min(co_i1, co_i2), math::max(co_i1, co_i2)};
    bbox_i.pad(BBOX_PADDING);

    /* Add some padding to the line segment i1-i2, otherwise we could just miss an
     * intersection. */
    const float2 padding_i = math::normalize(co_i2 - co_i1);
    const float2 padded_i1 = co_i1 - padding_i;
    const float2 padded_i2 = co_i2 + padding_i;

    visible_curves.foreach_index([&](const int curve_j) {
      /* Because intersecting the curves i with j and j with i, we skip one half to avoid
       * duplicating all the points. */
      if (curve_i > curve_j) {
        return;
      }

      /* Bounding box check: Skip curves that don't overlap segment i1-i2. */
      if (!bounds::intersect(bbox_i, screen_space_bbox[curve_j]).has_value()) {
        return;
      }

      const bool cyclic_j = cyclic[curve_j];
      const IndexRange curve_points_j = points_by_curve[curve_j];

      for (const int j : curve_points_j.index_range().drop_back(cyclic_j ? 0 : 1)) {
        const int point_j1 = curve_points_j[j];
        const int point_j2 = curve_points_j[(j + 1) % curve_points_j.size()];

        /* Don't self check. */
        if (curve_i == curve_j && (point_i1 == point_j1 || point_i1 == point_j2 ||
                                   point_i2 == point_j1 || point_i2 == point_j2))
        {
          continue;
        }

        const float2 co_j1 = screen_space_positions[point_j1];
        const float2 co_j2 = screen_space_positions[point_j2];

        Bounds<float2> bbox_j{math::min(co_j1, co_j2), math::max(co_j1, co_j2)};
        bbox_j.pad(BBOX_PADDING);

        /* Skip when bounding boxes of i1-i2 and j1-j2 don't overlap. */
        if (!bounds::intersect(bbox_i, bbox_j).has_value()) {
          continue;
        }

        /* Add some padding to the line segment j1-j2, otherwise we could just miss an
         * intersection. */
        const float2 padding_j = math::normalize(co_j2 - co_j1);
        const float2 padded_j1 = co_j1 - padding_j;
        const float2 padded_j2 = co_j2 + padding_j;

        /* Check for intersection. */
        const auto isect = math::isect_seg_seg(padded_i1, padded_i2, padded_j1, padded_j2);
        if (ELEM(isect.kind, isect.LINE_LINE_CROSS, isect.LINE_LINE_EXACT)) {
          const float factor_i = get_intersection_distance_of_segments(co_i1, co_i2, co_j1, co_j2);
          const float factor_j = get_intersection_distance_of_segments(co_j1, co_j2, co_i1, co_i2);

          /* If the intersection is outside of the edge, skip it. Note that exactly on the edge is
           * accepted. */
          if (factor_i < 0.0f || factor_i > 1.0f || factor_j < 0.0f || factor_j > 1.0f) {
            continue;
          }

          r_inters_per_curves[curve_i].append(r_intersections.size());
          r_inters_per_curves[curve_j].append(r_intersections.size());
          r_intersections.append(
              create_intersection(point_i1, point_j1, factor_i, factor_j, curve_i, curve_j));
        }
      }
    });
  }
}

/* TODO: This method of finding intersections is O(N^2) and should replaced with something faster.
 */
static void find_intersections_between_all_curves(const Span<float2> screen_space_positions,
                                                  const Span<Bounds<float2>> screen_space_bbox,
                                                  const OffsetIndices<int> points_by_curve,
                                                  const VArray<bool> &cyclic,
                                                  const IndexMask &visible_curves,
                                                  Array<Vector<int>> &r_inters_per_curves,
                                                  Vector<IntersectionPoint> &r_intersections)
{
  visible_curves.foreach_index([&](const int curve_i) {
    find_intersections_between_curve_and_curves(screen_space_positions,
                                                screen_space_bbox,
                                                points_by_curve,
                                                cyclic,
                                                visible_curves,
                                                curve_i,
                                                r_inters_per_curves,
                                                r_intersections);
  });
}

static void store_segment_map_on_intersections(const Span<Segment> all_segments,
                                               MutableSpan<IntersectionPoint> intersections)
{
  for (const int seg_i : all_segments.index_range()) {
    const Segment &segment = all_segments[seg_i];
    const int curve_i = segment.curve;

    if (segment.has_intersection(Side::Start)) {
      IntersectionPoint &inter_start = intersections[segment.intersection_index[Side::Start]];
      if (curve_i == inter_start.curve_i) {
        inter_start.segment_index_i[Side::End] = seg_i;
      }
      else {
        inter_start.segment_index_j[Side::End] = seg_i;
      }
    }

    if (segment.has_intersection(Side::End)) {
      IntersectionPoint &inter_end = intersections[segment.intersection_index[Side::End]];
      if (curve_i == inter_end.curve_i) {
        inter_end.segment_index_i[Side::Start] = seg_i;
      }
      else {
        inter_end.segment_index_j[Side::Start] = seg_i;
      }
    }
  }
}

static int create_segments_from_intersections_single_curve(
    const int curve_k,
    const Span<Vector<int>> inters_per_curves,
    const OffsetIndices<int> points_by_curve,
    const Span<IntersectionPoint> &intersections,
    const VArray<bool> &cyclic,
    Vector<Segment> &all_segments)
{
  const IndexRange points_k = points_by_curve[curve_k];
  const Span<int> inters = inters_per_curves[curve_k];

  const int start_size = all_segments.size();

  if (inters.size() == 0) {
    all_segments.append(Segment::from_curve(curve_k, points_k, cyclic[curve_k]));
    return 1;
  }

  if (inters.size() == 1 && cyclic[curve_k]) {
    const int int_p = inters.first();

    const IntersectionPoint &inter = intersections[int_p];

    all_segments.append(Segment::from_intersections(curve_k,
                                                    points_k,
                                                    inter.point_for_curve(curve_k),
                                                    inter.point_for_curve(curve_k),
                                                    inter.factor_for_curve(curve_k),
                                                    inter.factor_for_curve(curve_k),
                                                    int_p,
                                                    int_p));
    all_segments.last().full_wrap_loop = true;
    return 1;
  }

  Array<int> inter_sorted_ids = Array<int>(inters.size());
  array_utils::fill_index_range<int>(inter_sorted_ids);

  parallel_sort(inter_sorted_ids.begin(), inter_sorted_ids.end(), [&](int i1, int i2) {
    const IntersectionPoint &inter1 = intersections[inters[i1]];
    const IntersectionPoint &inter2 = intersections[inters[i2]];
    return inter1.parameter_for_curve(curve_k) < inter2.parameter_for_curve(curve_k);
  });

  if (cyclic[curve_k]) {
    const int int_p_1 = inters[inter_sorted_ids.first()];
    const int int_p_2 = inters[inter_sorted_ids.last()];

    const IntersectionPoint &inter_first = intersections[int_p_1];
    const IntersectionPoint &inter_last = intersections[int_p_2];

    all_segments.append(Segment::from_intersections(curve_k,
                                                    points_k,
                                                    inter_last.point_for_curve(curve_k),
                                                    inter_first.point_for_curve(curve_k),
                                                    inter_last.factor_for_curve(curve_k),
                                                    inter_first.factor_for_curve(curve_k),
                                                    int_p_2,
                                                    int_p_1));
  }
  else {
    const int int_p_1 = inters[inter_sorted_ids.first()];
    const IntersectionPoint &inter_first = intersections[int_p_1];

    if (inter_first.parameter_for_curve(curve_k) != float(points_k.first())) {
      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      std::nullopt,
                                                      inter_first.point_for_curve(curve_k),
                                                      std::nullopt,
                                                      inter_first.factor_for_curve(curve_k),
                                                      std::nullopt,
                                                      int_p_1));
    }
  }

  for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
    const int int_p_1 = inters[inter_sorted_ids[inter_id]];
    const int int_p_2 = inters[inter_sorted_ids[inter_id + 1]];

    const IntersectionPoint &inter_first = intersections[int_p_1];
    const IntersectionPoint &inter_last = intersections[int_p_2];

    if (inter_first.parameter_for_curve(curve_k) != inter_last.parameter_for_curve(curve_k)) {
      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      inter_first.point_for_curve(curve_k),
                                                      inter_last.point_for_curve(curve_k),
                                                      inter_first.factor_for_curve(curve_k),
                                                      inter_last.factor_for_curve(curve_k),
                                                      int_p_1,
                                                      int_p_2));
    }
  }

  if (!cyclic[curve_k]) {
    const int int_p_2 = inters[inter_sorted_ids.last()];
    const IntersectionPoint &inter_last = intersections[int_p_2];

    if (inter_last.parameter_for_curve(curve_k) != float(points_k.last())) {
      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      inter_last.point_for_curve(curve_k),
                                                      std::nullopt,
                                                      inter_last.factor_for_curve(curve_k),
                                                      std::nullopt,
                                                      int_p_2,
                                                      std::nullopt));
    }
  }

  return all_segments.size() - start_size;
}

static void create_segments_from_intersections(const Span<Vector<int>> inters_per_curves,
                                               const OffsetIndices<int> points_by_curve,
                                               const Span<IntersectionPoint> &intersections,
                                               const VArray<bool> &cyclic,
                                               Vector<Segment> &all_segments,
                                               MutableSpan<int> segments_num_per_curve)
{
  for (const int curve_k : points_by_curve.index_range()) {
    segments_num_per_curve[curve_k] = create_segments_from_intersections_single_curve(
        curve_k, inters_per_curves, points_by_curve, intersections, cyclic, all_segments);
  }
}

static bool check_and_join_segments(Segment &first, const Segment &second)
{
  if (first.curve != second.curve) {
    return false;
  }

  const float parameter_first_start = first.points[Side::Start] +
                                      first.intersection_factor[Side::Start];
  const float parameter_first_end = first.points[Side::End] + first.intersection_factor[Side::End];
  const float parameter_second_start = second.points[Side::Start] +
                                       second.intersection_factor[Side::Start];
  const float parameter_second_end = second.points[Side::End] +
                                     second.intersection_factor[Side::End];

  if ((parameter_first_end == parameter_second_start) ||
      (first.intersection_index[Side::End] == second.intersection_index[Side::Start] &&
       first.intersection_index[Side::End] != -1))
  {
    first.points[Side::End] = second.points[Side::End];
    first.intersection_factor[Side::End] = second.intersection_factor[Side::End];
    first.intersection_index[Side::End] = second.intersection_index[Side::End];

    return true;
  }
  if ((parameter_first_start == parameter_second_end) ||
      (first.intersection_index[Side::Start] == second.intersection_index[Side::End] &&
       first.intersection_index[Side::Start] != -1))
  {
    first.points[Side::Start] = second.points[Side::Start];
    first.intersection_factor[Side::Start] = second.intersection_factor[Side::Start];
    first.intersection_index[Side::Start] = second.intersection_index[Side::Start];

    return true;
  }

  return false;
}

static void cut_caps(bke::CurvesGeometry &dst,
                     const Span<Segment> segments,
                     const Span<bool> segment_reversed,
                     const Span<bool> cyclic,
                     const OffsetIndices<int> segment_offsets)
{
  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  bke::SpanAttributeWriter dst_start_caps = dst_attributes.lookup_or_add_for_write_span<int8_t>(
      "start_cap", bke::AttrDomain::Curve);
  bke::SpanAttributeWriter dst_end_caps = dst_attributes.lookup_or_add_for_write_span<int8_t>(
      "end_cap", bke::AttrDomain::Curve);

  threading::parallel_for(segment_offsets.index_range(), 4096, [&](const IndexRange curves) {
    for (const int curve_i : curves) {
      /* If the curve is cyclic, don't cut it. */
      if (cyclic[curve_i]) {
        continue;
      }

      const IndexRange segment_range = segment_offsets[curve_i];

      const int segment_index_first = segment_range.first();
      const bool reversed_first = segment_reversed[segment_index_first];
      const Segment &segment_first = segments[segment_index_first];
      const Side direction_first = reversed_first ? Side::End : Side::Start;
      const int inter_index_first = segment_first.intersection_index[direction_first];

      const int segment_index_last = segment_range.last();
      const bool reversed_last = segment_reversed[segment_index_last];
      const Segment &segment_last = segments[segment_index_last];
      const Side direction_last = reversed_last ? Side::Start : Side::End;
      const int inter_index_last = segment_last.intersection_index[direction_last];

      /* Check if there is a intersection and therefor the curve should be cut. */
      if (inter_index_first != -1) {
        dst_start_caps.span[curve_i] = GP_STROKE_CAP_TYPE_FLAT;
      }
      if (inter_index_last != -1) {
        dst_end_caps.span[curve_i] = GP_STROKE_CAP_TYPE_FLAT;
      }
    }
  });

  dst_start_caps.finish();
  dst_end_caps.finish();
}

using EncodedConnection = int;
static constexpr EncodedConnection SEGMENT_CONNECTION_NULL = 0;

/* We store the side as sign, but because a segment with index zero is valid, we shift by one. */
static EncodedConnection encode_index_and_side(const int index, const Side side)
{
  return side == Side::Start ? index + 1 : -(index + 1);
}

static int decode_index(const EncodedConnection encoded)
{
  return math::abs(encoded) - 1;
}

static Side decode_side(const EncodedConnection encoded)
{
  return encoded < 0 ? Side::End : Side::Start;
}

/* Both the start and end of every segment is connected to two other segments or null. */
using SegmentConnections = VecBase<EncodedConnection, 2>;

static void create_connections_from_curves(const OffsetIndices<int> segments_by_curve,
                                           const Span<bool> segments_to_keep,
                                           const VArray<bool> &is_cyclic,
                                           MutableSpan<SegmentConnections> segment_connections)
{

  threading::parallel_for(segments_by_curve.index_range(), 4096, [&](const IndexRange curves) {
    for (const int curve_i : curves) {
      const IndexRange segment_range = segments_by_curve[curve_i];

      if (segment_range.size() == 1) {
        if (segments_to_keep[segment_range.first()]) {
          segment_connections[segment_range.first()][Side::Start] = SEGMENT_CONNECTION_NULL;
          segment_connections[segment_range.first()][Side::End] = SEGMENT_CONNECTION_NULL;
        }
        continue;
      }

      for (const int segment_i : segment_range.drop_back(1)) {
        if (!segments_to_keep[segment_i]) {
          continue;
        }

        if (segments_to_keep[segment_i + 1]) {
          segment_connections[segment_i][Side::End] = encode_index_and_side(segment_i + 1,
                                                                            Side::Start);
          segment_connections[segment_i + 1][Side::Start] = encode_index_and_side(segment_i,
                                                                                  Side::End);
        }
        else {
          segment_connections[segment_i][Side::End] = SEGMENT_CONNECTION_NULL;
        }
      }

      if (!segments_to_keep[segment_range.last()]) {
        continue;
      }

      if (!is_cyclic[curve_i]) {
        segment_connections[segment_range.first()][Side::Start] = SEGMENT_CONNECTION_NULL;
        segment_connections[segment_range.last()][Side::End] = SEGMENT_CONNECTION_NULL;
        continue;
      }

      if (segments_to_keep[segment_range.first()]) {
        segment_connections[segment_range.first()][Side::Start] = encode_index_and_side(
            segment_range.last(), Side::End);
        segment_connections[segment_range.last()][Side::End] = encode_index_and_side(
            segment_range.first(), Side::Start);
      }
      else {
        segment_connections[segment_range.last()][Side::End] = SEGMENT_CONNECTION_NULL;
      }
    }
  });
}

static void follow_segment_connections(const Span<Segment> all_segments,
                                       const Span<bool> segments_to_keep,
                                       const Span<SegmentConnections> segment_connections,
                                       Vector<Segment> &segments,
                                       Vector<int> &segment_offset_data,
                                       Vector<bool> &segment_reversed,
                                       Vector<bool> &cyclic)
{
  BLI_assert(all_segments.size() == segments_to_keep.size());
  BLI_assert(all_segments.size() == segment_connections.size());

  segment_offset_data.append(0);

  Array<bool> processed_segments(all_segments.size(), false);
  int start_segment = 0;

  auto get_next_unprocessed_segment = [&]() {
    /* All segment before `start_segment` are guaranteed to be processed, so skip search them.
     * This optimization make the algorithm `O(N)` instead of `O(N^2)`.*/
    const int empty_num = start_segment;
    const int first_segment = processed_segments.as_span().drop_front(empty_num).first_index_try(
        false);

    if (first_segment == -1) {
      return -1;
    }
    return first_segment + empty_num;
  };

  /* Mark all segments that are not to keep as processed. */
  for (const int seg_i : all_segments.index_range()) {
    if (!segments_to_keep[seg_i]) {
      processed_segments[seg_i] = true;
    }
  }

  start_segment = get_next_unprocessed_segment();

  /* Follow each segment until it loops or ends. */
  while (start_segment != -1) {
    Vector<Segment> curve_segments;
    Vector<bool> curve_segment_reversed;

    auto append_segment = [&](const Segment &current_segment, const bool current_backwards) {
      if (curve_segments.size() == 0) {
        curve_segments.append(current_segment);
        curve_segment_reversed.append(current_backwards);
        return;
      }
      /* Check if the last segment can be joined with this one. */
      if (!check_and_join_segments(curve_segments.last(), current_segment)) {
        curve_segments.append(current_segment);
        curve_segment_reversed.append(current_backwards);
      }
    };

    /* Loop backwards to find the first segment. */
    bool current_backwards = true;
    int current_i = start_segment;
    bool curve_done = false;
    while (!curve_done) {
      const EncodedConnection next_encoded =
          segment_connections[current_i][current_backwards ? Side::Start : Side::End];

      if (next_encoded == SEGMENT_CONNECTION_NULL) {
        curve_done = true;
        break;
      }

      const int next_segment = decode_index(next_encoded);
      const Side next_side = decode_side(next_encoded);

      current_i = next_segment;
      current_backwards = next_side == Side::End;

      if (next_segment == start_segment) {
        curve_done = true;
        break;
      }
    }

    /* Reverse the direction. */
    current_backwards = !current_backwards;
    const int first_segment = current_i;

    /* Loop through forwards, adding segments until ending or looping. */
    curve_done = false;
    bool curve_closed = false;
    while (!curve_done) {
      if (processed_segments[current_i] == true) {
        BLI_assert_unreachable();
        break;
      }

      const Segment &current_segment = all_segments[current_i];
      processed_segments[current_i] = true;
      append_segment(current_segment, current_backwards);

      const EncodedConnection next_encoded =
          segment_connections[current_i][current_backwards ? Side::Start : Side::End];

      if (next_encoded == SEGMENT_CONNECTION_NULL) {
        curve_done = true;
        curve_closed = false;
        if (curve_segments.size() == 1) {
          curve_closed = curve_segments.last().is_loop();
        }
        break;
      }

      const int next_segment = decode_index(next_encoded);
      const Side next_side = decode_side(next_encoded);

      /* Check if we are back to the start. */
      if (next_segment == first_segment) {
        curve_done = true;
        curve_closed = true;

        BLI_assert(next_side == Side::Start);

        /* Check if the last segment can be joined to the first one. */
        if (curve_segments.size() == 1) {
          Segment &segment = curve_segments.last();

          const float parameter_start = segment.points[Side::Start] +
                                        segment.intersection_factor[Side::Start];
          const float parameter_end = segment.points[Side::End] +
                                      segment.intersection_factor[Side::End];

          if ((parameter_end == parameter_start) ||
              (segment.intersection_index[Side::End] == segment.intersection_index[Side::Start] &&
               segment.intersection_index[Side::End] != -1))
          {
            if (segment.intersection_factor[Side::Start] == segment.intersection_factor[Side::End])
            {
              segment.full_wrap_loop = true;
            }
          }
          break;
        }
        if (check_and_join_segments(curve_segments.first(), curve_segments.last())) {
          curve_segments.remove_last();
          curve_segment_reversed.remove_last();
        }

        break;
      }

      BLI_assert(segments_to_keep[next_segment]);
      BLI_assert(!processed_segments[next_segment]);

      current_i = next_segment;
      current_backwards = next_side == Side::End;
    }

    segments.extend(curve_segments);
    segment_reversed.extend(curve_segment_reversed);

    segment_offset_data.append(segments.size());
    cyclic.append(curve_closed);

    start_segment = get_next_unprocessed_segment();
  }
}

/* Compute bounding boxes of curves in screen space. The bounding boxes are used to speed
 * up the search for intersecting curves. */
static void compute_bounding_boxes(const OffsetIndices<int> src_points_by_curve,
                                   const Span<float2> screen_space_positions,
                                   MutableSpan<Bounds<float2>> screen_space_bbox)
{
  threading::parallel_for(
      src_points_by_curve.index_range(), 512, [&](const IndexRange src_curves) {
        for (const int src_curve : src_curves) {
          Bounds<float2> &bbox = screen_space_bbox[src_curve];

          const IndexRange src_points = src_points_by_curve[src_curve];
          bbox = *bounds::min_max(screen_space_positions.slice(src_points));

          /* Add some padding, otherwise we could just miss intersections. */
          bbox.pad(BBOX_PADDING);
        }
      });
}

static void compute_bounding_boxes(const OffsetIndices<int> src_points_by_curve,
                                   const GroupedSpan<int> shapes,
                                   const Span<float2> screen_space_positions,
                                   MutableSpan<Bounds<float2>> screen_space_bbox)
{
  threading::parallel_for(shapes.index_range(), 512, [&](const IndexRange shape_ids) {
    for (const int shape_id : shape_ids) {
      std::optional<Bounds<float2>> bbox;

      const Span<int> shape = shapes[shape_id];
      for (const int curve_i : shape) {
        const IndexRange src_points = src_points_by_curve[curve_i];
        bbox = bounds::merge(bbox, *bounds::min_max(screen_space_positions.slice(src_points)));
      }

      BLI_assert(bbox);

      /* Add some padding, otherwise we could just miss intersections. */
      bbox->pad(BBOX_PADDING);

      screen_space_bbox[shape_id] = *bbox;
    }
  });
}

namespace trim {

static bool check_line_segment_lasso_intersection(const int2 &pos_a,
                                                  const int2 &pos_b,
                                                  const Span<int2> mcoords)
{
  Bounds<int2> bbox_ab{math::min(pos_a, pos_b), math::max(pos_a, pos_b)};
  bbox_ab.pad(BBOX_PADDING);

  /* Check the lasso bounding box first as an optimization. */
  if (bbox_ab.intersects_segment(pos_a, pos_b) &&
      BLI_lasso_is_edge_inside(mcoords, pos_a.x, pos_a.y, pos_b.x, pos_b.y, IS_CLIPPED))
  {
    return true;
  }
  return false;
}

static void check_segments_in_lasso(const Span<float2> screen_space_positions,
                                    const Span<Bounds<float2>> screen_space_bbox,
                                    const Span<int2> mcoords,
                                    const Span<Segment> all_segments,
                                    const IndexMask &editable_curves,
                                    const OffsetIndices<int> segments_by_curve,
                                    MutableSpan<bool> segments_to_keep)
{
  const Bounds<int2> bbox_lasso_int = *bounds::min_max(mcoords);
  const Bounds<float2> bbox_lasso{float2(bbox_lasso_int.min), float2(bbox_lasso_int.max)};

  editable_curves.foreach_index(
      [&](const int curve_i) {
        /* To speed things up: Do a bounding box check on the curve and the lasso area. */
        if (!bounds::intersect(bbox_lasso, screen_space_bbox[curve_i]).has_value()) {
          return;
        }

        const IndexRange &segment_range = segments_by_curve[curve_i];
        for (const int segment_i : segment_range) {
          const Segment &segment = all_segments[segment_i];

          const IndexRange point_range = segment.point_range();

          if (point_range.is_empty()) {
            const float start_factor = segment.intersection_factor[Side::Start];
            const int2 start_edge = segment.edge(Side::Start);
            const float end_factor = segment.intersection_factor[Side::End];
            const int2 end_edge = segment.edge(Side::End);
            const float2 pos_1 = math::interpolate(screen_space_positions[start_edge.x],
                                                   screen_space_positions[start_edge.y],
                                                   start_factor);
            const float2 pos_2 = math::interpolate(screen_space_positions[end_edge.x],
                                                   screen_space_positions[end_edge.y],
                                                   end_factor);

            if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
              segments_to_keep[segment_i] = false;
            }

            continue;
          }

          for (const int64_t i : point_range.drop_back(1)) {
            const int point_i1 = segment.wrap_index(i);
            const int point_i2 = segment.wrap_index(i + 1);

            const float2 pos_1 = screen_space_positions[point_i1];
            const float2 pos_2 = screen_space_positions[point_i2];

            if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
              segments_to_keep[segment_i] = false;
              continue;
            }
          }

          if (segment_range.size() == 1 && segment.is_loop()) {
            const float2 pos_1 = screen_space_positions[segment.wrap_index(point_range.first())];
            const float2 pos_2 = screen_space_positions[segment.wrap_index(point_range.last())];

            if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
              segments_to_keep[segment_i] = false;
              continue;
            }
          }
          else {
            if (segment.has_intersection(Side::Start)) {
              const float start_factor = segment.intersection_factor[Side::Start];
              const int2 start_edge = segment.edge(Side::Start);
              const float2 pos_1 = math::interpolate(screen_space_positions[start_edge.x],
                                                     screen_space_positions[start_edge.y],
                                                     start_factor);
              const float2 pos_2 = screen_space_positions[segment.wrap_index(point_range.first())];

              if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
                segments_to_keep[segment_i] = false;
                continue;
              }
            }

            if (segment.has_intersection(Side::End)) {
              const float end_factor = segment.intersection_factor[Side::End];
              const int2 end_edge = segment.edge(Side::End);
              const float2 pos_1 = screen_space_positions[segment.wrap_index(point_range.last())];
              const float2 pos_2 = math::interpolate(screen_space_positions[end_edge.x],
                                                     screen_space_positions[end_edge.y],
                                                     end_factor);

              if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
                segments_to_keep[segment_i] = false;
                continue;
              }
            }
          }
        }
      },
      exec_mode::grain_size(128));
}

bke::CurvesGeometry trim_curve_segments(const bke::CurvesGeometry &src,
                                        const Span<float2> screen_space_positions,
                                        const Span<int2> mcoords,
                                        const IndexMask &editable_curves,
                                        const IndexMask &visible_curves,
                                        const bool keep_caps)
{
  if (src.is_empty()) {
    return src;
  }

  const OffsetIndices<int> src_points_by_curve = src.points_by_curve();
  const VArray<bool> is_cyclic = src.cyclic();

  Array<Bounds<float2>> screen_space_bbox(src.curves_num());
  compute_bounding_boxes(src_points_by_curve, screen_space_positions, screen_space_bbox);

  Vector<IntersectionPoint> intersections;
  Array<int> all_segment_offset_data(src_points_by_curve.size() + 1);
  Vector<Segment> all_segments;

  Array<Vector<int>> inters_per_curves(src_points_by_curve.size());
  find_intersections_between_all_curves(screen_space_positions,
                                        screen_space_bbox,
                                        src_points_by_curve,
                                        is_cyclic,
                                        visible_curves,
                                        inters_per_curves,
                                        intersections);
  create_segments_from_intersections(inters_per_curves,
                                     src_points_by_curve,
                                     intersections,
                                     is_cyclic,
                                     all_segments,
                                     all_segment_offset_data.as_mutable_span().drop_back(1));
  store_segment_map_on_intersections(all_segments, intersections);
  const OffsetIndices<int> segments_by_curve = offset_indices::accumulate_counts_to_offsets(
      all_segment_offset_data);

  Array<bool> segments_to_keep(all_segments.size(), true);
  check_segments_in_lasso(screen_space_positions,
                          screen_space_bbox,
                          mcoords,
                          all_segments,
                          editable_curves,
                          segments_by_curve,
                          segments_to_keep.as_mutable_span());

  Array<SegmentConnections> segment_connections(all_segments.size(),
                                                SegmentConnections(SEGMENT_CONNECTION_NULL));
  create_connections_from_curves(
      segments_by_curve, segments_to_keep, is_cyclic, segment_connections.as_mutable_span());

  Vector<Segment> segments;
  Vector<int> segment_offset_data;
  Vector<bool> segment_reversed;
  Vector<bool> cyclic;
  follow_segment_connections(all_segments,
                             segments_to_keep,
                             segment_connections,
                             segments,
                             segment_offset_data,
                             segment_reversed,
                             cyclic);
  const OffsetIndices<int> segment_offsets = OffsetIndices<int>(segment_offset_data);

  bke::CurvesGeometry dst = create_curves_from_segments(
      src, segments, segment_reversed, cyclic, segment_offsets);

  if (!keep_caps) {
    cut_caps(dst, segments, segment_reversed, cyclic, segment_offsets);
  }

  return dst;
}

bke::CurvesGeometry trim_curve_segment_ends(const bke::CurvesGeometry &src,
                                            const Span<float2> screen_space_positions,
                                            const IndexMask &editable_curves,
                                            const IndexMask &visible_curves,
                                            const bool keep_caps)
{
  if (src.is_empty()) {
    return src;
  }

  const OffsetIndices<int> src_points_by_curve = src.points_by_curve();
  const VArray<bool> is_cyclic = src.cyclic();

  Array<Bounds<float2>> screen_space_bbox(src.curves_num());
  compute_bounding_boxes(src_points_by_curve, screen_space_positions, screen_space_bbox);

  Vector<IntersectionPoint> intersections;
  Array<int> all_segment_offset_data(src_points_by_curve.size() + 1);
  Vector<Segment> all_segments;

  Array<Vector<int>> inters_per_curves(src_points_by_curve.size());
  find_intersections_between_all_curves(screen_space_positions,
                                        screen_space_bbox,
                                        src_points_by_curve,
                                        is_cyclic,
                                        visible_curves,
                                        inters_per_curves,
                                        intersections);
  create_segments_from_intersections(inters_per_curves,
                                     src_points_by_curve,
                                     intersections,
                                     is_cyclic,
                                     all_segments,
                                     all_segment_offset_data.as_mutable_span().drop_back(1));
  store_segment_map_on_intersections(all_segments, intersections);
  const OffsetIndices<int> segments_by_curve = offset_indices::accumulate_counts_to_offsets(
      all_segment_offset_data);

  Array<bool> segments_to_keep(all_segments.size(), true);
  /* Remove the end segments unless that would delete the whole curve. */
  editable_curves.foreach_index(
      [&](const int curve_i) {
        const IndexRange segment_range = segments_by_curve[curve_i];

        if (segment_range.size() > 2) {
          segments_to_keep[segment_range.first()] = false;
          segments_to_keep[segment_range.last()] = false;
        }
      },
      exec_mode::grain_size(128));

  Array<SegmentConnections> segment_connections(all_segments.size(),
                                                SegmentConnections(SEGMENT_CONNECTION_NULL));
  create_connections_from_curves(
      segments_by_curve, segments_to_keep, is_cyclic, segment_connections.as_mutable_span());

  Vector<Segment> segments;
  Vector<int> segment_offset_data;
  Vector<bool> segment_reversed;
  Vector<bool> cyclic;
  follow_segment_connections(all_segments,
                             segments_to_keep,
                             segment_connections,
                             segments,
                             segment_offset_data,
                             segment_reversed,
                             cyclic);
  const OffsetIndices<int> segment_offsets = OffsetIndices<int>(segment_offset_data);

  bke::CurvesGeometry dst = create_curves_from_segments(
      src, segments, segment_reversed, cyclic, segment_offsets);

  if (!keep_caps) {
    cut_caps(dst, segments, segment_reversed, cyclic, segment_offsets);
  }

  return dst;
}

}  // namespace trim

namespace carver {

/**
 * This is a heavily modified implementation of the Greiner-Hormann clipping algorithm.
 *
 * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
 * Transactions on Graphics. 17 (2): 71-83.
 *
 * The original Greiner-Hormann algorithm works in three phases:
 *  1: Find all intersections and sort them.
 *  2: Set the direction of all intersection point (the paper call it `entry_exit`)
 *  3: Create all polygons by following the direction of each intersection point until it
 * loops.
 *
 * This implementation adds the following:
 *  1: Groups of curves, called `fills`. This allows for input geometry with holes.
 *  2: Curves can have no fill, so they will get cut.
 *
 * This implementation works by:
 *  1: Break one subject fill and all clipping fills into segments and store their intersections.
 *  2: Remove all segments that are not contributing.
 *  3: Follow each segment until it loops or terminates.
 *  4: Repeat for every `subject` fill.
 */

static int intersect(const float2 &P1,
                     const float2 &P2,
                     const float2 &Q1,
                     const float2 &Q2,
                     float *r_alpha_P,
                     float *r_alpha_Q)
{
  double r_lambda;
  double r_mu;
  const int val = isect_seg_seg_v2_lambda_mu_db(
      double2(P1), double2(P2), double2(Q1), double2(Q2), &r_lambda, &r_mu);

  *r_alpha_P = r_lambda;
  *r_alpha_Q = r_mu;

  return val;
}

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

constexpr float epms = 0.01f;
constexpr float epms_factor = 0.05f;

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

  const float2 mid_point = math::interpolate(point1, point2, 0.35421f);

  const float eps_dis = math::min(epms_factor * math::distance(point1, point2), epms);

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
static void check_segments(const CurveBooleanOpParameters &op_params,
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

struct BooleanResult {
  Vector<Segment> segments;
  Vector<bool> segment_reversed;
  Vector<int> segment_offsets;
  Vector<bool> cyclic;
  Vector<int> fill_ids;

  void append_result(const BooleanResult &other_result, const int fill_id)
  {
    if (other_result.segments.is_empty()) {
      return;
    }

    for (const int i : other_result.segment_offsets.index_range().drop_front(1)) {
      segment_offsets.append(other_result.segment_offsets[i] + segments.size());
    }
    cyclic.extend(other_result.cyclic);
    segment_reversed.extend(other_result.segment_reversed);
    fill_ids.append_n_times(fill_id, other_result.cyclic.size());

    for (const int i : other_result.segments.index_range()) {
      segments.append(std::move(other_result.segments[i]));
    }
  }
};

static void find_intersections_between_curves(const Span<float2> points_i,
                                              const Span<float2> points_j,
                                              const int curve_i,
                                              const int curve_j,
                                              const bool cyclic_i,
                                              const bool cyclic_j,
                                              const int point_offset_i,
                                              const int point_offset_j,
                                              Array<Vector<int>> &r_inters_per_curves,
                                              Vector<IntersectionPoint> &r_intersections)
{
  for (const int i : points_i.index_range().drop_back(cyclic_i ? 0 : 1)) {
    for (const int j : points_j.index_range().drop_back(cyclic_j ? 0 : 1)) {
      float alpha_a, alpha_b;
      const int val = intersect(points_i[i],
                                points_i[(i + 1) % points_i.size()],
                                points_j[j],
                                points_j[(j + 1) % points_j.size()],
                                &alpha_a,
                                &alpha_b);
      if (val == ISECT_LINE_LINE_CROSS || val == ISECT_LINE_LINE_EXACT) {
        r_inters_per_curves[curve_i].append(r_intersections.size());
        r_inters_per_curves[curve_j].append(r_intersections.size());
        r_intersections.append(create_intersection(
            i + point_offset_i, j + point_offset_j, alpha_a, alpha_b, curve_i, curve_j));
      }
    }
  }
}

static void find_intersections_between_shapes(const Span<float2> points,
                                              const GroupedSpan<int> shapes,
                                              const IndexMask &shapes_i,
                                              const IndexMask &shapes_j,
                                              const OffsetIndices<int> points_by_curve,
                                              const VArray<bool> &cyclic,
                                              const bool self_intersection,
                                              Array<Vector<int>> &r_inters_per_curves,
                                              Vector<IntersectionPoint> &r_intersections)
{
  shapes_i.foreach_index([&](const int shape_i) {
    const Span<int> curves_i = shapes[shape_i];
    for (const int curve_i : curves_i) {

      const IndexRange points_i = points_by_curve[curve_i];
      const bool cyclic_i = cyclic[curve_i];

      shapes_j.foreach_index([&](const int shape_j) {
        const Span<int> curves_j = shapes[shape_j];
        for (const int curve_j : curves_j) {
          if (self_intersection && curve_i >= curve_j) {
            return;
          }

          const IndexRange points_j = points_by_curve[curve_j];
          const bool cyclic_j = cyclic[curve_j];

          find_intersections_between_curves(points.slice(points_i),
                                            points.slice(points_j),
                                            curve_i,
                                            curve_j,
                                            cyclic_i,
                                            cyclic_j,
                                            points_i.first(),
                                            points_j.first(),
                                            r_inters_per_curves,
                                            r_intersections);
        }
      });
    }
  });
}

static void create_connections_from_intersection_points(
    const Span<IntersectionPoint> &intersections,
    const Span<bool> &segments_to_keep,
    MutableSpan<SegmentConnections> segment_connections)
{
  auto connect = [&](const EncodedConnection point_1, const EncodedConnection point_2) {
    segment_connections[decode_index(point_1)][decode_side(point_1)] = encode_index_and_side(
        decode_index(point_2), decode_side(point_2));
    segment_connections[decode_index(point_2)][decode_side(point_2)] = encode_index_and_side(
        decode_index(point_1), decode_side(point_1));
  };

  for (const int inter_id : intersections.index_range()) {
    const IntersectionPoint &inter = intersections[inter_id];

    const EncodedConnection start_a = encode_index_and_side(inter.segment_index_i[Side::Start],
                                                            Side::End);
    const EncodedConnection end_a = encode_index_and_side(inter.segment_index_i[Side::End],
                                                          Side::Start);
    const EncodedConnection start_b = encode_index_and_side(inter.segment_index_j[Side::Start],
                                                            Side::End);
    const EncodedConnection end_b = encode_index_and_side(inter.segment_index_j[Side::End],
                                                          Side::Start);
    const bool is_start_a = start_a == SEGMENT_CONNECTION_NULL ?
                                false :
                                segments_to_keep[decode_index(start_a)];
    const bool is_end_a = end_a == SEGMENT_CONNECTION_NULL ? false :
                                                             segments_to_keep[decode_index(end_a)];
    const bool is_start_b = start_b == SEGMENT_CONNECTION_NULL ?
                                false :
                                segments_to_keep[decode_index(start_b)];
    const bool is_end_b = end_b == SEGMENT_CONNECTION_NULL ? false :
                                                             segments_to_keep[decode_index(end_b)];

    if (is_start_a && is_end_a && is_start_b && is_end_b) {
      connect(start_a, end_a);
      connect(start_b, end_b);
    }
    else if (is_start_a && is_end_a && is_start_b && !is_end_b) {
      connect(start_a, end_a);
    }
    else if (is_start_a && is_end_a && !is_start_b && is_end_b) {
      connect(start_a, end_a);
    }
    else if (is_start_a && is_end_a && !is_start_b && !is_end_b) {
      connect(start_a, end_a);
    }
    else if (is_start_a && !is_end_a && is_start_b && is_end_b) {
      connect(start_b, end_b);
    }
    else if (is_start_a && !is_end_a && is_start_b && !is_end_b) {
      connect(start_a, start_b);
    }
    else if (is_start_a && !is_end_a && !is_start_b && is_end_b) {
      connect(start_a, end_b);
    }
    else if (is_start_a && !is_end_a && !is_start_b && !is_end_b) {
    }
    else if (!is_start_a && is_end_a && is_start_b && is_end_b) {
      connect(start_b, end_b);
    }
    else if (!is_start_a && is_end_a && is_start_b && !is_end_b) {
      connect(end_a, start_b);
    }
    else if (!is_start_a && is_end_a && !is_start_b && is_end_b) {
      connect(end_a, end_b);
    }
    else if (!is_start_a && is_end_a && !is_start_b && !is_end_b) {
    }
    else if (!is_start_a && !is_end_a && is_start_b && is_end_b) {
      connect(start_b, end_b);
    }
    else if (!is_start_a && !is_end_a && is_start_b && !is_end_b) {
    }
    else if (!is_start_a && !is_end_a && !is_start_b && is_end_b) {
    }
    else if (!is_start_a && !is_end_a && !is_start_b && !is_end_b) {
    }
  }
}

static BooleanResult execute_single_boolean(const CurveBooleanOpParameters op_params,
                                            const int subj_shape_id,
                                            const Span<float2> points,
                                            const GroupedSpan<int> shapes,
                                            const OffsetIndices<int> points_by_curve,
                                            const IndexMask &clipping_shapes,
                                            const VArray<bool> &is_fill,
                                            const VArray<bool> &cyclic)
{
  Vector<IntersectionPoint> intersections;

  Array<Vector<int>> inters_per_curves(points_by_curve.size());

  const IndexMask subj_shapes = IndexRange::from_single(subj_shape_id);

  find_intersections_between_shapes(points,
                                    shapes,
                                    subj_shapes,
                                    clipping_shapes,
                                    points_by_curve,
                                    cyclic,
                                    false,
                                    inters_per_curves,
                                    intersections);

  Vector<Segment> all_segments;
  Array<int> all_segment_offset_data(points_by_curve.size() + 1, 0);

  for (const int curve_i : shapes[subj_shape_id]) {
    all_segment_offset_data[curve_i] = create_segments_from_intersections_single_curve(
        curve_i, inters_per_curves, points_by_curve, intersections, cyclic, all_segments);
  }

  clipping_shapes.foreach_index([&](const int clip_shape_i) {
    const Span<int> curves_j = shapes[clip_shape_i];
    for (const int curve_j : curves_j) {
      all_segment_offset_data[curve_j] = create_segments_from_intersections_single_curve(
          curve_j, inters_per_curves, points_by_curve, intersections, cyclic, all_segments);
    }
  });

  store_segment_map_on_intersections(all_segments, intersections);
  const OffsetIndices<int> segments_by_curve = offset_indices::accumulate_counts_to_offsets(
      all_segment_offset_data);

  Array<bool> all_inside_left(all_segments.size());
  Array<bool> all_inside_right(all_segments.size());

  for (const int curve_i : shapes[subj_shape_id]) {
    check_segments(op_params,
                   curve_i,
                   true,
                   subj_shape_id,
                   points,
                   shapes,
                   points_by_curve,
                   clipping_shapes,
                   all_segments,
                   segments_by_curve,
                   is_fill,
                   all_inside_left,
                   all_inside_right);
  }
  clipping_shapes.foreach_index([&](const int clip_shape_id) {
    const Span<int> curves_j = shapes[clip_shape_id];
    for (const int curve_j : curves_j) {
      check_segments(op_params,
                     curve_j,
                     false,
                     subj_shape_id,
                     points,
                     shapes,
                     points_by_curve,
                     clipping_shapes,
                     all_segments,
                     segments_by_curve,
                     is_fill,
                     all_inside_left,
                     all_inside_right);
    }
  });

  Array<bool> segments_to_keep(all_segments.size(), true);
  for (const int segment_i : all_segments.index_range()) {
    const Segment &segment = all_segments[segment_i];

    if (is_fill[segment.curve]) {
      if (!all_inside_left[segment_i] ^ all_inside_right[segment_i]) {
        segments_to_keep[segment_i] = false;
      }
    }
    else {
      BLI_assert(all_inside_left[segment_i] == all_inside_right[segment_i]);
      if (all_inside_left[segment_i]) {
        segments_to_keep[segment_i] = false;
      }
    }
  }

  Array<SegmentConnections> segment_connections(all_segments.size(),
                                                SegmentConnections(SEGMENT_CONNECTION_NULL));
  create_connections_from_intersection_points(
      intersections, segments_to_keep, segment_connections);

  BooleanResult result;
  follow_segment_connections(all_segments,
                             segments_to_keep,
                             segment_connections,
                             result.segments,
                             result.segment_offsets,
                             result.segment_reversed,
                             result.cyclic);

  return result;
}

static BooleanResult execute_boolean(const CurveBooleanOpParameters op_params,
                                     const Span<float2> positions_2d,
                                     const OffsetIndices<int> points_by_curve,
                                     const IndexMask &clipping_shapes,
                                     const GroupedSpan<int> shapes,
                                     const VArray<int> &fill_ids,
                                     const VArray<bool> &is_cyclic)
{
  BooleanResult results_all;
  results_all.segment_offsets.append(0);

  if (points_by_curve.is_empty()) {
    return results_all;
  }

  IndexMaskMemory memory;
  const IndexMask subject_shapes = clipping_shapes.complement(shapes.index_range(), memory);

  /* Treat filled curves as cyclical. */
  const VArray<bool> cyclic = VArray<bool>::from_func(points_by_curve.size(), [&](int64_t index) {
    return is_cyclic[index] || (fill_ids[index] != 0);
  });

  const VArray<bool> is_fill = VArray<bool>::from_func(points_by_curve.size(), [&](int64_t index) {
    const IndexRange points = points_by_curve[index];

    if (points.size() <= 2) {
      return false;
    }

    return fill_ids[index] != 0;
  });

  Array<Bounds<float2>> screen_space_bbox(shapes.size());
  compute_bounding_boxes(points_by_curve, shapes, positions_2d, screen_space_bbox);

  Array<bool> shapes_bbox_intersecting(shapes.size(), false);
  index_mask::masked_fill(shapes_bbox_intersecting.as_mutable_span(), true, clipping_shapes);

  /* Calculate which bounding boxes intersect the clipping shapes. */
  subject_shapes.foreach_index([&](const int64_t shape_i) {
    const Bounds<float2> bbox_i = screen_space_bbox[shape_i];

    bool box_intersects = false;

    clipping_shapes.foreach_index([&](const int64_t shape_j) {
      const Bounds<float2> bbox_j = screen_space_bbox[shape_j];

      if (bounds::intersect(bbox_i, bbox_j).has_value()) {
        box_intersects = true;
      }
    });

    if (box_intersects) {
      shapes_bbox_intersecting[shape_i] = true;
    }
  });

  subject_shapes.foreach_index([&](const int64_t shape_i) {
    /* Only do full calculation with the bounding boxes are intersecting. */
    if (shapes_bbox_intersecting[shape_i]) {
      const BooleanResult result = execute_single_boolean(op_params,
                                                          shape_i,
                                                          positions_2d,
                                                          shapes,
                                                          points_by_curve,
                                                          clipping_shapes,
                                                          is_fill,
                                                          cyclic);

      results_all.append_result(result, shape_i);
    }
    else {
      BooleanResult result;
      result.segment_offsets.append(0);

      const Span<int> shape = shapes[shape_i];
      for (const int pos_j : shape.index_range()) {
        const int curve_j = shape[pos_j];

        result.segments.append(
            Segment::from_curve(curve_j, points_by_curve[curve_j], is_cyclic[curve_j]));
        result.cyclic.append(is_cyclic[curve_j]);
        result.segment_offsets.append(pos_j + 1);
        result.segment_reversed.append(false);
      }

      results_all.append_result(result, shape_i);
    }
  });

  if (results_all.segments.is_empty()) {
    return results_all;
  }

  return results_all;
}

static bke::CurvesGeometry create_curves_from_segments(const bke::CurvesGeometry &src,
                                                       const Span<Segment> segments,
                                                       const Span<bool> segment_reversed,
                                                       const Span<bool> cyclic,
                                                       const Span<bool> is_segments_clipping,
                                                       const Span<int> dst_to_src_curves,
                                                       const OffsetIndices<int> segment_offsets,
                                                       const bool skip_clipping_attributes,
                                                       Vector<bool> &is_point_clipping)
{
  Array<bool> unchanged_curves(segment_offsets.size(), false);

  for (const int dst_curve_i : segment_offsets.index_range()) {
    const IndexRange segment_range = segment_offsets[dst_curve_i];
    if (segment_range.size() != 1) {
      continue;
    }

    const int segment_i = segment_range.first();
    const Segment &segment = segments[segment_i];
    if (segment.has_intersection(Side::Start) || segment.has_intersection(Side::End)) {
      continue;
    }

    if (is_segments_clipping[segment_i]) {
      continue;
    }

    unchanged_curves[dst_curve_i] = true;
  }

  IndexMaskMemory memory;
  const IndexMask unchanged_curves_mask = IndexMask::from_bools(unchanged_curves, memory);

  struct InterpolatePoint {
    int dst_point;
    int src_point_1;
    int src_point_2;
    float factor;
  };

  Array<int> point_offsets(segment_offsets.size() + 1);
  Vector<int2> points_to_copy;
  Vector<int2> clipping_points_to_copy;
  Vector<IndexRange> ranges_to_reverse;
  Vector<InterpolatePoint> point_to_interpolate;

  int i = 0;
  for (const int curve_i : segment_offsets.index_range()) {
    point_offsets[curve_i] = i;

    const bool unchanged = unchanged_curves[curve_i];

    const IndexRange segment_range = segment_offsets[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = segments[seg_i];
      const bool reversed = segment_reversed[seg_i];
      const bool is_clipping = is_segments_clipping[seg_i];
      const int point_num = segment.points_num();

      if (segment.has_intersection(reversed ? Side::End : Side::Start) && !segment.is_loop()) {
        const float start_alpha = segment.intersection_factor[reversed ? Side::End : Side::Start];
        const int2 start_edge = segment.edge(reversed ? Side::End : Side::Start);
        point_to_interpolate.append({i, start_edge.x, start_edge.y, start_alpha});
        is_point_clipping.append(is_clipping);
        i++;
      }

      is_point_clipping.append_n_times(is_clipping, point_num);

      if (!unchanged && !is_clipping) {
        segment.foreach_point(
            [&](const int index, const int pos) { points_to_copy.append(int2(pos + i, index)); });

        if (reversed) {
          ranges_to_reverse.append(IndexRange::from_begin_size(i, point_num));
        }
      }
      if (is_clipping) {
        if (reversed) {
          segment.foreach_point([&](const int index, const int pos) {
            clipping_points_to_copy.append(int2(point_num - 1 - pos + i, index));
          });
        }
        else {
          segment.foreach_point([&](const int index, const int pos) {
            clipping_points_to_copy.append(int2(pos + i, index));
          });
        }
      }

      i += point_num;

      if (seg_i == segment_range.last() &&
          segment.has_intersection(reversed ? Side::Start : Side::End) && !cyclic[curve_i])
      {
        const float end_alpha = segment.intersection_factor[reversed ? Side::Start : Side::End];
        const int2 end_edge = segment.edge(reversed ? Side::Start : Side::End);
        point_to_interpolate.append({i, end_edge.x, end_edge.y, end_alpha});
        is_point_clipping.append(is_clipping);
        i++;
      }
    }
  }
  point_offsets.last() = i;

  const OffsetIndices<int> dst_points_by_curve = OffsetIndices<int>(point_offsets);

  const bke::AttributeAccessor src_attributes = src.attributes();
  const VArray<bool> src_cyclic = src.cyclic();
  bke::CurvesGeometry dst_curves(dst_points_by_curve.total_size(), dst_points_by_curve.size());
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  dst_curves.offsets_for_write().copy_from(dst_points_by_curve.data());

  Vector<InterpolatePoint> clipping_point_to_interpolate;
  for (const int curve_i : dst_points_by_curve.index_range()) {
    const IndexRange points = dst_points_by_curve[curve_i];
    const Span<bool> range_is_clipping = is_point_clipping.as_span().slice(points);

    const IndexMask clipping_ranges = IndexMask::from_bools(range_is_clipping, memory);
    clipping_ranges.foreach_range([&](const IndexRange &range) {
      const IndexRange full_range = range.shift(points.first());
      int range_first = int(full_range.first() - 1);
      int range_last = int(full_range.last() + 1);
      if (range_first == points.first() - 1) {
        range_first = points.last();
      }
      if (range_last == points.last() + 1) {
        range_last = points.first();
      }
      for (const int i : range.index_range()) {
        const float t = (i + 1.0f) / (range.size() + 1.0f);
        clipping_point_to_interpolate.append({int(full_range[i]), range_first, range_last, t});
      }
    });
  }

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         dst_to_src_curves,
                         dst_attributes);

  MutableSpan<bool> dst_cyclic = dst_curves.cyclic_for_write();
  dst_cyclic.copy_from(cyclic);
  unchanged_curves_mask.foreach_index(
      [&](const int i) { dst_cyclic[i] = src_cyclic[dst_to_src_curves[i]]; });

  const OffsetIndices<int> src_points_by_curve = src.points_by_curve();

  /* Copy/Interpolate point attributes. */
  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes, dst_attributes, {bke::AttrDomain::Point}, {}))
  {
    bke::attribute_math::to_static_type(attribute.dst.span.type(), [&]<typename T>() {
      auto src_attr = attribute.src.typed<T>();
      auto dst_attr = attribute.dst.span.typed<T>();

      for (const InterpolatePoint &interpolate_point : point_to_interpolate) {
        dst_attr[interpolate_point.dst_point] = bke::attribute_math::mix2<T>(
            interpolate_point.factor,
            src_attr[interpolate_point.src_point_1],
            src_attr[interpolate_point.src_point_2]);
      }
      for (const int2 index : points_to_copy) {
        dst_attr[index.x] = src_attr[index.y];
      }
      for (const int2 index : clipping_points_to_copy) {
        dst_attr[index.x] = src_attr[index.y];
      }
      for (const IndexRange range : ranges_to_reverse) {
        dst_attr.slice(range).reverse();
      }
    });

    attribute.dst.finish();
  }

  if (skip_clipping_attributes) {
    src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
      if (iter.domain != bke::AttrDomain::Point) {
        return;
      }
      if (iter.data_type == bke::AttrType::String) {
        return;
      }
      const GVArraySpan src = *iter.get(bke::AttrDomain::Point);
      bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
          iter.name, bke::AttrDomain::Point, iter.data_type);
      if (!dst) {
        return;
      }
      unchanged_curves_mask.foreach_index([&](const int i) {
        dst.span.slice(dst_points_by_curve[i])
            .copy_from(src.slice(src_points_by_curve[dst_to_src_curves[i]]));
      });

      if (iter.name != ".positions_2d") {
        GMutableSpan attribute_data = dst.span;
        bke::attribute_math::to_static_type(attribute_data.type(), [&]<typename T>() {
          MutableSpan<T> span_data = attribute_data.typed<T>();

          for (const InterpolatePoint &interpolate_point : clipping_point_to_interpolate) {
            span_data[interpolate_point.dst_point] = bke::attribute_math::mix2<T>(
                interpolate_point.factor,
                span_data[interpolate_point.src_point_1],
                span_data[interpolate_point.src_point_2]);
          }
        });
      }

      dst.finish();
    });
  }

  return dst_curves;
}

static float4 transform_plane(const float4x4 &mat, const float4 &plane)
{
  float3 normal = float3(plane);
  float3 point = -normal * plane.w;

  normal = math::transform_direction(mat, normal);
  point = math::transform_point(mat, point);

  return float4(normal, -math::dot(normal, point));
}

static void separate_fill_islands(bke::CurvesGeometry &dst)
{
  bke::MutableAttributeAccessor attributes = dst.attributes_for_write();
  bke::SpanAttributeWriter<int> fill_ids = attributes.lookup_for_write_span<int>("fill_id");
  const VArray<float2> dst_positions_2d_attribute = *attributes.lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  BLI_assert(dst_positions_2d_attribute.is_span());
  const Span<float2> dst_positions_2d = dst_positions_2d_attribute.get_internal_span();

  const OffsetIndices<int> points_by_curve = dst.points_by_curve();

  auto is_curve_i_in_j = [&](const int curve_i, const int curve_j) {
    const IndexRange points_i = points_by_curve[curve_i];
    const IndexRange points_j = points_by_curve[curve_j];

    const Span<float2> poly_i = dst_positions_2d.slice(points_i);
    const Span<float2> poly_j = dst_positions_2d.slice(points_j);

    const int winding = point_in_polygon_winding_int(poly_i.first(), poly_j);

    return winding % 2 != 0;
  };

  Array<bool> is_curve_child(points_by_curve.size(), false);
  Array<Vector<int>> curve_children(points_by_curve.size());

  for (const int curve_i : points_by_curve.index_range()) {
    if (fill_ids.span[curve_i] == 0) {
      continue;
    }

    for (const int curve_j : points_by_curve.index_range()) {
      if (fill_ids.span[curve_j] != fill_ids.span[curve_i]) {
        continue;
      }

      if (is_curve_i_in_j(curve_i, curve_j)) {
        is_curve_child[curve_j] = true;
        curve_children[curve_i].append(curve_j);
      }
    }
  }

  int new_fill_id = bke::greasepencil::get_next_available_fill_id(fill_ids.span.varray());

  for (const int curve_i : points_by_curve.index_range()) {
    if (is_curve_child[curve_i]) {
      continue;
    }

    if (fill_ids.span[curve_i] == 0) {
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

bke::CurvesGeometry curve_boolean(const CurveBooleanOpParameters op_params,
                                  const bke::CurvesGeometry &curves,
                                  const GroupedSpan<int> shapes,
                                  const IndexMask &clipping_shapes)
{
  const bke::AttributeAccessor src_attributes = curves.attributes();

  const VArray<float2> src_positions_2d_attribute = *src_attributes.lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  BLI_assert(src_positions_2d_attribute.is_span());
  const Span<float2> src_positions_2d = src_positions_2d_attribute.get_internal_span();

  const VArray<int> fill_ids = *src_attributes.lookup<int>("fill_id", bke::AttrDomain::Curve);

  const BooleanResult result = execute_boolean(op_params,
                                               src_positions_2d,
                                               curves.points_by_curve(),
                                               clipping_shapes,
                                               shapes,
                                               fill_ids,
                                               curves.cyclic());

  if (result.segments.is_empty()) {
    return bke::CurvesGeometry();
  }

  const OffsetIndices<int> dst_segments_by_curve = OffsetIndices<int>(result.segment_offsets);

  Array<bool> is_src_curve_clipping(curves.curves_num(), false);
  for (const int curve_i : curves.curves_range()) {
    if (curve_i == curves.curves_range().last()) {
      is_src_curve_clipping[curve_i] = true;
    }
  }

  Array<bool> is_segments_clipping(result.segments.size());
  for (const int seg_i : result.segments.index_range()) {
    const Segment &segment = result.segments[seg_i];
    is_segments_clipping[seg_i] = is_src_curve_clipping[segment.curve];
  }

  Array<int> dst_to_src_curves(dst_segments_by_curve.size());
  for (const int i : dst_segments_by_curve.index_range()) {
    const IndexRange segment_range = dst_segments_by_curve[i];
    const int fill_id = result.fill_ids[i];
    dst_to_src_curves[i] = fill_id;

    /* Prioritize non-clipping curves. */
    for (const int seg_i : segment_range) {
      const int curve_i = result.segments[seg_i].curve;
      if (!is_src_curve_clipping[curve_i]) {
        dst_to_src_curves[i] = curve_i;
        break;
      }
    }
  }

  Vector<bool> is_point_clipping;
  bke::CurvesGeometry dst_curves = create_curves_from_segments(curves,
                                                               result.segments,
                                                               result.segment_reversed,
                                                               result.cyclic,
                                                               is_segments_clipping,
                                                               dst_to_src_curves,
                                                               dst_segments_by_curve,
                                                               op_params.skip_clipping_attributes,
                                                               is_point_clipping);

  if (!op_params.keep_caps) {
    cut_caps(dst_curves,
             result.segments,
             result.segment_reversed,
             result.cyclic,
             dst_segments_by_curve);
  }

  if (op_params.separate_islands) {
    separate_fill_islands(dst_curves);
  }

  return dst_curves;
}

bke::CurvesGeometry curve_boolean_with_planes(const CurveBooleanOpParameters op_params,
                                              const bke::CurvesGeometry &curves,
                                              const GroupedSpan<int> shapes,
                                              const Span<float4> normal_planes,
                                              const IndexMask &clipping_shapes,
                                              const float4x4 &layer_to_world,
                                              const ARegion &region)
{
  const bke::AttributeAccessor src_attributes = curves.attributes();

  const VArray<float2> src_positions_2d_attribute = *src_attributes.lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  BLI_assert(src_positions_2d_attribute.is_span());
  const Span<float2> src_positions_2d = src_positions_2d_attribute.get_internal_span();

  const VArray<int> fill_ids = *src_attributes.lookup<int>("fill_id", bke::AttrDomain::Curve);

  const BooleanResult result = execute_boolean(op_params,
                                               src_positions_2d,
                                               curves.points_by_curve(),
                                               clipping_shapes,
                                               shapes,
                                               fill_ids,
                                               curves.cyclic());

  if (result.segments.is_empty()) {
    return bke::CurvesGeometry();
  }

  const OffsetIndices<int> dst_segments_by_curve = OffsetIndices<int>(result.segment_offsets);

  Array<bool> is_src_curve_clipping(curves.curves_num(), false);
  for (const int curve_i : curves.curves_range()) {
    if (curve_i == curves.curves_range().last()) {
      is_src_curve_clipping[curve_i] = true;
    }
  }

  Array<bool> is_segments_clipping(result.segments.size());
  for (const int seg_i : result.segments.index_range()) {
    const Segment &segment = result.segments[seg_i];
    is_segments_clipping[seg_i] = is_src_curve_clipping[segment.curve];
  }

  Array<int> dst_to_src_curves(dst_segments_by_curve.size());
  for (const int i : dst_segments_by_curve.index_range()) {
    const IndexRange segment_range = dst_segments_by_curve[i];
    const int fill_id = result.fill_ids[i];
    dst_to_src_curves[i] = fill_id;

    /* Prioritize non-clipping curves. */
    for (const int seg_i : segment_range) {
      const int curve_i = result.segments[seg_i].curve;
      if (!is_src_curve_clipping[curve_i]) {
        dst_to_src_curves[i] = curve_i;
        break;
      }
    }
  }

  Vector<bool> is_point_clipping;
  bke::CurvesGeometry dst_curves = create_curves_from_segments(curves,
                                                               result.segments,
                                                               result.segment_reversed,
                                                               result.cyclic,
                                                               is_segments_clipping,
                                                               dst_to_src_curves,
                                                               dst_segments_by_curve,
                                                               op_params.skip_clipping_attributes,
                                                               is_point_clipping);

  const VArray<float2> dst_positions_2d_attribute = *dst_curves.attributes().lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  const OffsetIndices<int> points_by_curve = dst_curves.points_by_curve();
  BLI_assert(dst_positions_2d_attribute.is_span());
  const Span<float2> positions_2d = dst_positions_2d_attribute.get_internal_span();

  MutableSpan<float3> positions = dst_curves.positions_for_write();
  const float4x4 world_to_layer = math::invert(layer_to_world);
  for (const int curve_i : dst_curves.curves_range()) {
    const int src_curve = dst_to_src_curves[curve_i];
    const float4 &plane = transform_plane(layer_to_world, normal_planes[src_curve]);
    const IndexRange points = points_by_curve[curve_i];

    for (const int point_i : points) {
      if (is_point_clipping[point_i]) {
        ED_view3d_win_to_3d_on_plane(
            &region, plane, positions_2d[point_i], false, positions[point_i]);
        positions[point_i] = math::transform_point(world_to_layer, positions[point_i]);
      }
    }
  }

  if (!op_params.keep_caps) {
    cut_caps(dst_curves,
             result.segments,
             result.segment_reversed,
             result.cyclic,
             dst_segments_by_curve);
  }

  if (op_params.separate_islands) {
    separate_fill_islands(dst_curves);
  }

  return dst_curves;
}

}  // namespace carver

}  // namespace blender::ed::greasepencil
