/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cmath>

#include <fmt/format.h>

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_spherical_harmonics.hh"
#include "BLI_rand.hh"

#include "BKE_attribute.hh"
#include "BKE_gsplat.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_pointcloud.hh"

#include "DNA_pointcloud_types.h"

namespace blender::bke::gsplat::tests {

static float3x3 rotation_matrix(const float3 axis, const float angle)
{
  return math::from_rotation<float3x3>(math::AxisAngle(math::normalize(axis), angle));
}

static math::Quaternion random_rotation(RandomNumberGenerator &rng)
{
  return math::normalize(math::Quaternion(rng.get_float() - 0.5f,
                                          rng.get_float() - 0.5f,
                                          rng.get_float() - 0.5f,
                                          rng.get_float() - 0.5f));
}

/* Log-uniform in [min_scale, 1] per axis, so flat splats are included. */
static float3 random_scale(RandomNumberGenerator &rng, const float min_scale)
{
  const float exponent = std::log10(min_scale);
  return float3(std::pow(10.0f, exponent * rng.get_float()),
                std::pow(10.0f, exponent * rng.get_float()),
                std::pow(10.0f, exponent * rng.get_float()));
}

static float3 random_float3(RandomNumberGenerator &rng)
{
  return float3(rng.get_float(), rng.get_float(), rng.get_float()) * 2.0f - 1.0f;
}

static float quaternion_length(const math::Quaternion &q)
{
  return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
}

/* A splat with factor F = R * S has the covariance F * Fᵀ. The transformed splat must have the
 * covariance (A * F) * (A * F)ᵀ, which holds exactly when X = (A * F)⁻¹ * F' is orthogonal. This
 * measures the error relative to the size of the splat along every axis, so errors on the small
 * axes of flat splats are not hidden by the large ones. */
static void expect_covariances_transformed(const float3x3 &linear,
                                           const float min_scale,
                                           const float tolerance)
{
  RandomNumberGenerator rng(3);
  Array<math::Quaternion> rotations(64);
  Array<float3> scales(64);
  for (const int i : rotations.index_range()) {
    rotations[i] = random_rotation(rng);
    scales[i] = random_scale(rng, min_scale);
  }
  const Array<math::Quaternion> original_rotations = rotations;
  const Array<float3> original_scales = scales;

  transform(linear, rotations, scales, {});

  for (const int i : rotations.index_range()) {
    SCOPED_TRACE(i);
    EXPECT_NEAR(quaternion_length(rotations[i]), 1.0f, 1e-5f);
    const float3x3 expected_factor = linear *
                                     math::from_rotation<float3x3>(original_rotations[i]) *
                                     math::from_scale<float3x3>(original_scales[i]);
    const float3x3 factor = math::from_rotation<float3x3>(rotations[i]) *
                            math::from_scale<float3x3>(scales[i]);
    const float3x3 x = math::invert(expected_factor) * factor;
    const float3x3 xxT = x * math::transpose(x);
    EXPECT_M3_NEAR(xxT, float3x3::identity(), tolerance);
  }
}

TEST(gsplat_transform, Rotation)
{
  expect_covariances_transformed(rotation_matrix(float3(1, 2, 3), 0.7f), 1e-2f, 1e-4f);
}

TEST(gsplat_transform, UniformScaleAndMirror)
{
  expect_covariances_transformed(math::from_scale<float3x3>(float3(-2.5f, 2.5f, 2.5f)) *
                                     rotation_matrix(float3(-1, 0.5f, 2), 2.1f),
                                 1e-2f,
                                 1e-4f);
}

TEST(gsplat_transform, NonUniformScale)
{
  expect_covariances_transformed(rotation_matrix(float3(1, 2, 3), 0.7f) *
                                     math::from_scale<float3x3>(float3(1.0f, 3.0f, 0.2f)),
                                 1e-2f,
                                 1e-3f);
}

TEST(gsplat_transform, Shear)
{
  float3x3 shear = float3x3::identity();
  shear[1][0] = 0.8f;
  expect_covariances_transformed(shear, 1e-2f, 1e-3f);
}

TEST(gsplat_transform, MirroredNonUniformScale)
{
  expect_covariances_transformed(rotation_matrix(float3(1, 2, 3), 0.7f) *
                                     math::from_scale<float3x3>(float3(-1.0f, 3.0f, 0.2f)),
                                 1e-2f,
                                 1e-3f);
}

/* Decomposing `A Σ Aᵀ` directly would square the scales and lose the small axes of flat splats to
 * round-off. */
TEST(gsplat_transform, FlatSplatsUnderNonUniformScale)
{
  expect_covariances_transformed(rotation_matrix(float3(1, 2, 3), 0.7f) *
                                     math::from_scale<float3x3>(float3(1.0f, 3.0f, 0.2f)),
                                 1e-3f,
                                 1e-2f);
}

TEST(gsplat_transform, DegenerateMatrixGivesFiniteValues)
{
  RandomNumberGenerator rng(4);
  Array<math::Quaternion> rotations(16);
  Array<float3> scales(16);
  for (const int i : rotations.index_range()) {
    rotations[i] = random_rotation(rng);
    scales[i] = random_scale(rng, 1e-2f);
  }
  transform(math::from_scale<float3x3>(float3(1.0f, 1.0f, 0.0f)) *
                rotation_matrix(float3(1, 2, 3), 0.7f),
            rotations,
            scales,
            {});
  for (const int i : rotations.index_range()) {
    EXPECT_TRUE(std::isfinite(rotations[i].w) && std::isfinite(rotations[i].x) &&
                std::isfinite(rotations[i].y) && std::isfinite(rotations[i].z));
    EXPECT_TRUE(std::isfinite(scales[i].x) && std::isfinite(scales[i].y) &&
                std::isfinite(scales[i].z));
  }
}

static void expect_sh_rotated_by(const float3x3 &linear, const float3x3 &expected_rotation)
{
  constexpr int degree = 3;
  constexpr int coefficients_num = 15;
  constexpr int points_num = 4;
  RandomNumberGenerator rng(5);
  Array<Array<float3>> sh(coefficients_num, Array<float3>(points_num));
  Array<MutableSpan<float3>> sh_spans(coefficients_num);
  for (const int i : sh.index_range()) {
    for (float3 &value : sh[i]) {
      value = random_float3(rng);
    }
    sh_spans[i] = sh[i];
  }
  const Array<Array<float3>> original = sh;

  transform(linear, {}, {}, sh_spans);

  const math::SphericalHarmonicsRotation rotation(expected_rotation, degree);
  for (const int point : IndexRange(points_num)) {
    Array<float3> expected(coefficients_num);
    for (const int i : IndexRange(coefficients_num)) {
      expected[i] = original[i][point];
    }
    rotation.rotate(expected);
    for (const int i : IndexRange(coefficients_num)) {
      SCOPED_TRACE(i);
      EXPECT_V3_NEAR(sh[i][point], expected[i], 1e-5f);
    }
  }
}

TEST(gsplat_transform, SphericalHarmonicsUnderRotation)
{
  const float3x3 rotation = rotation_matrix(float3(1, 2, 3), 0.7f);
  expect_sh_rotated_by(rotation, rotation);
}

TEST(gsplat_transform, SphericalHarmonicsUnderScaledMirror)
{
  const float3x3 mirror = math::from_scale<float3x3>(float3(-1.0f, 1.0f, 1.0f)) *
                          rotation_matrix(float3(1, 2, 3), 0.7f);
  expect_sh_rotated_by(mirror * 2.0f, mirror);
}

/* The polar decomposition of R * D, with D a positive diagonal matrix, has the rotation R. */
TEST(gsplat_transform, SphericalHarmonicsUnderNonUniformScaleUseRotationPart)
{
  const float3x3 rotation = rotation_matrix(float3(1, 2, 3), 0.7f);
  expect_sh_rotated_by(rotation * math::from_scale<float3x3>(float3(1.0f, 3.0f, 0.2f)), rotation);
}

TEST(gsplat_transform, DegreeForCoefficientsNum)
{
  EXPECT_EQ(sh_degree_for_coefficients_num(0), 0);
  EXPECT_EQ(sh_degree_for_coefficients_num(2), 0);
  EXPECT_EQ(sh_degree_for_coefficients_num(3), 1);
  EXPECT_EQ(sh_degree_for_coefficients_num(7), 1);
  EXPECT_EQ(sh_degree_for_coefficients_num(8), 2);
  EXPECT_EQ(sh_degree_for_coefficients_num(15), 3);
  EXPECT_EQ(sh_degree_for_coefficients_num(24), 4);
  EXPECT_EQ(sh_degree_for_coefficients_num(100), 4);
}

class GSplatAttributesTest : public BlenderGTestBase {};

static PointCloud *create_splats(const PointCloudType type, const int sh_num)
{
  PointCloud *pointcloud = BKE_pointcloud_new_nomain(type, 4);
  MutableAttributeAccessor attributes = pointcloud->attributes_for_write();
  RandomNumberGenerator rng(11);
  SpanAttributeWriter rotations = attributes.lookup_or_add_for_write_only_span<math::Quaternion>(
      "rotation", AttrDomain::Point);
  for (math::Quaternion &rotation : rotations.span) {
    rotation = random_rotation(rng);
  }
  rotations.finish();
  SpanAttributeWriter scales = attributes.lookup_or_add_for_write_only_span<float3>(
      "scale", AttrDomain::Point);
  for (float3 &scale : scales.span) {
    scale = random_scale(rng, 1e-2f);
  }
  scales.finish();
  for (const int i : IndexRange(sh_num)) {
    SpanAttributeWriter sh = attributes.lookup_or_add_for_write_only_span<float3>(
        fmt::format("radiance:sh_{}", i), AttrDomain::Point);
    for (float3 &value : sh.span) {
      value = random_float3(rng);
    }
    sh.finish();
  }
  return pointcloud;
}

template<typename T> static Array<T> read(const PointCloud &pointcloud, const StringRef name)
{
  const VArraySpan<T> values = *pointcloud.attributes().lookup<T>(name, AttrDomain::Point);
  return Array<T>(values);
}

static float4x4 test_transform()
{
  return math::from_loc_rot_scale<float4x4>(
      float3(1, 2, 3),
      math::to_quaternion(math::AxisAngle(math::normalize(float3(1, 2, 3)), 0.7f)),
      float3(1.0f, 3.0f, 0.2f));
}

static void expect_equal(const Span<math::Quaternion> a, const Span<math::Quaternion> b)
{
  ASSERT_EQ(a.size(), b.size());
  for (const int i : a.index_range()) {
    const float4 ai(a[i]);
    const float4 bi(b[i]);
    EXPECT_V4_NEAR(ai, bi, 1e-6f);
  }
}

static void expect_equal(const Span<float3> a, const Span<float3> b)
{
  ASSERT_EQ(a.size(), b.size());
  for (const int i : a.index_range()) {
    EXPECT_V3_NEAR(a[i], b[i], 1e-6f);
  }
}

TEST_F(GSplatAttributesTest, PointsTypeIsNotTransformed)
{
  PointCloud *pointcloud = create_splats(PointCloudType::Points, 3);
  const Array<math::Quaternion> rotations = read<math::Quaternion>(*pointcloud, "rotation");
  const Array<float3> scales = read<float3>(*pointcloud, "scale");
  const Array<float3> sh_0 = read<float3>(*pointcloud, "radiance:sh_0");

  transform_attributes(test_transform(), *pointcloud);

  expect_equal(read<math::Quaternion>(*pointcloud, "rotation"), rotations);
  expect_equal(read<float3>(*pointcloud, "scale"), scales);
  expect_equal(read<float3>(*pointcloud, "radiance:sh_0"), sh_0);
  BKE_id_free(nullptr, pointcloud);
}

TEST_F(GSplatAttributesTest, GSplatTypeIsTransformed)
{
  PointCloud *pointcloud = create_splats(PointCloudType::GSplat, 3);
  Array<math::Quaternion> rotations = read<math::Quaternion>(*pointcloud, "rotation");
  Array<float3> scales = read<float3>(*pointcloud, "scale");
  Array<Array<float3>> sh(3);
  Array<MutableSpan<float3>> sh_spans(3);
  for (const int i : IndexRange(3)) {
    sh[i] = read<float3>(*pointcloud, fmt::format("radiance:sh_{}", i));
    sh_spans[i] = sh[i];
  }
  transform(float3x3(test_transform()), rotations, scales, sh_spans);

  transform_attributes(test_transform(), *pointcloud);

  expect_equal(read<math::Quaternion>(*pointcloud, "rotation"), rotations);
  expect_equal(read<float3>(*pointcloud, "scale"), scales);
  for (const int i : IndexRange(3)) {
    expect_equal(read<float3>(*pointcloud, fmt::format("radiance:sh_{}", i)), sh[i]);
  }
  BKE_id_free(nullptr, pointcloud);
}

TEST_F(GSplatAttributesTest, ScaleWithoutRotationIsNotTransformed)
{
  PointCloud *pointcloud = create_splats(PointCloudType::GSplat, 0);
  pointcloud->attributes_for_write().remove("rotation");
  const Array<float3> scales = read<float3>(*pointcloud, "scale");

  transform_attributes(test_transform(), *pointcloud);

  expect_equal(read<float3>(*pointcloud, "scale"), scales);
  EXPECT_FALSE(pointcloud->attributes().contains("rotation"));
  BKE_id_free(nullptr, pointcloud);
}

TEST_F(GSplatAttributesTest, WrongRotationTypeIsNotTransformed)
{
  PointCloud *pointcloud = create_splats(PointCloudType::GSplat, 0);
  MutableAttributeAccessor attributes = pointcloud->attributes_for_write();
  attributes.remove("rotation");
  attributes.add<float4>("rotation",
                         AttrDomain::Point,
                         AttributeInitVArray(VArray<float4>::from_single(float4(1, 0, 0, 0), 4)));
  const Array<float3> scales = read<float3>(*pointcloud, "scale");

  transform_attributes(test_transform(), *pointcloud);

  expect_equal(read<float3>(*pointcloud, "scale"), scales);
  BKE_id_free(nullptr, pointcloud);
}

TEST_F(GSplatAttributesTest, IncompleteSphericalHarmonicsBandIsNotTransformed)
{
  PointCloud *pointcloud = create_splats(PointCloudType::GSplat, 5);
  Array<Array<float3>> sh(5);
  for (const int i : IndexRange(5)) {
    sh[i] = read<float3>(*pointcloud, fmt::format("radiance:sh_{}", i));
  }
  const float3x3 rotation = rotation_matrix(float3(1, 2, 3), 0.7f);

  transform_attributes(float4x4(rotation), *pointcloud);

  const math::SphericalHarmonicsRotation band_1(rotation, 1);
  for (const int point : IndexRange(4)) {
    Array<float3> expected = {sh[0][point], sh[1][point], sh[2][point]};
    band_1.rotate(expected);
    for (const int i : IndexRange(3)) {
      EXPECT_V3_NEAR(
          read<float3>(*pointcloud, fmt::format("radiance:sh_{}", i))[point], expected[i], 1e-5f);
    }
  }
  expect_equal(read<float3>(*pointcloud, "radiance:sh_3"), sh[3]);
  expect_equal(read<float3>(*pointcloud, "radiance:sh_4"), sh[4]);
  BKE_id_free(nullptr, pointcloud);
}

}  // namespace blender::bke::gsplat::tests
