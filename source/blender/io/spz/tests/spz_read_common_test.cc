/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_spherical_harmonics.hh"
#include "BLI_rand.hh"

#include "spz_read_common.hh"

namespace blender::io::spz::tests {

/* The axis conversion rotates the spherical harmonics by a quarter turn about X, using a
 * hard-coded table. It has to agree with the general rotation for every band. */
TEST(spz_read_common, AxisConversionMatchesGeneralRotation)
{
  constexpr int degree = 4;
  const int coefficients_num = gsplat::dimension_for_degree(degree);

  RandomNumberGenerator rng(7);
  Array<Array<float3>> sh_data(coefficients_num, Array<float3>(1));
  Array<float3> expected(coefficients_num);
  for (const int i : IndexRange(coefficients_num)) {
    sh_data[i][0] = float3(rng.get_float(), rng.get_float(), rng.get_float()) * 2.0f - 1.0f;
    expected[i] = sh_data[i][0];
  }
  Array<MutableSpan<float3>> sh_attrs(coefficients_num);
  for (const int i : IndexRange(coefficients_num)) {
    sh_attrs[i] = sh_data[i];
  }

  Array<float3> positions(1, float3(0.0f));
  Array<math::Quaternion> rotations(1, math::Quaternion::identity());
  convert_axis_to_blender(positions, rotations, sh_attrs);

  const float3x3 quarter_turn_x = math::from_rotation<float3x3>(
      math::AxisAngle(math::AxisSigned::X_POS, math::AngleRadian(float(M_PI_2))));
  math::SphericalHarmonicsRotation(quarter_turn_x, degree).rotate(expected);

  for (const int i : IndexRange(coefficients_num)) {
    SCOPED_TRACE(i);
    EXPECT_V3_NEAR(sh_data[i][0], expected[i], 1e-5f);
  }
}

}  // namespace blender::io::spz::tests
