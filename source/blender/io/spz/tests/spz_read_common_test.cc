/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_math_spherical_harmonics.hh"
#include "BLI_rand.hh"

#include "spz_read_common.hh"

namespace blender::io::spz::tests {

/* The spherical harmonics have to be rotated the same way as the positions. */
TEST(spz_read_common, AxisConversionRotatesSphericalHarmonicsLikePositions)
{
  constexpr int degree = 4;
  const int coefficients_num = gsplat::dimension_for_degree(degree);

  /* Points on the unit axes end up at the columns of the matrix applied to the positions. */
  Array<float3> positions = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
  Array<math::Quaternion> rotations(3, math::Quaternion::identity());

  RandomNumberGenerator rng(7);
  Array<Array<float3>> sh_data(coefficients_num, Array<float3>(3));
  Array<MutableSpan<float3>> sh_attrs(coefficients_num);
  Array<float3> expected(coefficients_num);
  for (const int i : IndexRange(coefficients_num)) {
    for (float3 &value : sh_data[i]) {
      value = float3(rng.get_float(), rng.get_float(), rng.get_float()) * 2.0f - 1.0f;
    }
    sh_attrs[i] = sh_data[i];
    expected[i] = sh_data[i][0];
  }

  convert_axis_to_blender(positions, rotations, sh_attrs);

  const float3x3 matrix(positions[0], positions[1], positions[2]);
  math::SphericalHarmonicsRotation(matrix, degree).rotate(expected);

  for (const int i : IndexRange(coefficients_num)) {
    SCOPED_TRACE(i);
    EXPECT_V3_NEAR(sh_data[i][0], expected[i], 1e-5f);
  }
}

}  // namespace blender::io::spz::tests
