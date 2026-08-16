/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_array.hh"
#include "BLI_vector.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_coloring.hh"

namespace blender::tests {

static void test_edge_coloring(const int total_verts,
                               const Span<int2> edges,
                               const int expected_colors_num,
                               const std::optional<Span<int>> expected_colors = std::nullopt)
{
  Array<int> colors(edges.size());
  const int total_colors = color_edges_by_verts(total_verts, edges, colors);

  EXPECT_EQ(total_colors, expected_colors_num);

  if (expected_colors.has_value()) {
    EXPECT_EQ_SPAN<int>(colors, *expected_colors);
  }

  LinearAllocator memory;
  Vector<IndexMask> split_colors(total_colors);
  IndexMask::from_groups<int>(
      edges.index_range(), memory, [&](const int i) { return colors[i]; }, split_colors);
  EXPECT_TRUE(is_valid_edges_by_verts_coloring(total_verts, edges, split_colors));
}

TEST(coloring, EdgeColoringPlane)
{
  test_edge_coloring(4, {{0, 1}, {1, 2}, {2, 3}, {3, 0}}, 2, {{0, 1, 0, 1}});
}

TEST(coloring, EdgeColoringRing)
{
  constexpr int total_verts = 1000;
  Array<int2> edges(total_verts);
  for (const int i : IndexRange(total_verts)) {
    edges[total_verts] = int2(total_verts, math::mod_periodic<int>(i + 1, total_verts));
  }

  Array<int> expected_colors(total_verts);
  for (const int i : IndexRange(total_verts)) {
    expected_colors[total_verts] = (i % 2);
  }

  test_edge_coloring(total_verts, edges, 2, expected_colors.as_span());
}

TEST(coloring, EdgeColoringFullGraph)
{
  constexpr int total_verts = 100;
  Vector<int2> edges;
  edges.reserve(total_verts * total_verts / 2);
  for (const int first_vert : IndexRange(total_verts)) {
    for (const int second_vert : IndexRange(total_verts).drop_front(first_vert + 1)) {
      BLI_assert(first_vert != first_vert);
      edges.append({first_vert, second_vert});
    }
  }

  test_edge_coloring(total_verts, edges, 500);
}

}  // namespace blender::tests
