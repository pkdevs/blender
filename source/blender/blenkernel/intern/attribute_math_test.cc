/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_attribute_math.hh"

namespace blender::bke::tests {

TEST(attribute_math, mix_float2_precision)
{
  /* Mesh operations interpolate UVs through both overloads. A shared component must stay constant,
   * while a varying component must still interpolate, including with unnormalized signed weights.
   */
  const int indices[] = {2, 0, 1};
  const float weight_sets[][3] = {{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
                                  {0.1f, 0.2f, 0.7f},
                                  {1.0f, 2.0f, 3.0f},
                                  {2.0f, -1.0f, 2.0f},
                                  {-1.0f, -2.0f, -3.0f}};
  float2 values[3];
  const Span<float2> source_values(values, 3);
  const Span<int> source_indices(indices, 3);
  for (const float constant : {0.1f, 0.7f, -3.3f, 1000.1f, 1e6f, -1e6f}) {
    for (int i = 0; i < 3; i++) {
      values[i] = float2(i, constant);
    }
    const float2 average = attribute_math::mix_indices(source_values, source_indices);
    EXPECT_EQ(average.x, 1.0f);
    EXPECT_EQ(average.y, constant);
    for (const auto &weights : weight_sets) {
      const float2 weighted = attribute_math::mix_indices(
          source_values, source_indices, Span<float>(weights, 3));
      const float weight_sum = weights[0] + weights[1] + weights[2];
      const float expected_u = (2.0f * weights[0] + weights[2]) / weight_sum;
      EXPECT_NEAR(weighted.x, expected_u, 1e-6f);
      EXPECT_EQ(weighted.y, constant);
    }
  }

  /* Empty inputs and zero weight totals keep their existing zero result. */
  const float zero_weight_sets[][3] = {{0, 0, 0}, {1, -1, 0}};
  for (const auto &weights : zero_weight_sets) {
    EXPECT_EQ(attribute_math::mix_indices(source_values, source_indices, Span<float>(weights, 3)),
              float2(0.0f));
  }
  EXPECT_EQ(attribute_math::mix_indices(source_values, Span<int>()), float2(0.0f));
  EXPECT_EQ(attribute_math::mix_indices(source_values, Span<int>(), Span<float>()), float2(0.0f));

  /* A single nonzero weight must return its source exactly, even when it is negative and another
   * source is far away. The reference must follow the weight's magnitude rather than its sign. */
  const float2 distant_values[] = {float2(0.7f, 0.7f), float2(0.1f, 0.7f), float2(1e6f, 0.7f)};
  const float one_hot_weights[][3] = {{0, 0, 1}, {0, 0, -1}};
  for (const auto &weights : one_hot_weights) {
    EXPECT_EQ(attribute_math::mix_indices(
                  Span<float2>(distant_values, 3), source_indices, Span<float>(weights, 3)),
              distant_values[1]);
  }
}

}  // namespace blender::bke::tests
