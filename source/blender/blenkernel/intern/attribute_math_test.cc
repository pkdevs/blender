/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_attribute_math.hh"

namespace blender::bke::tests {

TEST(attribute_math, mix_float2_precision)
{
  /* Normalized weights must preserve a shared UV component while interpolating the other one. */
  const int indices[] = {2, 0, 1};
  const float weight_sets[][3] = {{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
                                  {0.1f, 0.2f, 0.7f},
                                  {0.45f, 0.45f, 0.1f},
                                  {1.0f, 0.0f, 0.0f}};
  for (const float constant : {0.1f, 0.7f, -3.3f, 1000.1f, 1e6f, -1e6f}) {
    const float2 values[] = {float2(0, constant), float2(1, constant), float2(2, constant)};
    for (const auto &weights : weight_sets) {
      const float2 result = attribute_math::mix_indices(
          Span<float2>(values, 3), Span<int>(indices, 3), Span<float>(weights, 3));
      EXPECT_NEAR(result.x, 2.0f * weights[0] + weights[2], 1e-6f);
      EXPECT_EQ(result.y, constant);
    }
  }
}

}  // namespace blender::bke::tests
