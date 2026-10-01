/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Utilities for point clouds of the #PointCloudType::GSplat type.
 */

#include "BLI_math_matrix_types.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender {
struct PointCloud;
}

namespace blender::bke {
class AttributeAccessor;
}

namespace blender::bke::gsplat {

/**
 * The number of complete spherical harmonics bands above band 0 that fit in the given number of
 * `radiance:sh_*` coefficients, at most #math::SphericalHarmonicsRotation::max_degree.
 */
int sh_degree_for_coefficients_num(int coefficients_num);

/**
 * Transform gaussian splats by the linear part of a transform, the same way the renderers show an
 * object transform: the covariance becomes `A Σ Aᵀ`, and the spherical harmonics are rotated by
 * the rotation part of `A`, which is exact unless `A` has a non-uniform scale.
 *
 * \param rotations, scales: Either both empty or both with one item per splat.
 * \param sh: One span per coefficient, holding complete bands. At most
 * `(math::SphericalHarmonicsRotation::max_degree + 1)^2 - 1` spans (degree <= max_degree). Can be
 * empty.
 */
void transform(const float3x3 &linear,
               MutableSpan<math::Quaternion> rotations,
               MutableSpan<float3> scales,
               Span<MutableSpan<float3>> sh);

/** The gaussian splat attributes of a point cloud that #transform_attributes changes. */
struct AttributesToTransform {
  /** Whether the `rotation` and `scale` attributes are transformed. */
  bool rotation_and_scale = false;
  /** The number of `radiance:sh_*` attributes that are transformed, always complete bands. */
  int sh_num = 0;
};

AttributesToTransform attributes_to_transform(const AttributeAccessor &attributes);

/**
 * Transform the gaussian splat attributes of the point cloud, see #transform. Does nothing for
 * point clouds that are not of the #PointCloudType::GSplat type. Positions are not changed.
 *
 * `rotation` and `scale` are only transformed when both exist. Only complete bands of the
 * `radiance:sh_*` attributes are transformed. Attributes are never created.
 */
void transform_attributes(const float4x4 &object_transform, PointCloud &pointcloud);

}  // namespace blender::bke::gsplat
