/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_straight_skeleton.hh"

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name 2D Geometric Primitives for Verification
 * \{ */

inline double orient2d(const double2 &a, const double2 &b, const double2 &c)
{
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline bool point_on_segment_interior(const double2 &p,
                                      const double2 &a,
                                      const double2 &b,
                                      const double tol = 1e-6)
{
  const double2 ab = b - a;
  const double len_sq = math::length_squared(ab);
  if (len_sq < tol * tol) {
    return false;
  }
  const double cross = std::abs(orient2d(a, b, p));
  if (cross / std::sqrt(len_sq) > tol) {
    return false;
  }
  const double dot = math::dot(p - a, ab);
  if (dot > tol * std::sqrt(len_sq) && dot < len_sq - tol * std::sqrt(len_sq)) {
    return true;
  }
  return false;
}

/**
 * Returns true if segment (a, b) and segment (c, d) intersect improperly:
 * - either strict interior intersection (crossing like an X)
 * - or one endpoint lies strictly inside the other segment
 * - or they overlap collinearly with positive length
 */
inline bool segments_intersect_improper(const double2 &a,
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
  const bool a_c = math::length(a - c) < tol;
  const bool a_d = math::length(a - d) < tol;
  const bool b_c = math::length(b - c) < tol;
  const bool b_d = math::length(b - d) < tol;
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

inline bool verify_straight_skeleton(const StraightSkeletonInput &input,
                                     const StraightSkeletonOutput &output,
                                     std::string &r_error)
{
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
      area += (p1.x * p2.y - p2.x * p1.y);
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
      domain_area += (p1.x * p2.y - p2.x * p1.y);
    }
    domain_area *= 0.5;

    for (const int h : IndexRange(input.hole_offsets.size())) {
      const IndexRange r = input.hole_offsets[h];
      const int h_num = int(r.size());
      double hole_area = 0.0;
      for (const int i : IndexRange(h_num)) {
        const double2 p1 = input.vert[input.hole_vert_indices[r[i]]];
        const double2 p2 = input.vert[input.hole_vert_indices[r[(i + 1) % h_num]]];
        hole_area += (p1.x * p2.y - p2.x * p1.y);
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

}  // namespace blender::straightskeleton
