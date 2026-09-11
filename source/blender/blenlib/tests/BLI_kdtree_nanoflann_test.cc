/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdtree.hh"
#include "BLI_kdtree_nanoflann.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

namespace blender::tests {

using Nearest = KDTreeNanoflann<float3>::Nearest;

static Array<float3> random_points(const int points_num, const int seed = 0)
{
  RandomNumberGenerator random(seed);
  Array<float3> positions(points_num);
  for (const int i : positions.index_range()) {
    positions[i] = float3(random.get_float(), random.get_float(), random.get_float());
  }
  return positions;
}

/** Reference implementation used to check the results of the tree. */
static Vector<Nearest> brute_force_sorted(const Span<float3> positions,
                                          const Span<int> indices,
                                          const float3 &position)
{
  Vector<Nearest> result;
  for (const int i : indices) {
    result.append({i, math::distance_squared(positions[i], position)});
  }
  std::ranges::sort(result, [](const Nearest &a, const Nearest &b) {
    return a.distance_sq < b.distance_sq || (a.distance_sq == b.distance_sq && a.index < b.index);
  });
  return result;
}

static Vector<int> all_indices(const int points_num)
{
  Vector<int> indices(points_num);
  array_utils::fill_index_range(indices.as_mutable_span());
  return indices;
}

TEST(kdtree_nanoflann, Empty)
{
  const Array<float3> positions(0);
  const KDTreeNanoflann<float3> tree(positions);
  EXPECT_TRUE(tree.is_empty());
  EXPECT_EQ(tree.size(), 0);
  EXPECT_EQ(tree.find_nearest(float3(0.0f)), -1);
  EXPECT_EQ(tree.count_in_radius(float3(0.0f), 100.0f), 0);

  Array<Nearest> nearest(4);
  EXPECT_EQ(tree.find_nearest_n(float3(0.0f), nearest), 0);

  Vector<Nearest> in_radius;
  EXPECT_EQ(tree.find_in_radius(float3(0.0f), 100.0f, in_radius), 0);
}

TEST(kdtree_nanoflann, SinglePoint)
{
  const Array<float3> positions = {float3(1.0f, 2.0f, 3.0f)};
  const KDTreeNanoflann<float3> tree(positions);
  EXPECT_EQ(tree.size(), 1);

  float distance_sq = -1.0f;
  EXPECT_EQ(tree.find_nearest(float3(1.0f, 2.0f, 4.0f), &distance_sq), 0);
  EXPECT_FLOAT_EQ(distance_sq, 1.0f);

  /* The only point is filtered out. */
  EXPECT_EQ(tree.find_nearest_filtered(float3(0.0f), [](const int /*index*/) { return false; }),
            -1);
}

TEST(kdtree_nanoflann, FindNearest)
{
  const Array<float3> positions = random_points(1000);
  const KDTreeNanoflann<float3> tree(positions);
  EXPECT_EQ(tree.size(), 1000);
  EXPECT_EQ(tree.points().data(), positions.data());

  const Vector<int> indices = all_indices(positions.size());
  const Array<float3> queries = random_points(50, 1);
  for (const float3 &query : queries) {
    const Vector<Nearest> expected = brute_force_sorted(positions, indices, query);
    float distance_sq = -1.0f;
    const int index = tree.find_nearest(query, &distance_sq);
    EXPECT_EQ(index, expected.first().index);
    EXPECT_FLOAT_EQ(distance_sq, expected.first().distance_sq);
  }
}

TEST(kdtree_nanoflann, FindNearestFiltered)
{
  const Array<float3> positions = random_points(500);
  const KDTreeNanoflann<float3> tree(positions);
  const Vector<int> indices = all_indices(positions.size());

  /* Skipping the search point itself is the common use of the filter. */
  for (const int i : positions.index_range()) {
    const Vector<Nearest> expected = brute_force_sorted(positions, indices, positions[i]);
    ASSERT_EQ(expected.first().index, i);
    const int index = tree.find_nearest_filtered(positions[i],
                                                 [&](const int other) { return other != i; });
    EXPECT_EQ(index, expected[1].index);
  }

  /* Only even indices are accepted. */
  const int index = tree.find_nearest_filtered(positions[3],
                                               [](const int other) { return other % 2 == 0; });
  ASSERT_GE(index, 0);
  EXPECT_EQ(index % 2, 0);
  const Vector<Nearest> expected = brute_force_sorted(positions, indices, positions[3]);
  for (const Nearest &nearest : expected) {
    if (nearest.index % 2 == 0) {
      EXPECT_EQ(nearest.index, index);
      break;
    }
  }
}

TEST(kdtree_nanoflann, FindNearestN)
{
  const Array<float3> positions = random_points(400);
  const KDTreeNanoflann<float3> tree(positions);
  const Vector<int> indices = all_indices(positions.size());
  const Array<float3> queries = random_points(20, 2);

  for (const float3 &query : queries) {
    const Vector<Nearest> expected = brute_force_sorted(positions, indices, query);
    Array<Nearest> nearest(7);
    const int found = tree.find_nearest_n(query, nearest);
    ASSERT_EQ(found, 7);
    for (const int i : IndexRange(found)) {
      EXPECT_EQ(nearest[i].index, expected[i].index);
      EXPECT_FLOAT_EQ(nearest[i].distance_sq, expected[i].distance_sq);
      if (i > 0) {
        EXPECT_LE(nearest[i - 1].distance_sq, nearest[i].distance_sq);
      }
    }
  }
}

TEST(kdtree_nanoflann, FindNearestNMoreThanSize)
{
  const Array<float3> positions = random_points(5);
  const KDTreeNanoflann<float3> tree(positions);
  Array<Nearest> nearest(10);
  EXPECT_EQ(tree.find_nearest_n(float3(0.5f), nearest), 5);
}

TEST(kdtree_nanoflann, Radius)
{
  const Array<float3> positions = random_points(600);
  const KDTreeNanoflann<float3> tree(positions);
  const Vector<int> indices = all_indices(positions.size());
  const Array<float3> queries = random_points(20, 3);

  for (const float3 &query : queries) {
    for (const float radius : {0.05f, 0.2f, 2.0f}) {
      Vector<Nearest> expected;
      for (const Nearest &nearest : brute_force_sorted(positions, indices, query)) {
        if (nearest.distance_sq <= radius * radius) {
          expected.append(nearest);
        }
      }

      Vector<Nearest> found;
      EXPECT_EQ(tree.find_in_radius(query, radius, found), expected.size());
      for (const int i : expected.index_range()) {
        EXPECT_EQ(found[i].index, expected[i].index);
        EXPECT_FLOAT_EQ(found[i].distance_sq, expected[i].distance_sq);
      }

      EXPECT_EQ(tree.count_in_radius(query, radius), expected.size());

      Vector<int> visited;
      tree.foreach_in_radius(query, radius, [&](const int index, const float distance_sq) {
        EXPECT_FLOAT_EQ(distance_sq, math::distance_squared(positions[index], query));
        visited.append(index);
      });
      std::ranges::sort(visited);
      ASSERT_EQ(visited.size(), expected.size());
      Vector<int> expected_indices;
      for (const Nearest &nearest : expected) {
        expected_indices.append(nearest.index);
      }
      std::ranges::sort(expected_indices);
      EXPECT_EQ(visited, expected_indices);
    }
  }
}

TEST(kdtree_nanoflann, MaskedBuild)
{
  const Array<float3> positions = random_points(300);
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_predicate(
      positions.index_range(), memory, [](const int64_t i) { return i % 3 == 0; });
  const KDTreeNanoflann<float3> tree(positions, mask);
  EXPECT_EQ(tree.size(), mask.size());

  const Vector<int> indices = mask.to_indices<int>();
  const Array<float3> queries = random_points(20, 4);
  for (const float3 &query : queries) {
    const Vector<Nearest> expected = brute_force_sorted(positions, indices, query);

    /* Results are indices into the original array, not positions in the mask. */
    float distance_sq = -1.0f;
    const int index = tree.find_nearest(query, &distance_sq);
    EXPECT_EQ(index, expected.first().index);
    EXPECT_EQ(index % 3, 0);
    EXPECT_FLOAT_EQ(distance_sq, expected.first().distance_sq);

    Array<Nearest> nearest(4);
    ASSERT_EQ(tree.find_nearest_n(query, nearest), 4);
    for (const int i : IndexRange(4)) {
      EXPECT_EQ(nearest[i].index, expected[i].index);
    }

    Vector<Nearest> found;
    tree.find_in_radius(query, 0.3f, found);
    for (const Nearest &nearest : found) {
      EXPECT_EQ(nearest.index % 3, 0);
      EXPECT_LE(nearest.distance_sq, 0.3f * 0.3f);
    }
  }

  const int filtered = tree.find_nearest_filtered(positions[0],
                                                  [](const int index) { return index != 0; });
  EXPECT_NE(filtered, 0);
  EXPECT_EQ(filtered % 3, 0);
}

TEST(kdtree_nanoflann, MaskedBuildFullRange)
{
  const Array<float3> positions = random_points(100);
  const KDTreeNanoflann<float3> tree(positions, IndexMask(positions.index_range()));
  EXPECT_EQ(tree.size(), 100);
  EXPECT_EQ(tree.find_nearest(positions[42]), 42);
}

TEST(kdtree_nanoflann, EmptyMask)
{
  const Array<float3> positions = random_points(100);
  const KDTreeNanoflann<float3> tree(positions, IndexMask(0));
  EXPECT_TRUE(tree.is_empty());
  EXPECT_EQ(tree.find_nearest(positions[0]), -1);
}

TEST(kdtree_nanoflann, DuplicatePoints)
{
  /* Many points at the same position is a common degenerate case. */
  Array<float3> positions(100);
  for (const int i : positions.index_range()) {
    positions[i] = i < 60 ? float3(1.0f, 1.0f, 1.0f) : float3(float(i), 0.0f, 0.0f);
  }
  const KDTreeNanoflann<float3> tree(positions);

  EXPECT_LT(tree.find_nearest(float3(1.0f, 1.0f, 1.0f)), 60);
  EXPECT_EQ(tree.count_in_radius(float3(1.0f, 1.0f, 1.0f), 0.001f), 60);

  Array<Nearest> nearest(70);
  EXPECT_EQ(tree.find_nearest_n(float3(1.0f, 1.0f, 1.0f), nearest), 70);
  for (const int i : IndexRange(60)) {
    EXPECT_LT(nearest[i].index, 60);
    EXPECT_FLOAT_EQ(nearest[i].distance_sq, 0.0f);
  }

  Vector<Nearest> found;
  EXPECT_EQ(tree.find_in_radius(float3(1.0f, 1.0f, 1.0f), 0.5f, found), 60);
  /* Equal distances are broken by index, so the order is deterministic. */
  for (const int i : IndexRange(60)) {
    EXPECT_EQ(found[i].index, i);
  }
}

/**
 * The reference implementation, so the new one can be checked to produce exactly the same
 * clusters. Points are inserted in index order and #kdtree_calc_duplicates_fast is told to visit
 * them in index order, which is how the geometry nodes use it.
 */
static Array<int> bli_calc_duplicates(const Span<float3> positions,
                                      const Span<int> indices,
                                      const float range,
                                      const int array_size)
{
  KDTree<float3> *tree = kdtree_new<float3>(indices.size());
  for (const int i : indices) {
    kdtree_insert<float3>(tree, i, positions[i]);
  }
  kdtree_balance<float3>(tree);
  Array<int> duplicates(array_size, -1);
  kdtree_calc_duplicates_fast<float3>(tree, range, true, duplicates.data());
  kdtree_free<float3>(tree);
  return duplicates;
}

TEST(kdtree_nanoflann, CalcDuplicatesMatchesOldTree)
{
  for (const int points_num : {10, 100, 2000}) {
    const Array<float3> positions = random_points(points_num, 5);
    const Vector<int> indices = all_indices(points_num);
    /* From "no point is close enough" to "everything is one cluster". */
    for (const float range : {0.001f, 0.02f, 0.05f, 0.2f, 5.0f}) {
      const Array<int> expected = bli_calc_duplicates(positions, indices, range, points_num);

      const KDTreeNanoflann<float3> tree(positions);
      Array<int> duplicates(points_num, -1);
      const int found = tree.calc_duplicates(range, duplicates);

      EXPECT_EQ(Span<int>(duplicates), Span<int>(expected))
          << points_num << " points, range " << range;
      int expected_found = 0;
      for (const int i : IndexRange(points_num)) {
        if (expected[i] != -1 && expected[i] != i) {
          expected_found++;
        }
      }
      EXPECT_EQ(found, expected_found);
    }
  }
}

TEST(kdtree_nanoflann, CalcDuplicatesWithDuplicatePoints)
{
  /* Exactly equal coordinates are the case this is used for in the first place. */
  Array<float3> positions(200);
  for (const int i : positions.index_range()) {
    positions[i] = float3(float(i % 10), 0.0f, 0.0f);
  }
  const Vector<int> indices = all_indices(positions.size());
  const Array<int> expected = bli_calc_duplicates(positions, indices, 0.1f, positions.size());

  const KDTreeNanoflann<float3> tree(positions);
  Array<int> duplicates(positions.size(), -1);
  tree.calc_duplicates(0.1f, duplicates);
  EXPECT_EQ(Span<int>(duplicates), Span<int>(expected));

  /* The first 10 points are the targets, everything else merges into them. */
  for (const int i : IndexRange(10)) {
    EXPECT_EQ(duplicates[i], i);
  }
  for (const int i : IndexRange(10, 190)) {
    EXPECT_EQ(duplicates[i], i % 10);
  }
}

TEST(kdtree_nanoflann, CalcDuplicatesMasked)
{
  const Array<float3> positions = random_points(500, 6);
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_predicate(
      positions.index_range(), memory, [](const int64_t i) { return i % 4 != 0; });
  const Vector<int> indices = mask.to_indices<int>();

  const Array<int> expected = bli_calc_duplicates(positions, indices, 0.1f, positions.size());

  const KDTreeNanoflann<float3> tree(positions, mask);
  Array<int> duplicates(positions.size(), -1);
  tree.calc_duplicates(0.1f, duplicates);
  EXPECT_EQ(Span<int>(duplicates), Span<int>(expected));

  /* Points outside the mask are never touched. */
  for (const int i : positions.index_range()) {
    if (i % 4 == 0) {
      EXPECT_EQ(duplicates[i], -1);
    }
  }

  /* Building from an index array must give the same result as building from the mask. */
  const KDTreeNanoflann<float3> tree_from_indices(positions, indices.as_span());
  Array<int> from_indices(positions.size(), -1);
  tree_from_indices.calc_duplicates(0.1f, from_indices);
  EXPECT_EQ(Span<int>(from_indices), Span<int>(expected));
}

TEST(kdtree_nanoflann, CalcDuplicatesPresetTargets)
{
  const Array<float3> positions = random_points(300, 7);
  const Vector<int> indices = all_indices(positions.size());

  /* Points preset to their own index are kept out of any cluster but can still be targets. */
  Array<int> duplicates(positions.size(), -1);
  for (const int i : IndexRange(0, 20)) {
    duplicates[i] = i;
  }
  Array<int> expected(positions.size(), -1);
  for (const int i : IndexRange(0, 20)) {
    expected[i] = i;
  }
  {
    KDTree<float3> *tree = kdtree_new<float3>(indices.size());
    for (const int i : indices) {
      kdtree_insert<float3>(tree, i, positions[i]);
    }
    kdtree_balance<float3>(tree);
    kdtree_calc_duplicates_fast<float3>(tree, 0.15f, true, expected.data());
    kdtree_free<float3>(tree);
  }

  const KDTreeNanoflann<float3> tree(positions);
  tree.calc_duplicates(0.15f, duplicates);
  EXPECT_EQ(Span<int>(duplicates), Span<int>(expected));
  for (const int i : IndexRange(0, 20)) {
    EXPECT_EQ(duplicates[i], i);
  }
}

/**
 * Trees past a certain size are built with several threads, which has to give exactly the same
 * tree as building it directly. Compares against a brute force search over the same points.
 */
TEST(kdtree_nanoflann, ThreadedBuild)
{
  /* Larger than the point cound threading threshold. */
  const Array<float3> positions = random_points(50000, 8);
  const KDTreeNanoflann<float3> tree(positions);
  EXPECT_EQ(tree.size(), 50000);

  const Array<float3> queries = random_points(50, 9);
  for (const float3 &query : queries) {
    int expected_index = -1;
    float expected_distance_sq = FLT_MAX;
    for (const int i : positions.index_range()) {
      const float distance_sq = math::distance_squared(positions[i], query);
      if (distance_sq < expected_distance_sq) {
        expected_distance_sq = distance_sq;
        expected_index = i;
      }
    }

    float distance_sq = -1.0f;
    EXPECT_EQ(tree.find_nearest(query, &distance_sq), expected_index);
    EXPECT_FLOAT_EQ(distance_sq, expected_distance_sq);
  }

  /* The merging results are order dependent, so they would show any difference in the tree. */
  Array<int> duplicates(positions.size(), -1);
  const int found = tree.calc_duplicates(0.02f, duplicates);
  EXPECT_GT(found, 0);

  const Vector<int> indices = all_indices(positions.size());
  const Array<int> expected = bli_calc_duplicates(positions, indices, 0.02f, positions.size());
  EXPECT_EQ(Span<int>(duplicates), Span<int>(expected));
}

TEST(kdtree_nanoflann, Float2)
{
  const Array<float2> positions = {
      float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 5.0f), float2(4.0f, 4.0f)};
  const KDTreeNanoflann<float2> tree(positions);
  EXPECT_EQ(tree.find_nearest(float2(0.9f, 0.1f)), 1);
  EXPECT_EQ(tree.count_in_radius(float2(0.0f, 0.0f), 1.5f), 2);
}

}  // namespace blender::tests
