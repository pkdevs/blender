/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_kdtree.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::tests {

template<typename T> static void insert_all(KDTree<T> &tree, Span<T> values)
{
  BLI_assert(tree.nodes_len == 0);
  BLI_assert(tree.nodes_len_capacity == values.size());
  for (const int i : values.index_range()) {
    kdtree_insert(&tree, i, values[i]);
  }
}

template<typename T>
static void test_balance_order(const Span<T> coords, const Span<std::pair<int, T>> expected)
{
  BLI_assert(coords.size() == expected.size());

  KDTree<T> &tree = *kdtree_new<T>(coords.size());
  BLI_SCOPED_DEFER([&]() { kdtree_free(&tree); });

  insert_all(tree, coords);
  kdtree_balance(&tree);

  Array<std::pair<int, T>> balanced(coords.size());
  for (const int i : coords.index_range()) {
    balanced[i] = std::make_pair(tree.nodes[i].index, tree.nodes[i].co);
  }

  EXPECT_EQ_SPAN<std::pair<int, T>>(expected, balanced);
}

static const std::array<float, 10> seqential_1d_coords(
    {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f});

TEST(kdtree, Balance1D)
{
  test_balance_order<float>(seqential_1d_coords,
                            {{0, 0.0f},
                             {1, 1.0f},
                             {2, 2.0f},
                             {3, 3.0f},
                             {4, 4.0f},
                             {5, 5.0f},
                             {6, 6.0f},
                             {7, 7.0f},
                             {8, 8.0f},
                             {9, 9.0f}});
}

TEST(kdtree, Balance2D)
{
  test_balance_order<float2>({{0.0f, 9.0f},
                              {1.0f, 8.0f},
                              {2.0f, 7.0f},
                              {3.0f, 6.0f},
                              {4.0f, 5.0f},
                              {5.0f, 4.0f},
                              {6.0f, 3.0f},
                              {7.0f, 2.0f},
                              {8.0f, 1.0f},
                              {9.0f, 0.0f}},
                             {{3, {3.0f, 6.0f}},
                              {4, {4.0f, 5.0f}},
                              {2, {2.0f, 7.0f}},
                              {0, {0.0f, 9.0f}},
                              {1, {1.0f, 8.0f}},
                              {5, {5.0f, 4.0f}},
                              {8, {8.0f, 1.0f}},
                              {9, {9.0f, 0.0f}},
                              {7, {7.0f, 2.0f}},
                              {6, {6.0f, 3.0f}}});
}

TEST(kdtree, BalanceStable1D)
{
  test_balance_order<float>({2.0f, 1.0f, 2.0f, 3.0f, 4.0f, 9.0f, 9.0f, 0.0f, 8.0f, 9.0f},
                            {{7, 0.0f},
                             {1, 1.0f},
                             {2, 2.0f},
                             {0, 2.0f},
                             {3, 3.0f},
                             {4, 4.0f},
                             {8, 8.0f},
                             {5, 9.0f},
                             {6, 9.0f},
                             {9, 9.0f}});
}

TEST(kdtree, BalanceStable2D)
{
  test_balance_order<float2>({{1.0f, 9.0f},
                              {1.0f, 1.0f},
                              {1.0f, 7.0f},
                              {1.0f, 6.0f},
                              {1.0f, 5.0f},
                              {1.0f, 4.0f},
                              {0.0f, 1.0f},
                              {1.0f, 1.0f},
                              {0.0f, 1.0f},
                              {0.0f, 1.0f}},
                             {{9, {0.0f, 1.0f}},
                              {7, {1.0f, 1.0f}},
                              {1, {1.0f, 1.0f}},
                              {6, {0.0f, 1.0f}},
                              {8, {0.0f, 1.0f}},
                              {0, {1.0f, 9.0f}},
                              {4, {1.0f, 5.0f}},
                              {5, {1.0f, 4.0f}},
                              {3, {1.0f, 6.0f}},
                              {2, {1.0f, 7.0f}}});
}

template<typename T>
static void test_foreach_node(const Span<T> coords,
                              const std::optional<int> skip_index,
                              const Span<int> expected)
{
  KDTree<T> &tree = *kdtree_new<T>(coords.size());
  BLI_SCOPED_DEFER([&]() { kdtree_free(&tree); });

  insert_all(tree, coords);
  kdtree_balance(&tree);

  Vector<int> visited;
  kdtree_foreach_node(
      tree,
      [&](const KDTreeNode<T> &node) -> bool {
        visited.append(node.index);
        return true;
      },
      [&](const KDTreeNode<T> & /*node_parent*/, const KDTreeNode<T> &node) -> bool {
        if (node.index == skip_index) {
          return false;
        }
        return true;
      },
      [&](const KDTreeNode<T> & /*node*/) -> bool { return true; });

  EXPECT_EQ_SPAN<int>(expected, visited);
}

TEST(kdtree, ForeachNode)
{
  test_foreach_node<float>(seqential_1d_coords, std::nullopt, {5, 2, 1, 0, 4, 3, 8, 7, 6, 9});
}

TEST(kdtree, ForeachSomeNode1)
{
  test_foreach_node<float>(seqential_1d_coords, 2, {5, 8, 7, 6, 9});
}

TEST(kdtree, ForeachSomeNode2)
{
  test_foreach_node<float>(seqential_1d_coords, 8, {5, 2, 1, 0, 4, 3});
}

TEST(kdtree, ForeachSomeNode3)
{
  test_foreach_node<float>(seqential_1d_coords, 6, {5, 2, 1, 0, 4, 3, 8, 7, 9});
}

TEST(kdtree, ForeachSomeNode4)
{
  test_foreach_node<float>(seqential_1d_coords, 1, {5, 2, 4, 3, 8, 7, 6, 9});
}

template<typename T>
static void test_foreach_around(const Span<T> coords, const T &center, const Span<T> expected)
{
  KDTree<T> &tree = *kdtree_new<T>(coords.size());
  BLI_SCOPED_DEFER([&]() { kdtree_free(&tree); });

  insert_all(tree, coords);
  kdtree_balance(&tree);

  Vector<float2> visited;
  kdtree_foreach_node_around(
      tree, center, [&](const KDTreeNode<float2> &node, const float old_dist) {
        const float dist_sq = math::distance_squared(node.co, center);
        if (dist_sq > old_dist) {
          return old_dist;
        }
        visited.append(node.co);
        return dist_sq;
      });

  EXPECT_EQ_SPAN<T>(expected, visited);
}

static const std::array<float2, 16> grid_2d_4x4({{-1.0f, -1.0f},
                                                 {-0.5f, -1.0f},
                                                 {-1.0f, -0.5f},
                                                 {-0.5f, -0.5f},
                                                 {1.0f, -1.0f},
                                                 {0.5f, -1.0f},
                                                 {1.0f, -0.5f},
                                                 {0.5f, -0.5f},
                                                 {1.0f, 1.0f},
                                                 {0.5f, 1.0f},
                                                 {1.0f, 0.5f},
                                                 {0.5f, 0.5f},
                                                 {-1.0f, 1.0f},
                                                 {-0.5f, 1.0f},
                                                 {-1.0f, 0.5f},
                                                 {-0.5f, 0.5f}});

TEST(kdtree, ForeachNearest1)
{

  test_foreach_around<float2>(
      grid_2d_4x4, float2(0.0f), {{0.5f, 0.5f}, {-0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}});
}

TEST(kdtree, ForeachNearest2)
{

  test_foreach_around<float2>(
      grid_2d_4x4,
      float2(-0.75, 0.75),
      {{0.5, 0.5f}, {-0.5f, 0.5f}, {-1.0f, 0.5f}, {-0.5f, 1.0f}, {-1.0f, 1.0f}});
}

TEST(kdtree, ForeachNearest3)
{
  test_foreach_around<float2>(
      grid_2d_4x4, float2(0.0, 2.0), {{0.5, 0.5f}, {-0.5f, 0.5f}, {-0.5f, 1.0f}});
}

TEST(kdtree, ForeachRange) {}

}  // namespace blender::tests
