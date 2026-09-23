/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_spherical_harmonics.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"

#include <array>
#include <cmath>

namespace blender::math::tests {

/* Real spherical harmonics basis functions of a band, in the same convention as the renderers
 * (see `intern/cycles/util/math_spherical_harmonics.h`): the standard real form multiplied by
 * (-1)^|m|, ordered from m = -l to m = l. */
static void eval_basis(const int l, const float3 v, MutableSpan<float> r_basis)
{
  const float x = v.x, y = v.y, z = v.z;
  const float pi = float(M_PI);
  switch (l) {
    case 1: {
      const float c = std::sqrt(3.0f / (4.0f * pi));
      r_basis[0] = -c * y;
      r_basis[1] = c * z;
      r_basis[2] = -c * x;
      break;
    }
    case 2: {
      const float c0 = 0.25f * std::sqrt(5.0f / pi);
      const float c1 = 0.5f * std::sqrt(15.0f / pi);
      r_basis[0] = c1 * x * y;
      r_basis[1] = -c1 * y * z;
      r_basis[2] = c0 * (3.0f * z * z - 1.0f);
      r_basis[3] = -c1 * x * z;
      r_basis[4] = 0.5f * c1 * (x * x - y * y);
      break;
    }
    case 3: {
      const float c3 = 0.25f * std::sqrt(35.0f / (2.0f * pi));
      const float c2 = 0.5f * std::sqrt(105.0f / pi);
      const float c1 = 0.25f * std::sqrt(21.0f / (2.0f * pi));
      const float c0 = 0.25f * std::sqrt(7.0f / pi);
      r_basis[0] = -c3 * y * (3.0f * x * x - y * y);
      r_basis[1] = c2 * x * y * z;
      r_basis[2] = -c1 * y * (5.0f * z * z - 1.0f);
      r_basis[3] = c0 * z * (5.0f * z * z - 3.0f);
      r_basis[4] = -c1 * x * (5.0f * z * z - 1.0f);
      r_basis[5] = 0.5f * c2 * z * (x * x - y * y);
      r_basis[6] = -c3 * x * (x * x - 3.0f * y * y);
      break;
    }
    case 4: {
      const float c4 = 0.75f * std::sqrt(35.0f / pi);
      const float c3 = 0.75f * std::sqrt(35.0f / (2.0f * pi));
      const float c2 = 0.75f * std::sqrt(5.0f / pi);
      const float c1 = 0.75f * std::sqrt(5.0f / (2.0f * pi));
      const float c0 = (3.0f / 16.0f) * std::sqrt(1.0f / pi);
      const float x2 = x * x, y2 = y * y, z2 = z * z;
      r_basis[0] = c4 * x * y * (x2 - y2);
      r_basis[1] = -c3 * y * z * (3.0f * x2 - y2);
      r_basis[2] = c2 * x * y * (7.0f * z2 - 1.0f);
      r_basis[3] = -c1 * y * z * (7.0f * z2 - 3.0f);
      r_basis[4] = c0 * (35.0f * z2 * z2 - 30.0f * z2 + 3.0f);
      r_basis[5] = -c1 * x * z * (7.0f * z2 - 3.0f);
      r_basis[6] = 0.5f * c2 * (x2 - y2) * (7.0f * z2 - 1.0f);
      r_basis[7] = -c3 * x * z * (x2 - 3.0f * y2);
      r_basis[8] = 0.25f * c4 * (x2 * (x2 - 3.0f * y2) - y2 * (3.0f * x2 - y2));
      break;
    }
    default:
      BLI_assert_unreachable();
  }
}

/* Evaluate the function represented by the coefficients of bands 1 to `degree`. */
static float3 eval_sh(const Span<float3> coefficients, const int degree, const float3 direction)
{
  float3 result(0.0f);
  std::array<float, 9> basis;
  int offset = 0;
  for (int l = 1; l <= degree; l++) {
    eval_basis(l, direction, basis);
    for (int i = 0; i < 2 * l + 1; i++) {
      result += basis[i] * coefficients[offset + i];
    }
    offset += 2 * l + 1;
  }
  return result;
}

static int coefficients_num(const int degree)
{
  return (degree + 1) * (degree + 1) - 1;
}

static Array<float3> random_coefficients(const int degree, RandomNumberGenerator &rng)
{
  Array<float3> coefficients(coefficients_num(degree));
  for (float3 &c : coefficients) {
    c = float3(rng.get_float(), rng.get_float(), rng.get_float()) * 2.0f - 1.0f;
  }
  return coefficients;
}

/* The rotated coefficients must represent g(d) = f(M^-1 * d), where f is the original function. */
static void expect_matches_transformed_evaluation(const float3x3 &matrix, const int degree)
{
  RandomNumberGenerator rng(4891);
  const Array<float3> original = random_coefficients(degree, rng);
  Array<float3> rotated = original;
  SphericalHarmonicsRotation(matrix, degree).rotate(rotated);

  const float3x3 inverse = math::transpose(matrix);
  for (int i = 0; i < 64; i++) {
    const float3 direction = rng.get_unit_float3();
    EXPECT_V3_NEAR(eval_sh(rotated, degree, direction),
                   eval_sh(original, degree, inverse * direction),
                   1e-4f);
  }
}

static float3x3 rotation_matrix(const float3 axis, const float angle)
{
  return from_rotation<float3x3>(AxisAngle(math::normalize(axis), angle));
}

TEST(math_spherical_harmonics, Identity)
{
  RandomNumberGenerator rng(1);
  const Array<float3> original = random_coefficients(4, rng);
  Array<float3> rotated = original;
  SphericalHarmonicsRotation(float3x3::identity(), 4).rotate(rotated);
  for (const int i : original.index_range()) {
    EXPECT_V3_NEAR(rotated[i], original[i], 1e-6f);
  }
}

/* Band 1 transforms like a vector: a quarter turn about X maps Y to Z. The expected values are
 * independent of the evaluation-based checks below. */
TEST(math_spherical_harmonics, QuarterTurnAboutXBand1)
{
  Array<float3> coefficients = {float3(1.0f), float3(2.0f), float3(3.0f)};
  SphericalHarmonicsRotation(rotation_matrix(float3(1, 0, 0), float(M_PI_2)), 1)
      .rotate(coefficients);
  EXPECT_V3_NEAR(coefficients[0], float3(2.0f), 1e-6f);
  EXPECT_V3_NEAR(coefficients[1], float3(-1.0f), 1e-6f);
  EXPECT_V3_NEAR(coefficients[2], float3(3.0f), 1e-6f);
}

TEST(math_spherical_harmonics, QuarterTurns)
{
  expect_matches_transformed_evaluation(rotation_matrix(float3(1, 0, 0), float(M_PI_2)), 4);
  expect_matches_transformed_evaluation(rotation_matrix(float3(0, 1, 0), float(M_PI_2)), 4);
  expect_matches_transformed_evaluation(rotation_matrix(float3(0, 0, 1), float(M_PI_2)), 4);
}

TEST(math_spherical_harmonics, ArbitraryRotations)
{
  expect_matches_transformed_evaluation(rotation_matrix(float3(1, 2, 3), 0.7f), 4);
  expect_matches_transformed_evaluation(rotation_matrix(float3(-0.3f, 0.1f, 0.9f), 2.9f), 4);
  expect_matches_transformed_evaluation(rotation_matrix(float3(0, 1, 1), float(M_PI)), 4);
}

TEST(math_spherical_harmonics, Reflections)
{
  expect_matches_transformed_evaluation(from_scale<float3x3>(float3(-1.0f, 1.0f, 1.0f)), 4);
  expect_matches_transformed_evaluation(from_scale<float3x3>(float3(-1.0f)), 4);
  expect_matches_transformed_evaluation(
      from_scale<float3x3>(float3(1.0f, 1.0f, -1.0f)) * rotation_matrix(float3(1, 2, 3), 0.7f), 4);
}

TEST(math_spherical_harmonics, LowerDegrees)
{
  const float3x3 matrix = rotation_matrix(float3(2, -1, 1), 1.3f);
  for (int degree = 1; degree <= 3; degree++) {
    expect_matches_transformed_evaluation(matrix, degree);
  }
}

}  // namespace blender::math::tests
