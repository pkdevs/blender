/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_math_vector_types.hh"

#include "BKE_customdata.hh"

namespace blender::bke::tests {

TEST(customdata, interp_float2_precision)
{
  /* Test UV interpolation precision with a variety of weights and values. */
  const float weight_sets[][3] = {{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
                                  {0.1f, 0.2f, 0.7f},
                                  {0.45f, 0.45f, 0.1f},
                                  {1.3f, -0.1f, -0.2f},
                                  {-0.7f, 0.9f, 0.8f},
                                  {1.0f, 0.0f, 0.0f},
                                  {1e4f, -5e3f, 1.0f - 5e3f}};
  const float constants[] = {0.1f, 0.7f, -3.3f, 1000.1f, 1e6f, -1e6f};

  CustomData data;
  CustomData_reset(&data);
  float2 *values = static_cast<float2 *>(
      CustomData_add_layer(&data, CD_PROP_FLOAT2, CD_CONSTRUCT, 4));
  const int src_indices[3] = {0, 1, 2};

  for (const float constant : constants) {
    for (const auto &weights : weight_sets) {
      for (int i = 0; i < 3; i++) {
        values[i] = float2(i, constant);
      }
      CustomData_interp(&data, &data, src_indices, weights, 3, 3);
      EXPECT_NEAR(values[3].x, weights[1] + 2.0f * weights[2], 1e-6f);
      EXPECT_EQ(values[3].y, constant);
    }
  }

  /* Zero weights, as from a degenerate face, return the first source. */
  const float zero_weights[3] = {0.0f, 0.0f, 0.0f};
  CustomData_interp(&data, &data, src_indices, zero_weights, 3, 3);
  EXPECT_EQ(values[3], values[0]);

  CustomData_free(&data);
}

}  // namespace blender::bke::tests
