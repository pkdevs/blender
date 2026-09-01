/* SPDX-FileCopyrightText: 2021-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/bvh/util.h"
#include "kernel/geom/motion_point.h"

CCL_NAMESPACE_BEGIN

/* Motion Gaussian Splat Primitive
 *
 * These are stored as regular gaussian splats, plus extra positions and radii at times other than
 * the frame center. Computing the point at a given ray time is a matter of interpolation of the
 * two steps between which the ray time lies.
 *
 * The extra points are stored as additional motion steps in ATTR_STD_POSITION. */

#if defined(__GSPLATS__)

ccl_device_inline float4 motion_gsplat_point(KernelGlobals kg,
                                             const int object,
                                             const int prim,
                                             const float time)
{
  return motion_point(kg, object, prim, time);
}

ccl_device_forceinline void motion_gsplat_compute_info(KernelGlobals kg,
                                                       const int object,
                                                       const float time,
                                                       ccl_private int *num_steps,
                                                       ccl_private int *step,
                                                       ccl_private float *t)
{
  *num_steps = kernel_data_fetch(objects, object).num_geom_steps;

  const int max_step = *num_steps - 1;
  *step = min((int)(time * max_step), max_step - 1);
  *t = time * max_step - *step;
}

ccl_device_inline Quaternion motion_gsplat_rotation_for_step(KernelGlobals kg,
                                                             int offset,
                                                             const int num_verts,
                                                             const int num_steps,
                                                             const int step,
                                                             const int prim)
{
  offset += motion_point_offset_for_step(num_verts, num_steps, step);
  return kernel_data_fetch(attributes_quaternion, offset + prim);
}

ccl_device_forceinline Quaternion motion_gsplat_rotation_interpolate(const Quaternion r1,
                                                                     const Quaternion r2,
                                                                     const float t)
{
  /* Use linear interpolation which is cheap and wors good enough. This is also what
   * quat_interpolate() does for HW-RT.
   * Note that interpolation is supposed to happen on normalized quaternions, and currently no
   * normalization is enforced on the gsplat attributes. */
  /* TODO(sergey): Investigate whether normalizing ahead of time brings performance improvement.
   * The tricky part with doing it ahead of time is that it could would mean data could no longer
   * be implicitly shared with the host application. */
  return lerp(normalized(r1), normalized(r2), t);
}

ccl_device_inline Quaternion motion_gsplat_rotation(KernelGlobals kg,
                                                    const int object,
                                                    const int prim,
                                                    const float time)
{
  /* Get motion info. */
  const int num_verts = kernel_data_fetch(objects, object).numverts;
  int step, num_steps;
  float t;
  motion_gsplat_compute_info(kg, object, time, &num_steps, &step, &t);

  /* Fetch key rotations. */
  const int offset = kernel_data_fetch(objects, object).gsplat.rotation_offset;
  const Quaternion rotation = motion_gsplat_rotation_for_step(
      kg, offset, num_verts, num_steps, step, prim);
  const Quaternion next_rotation = motion_gsplat_rotation_for_step(
      kg, offset, num_verts, num_steps, step + 1, prim);

  /* Interpolate between steps. */
  return motion_gsplat_rotation_interpolate(rotation, next_rotation, t);
}

ccl_device_inline float4 motion_gsplat_radiance_base_for_step(KernelGlobals kg,
                                                              int offset,
                                                              const int num_verts,
                                                              const int num_steps,
                                                              const int step,
                                                              const int prim)
{
  offset += motion_point_offset_for_step(num_verts, num_steps, step);
  return kernel_data_fetch(attributes_float4, offset + prim);
}

ccl_device_inline float4 motion_gsplat_radiance_base(KernelGlobals kg,
                                                     const int object,
                                                     const int prim,
                                                     const float time)
{
  /* Get motion info. */
  const int num_verts = kernel_data_fetch(objects, object).numverts;
  int step, num_steps;
  float t;
  motion_gsplat_compute_info(kg, object, time, &num_steps, &step, &t);

  /* Fetch key radiance bases. */
  const int offset = kernel_data_fetch(objects, object).gsplat.radiance_base_offset;
  const float4 radiance_base = motion_gsplat_radiance_base_for_step(
      kg, offset, num_verts, num_steps, step, prim);
  const float4 next_radiance_base = motion_gsplat_radiance_base_for_step(
      kg, offset, num_verts, num_steps, step + 1, prim);

  /* Interpolate between steps. */
  return (1.0f - t) * radiance_base + t * next_radiance_base;
}

ccl_device_inline float3 motion_gsplat_scale_for_step(KernelGlobals kg,
                                                      int offset,
                                                      const int num_verts,
                                                      const int num_steps,
                                                      const int step,
                                                      const int prim)
{
  offset += motion_point_offset_for_step(num_verts, num_steps, step);
  return kernel_data_fetch(attributes_float3, offset + prim);
}

ccl_device_inline float3 motion_gsplat_scale(KernelGlobals kg,
                                             const int object,
                                             const int prim,
                                             const float time)
{
  /* Get motion info. */
  const int num_verts = kernel_data_fetch(objects, object).numverts;
  int step, num_steps;
  float t;
  motion_gsplat_compute_info(kg, object, time, &num_steps, &step, &t);

  /* Fetch key scales. */
  const int offset = kernel_data_fetch(objects, object).gsplat.scale_offset;
  const float3 scale = motion_gsplat_scale_for_step(kg, offset, num_verts, num_steps, step, prim);
  const float3 next_scale = motion_gsplat_scale_for_step(
      kg, offset, num_verts, num_steps, step + 1, prim);

  /* Interpolate between steps. */
  return (1.0f - t) * scale + t * next_scale;
}

#endif /* __GSPLATS__ */

CCL_NAMESPACE_END
