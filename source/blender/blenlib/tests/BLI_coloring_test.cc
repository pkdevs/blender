/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_coloring.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

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
  constexpr int total_verts = 20;
  Array<int2> edges(total_verts);
  for (const int i : IndexRange(total_verts)) {
    edges[i] = int2(i, math::mod_periodic<int>(i + 1, total_verts));
  }

  Array<int> expected_colors(total_verts);
  for (const int i : IndexRange(total_verts)) {
    expected_colors[i] = (i % 2);
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
      BLI_assert(first_vert != second_vert);
      edges.append({first_vert, second_vert});
    }
  }

  test_edge_coloring(total_verts, edges, 127);
}

TEST(coloring, PointColoring1)
{
  Array<int> colors(5);
  color_verts_by_groups({0, 1, 0, 1, 0}, IndexRange(2), colors);
  EXPECT_EQ_SPAN<int>(colors, {0, 0, 1, 1, 2});
}

TEST(coloring, PointColoring2)
{
  Array<int> colors(1);
  color_verts_by_groups({99}, IndexRange(100), colors);
  EXPECT_EQ_SPAN<int>(colors, {0});
}

TEST(coloring, PointColoring3)
{
  Array<int> colors(5);
  color_verts_by_groups({1, 1, 1, 1, 1}, IndexRange(2), colors);
  EXPECT_EQ_SPAN<int>(colors, {0, 1, 2, 3, 4});
}

TEST(coloring, PointColoring4)
{
  Array<int> colors(5);
  color_verts_by_groups({0, 1, 9, 1, 0}, IndexRange(10), colors);
  EXPECT_EQ_SPAN<int>(colors, {0, 0, 0, 1, 1});
}

static void test_groups_coloring(const Span<Span<int>> groups,
                                 const int expected_colors_num,
                                 const Span<int> expected_colors)
{
  Array<int> group_offsets_data(groups.size() + 1);
  for (const int i : groups.index_range()) {
    group_offsets_data[i] = groups[i].size();
  }
  const OffsetIndices<int> group_offsets = offset_indices::accumulate_counts_to_offsets(
      group_offsets_data);
  Array<int> group_indices_data(group_offsets.total_size());
  for (const int i : groups.index_range()) {
    group_indices_data.as_mutable_span().slice(group_offsets[i]).copy_from(groups[i]);
  }
  const GroupedSpan<int> vert_groups(group_offsets, group_indices_data.as_span());

  Array<int> colors(groups.size());
  LinearAllocator memory;
  const int total_colors = color_verts_by_groups(vert_groups, colors.as_mutable_span());

  EXPECT_EQ(total_colors, expected_colors_num);

  EXPECT_EQ_SPAN<int>(colors, expected_colors);

  Vector<IndexMask> split_colors(total_colors);
  IndexMask::from_groups<int>(
      vert_groups.index_range(), memory, [&](const int i) { return colors[i]; }, split_colors);
  EXPECT_TRUE(is_valid_verts_by_groups_coloring(vert_groups, split_colors));
}

TEST(coloring, GroupsColoring1)
{
  test_groups_coloring({{0}, {0}, {0}}, 3, {1, 1, 0});
}

TEST(coloring, GroupsColoring2)
{
  test_groups_coloring({{0}, {1}, {2}}, 1, {0, 0, 0});
}

TEST(coloring, GroupsColoring3)
{
  test_groups_coloring({{0}, {0, 1}, {0, 2}}, 2, {0, 1, 1});
}

TEST(coloring, GroupsColoring4)
{
  test_groups_coloring({{0, 1}, {0}, {2}}, 2, {0, 1, 0});
}

}  // namespace blender::tests
