/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include <optional>

#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"

namespace blender::math {

/* -------------------------------------------------------------------- */
/** \name Normals & Area
 * \{ */

/**
 * Compute the normalized normal of a quad (v1, v2, v3, v4).
 * Uses the cross product of the two diagonals, so the result is
 * correct for planar quads and a reasonable approximation for
 * slightly non-planar ones.
 * The result is not guaranteed to be non-degenerate.
 */
template<typename T>
[[nodiscard]] inline VecBase<T, 3> normal_quad(const VecBase<T, 3> &v1,
                                               const VecBase<T, 3> &v2,
                                               const VecBase<T, 3> &v3,
                                               const VecBase<T, 3> &v4)
{
  return normalize(cross(v1 - v3, v2 - v4));
}

/**
 * Compute the area of a triangle (v1, v2, v3) in 3D space.
 */
template<typename T>
[[nodiscard]] inline T area_tri(const VecBase<T, 3> &v1,
                                const VecBase<T, 3> &v2,
                                const VecBase<T, 3> &v3)
{
  return T(0.5) * length(cross(v1 - v2, v2 - v3));
}

/**
 * Compute the cotangent of the angle at vertex \a v1 in the
 * triangle (v1, v2, v3). Returns 0 for degenerate triangles.
 */
template<typename T>
[[nodiscard]] inline T cotangent_tri_weight(const VecBase<T, 3> &v1,
                                            const VecBase<T, 3> &v2,
                                            const VecBase<T, 3> &v3)
{
  const VecBase<T, 3> a = v2 - v1;
  const VecBase<T, 3> b = v3 - v1;
  const T c = length(cross(a, b));
  return (c > std::numeric_limits<T>::epsilon()) ? (dot(a, b) / c) : T(0);
}

/**
 * Compute the angle (in radians) at vertex \a b in the triangle (\a a, \a b, \a c).
 * Returns 0 for degenerate cases where either edge from \a b has zero length.
 *
 * This is the generalized equivalent of the C-API #angle_v3v3v3, but works for
 * any floating-point type (including `double`).
 */
template<typename T>
[[nodiscard]] inline T angle_on_tri(const VecBase<T, 3> &a,
                                    const VecBase<T, 3> &b,
                                    const VecBase<T, 3> &c)
{
  const VecBase<T, 3> d1 = normalize(a - b);
  const VecBase<T, 3> d2 = normalize(c - b);
  return safe_acos(-dot(d1, d2));
}

/**
 * Return the factor by which a displacement along \a face_normal must be scaled
 * to obtain a displacement of unit length along \a vert_normal.
 *
 * Both vectors must be normalized. This is the generalized, double-capable
 * equivalent of the C-API #shell_v3v3_normalized_to_dist.
 */
template<typename T>
[[nodiscard]] inline T shell_angle_to_dist(const VecBase<T, 3> &vert_normal,
                                           const VecBase<T, 3> &face_normal)
{
  const T angle_cos = std::abs(dot(vert_normal, face_normal));
  return (UNLIKELY(angle_cos < std::numeric_limits<T>::epsilon())) ? T(1) : (T(1) / angle_cos);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Planes
 *
 * Planes are represented as a #float4 (or #VecBase<T, 4>) where the
 * first three components are the plane normal and the fourth component
 * is the signed offset such that `dot(normal, p) + w == 0` for any
 * point \a p on the plane.
 * \{ */

/**
 * Build a plane from a point and a (not necessarily unit-length) normal.
 */
template<typename T>
[[nodiscard]] inline VecBase<T, 4> plane_from_point_normal(const VecBase<T, 3> &co,
                                                           const VecBase<T, 3> &no)
{
  return VecBase<T, 4>(no.x, no.y, no.z, -dot(no, co));
}

/**
 * Return the signed distance of \a co from \a plane.
 * Positive values are on the side the normal points toward.
 *
 * \note The plane normal need not be unit length; the result is scaled accordingly.
 */
template<typename T>
[[nodiscard]] inline T plane_point_side(const VecBase<T, 4> &plane, const VecBase<T, 3> &co)
{
  return co.x * plane.x + co.y * plane.y + co.z * plane.z + plane.w;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Distance & Closest Points
 * \{ */

/**
 * Closest point on the line segment (\a l1, \a l2) to point \a p.
 * Works for any number of dimensions \a Size.
 */
template<typename T, int Size>
[[nodiscard]] inline VecBase<T, Size> closest_to_line_segment(const VecBase<T, Size> &p,
                                                              const VecBase<T, Size> &l1,
                                                              const VecBase<T, Size> &l2)
{
  const VecBase<T, Size> d = l2 - l1;
  const T len_sq = length_squared(d);
  if (len_sq == T(0)) {
    return l1;
  }
  const T lambda = dot(p - l1, d) / len_sq;
  if (lambda <= T(0)) {
    return l1;
  }
  if (lambda >= T(1)) {
    return l2;
  }
  return l1 + lambda * d;
}

/**
 * Squared distance from point \a p to the line segment (\a l1, \a l2).
 * Works for any number of dimensions \a Size.
 */
template<typename T, int Size>
[[nodiscard]] inline T dist_squared_to_line_segment(const VecBase<T, Size> &p,
                                                    const VecBase<T, Size> &l1,
                                                    const VecBase<T, Size> &l2)
{
  return length_squared(p - closest_to_line_segment(p, l1, l2));
}

/**
 * Closest point on a plane to \a pt, for a plane with unit-length normal.
 *
 * \note The plane normal \a (plane.x, plane.y, plane.z) must be unit length.
 */
template<typename T>
[[nodiscard]] inline VecBase<T, 3> closest_to_plane_normalized(const VecBase<T, 4> &plane,
                                                               const VecBase<T, 3> &pt)
{
  const VecBase<T, 3> n(plane.x, plane.y, plane.z);
  return pt - (dot(n, pt) + plane.w) * n;
}

/**
 * Closest point on a plane to \a pt.
 *
 * \note The plane normal need not be unit length. For unit normals,
 * prefer #closest_to_plane_normalized which avoids a division.
 */
template<typename T>
[[nodiscard]] inline VecBase<T, 3> closest_to_plane(const VecBase<T, 4> &plane,
                                                    const VecBase<T, 3> &pt)
{
  const VecBase<T, 3> n(plane.x, plane.y, plane.z);
  const T len_sq = length_squared(n);
  const T side = plane_point_side(plane, pt);
  return pt - (side / len_sq) * n;
}

/**
 * Squared distance from point \a p to \a plane.
 *
 * \note The plane normal need not be unit length.
 */
template<typename T>
[[nodiscard]] inline T dist_squared_to_plane(const VecBase<T, 3> &p, const VecBase<T, 4> &plane)
{
  const VecBase<T, 3> n(plane.x, plane.y, plane.z);
  const T len_sq = length_squared(n);
  const T side = plane_point_side(plane, p);
  const T fac = side / len_sq;
  return len_sq * (fac * fac);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projection Factors
 * \{ */

/**
 * Return the scalar factor \a lambda such that
 * `l1 + lambda * (l2 - l1)` is the closest point on the infinite line
 * through (\a l1, \a l2) to \a p.
 * Returns 0 when the line is degenerate (l1 == l2).
 * Works for any number of dimensions \a Size.
 */
template<typename T, int Size>
[[nodiscard]] inline T line_point_factor(const VecBase<T, Size> &p,
                                         const VecBase<T, Size> &l1,
                                         const VecBase<T, Size> &l2)
{
  const VecBase<T, Size> d = l2 - l1;
  const T len_sq = length_squared(d);
  if (len_sq == T(0)) {
    return T(0);
  }
  return dot(p - l1, d) / len_sq;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Matrix Utilities
 * \{ */

/**
 * Build a 3x3 matrix whose rows map 3D coordinates so that the
 * dominant two axes of \a normal become the X and Y columns, allowing
 * vectors to be projected into the plane of \a normal.
 *
 * Works for any floating-point scalar type \a T (including `double`).
 * \param normal: A unit-length vector.
 */
template<typename T>
[[nodiscard]] inline MatBase<T, 3, 3> axis_dominant_to_m3(const VecBase<T, 3> &normal)
{
  /* Build two unit vectors orthogonal to normal using the same method as
   * #ortho_basis_v3v3_v3, then assemble them as the rows of the projection
   * matrix (equivalent to transposing the column-orthonormal basis). */
  const T eps = std::numeric_limits<T>::epsilon();
  const T f = normal.x * normal.x + normal.y * normal.y;
  VecBase<T, 3> r0, r1;
  if (f > eps) {
    const T d = T(1) / std::sqrt(f);
    r0 = VecBase<T, 3>(normal.y * d, -normal.x * d, T(0));
    r1 = VecBase<T, 3>(-normal.z * r0.y, normal.z * r0.x, normal.x * r0.y - normal.y * r0.x);
  }
  else {
    /* Degenerate: normal is nearly aligned with Z. */
    const T s = (normal.z < T(0)) ? T(-1) : T(1);
    r0 = VecBase<T, 3>(s, T(0), T(0));
    r1 = VecBase<T, 3>(T(0), T(1), T(0));
  }
  /* Rows of the matrix are the two tangent basis vectors and the normal.
   * The transpose maps world-space to the (r0, r1, normal) frame, so
   * the first two components of the result are the 2-D projection. */
  return MatBase<T, 3, 3>(r0, r1, normal);
}

/**
 * Like #axis_dominant_to_m3 but flips the normal, so the projected Z
 * coordinate is negated. This is the equivalent of #axis_dominant_v3_to_m3_negate.
 *
 * \param normal: A unit-length vector.
 */
template<typename T>
[[nodiscard]] inline MatBase<T, 3, 3> axis_dominant_to_m3_negate(const VecBase<T, 3> &normal)
{
  return axis_dominant_to_m3<T>(-normal);
}

/**
 * Project the 3-D vector \a v onto the 2-D plane whose projection matrix is \a m.
 * Equivalent to #mul_v2_m3v3: returns only the first two components of \a m * \a v.
 *
 * \param m: A 3x3 matrix (e.g. from #axis_dominant_to_m3).
 * \param v: A 3-D vector to project.
 */
template<typename T>
[[nodiscard]] inline VecBase<T, 2> project_v3_to_v2(const MatBase<T, 3, 3> &m,
                                                    const VecBase<T, 3> &v)
{
  const VecBase<T, 3> result = m * v;
  return VecBase<T, 2>(result.x, result.y);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Intersection
 * \{ */

/**
 * Find the closest points between two infinite 3D lines.
 *
 * \param v1, v2: Two points on line A.
 * \param v3, v4: Two points on line B.
 * \param r_i1: Closest point on line A to line B.
 * \param r_i2: Closest point on line B to line A.
 * \return Number of intersection points:
 *   - 0: Lines are collinear.
 *   - 1: Lines are coplanar; \a r_i1 is set to the intersection.
 *   - 2: \a r_i1 and \a r_i2 are the nearest points on each line.
 */
[[nodiscard]] inline int isect_line_line(const float3 &v1,
                                         const float3 &v2,
                                         const float3 &v3,
                                         const float3 &v4,
                                         float3 &r_i1,
                                         float3 &r_i2)
{
  return isect_line_line_v3(v1, v2, v3, v4, &r_i1.x, &r_i2.x);
}

/**
 * Intersect the infinite line through (\a l1, \a l2) with the plane
 * defined by point \a plane_co and (not necessarily unit) normal \a plane_no.
 *
 * \return The intersection point, or `std::nullopt` when the line is
 * parallel to the plane.
 */
[[nodiscard]] inline std::optional<float3> isect_line_plane(const float3 &l1,
                                                            const float3 &l2,
                                                            const float3 &plane_co,
                                                            const float3 &plane_no)
{
  float3 r_isect_co;
  if (isect_line_plane_v3(&r_isect_co.x, l1, l2, plane_co, plane_no)) {
    return r_isect_co;
  }
  return std::nullopt;
}

/**
 * Test whether point \a pt lies inside the triangle (v1, v2, v3) in 2D.
 *
 * \return
 *   - `+1`: Inside, triangle wound counter-clockwise.
 *   - `-1`: Inside, triangle wound clockwise.
 *   -  `0`: Outside.
 */
template<typename T>
[[nodiscard]] inline int isect_point_tri(const VecBase<T, 2> &pt,
                                         const VecBase<T, 2> &v1,
                                         const VecBase<T, 2> &v2,
                                         const VecBase<T, 2> &v3)
{
  const T s1 = cross(v2 - v1, pt - v1);
  const T s2 = cross(v3 - v2, pt - v2);
  const T s3 = cross(v1 - v3, pt - v3);
  if (s1 >= T(0) && s2 >= T(0) && s3 >= T(0)) {
    return 1;
  }
  if (s1 <= T(0) && s2 <= T(0) && s3 <= T(0)) {
    return -1;
  }
  return 0;
}

/**
 * Test whether point \a p lies inside the convex quad (v1, v2, v3, v4) in 2D.
 *
 * \return
 *   - `+1`: Inside, quad wound counter-clockwise.
 *   - `-1`: Inside, quad wound clockwise.
 *   -  `0`: Outside.
 */
template<typename T>
[[nodiscard]] inline int isect_point_quad(const VecBase<T, 2> &p,
                                          const VecBase<T, 2> &v1,
                                          const VecBase<T, 2> &v2,
                                          const VecBase<T, 2> &v3,
                                          const VecBase<T, 2> &v4)
{
  const T s1 = cross(v2 - v1, p - v1);
  const T s2 = cross(v3 - v2, p - v2);
  const T s3 = cross(v4 - v3, p - v3);
  const T s4 = cross(v1 - v4, p - v4);
  if (s1 >= T(0) && s2 >= T(0) && s3 >= T(0) && s4 >= T(0)) {
    return 1;
  }
  if (s1 <= T(0) && s2 <= T(0) && s3 <= T(0) && s4 <= T(0)) {
    return -1;
  }
  return 0;
}

/**
 * Test whether point \a pt lies inside a polygon in 2D.
 * Uses the ray-casting method.
 *
 * \param verts: The polygon vertices, in order (CW or CCW).
 * \return True when \a pt is inside the polygon.
 */
template<typename T>
[[nodiscard]] inline bool isect_point_poly(const VecBase<T, 2> &pt, Span<VecBase<T, 2>> verts)
{
  const int64_t nr = verts.size();
  bool inside = false;
  for (int64_t i = 0, j = nr - 1; i < nr; j = i++) {
    if (((verts[i].y > pt.y) != (verts[j].y > pt.y)) &&
        (pt.x <
         (verts[j].x - verts[i].x) * (pt.y - verts[i].y) / (verts[j].y - verts[i].y) + verts[i].x))
    {
      inside = !inside;
    }
  }
  return inside;
}

[[nodiscard]] inline bool isect_point_poly(const float2 &pt, Span<float2> verts)
{
  return isect_point_poly<float>(pt, verts);
}

[[nodiscard]] inline bool isect_point_poly(const double2 &pt, Span<double2> verts)
{
  return isect_point_poly<double>(pt, verts);
}

[[nodiscard]] inline bool isect_point_poly(const int2 &pt, Span<int2> verts)
{
  return isect_point_poly<int>(pt, verts);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Barycentric Coordinates
 * \{ */

/**
 * Compute the barycentric weights of point \a co with respect to
 * triangle (v1, v2, v3) in 2D.
 */
[[nodiscard]] inline float3 barycentric_weights(const float2 &v1,
                                                const float2 &v2,
                                                const float2 &v3,
                                                const float2 &co)
{
  float3 w;
  barycentric_weights_v2(v1, v2, v3, co, &w.x);
  return w;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Polygon Utilities
 * \{ */

/**
 * Compute the total number of triangles needed to tessellate a mesh,
 * or the index of the first triangle for a polygon given its loop
 * start index.
 *
 * \param poly_count: Number of polygons (or polygon index).
 * \param corner_count: Number of corners / loop index.
 *
 * \note Derived from the geometric identity that each n-gon (n >= 3)
 * requires (n - 2) triangles.
 */
[[nodiscard]] constexpr int poly_to_tri_count(const int poly_count, const int corner_count)
{
  return corner_count - (poly_count * 2);
}

/**
 * Compute the area of a 2D polygon given its vertices.
 */
[[nodiscard]] inline float area_poly(Span<float2> verts)
{
  return area_poly_v2(reinterpret_cast<const float (*)[2]>(verts.data()), uint(verts.size()));
}

/**
 * Compute the signed area of a 2D polygon given its vertices.
 * Positive if counter-clockwise (CCW), negative if clockwise (CW).
 */
[[nodiscard]] inline float area_poly_signed(Span<float2> verts)
{
  return area_poly_signed_v2(reinterpret_cast<const float (*)[2]>(verts.data()), uint(verts.size()));
}

template<typename T>
[[nodiscard]] inline T area_poly_signed(Span<VecBase<T, 2>> verts)
{
  if (verts.size() < 3) {
    return T(0);
  }
  T area = T(0);
  const int64_t n = verts.size();
  for (int64_t i = 0; i < n; i++) {
    const int64_t next = (i + 1 == n) ? 0 : i + 1;
    area += cross(verts[i], verts[next]);
  }
  return T(0.5) * area;
}

/**
 * Compute mean-value interpolation weights for point \a co inside the 2D polygon
 * defined by \a verts. Writes one weight per vertex into \a r_weights.
 *
 * \param r_weights: Output weights, must have the same size as \a verts.
 * \param verts: Polygon vertices, in order.
 * \param co: The query point.
 */
inline void interp_weights_poly(MutableSpan<float> r_weights, Span<float2> verts, const float2 &co)
{
  BLI_assert(r_weights.size() == verts.size());
  interp_weights_poly_v2(
      r_weights.data(),
      /* Cast needed because the C API takes a non-const pointer
       * to a 2D float array; the values are read-only here. */
      const_cast<float (*)[2]>(reinterpret_cast<const float (*)[2]>(verts.data())),
      int(verts.size()),
      co);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Interpolation
 * \{ */

/**
 * Bilinear interpolation of four 3D points at parameter (u, v).
 *
 * The four vertices are ordered as a bilinear patch:
 * \code{.unparsed}
 *  v=1  v3---v2
 *       |    |
 *  v=0  v0---v1
 *       u=0  u=1
 * \endcode
 *
 * \param v0, v1, v2, v3: The four corner points.
 * \param u: Interpolation parameter in the first direction (0..1).
 * \param v: Interpolation parameter in the second direction (0..1).
 * \return The interpolated point.
 */
template<typename T>
[[nodiscard]] inline VecBase<T, 3> interp_bilinear_quad(const VecBase<T, 3> &v0,
                                                        const VecBase<T, 3> &v1,
                                                        const VecBase<T, 3> &v2,
                                                        const VecBase<T, 3> &v3,
                                                        const T u,
                                                        const T v)
{
  return v0 * ((T(1) - u) * (T(1) - v)) + v1 * (u * (T(1) - v)) + v2 * (u * v) +
         v3 * ((T(1) - u) * v);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mapping
 * \{ */

/**
 * Map a 3D direction vector \a co to spherical (u, v) texture
 * coordinates returned as #float2.
 *
 * \note Matches #map_to_sphere.
 */
[[nodiscard]] inline float2 map_to_sphere(const float3 &co)
{
  float2 result;
  blender::map_to_sphere(&result.x, &result.y, co.x, co.y, co.z);
  return result;
}

/** \} */

}  // namespace blender::math
