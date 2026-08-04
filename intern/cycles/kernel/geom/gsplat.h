/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"
#include "kernel/types.h"

/* Gaussian splats evaluation and utility functions.
 *
 * References:
 *
 *  [chen2024pgsr]  Nicolas Moenne-Loccoz et. al.
 *                  "PGSR: Planar-based Gaussian Splatting for Efficient and High-Fidelity Surface
 *                  Reconstruction" (2024)
 *                  https://zju3dv.github.io/pgsr/
 *
 *  [3dgs2023]  Bernhard Kebl, Georgios Kopanas et al.
 *              3D Gaussian Splatting for Real-Time Radiance Field Rendering
 *              ACM Transactions on Graphics (SIGGRAPH 2023)
 *              https://repo-sam.inria.fr/fungraph/3d-gaussian-splatting/
 */

CCL_NAMESPACE_BEGIN

#if defined(__GSPLATS__)

ccl_device_inline float3 gsplat_radiance(KernelGlobals kg, const ccl_private ShaderData &sd)
{
  const int radiance_base_offset =
      kernel_data_fetch(objects, sd.object).gsplat_radiance_base_offset;
  const float4 radiance_base = kernel_data_fetch(attributes_float4,
                                                 radiance_base_offset + sd.prim);

  /* Transform incident direction into the local pointcloud object.
   * This is easier than transforming spherical harmonics into the world space. */
  float3 direction = -sd.wi;
  object_inverse_dir_transform(kg, &sd, &direction);
  kernel_assert((kernel_data_fetch(object_flag, sd.object) & SD_OBJECT_TRANSFORM_APPLIED) == 0);

  float3 color_srgb = spherical_harmonics_evaluate_band_L0(direction, make_float3(radiance_base));

  const int radiance_spherical_harmonics_offset =
      kernel_data_fetch(objects, sd.object).gsplat_radiance_spherical_harmonics_offset;
  if (radiance_spherical_harmonics_offset != ATTR_STD_NOT_FOUND) {
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics = kernel_data_fetch(
        attributes_spherical_harmonics, radiance_spherical_harmonics_offset + sd.prim);
    color_srgb += spherical_harmonics_evaluate(packed_spherical_harmonics, direction);
  }

  /* Map to linear color space, add 1/2 offset to mirror [3dgs2023]. */
  const float3 color_rec709 = color_srgb_to_linear_v3(color_srgb + 0.5f);
  return rec709_to_rgb(kg, make_float3(color_rec709));
}

ccl_device_inline float3 gsplat_normal(KernelGlobals kg, const int object, const int prim)
{
  /* Follows the "Flattening 3D Gaussian" section of [chen2024pgsr].
   *
   * The intuitive way to think about it is that gsplat is a pancake, so its normal points in the
   * direction of the smallest scale. The implementation here checks which of the scale axis is the
   * smallest and calculates corresponding axis of the rotation matrix. */

  /* TODO(sergey): The paper mentions ambiguity of the normal direction when there are two
   * directions for the shortest axis. Current code does not deal with this. */

  /* TODO(sergey): There might be much better normal approximation. */

  const int scale_offset = kernel_data_fetch(objects, object).gsplat_scale_offset;
  const int rotation_offset = kernel_data_fetch(objects, object).gsplat_rotation_offset;
  const float3 scale = kernel_data_fetch(attributes_float3, scale_offset + prim);
  const float4 rotation = kernel_data_fetch(attributes_float4, rotation_offset + prim);

  const float normalizer = 1.0f / (rotation.x * rotation.x + rotation.y * rotation.y +
                                   rotation.z * rotation.z + rotation.w * rotation.w);

  const float a = rotation.x;  // w
  const float b = rotation.y;  // x
  const float c = rotation.z;  // y
  const float d = rotation.w;  // z

  if ((scale.x <= scale.y) && (scale.x <= scale.z)) {
    return make_float3(
        a * a + b * b - c * c - d * d, 2.0f * (b * c - a * d), 2.0f * (a * c + b * d));
  }
  if ((scale.y < scale.x) && (scale.y <= scale.z)) {
    return make_float3(
               2.0f * (a * d + b * c), a * a - b * b + c * c - d * d, 2.0f * (c * d - a * b)) *
           normalizer;
  }
  // if ((scale.z < scale.x) && (scale.z < scale.y))
  return make_float3(
             2.0f * (b * d - a * c), 2.0f * (a * b + c * d), a * a - b * b - c * c + d * d) *
         normalizer;
}

#endif /* __GSPLATS__ */

CCL_NAMESPACE_END
