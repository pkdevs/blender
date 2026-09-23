/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cmath>

#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_gtest_base.hh"
#include "BKE_pointcloud.hh"

#include "DNA_pointcloud_types.h"

#include "GEO_transform.hh"

#include "testing/testing.h"

namespace blender::geometry::tests {

class TransformGeometryTest : public bke::BlenderGTestBase {};

TEST_F(TransformGeometryTest, GaussianSplatsAreRotated)
{
  PointCloud *pointcloud = BKE_pointcloud_new_nomain(PointCloudType::GSplat, 1);
  bke::MutableAttributeAccessor attributes = pointcloud->attributes_for_write();
  attributes.add<math::Quaternion>("rotation",
                                   bke::AttrDomain::Point,
                                   bke::AttributeInitVArray(VArray<math::Quaternion>::from_single(
                                       math::Quaternion::identity(), 1)));
  attributes.add<float3>(
      "scale",
      bke::AttrDomain::Point,
      bke::AttributeInitVArray(VArray<float3>::from_single(float3(1.0f, 2.0f, 3.0f), 1)));
  bke::GeometrySet geometry = bke::GeometrySet::from_pointcloud(pointcloud);

  const math::Quaternion rotation = math::to_quaternion(
      math::AxisAngle(math::AxisSigned::Z_POS, math::AngleRadian(float(M_PI_2))));
  transform_geometry(geometry, math::from_rotation<float4x4>(rotation));

  const bke::AttributeAccessor result = geometry.get_pointcloud()->attributes();
  const math::Quaternion result_rotation = (*result.lookup<math::Quaternion>(
      "rotation", bke::AttrDomain::Point))[0];
  const float3 result_scale = (*result.lookup<float3>("scale", bke::AttrDomain::Point))[0];
  /* q and -q are the same rotation. */
  EXPECT_NEAR(std::abs(math::dot(float4(result_rotation), float4(rotation))), 1.0f, 1e-6f);
  EXPECT_V3_NEAR(result_scale, float3(1.0f, 2.0f, 3.0f), 1e-6f);
}

}  // namespace blender::geometry::tests
