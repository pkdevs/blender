/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/geom/object.h"
#include "kernel/geom/triangle.h"

#include "util/math_fast.h"
#include "util/math_intersect.h"

CCL_NAMESPACE_BEGIN

/* returns true if the triangle is has motion blur or an instancing transform applied */
ccl_device_inline bool triangle_world_space_vertices(
    KernelGlobals kg, const int object, const int prim, const float time, float3 V[3])
{
  bool has_motion = false;
  const uint object_flag = kernel_data_fetch(object_flag, object);

  if (object_flag & SD_OBJECT_HAS_VERTEX_MOTION && time >= 0.0f) {
    motion_triangle_vertices(kg, object, prim, time, V);
    has_motion = true;
  }
  else {
    triangle_vertices(kg, object, prim, V);
  }

  if (!(object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
#ifdef __OBJECT_MOTION__
    const float object_time = (time >= 0.0f) ? time : 0.5f;
    const Transform tfm = object_fetch_transform_motion_test(kg, object, object_time, nullptr);
#else
    Transform tfm = object_fetch_transform(kg, object, OBJECT_TRANSFORM);
#endif
    V[0] = transform_point(&tfm, V[0]);
    V[1] = transform_point(&tfm, V[1]);
    V[2] = transform_point(&tfm, V[2]);
    has_motion = true;
  }
  return has_motion;
}

ccl_device_inline float triangle_light_pdf_area_sampling(const float3 Ng,
                                                         const float3 I,
                                                         const float t)
{
  const float cos_pi = fabsf(dot(Ng, I));

  if (cos_pi == 0.0f) {
    return 0.0f;
  }

  return t * t / cos_pi;
}

ccl_device_forceinline float triangle_light_pdf(KernelGlobals kg,
                                                const ccl_private ShaderData *sd,
                                                const float t)
{
  /* A naive heuristic to decide between costly solid angle sampling
   * and simple area sampling, comparing the distance to the triangle plane
   * to the length of the edges of the triangle. */

  float3 V[3];
  const bool has_motion = triangle_world_space_vertices(kg, sd->object, sd->prim, sd->time, V);

  const float3 e0 = V[1] - V[0];
  const float3 e1 = V[2] - V[0];
  const float3 e2 = V[2] - V[1];
  const float longest_edge_squared = max(len_squared(e0), max(len_squared(e1), len_squared(e2)));
  const float3 N = cross(e0, e1);
  const float distance_to_plane = fabsf(dot(N, sd->wi * t)) / dot(N, N);
  const float area = 0.5f * len(N);

  float pdf;

  if (longest_edge_squared > distance_to_plane * distance_to_plane) {
    /* sd contains the point on the light source
     * calculate Px, the point that we're shading */
    const float3 Px = sd->P + sd->wi * t;

    const float3 A = safe_normalize(V[0] - Px);
    const float3 B = safe_normalize(V[1] - Px);
    const float3 C = safe_normalize(V[2] - Px);

    const float solid_angle = 2.0f * fast_atan2f(fabsf(dot(A, cross(B, C))),
                                                 (1.0f + dot(B, C) + dot(A, C) + dot(A, B)));

    /* distribution_pdf_triangles is calculated over triangle area, but we're not sampling over
     * its area */
    if (UNLIKELY(solid_angle == 0.0f)) {
      return 0.0f;
    }
    pdf = 1.0f / solid_angle;
  }
  else {
    if (UNLIKELY(area == 0.0f)) {
      return 0.0f;
    }

    pdf = triangle_light_pdf_area_sampling(sd->Ng, sd->wi, t) / area;
  }

  /* Belongs in distribution.h but can reuse computations here. */
  if (!kernel_data.integrator.use_light_tree) {
    float distribution_area = area;

    if (has_motion && area != 0.0f) {
      /* For motion blur need area of triangle at fixed time as used in the CDF. */
      triangle_world_space_vertices(kg, sd->object, sd->prim, -1.0f, V);
      distribution_area = triangle_area(V[0], V[1], V[2]);
    }

    pdf *= distribution_area * kernel_data.integrator.distribution_pdf_triangles;
  }

  return pdf;
}

template<bool in_volume_segment>
ccl_device_forceinline bool triangle_light_sample(KernelGlobals kg,
                                                  const int prim,
                                                  const int object,
                                                  const float2 rand,
                                                  const float time,
                                                  ccl_private LightSample *ls,
                                                  const float3 P)
{
  /* A naive heuristic to decide between costly solid angle sampling
   * and simple area sampling, comparing the distance to the triangle plane
   * to the length of the edges of the triangle. */

  float3 V[3];
  const bool has_motion = triangle_world_space_vertices(kg, object, prim, time, V);

  const float3 e0 = V[1] - V[0];
  const float3 e1 = V[2] - V[0];
  const float3 e2 = V[2] - V[1];
  const float longest_edge_squared = max(len_squared(e0), max(len_squared(e1), len_squared(e2)));
  float3 N0 = cross(e0, e1);
  /* Flip normal if necessary. */
  const uint object_flag = kernel_data_fetch(object_flag, object);
  if (object_flag & SD_OBJECT_NEGATIVE_SCALE) {
    N0 = -N0;
  }

  /* Do not draw samples from the side without MIS. */
  ls->shader = kernel_data_fetch(tri_shader, prim); // !!!
  const float distance_to_plane = dot(N0, V[0] - P) / dot(N0, N0);
  const int ls_shader_flag = kernel_data_fetch(shaders, ls->shader & SHADER_MASK).flags;
  if (!in_volume_segment &&
      !(ls_shader_flag & (distance_to_plane > 0 ? SD_MIS_BACK : SD_MIS_FRONT)))
  {
    return false;
  }

  float Nl = 0.0f;
  ls->Ng = safe_normalize_len(N0, &Nl);
  const float area = 0.5f * Nl;

  ls->eval_fac = 1.0f;
  ls->object = object;
  ls->prim = prim;
  ls->shader |= SHADER_USE_MIS;
  ls->type = LIGHT_TRIANGLE;
  ls->group = object_lightgroup(kg, object);

  if (!in_volume_segment && (longest_edge_squared > distance_to_plane * distance_to_plane)) {
    /* A modified version of James Arvo, "Stratified Sampling of Spherical Triangles"
     * http://www.graphics.cornell.edu/pubs/1995/Arv95c.pdf */

    /* Project the triangle to the unit sphere and calculate the three unit vector that spans the
     * spherical triangle. */
    const float3 A = safe_normalize(V[0] - P);
    const float3 B = safe_normalize(V[1] - P);
    const float3 C = safe_normalize(V[2] - P);

    const float cos_a = dot(B, C);
    const float cos_b = dot(A, C);
    const float cos_c = dot(A, B);

    const float mixed_product = fabsf(dot(A, cross(B, C)));

    /* The area of the spherical triangle is equal to the subtended solid angle. */
    const float solid_angle = 2.0f * fast_atan2f(mixed_product, (1.0f + cos_a + cos_b + cos_c));

    /* Compute the angle at A. */
    const float cos_alpha = dot(safe_normalize(cross(A, B)), safe_normalize(cross(A, C)));
    const float sin_alpha = sin_from_cos(cos_alpha);
    const float alpha = safe_acosf(cos_alpha);

    /* Select a random sub-area of the spherical triangle and calculate the third vertex C_ of that
     * new triangle. */
    const float A_hat = rand.x * solid_angle;
    float sin_phi;
    float cos_phi;
    fast_sincosf(A_hat - alpha, &sin_phi, &cos_phi);
    const float u = cos_phi - cos_alpha;
    const float v = sin_phi + sin_alpha * cos_c;
    const float num = (v * cos_phi - u * sin_phi) * cos_alpha - v;
    const float den = (v * sin_phi + u * cos_phi) * sin_alpha;
    const float q = (den == 0.0f) ? 1.0f : num / den;

    const float3 U = safe_normalize(C - cos_b * A);
    const float3 C_ = safe_normalize(q * A + sin_from_cos(q) * U);

    /* Finally, select a random point along the edge of the new triangle
     * That point on the spherical triangle is the sampled ray direction */
    const float z = 1.0f - rand.y * (1.0f - dot(C_, B));
    ls->D = z * B + sin_from_cos(z) * safe_normalize(C_ - dot(C_, B) * B);

    /* calculate intersection with the planar triangle */
    float unused_u, unused_v;
    if (!ray_triangle_intersect( // !!!
            P, ls->D, 0.0f, FLT_MAX, V[0], V[1], V[2], &unused_u, &unused_v, &ls->t))
    {
      ls->pdf = 0.0f;
      return false;
    }

    ls->P = P + ls->D * ls->t;

    /* distribution_pdf_triangles is calculated over triangle area, but we're sampling over solid
     * angle */
    if (UNLIKELY(solid_angle == 0.0f)) {
      ls->pdf = 0.0f;
      return false;
    }
    ls->pdf = 1.0f / solid_angle;
  }
  else {
    if (UNLIKELY(area == 0.0f)) {
      return 0.0f;
    }

    /* compute random point in triangle. From Eric Heitz's "A Low-Distortion Map Between Triangle
     * and Square" */
    float u = rand.x;
    float v = rand.y;

    if (v > u) {
      u *= 0.5f;
      v -= u;
    }
    else {
      v *= 0.5f;
      u -= v;
    }

    const float t = 1.0f - u - v;
    ls->P = t * V[0] + u * V[1] + v * V[2];
    /* compute incoming direction, distance and pdf */
    ls->D = normalize_len(ls->P - P, &ls->t);
    ls->pdf = triangle_light_pdf_area_sampling(ls->Ng, -ls->D, ls->t) / area;
  }

  /* Belongs in distribution.h but can reuse computations here. */
  if (!kernel_data.integrator.use_light_tree) {
    float distribution_area = area;

    if (has_motion && area != 0.0f) {
      /* For motion blur need area of triangle at fixed time as used in the CDF. */
      triangle_world_space_vertices(kg, object, prim, -1.0f, V);
      distribution_area = triangle_area(V[0], V[1], V[2]);
    }

    ls->pdf_selection = distribution_area * kernel_data.integrator.distribution_pdf_triangles;
  }

  return (ls->pdf > 0.0f);
}

ccl_device_inline void triangle_light_sample_from_intersection(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private const ShaderData *sd,
    ccl_private LightSample *ccl_restrict ls,
    ccl_private LightSampleReference *ccl_restrict reference)
{
  const float3 ray_D = INTEGRATOR_STATE(state, ray, D);

  ls->shader = sd->shader;
  ls->D = ray_D;
  ls->t = sd->ray_length;
  ls->P = INTEGRATOR_STATE(state, ray, P) + ray_D * ls->t;
  ls->object = sd->object;
  ls->prim = sd->prim;
  ls->type = LIGHT_TRIANGLE;
  ls->group = object_lightgroup(kg, ls->object);

  *reference = light_reference_empty();
  reference->type = LIGHT_TRIANGLE;
  reference->object = sd->object;
  reference->prim = sd->prim;
  reference->param = make_float2(sd->u, sd->v);
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM;

  /* Normal. */
  float3 V[3];
  triangle_world_space_vertices(
      kg, ls->object, ls->prim, INTEGRATOR_STATE(state, ray, time), V);

  const float3 e0 = V[1] - V[0];
  const float3 e1 = V[2] - V[0];
  ls->Ng = safe_normalize(cross(e0, e1));

  if (dot(ls->Ng, sd->wi) < 0.0f) {
    /* TODO(weizhen): MIS. */
    ls->Ng = -ls->Ng;
  }
}

/* Find the ray segment lit by the triangle light. */
ccl_device_inline bool triangle_light_valid_ray_segment(KernelGlobals kg,
                                                        const float3 P,
                                                        const float3 D,
                                                        ccl_private Interval<float> *t_range,
                                                        const ccl_private LightSample *ls)
{
  const int shader_flag = kernel_data_fetch(shaders, ls->shader & SHADER_MASK).flags;
  const int SD_MIS_BOTH = SD_MIS_BACK | SD_MIS_FRONT;
  if ((shader_flag & SD_MIS_BOTH) == SD_MIS_BOTH) {
    /* Both sides are sampled, the complete ray segment is visible. */
    return true;
  }

  /* Only one side is sampled, intersect the ray and the triangle light plane to find the visible
   * ray segment. Flip normal if Emission Sampling is set to back. */
  const float3 N = ls->Ng;
  return ray_plane_intersect((shader_flag & SD_MIS_BACK) ? -N : N, P, D, t_range);
}

template<bool in_volume_segment>
ccl_device_forceinline bool triangle_light_tree_parameters(
    KernelGlobals kg,
    const ccl_global KernelLightTreeEmitter *kemitter,
    const float3 centroid,
    const float3 P,
    const float3 N,
    const KernelBoundingCone bcone,
    ccl_private float &cos_theta_u,
    ccl_private float2 &distance,
    ccl_private float3 &point_to_centroid)
{
  /* TODO: a cheap substitute for minimal distance between point and primitive. Does it worth the
   * overhead to compute the accurate minimal distance? */
  float min_distance;
  point_to_centroid = safe_normalize_len(centroid - P, &min_distance);
  distance = make_float2(min_distance, min_distance);

  cos_theta_u = FLT_MAX;

  float3 vertices[3];
  triangle_vertices(kg, kemitter->object_id, kemitter->triangle.id, vertices);

  bool shape_above_surface = false;
  for (int i = 0; i < 3; i++) {
    const float3 corner = vertices[i];
    float distance_point_to_corner;
    const float3 point_to_corner = safe_normalize_len(corner - P, &distance_point_to_corner);
    cos_theta_u = fminf(cos_theta_u, dot(point_to_centroid, point_to_corner));
    shape_above_surface |= dot(point_to_corner, N) > 0;
    if (!in_volume_segment) {
      distance.x = fmaxf(distance.x, distance_point_to_corner);
    }
  }

  const bool front_facing = bcone.theta_o != 0.0f || dot(bcone.axis, point_to_centroid) < 0;

  return front_facing && shape_above_surface;
}

ccl_device float2 triangle_light_uv(KernelGlobals kg,
                                    const int object,
                                    const int prim,
                                    const float time,
                                    const float3 ray_P,
                                    const float3 ray_D)
{
  float3 V[3];
  triangle_world_space_vertices(kg, object, prim, time, V);
  return ray_triangle_uv(ray_P, ray_D, V[0], V[1], V[2]);
}

ccl_device_inline bool triangle_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    const float time,
    ccl_private LightSampleReference *reference)
{
  *reference = light_reference_empty();

  if (ls->type != LIGHT_TRIANGLE || ls->object < 0 || ls->prim < 0) {
    return false;
  }

  float3 V[3];
  triangle_world_space_vertices(kg, ls->object, ls->prim, time, V);

  /*
   * The persistent parameterization is the triangle's barycentric
   * coordinates:
   *     P = (1 - u - v) * V0 + u * V1 + v * V2
   *
   * These coordinates are emitter-local and therefore independent of
   * the current shading target.
   */
  const float3 e0 = V[1] - V[0];
  const float3 e1 = V[2] - V[0];
  const float3 p = ls->P - V[0];

  const float d00 = dot(e0, e0);
  const float d01 = dot(e0, e1);
  const float d11 = dot(e1, e1);
  const float d20 = dot(p, e0);
  const float d21 = dot(p, e1);

  const float denom = d00 * d11 - d01 * d01;

  if (!(isfinite(denom) && denom > 0.0f)) {
    return false;
  }

  const float u = (d11 * d20 - d01 * d21) / denom;
  const float v = (d00 * d21 - d01 * d20) / denom;

  if (!(isfinite(u) && isfinite(v))) {
    return false;
  }

  /*
   * The sampled point must lie on the triangle.
   *
   * A small tolerance is allowed for floating-point reconstruction,
   * particularly for samples produced through solid-angle sampling.
   */
  const float w = 1.0f - u - v;
  constexpr float epsilon = 1e-5f;

  if (u < -epsilon || v < -epsilon || w < -epsilon) {
    return false;
  }

  reference->type = LIGHT_TRIANGLE;
  reference->object = ls->object;
  reference->prim = ls->prim;
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM;
  reference->param = make_float2(u, v);

  return true;
}

ccl_device_inline bool triangle_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private LightSampleReference *reference,
    const float3 P,
    const float time,
    ccl_private LightSample *ls)
{
  *ls = {};

  if (!light_reference_is_valid(reference) ||
      reference->type != LIGHT_TRIANGLE ||
      reference->object < 0 || reference->prim < 0 ||
      !(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM))
  {
    return false;
  }

  float3 V[3];
  triangle_world_space_vertices(kg, reference->object, reference->prim, time, V);

  const float3 e0 = V[1] - V[0];
  const float3 e1 = V[2] - V[0];

  ls->object = reference->object;
  ls->prim = reference->prim;

  ls->Ng = safe_normalize(cross(e0, e1));

  /* Flip normal if necessary. */
  const int object_flag = kernel_data_fetch(object_flag, ls->object);
  if (object_flag & SD_OBJECT_NEGATIVE_SCALE) {
    ls->Ng = -ls->Ng;
  }

  ls->eval_fac = 1.0f;

/*
TODO(Tri): Full reconstruction of tri invariants?
object done
prim done
Ng (Kinda done)
shader Done
type Done
group Done
pdf_selection
pdf Done
P Done
D Done
t
*/

  // TODO(Tri):
  // /* Belongs in distribution.h but can reuse computations here. */
  // if (!kernel_data.integrator.use_light_tree) {
  //   float distribution_area = area;

  //   if (has_motion && area != 0.0f) {
  //     /* For motion blur need area of triangle at fixed time as used in the CDF. */
  //     triangle_world_space_vertices(kg, object, prim, -1.0f, V);
  //     distribution_area = triangle_area(V[0], V[1], V[2]);
  //   }

  //   ls->pdf_selection = distribution_area * kernel_data.integrator.distribution_pdf_triangles;
  // }

  /* Restore triangle light metadata. */
  ls->shader = kernel_data_fetch(tri_shader, ls->prim) | SHADER_USE_MIS;
  ls->type = LIGHT_TRIANGLE;
  ls->group = object_lightgroup(kg, ls->object);

  /* Reconstruct the emitter position from the persistent barycentric coords */
  const float u = reference->param.x;
  const float v = reference->param.y;
  const float w = 1.0f - u - v;

  if (!(isfinite(u) && isfinite(v) && isfinite(w)) ||
      u < 0.0f || v < 0.0f || w < 0.0f)
  {
    return false;
  }

  ls->P = w * V[0] + u * V[1] + v * V[2];
  /* Target-dependent direction and distance. */
  ls->D = safe_normalize_len(ls->P - P, &ls->t);

  if (!isfinite(ls->t) || ls->t <= 0.0f) {
    return false;
  }

  const float area = 0.5f * len(cross(e0, e1));

  if (area == 0.0f) {
    return false;
  }

  ls->pdf = triangle_light_pdf_area_sampling(
      ls->Ng, -ls->D, ls->t) / area;

  return isfinite(ls->pdf) && ls->pdf > 0.0f;
}

CCL_NAMESPACE_END
