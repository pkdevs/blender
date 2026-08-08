/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 * \brief A KD-tree for nearest neighbor search.
 */

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_kdtree_types.hh"
#include "BLI_math_base_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_stack.hh"
#include "BLI_vector.hh"

#include "PRF_profile.hh"

#include <algorithm>
#include <optional>

namespace blender {

namespace detail {

constexpr int kd_stack_init = 100;     /* initial size for array (on the stack) */
constexpr int kd_near_alloc_inc = 100; /* alloc increment for collecting nearest */
constexpr int kd_found_alloc_inc = 50; /* alloc increment for collecting nearest */

constexpr uint kd_node_unset = (uint(-1));

/**
 * When set we know all values are unbalanced,
 * otherwise clear them when re-balancing: see #62210.
 */
constexpr uint kd_node_root_is_init = (uint(-2));

template<typename CoordT>
inline typename KDTreeCoordTraits<CoordT>::ValueType axis_get(const CoordT &co, uint axis)
{
  return KDTreeCoordTraits<CoordT>::get(co, axis);
}

template<typename CoordT>
inline typename KDTreeCoordTraits<CoordT>::ValueType distance_squared(const CoordT &a,
                                                                      const CoordT &b)
{
  return math::distance_squared(a, b);
}
template<> inline float distance_squared<float>(const float &a, const float &b)
{
  const float d = a - b;
  return d * d;
}

}  // namespace detail

/**
 * Creates or free a kdtree
 * \param nodes_len_capacity: The maximum length this KD-tree may hold.
 */
template<typename CoordT> inline KDTree<CoordT> *kdtree_new(uint nodes_len_capacity)
{
  KDTree<CoordT> *tree;

  tree = MEM_new_zeroed<KDTree<CoordT>>("KDTree");
  tree->nodes = MEM_new_array_uninitialized<KDTreeNode<CoordT>>(nodes_len_capacity,
                                                                "KDTreeNode<>");
  tree->nodes_len = 0;
  tree->root = detail::kd_node_root_is_init;
  tree->max_node_index = -1;

#ifndef NDEBUG
  tree->is_balanced = false;
  tree->nodes_len_capacity = nodes_len_capacity;
#endif

  return tree;
}

template<typename CoordT> inline void kdtree_free(KDTree<CoordT> *tree)
{
  if (tree) {
    MEM_delete(tree->nodes);
    MEM_delete(tree);
  }
}

/**
 * Construction: first insert points, then call balance. Normal is optional.
 */
template<typename CoordT>
inline void kdtree_insert(KDTree<CoordT> *tree, int index, const CoordT &co)
{
  KDTreeNode<CoordT> *node = &tree->nodes[tree->nodes_len++];

#ifndef NDEBUG
  BLI_assert(tree->nodes_len <= tree->nodes_len_capacity);
#endif

  /* NOTE: array isn't calloc'd,
   * need to initialize all struct members */

  node->left = node->right = detail::kd_node_unset;
  node->co = co;
  node->index = index;
  node->d = 0;
  tree->max_node_index = std::max(tree->max_node_index, index);

#ifndef NDEBUG
  tree->is_balanced = false;
#endif
}

namespace detail {

template<typename CoordT>
static uint kdtree_balance(KDTreeNode<CoordT> *nodes, uint nodes_len, uint axis, const uint ofs)
{
  KDTreeNode<CoordT> *node;
  typename KDTree<CoordT>::ValueType co;
  uint left, right, median, i, j;

  if (nodes_len <= 0) {
    return detail::kd_node_unset;
  }
  if (nodes_len == 1) {
    return 0 + ofs;
  }

  /* Quick-sort style sorting around median. */
  left = 0;
  right = nodes_len - 1;
  median = nodes_len / 2;

  while (right > left) {
    co = axis_get(nodes[right].co, axis);
    i = left - 1;
    j = right;

    while (true) {
      while (axis_get(nodes[++i].co, axis) < co) { /* pass */
      }
      while (axis_get(nodes[--j].co, axis) > co && j > left) { /* pass */
      }

      if (i >= j) {
        break;
      }

      SWAP(KDTreeNode_head<CoordT>,
           *(KDTreeNode_head<CoordT> *)&nodes[i],
           *(KDTreeNode_head<CoordT> *)&nodes[j]);
    }

    SWAP(KDTreeNode_head<CoordT>,
         *(KDTreeNode_head<CoordT> *)&nodes[i],
         *(KDTreeNode_head<CoordT> *)&nodes[right]);
    if (i >= median) {
      right = i - 1;
    }
    if (i <= median) {
      left = i + 1;
    }
  }

  /* Set node and sort sub-nodes. */
  node = &nodes[median];
  node->d = axis;
  axis = (axis + 1) % KDTree<CoordT>::DimsNum;
  node->left = kdtree_balance(nodes, median, axis, ofs);
  node->right = kdtree_balance(
      nodes + median + 1, (nodes_len - (median + 1)), axis, (median + 1) + ofs);

  return median + ofs;
}

}  // namespace detail

template<typename CoordT> inline void kdtree_balance(KDTree<CoordT> *tree)
{
  PRF_scope(ProfileCategory::Default);
  if (tree->root != detail::kd_node_root_is_init) {
    for (uint i = 0; i < tree->nodes_len; i++) {
      tree->nodes[i].left = detail::kd_node_unset;
      tree->nodes[i].right = detail::kd_node_unset;
    }
  }

  tree->root = detail::kdtree_balance<CoordT>(tree->nodes, tree->nodes_len, 0, 0);

#ifndef NDEBUG
  tree->is_balanced = true;
#endif
}

/**
 * Main way to go over nodes of the tree.
 * \param func: main loop body with ability to terminate early.
 * \param visite_child: predicate if node child should be visited or not.
 * \param left_first: optionaly control order of visiting children.
 */
template<typename CoordT, typename Func, typename ChildFunc, typename OrderFunc>
inline void kdtree_foreach_node(const KDTree<CoordT> &tree,
                                Func &&func,
                                ChildFunc &&visite_child,
                                OrderFunc &&left_is_first)
{
  const Span<KDTreeNode<CoordT>> nodes(tree.nodes, tree.nodes_len);

#ifndef NDEBUG
  BLI_assert(tree.is_balanced == true);
#endif

  if (tree.root == detail::kd_node_unset) [[unlikely]] {
    return;
  }

  Stack<uint, detail::kd_stack_init> stack;
  stack.push(tree.root);

  while (!stack.is_empty()) {
    const KDTreeNode<CoordT> &node = nodes[stack.pop()];
    if (func(node) == false) {
      break;
    }

    if (!ELEM(detail::kd_node_unset, node.left, node.right) && left_is_first(node)) {
      if (node.right != detail::kd_node_unset && visite_child(node, nodes[node.right])) {
        stack.push(node.right);
      }
      if (node.left != detail::kd_node_unset && visite_child(node, nodes[node.left])) {
        stack.push(node.left);
      }
      continue;
    }

    if (node.left != detail::kd_node_unset && visite_child(node, nodes[node.left])) {
      stack.push(node.left);
    }
    if (node.right != detail::kd_node_unset && visite_child(node, nodes[node.right])) {
      stack.push(node.right);
    }
  }
}

template<typename CoordT, typename Func, typename ChildFunc>
inline void kdtree_foreach_node(const KDTree<CoordT> &tree, Func &&func, ChildFunc &&visite_child)
{
  kdtree_foreach_node(
      tree, func, visite_child, [](const KDTreeNode<CoordT> & /*node*/) { return false; });
}

template<typename CoordT>
inline bool kdtree_is_left_child(const KDTree<CoordT> &tree,
                                 const KDTreeNode<CoordT> &parent_node,
                                 const KDTreeNode<CoordT> &node)
{
  const int node_index = std::distance<const KDTreeNode<CoordT> *>(tree.nodes, &node);
  BLI_assert(ELEM(node_index, parent_node.left, parent_node.right));
  return parent_node.left == node_index;
}

/**
 * Same as #kdtree_foreach_node but only check nodes more nearest to some point than result of
 * #func.
 */
template<typename CoordT, typename Func>
inline void kdtree_foreach_node_around(const KDTree<CoordT> &tree, const CoordT &co, Func &&func)
{
  using ValueType = KDTree<CoordT>::ValueType;
  ValueType min_sq_dist = std::numeric_limits<ValueType>::max();

  kdtree_foreach_node(
      tree,
      [&](const KDTreeNode<CoordT> &node) {
        const std::optional<ValueType> new_value = func(node, min_sq_dist);
        min_sq_dist = new_value.value_or(0.0f);
        return new_value.has_value();
      },
      [&](const KDTreeNode<CoordT> &node, const KDTreeNode<CoordT> &child) {
        const ValueType max_dist = detail::axis_get(co, node.d) -
                                   detail::axis_get(node.co, node.d);
        const bool check_both_space_half = math::square(max_dist) <= min_sq_dist;
        if (check_both_space_half) {
          return true;
        }
        BLI_assert(!math::is_zero(max_dist));

        const bool coord_sign = math::sign(max_dist);
        const bool child_sign = kdtree_is_left_child(tree, node, child);
        const bool is_same_space_half = coord_sign == child_sign;
        return is_same_space_half;
      },
      [&](const KDTreeNode<CoordT> &node) {
        return detail::axis_get(co, node.d) < detail::axis_get(node.co, node.d);
      });
}

/**
 * A version of #kdtree_find_nearest which runs a callback
 * to filter out values.
 *
 * \param filter_cb: Filter find results,
 * Return codes: (1: accept, 0: skip, -1: immediate exit).
 */
template<typename CoordT, typename Filter>
inline int kdtree_find_nearest_cb(const KDTree<CoordT> *tree,
                                  const CoordT &co,
                                  KDTreeNearest<CoordT> *r_nearest,
                                  Filter &&filter_cb)
{
  using ValueType = KDTree<CoordT>::ValueType;
  int min_node_index = -1;

  kdtree_foreach_node_around(
      *tree,
      co,
      [&](const KDTreeNode<CoordT> &node, const ValueType &old_dist) -> std::optional<ValueType> {
        const ValueType dist_sq = detail::distance_squared(node.co, co);
        if (old_dist <= dist_sq) {
          return old_dist;
        }

        switch (filter_cb(node.index, node.co, dist_sq)) {
          case 0:
            return old_dist;
          case 1: {
            min_node_index = std::distance<const KDTreeNode<CoordT> *>(tree->nodes, &node);
            return dist_sq;
          }
          case -1:
            return std::nullopt;
        }

        BLI_assert_unreachable();
        return {};
      });

  if (min_node_index == -1) {
    return -1;
  }

  if (r_nearest) {
    r_nearest->index = tree->nodes[min_node_index].index;
    r_nearest->dist = math::sqrt(detail::distance_squared(tree->nodes[min_node_index].co, co));
    r_nearest->co = tree->nodes[min_node_index].co;
  }

  return tree->nodes[min_node_index].index;
}

/**
 * Find nearest returns index, and -1 if no node is found.
 */
template<typename CoordT>
inline int kdtree_find_nearest(const KDTree<CoordT> *tree,
                               const CoordT &co,
                               KDTreeNearest<CoordT> *r_nearest)
{
  return kdtree_find_nearest_cb<CoordT>(
      tree,
      co,
      r_nearest,
      [](const uint /*index*/, const CoordT & /*coord*/, const auto /*dist*/) { return 1; });
}

namespace detail {

template<typename CoordT>
static void nearest_ordered_insert(KDTreeNearest<CoordT> *nearest,
                                   uint *nearest_len,
                                   const uint nearest_len_capacity,
                                   const int index,
                                   const typename KDTree<CoordT>::ValueType dist,
                                   const CoordT &co)
{
  uint i;

  if (*nearest_len < nearest_len_capacity) {
    (*nearest_len)++;
  }

  for (i = *nearest_len - 1; i > 0; i--) {
    if (dist >= nearest[i - 1].dist) {
      break;
    }
    nearest[i] = nearest[i - 1];
  }

  nearest[i].index = index;
  nearest[i].dist = dist;
  nearest[i].co = co;
}

}  // namespace detail

/**
 * Find \a nearest_len_capacity nearest returns number of points found, with results in nearest.
 *
 * \param r_nearest: An array of nearest, sized at least \a nearest_len_capacity.
 */
template<typename CoordT, typename Func>
inline int kdtree_find_nearest_n_with_len_squared_cb(const KDTree<CoordT> *tree,
                                                     const CoordT &co,
                                                     KDTreeNearest<CoordT> r_nearest[],
                                                     const uint nearest_len_capacity,
                                                     Func &&len_sq_fn)
{
  using ValueType = KDTree<CoordT>::ValueType;
  if (nearest_len_capacity == 0) [[unlikely]] {
    return 0;
  }

  uint nearest_len = 0;

  kdtree_foreach_node_around(
      *tree, co, [&](const KDTreeNode<CoordT> &node, const ValueType old_dist) {
        const ValueType dist_sq = len_sq_fn(node.co, co);
        if (old_dist < dist_sq) {
          return old_dist;
        }

        detail::nearest_ordered_insert<CoordT>(
            r_nearest, &nearest_len, nearest_len_capacity, node.index, dist_sq, node.co);
        return r_nearest[nearest_len - 1].dist;
      });

  for (int i = 0; i < nearest_len; i++) {
    r_nearest[i].dist = sqrtf(r_nearest[i].dist);
  }

  return int(nearest_len);
}

template<typename CoordT>
inline int kdtree_find_nearest_n(const KDTree<CoordT> *tree,
                                 const CoordT &co,
                                 KDTreeNearest<CoordT> r_nearest[],
                                 uint nearest_len_capacity)
{
  return kdtree_find_nearest_n_with_len_squared_cb<CoordT>(
      tree, co, r_nearest, nearest_len_capacity, [](const CoordT &a, const CoordT &b) {
        return detail::distance_squared(a, b);
      });
}

namespace detail {

template<typename CoordT> static int nearest_cmp_dist(const void *a, const void *b)
{
  const KDTreeNearest<CoordT> *kda = static_cast<const KDTreeNearest<CoordT> *>(a);
  const KDTreeNearest<CoordT> *kdb = static_cast<const KDTreeNearest<CoordT> *>(b);

  if (kda->dist < kdb->dist) {
    return -1;
  }
  if (kda->dist > kdb->dist) {
    return 1;
  }
  return 0;
}

template<typename CoordT>
static void nearest_add_in_range(KDTreeNearest<CoordT> **r_nearest,
                                 uint nearest_index,
                                 uint *nearest_len_capacity,
                                 const int index,
                                 const typename KDTree<CoordT>::ValueType dist,
                                 const CoordT &co)
{
  KDTreeNearest<CoordT> *to;

  if (nearest_index >= *nearest_len_capacity) [[unlikely]] {
    *r_nearest = static_cast<KDTreeNearest<CoordT> *>(MEM_realloc_uninitialized_id(
        *r_nearest,
        (*nearest_len_capacity += detail::kd_found_alloc_inc) * sizeof(KDTreeNode<CoordT>),
        __func__));
  }

  to = (*r_nearest) + nearest_index;

  to->index = index;
  to->dist = sqrtf(dist);
  to->co = co;
}

}  // namespace detail

template<typename CoordT, typename RangeT, typename Func>
inline void kdtree_foreach_node_in_range(const KDTree<CoordT> &tree,
                                         const CoordT &co,
                                         const RangeT &range,
                                         Func &&func)
{
  using ValueType = KDTree<CoordT>::ValueType;
  kdtree_foreach_node(
      tree, func, [&](const KDTreeNode<CoordT> &node, const KDTreeNode<CoordT> &child) {
        const ValueType max_dist = detail::axis_get(node.co, node.d) -
                                   detail::axis_get(co, node.d);
        if (math::abs(max_dist) <= range) {
          return true;
        }

        const bool coord_sign = math::sign(max_dist);
        const bool child_sign = kdtree_is_left_child(tree, node, child);
        const bool is_same_space_half = coord_sign == child_sign;
        return is_same_space_half;
      });
}

/**
 * Range search returns number of points nearest_len, with results in nearest
 *
 * \param r_nearest: Allocated array of nearest nearest_len (caller is responsible for freeing).
 */
template<typename CoordT, typename Func>
inline int kdtree_range_search_with_len_squared_cb(const KDTree<CoordT> *tree,
                                                   const CoordT &co,
                                                   KDTreeNearest<CoordT> **r_nearest,
                                                   const typename KDTree<CoordT>::ValueType range,
                                                   Func &&len_sq_fn)
{
  using ValueType = KDTree<CoordT>::ValueType;
  KDTreeNearest<CoordT> *nearest = nullptr;
  uint nearest_len = 0;
  uint nearest_len_capacity = 0;

  kdtree_foreach_node_in_range(*tree, co, range, [&](const KDTreeNode<CoordT> &node) {
    const ValueType sq_value = len_sq_fn(node.co, co);
    if (sq_value > math::square(range)) {
      return true;
    }

    detail::nearest_add_in_range<CoordT>(
        &nearest, nearest_len++, &nearest_len_capacity, node.index, sq_value, node.co);
    return true;
  });

  if (nearest_len) {
    qsort(nearest, nearest_len, sizeof(KDTreeNearest<CoordT>), detail::nearest_cmp_dist<CoordT>);
  }

  *r_nearest = nearest;

  return int(nearest_len);
}

template<typename CoordT>
inline int kdtree_range_search(const KDTree<CoordT> *tree,
                               const CoordT &co,
                               KDTreeNearest<CoordT> **r_nearest,
                               typename KDTree<CoordT>::ValueType range)
{
  return kdtree_range_search_with_len_squared_cb<CoordT>(
      tree, co, r_nearest, range, [](const CoordT &a, const CoordT &b) {
        return detail::distance_squared(a, b);
      });
}

/**
 * A version of #kdtree_range_search which runs a callback
 * instead of allocating an array.
 *
 * \param search_cb: Called for every node found in \a range,
 * false return value performs an early exit.
 *
 * \note the order of calls isn't sorted based on distance.
 */
template<typename CoordT, typename Fn>
inline void kdtree_range_search_cb(const KDTree<CoordT> *tree,
                                   const CoordT &co,
                                   typename KDTree<CoordT>::ValueType range,
                                   Fn &&search_cb)
{
  using ValueType = KDTree<CoordT>::ValueType;
  kdtree_foreach_node_in_range(*tree, co, range, [&](const KDTreeNode<CoordT> &node) {
    const ValueType sq_value = detail::distance_squared(node.co, co);
    if (sq_value > math::square(range)) {
      return true;
    }
    return search_cb(node.index, node.co, detail::distance_squared(co, node.co));
  });
}

namespace detail {

/**
 * Use when we want to loop over nodes ordered by index.
 * Requires indices to be aligned with nodes.
 */
template<typename CoordT> static Vector<int> kdtree_order(const KDTree<CoordT> *tree)
{
  const KDTreeNode<CoordT> *nodes = tree->nodes;
  Vector<int> order(tree->max_node_index + 1, -1);
  for (uint i = 0; i < tree->nodes_len; i++) {
    order[nodes[i].index] = int(i);
  }
  return order;
}

}  // namespace detail

/**
 * Find duplicate points in \a range.
 * Favors speed over quality since it doesn't find the best target vertex for merging.
 * Nodes are looped over, duplicates are added when found.
 * Nevertheless results are predictable.
 *
 * \param range: Coordinates in this range are candidates to be merged.
 * \param use_index_order: Loop over the coordinates ordered by #KDTreeNode.index
 * At the expense of some performance, this ensures the layout of the tree doesn't influence
 * the iteration order.
 * \param duplicates: An array of int's the length of #KDTree.nodes_len
 * Values initialized to -1 are candidates to me merged.
 * Setting the index to its own position in the array prevents it from being touched,
 * although it can still be used as a target.
 * \returns The number of merges found (includes any merges already in the \a duplicates array).
 *
 * \note Merging is always a single step (target indices won't be marked for merging).
 */
template<typename CoordT>
inline int kdtree_calc_duplicates_fast(const KDTree<CoordT> *tree,
                                       const typename KDTree<CoordT>::ValueType range,
                                       const bool use_index_order,
                                       int *duplicates)
{
  using ValueType = KDTree<CoordT>::ValueType;
  PRF_scope(ProfileCategory::Default);

  const auto mark_nodes_around = [&](const KDTreeNode<CoordT> &node, const int value) {
    int found = 0;
    kdtree_foreach_node_in_range(*tree, node.co, range, [&](const KDTreeNode<CoordT> &other_node) {
      if (&other_node == &node) {
        return true;
      }

      if (duplicates[other_node.index] == -1) {
        return true;
      }

      const ValueType sq_value = detail::distance_squared(other_node.co, node.co);
      if (sq_value > math::square(range)) {
        return true;
      }

      duplicates[other_node.index] = value;
      found++;
      return true;
    });
    return found;
  };

  int found = 0;
  const auto deduplicate_nodes = [&](const KDTreeNode<CoordT> &node) {
    const int index = node.index;
    if (!ELEM(duplicates[index], -1, index)) {
      return;
    }

    const int found_nodes_num = mark_nodes_around(node, index);
    found += found_nodes_num;
    if (found_nodes_num == 0) {
      return;
    }

    /* Prevent chains of doubles. */
    duplicates[index] = index;
  };

  if (use_index_order) {
    Vector<int> order = detail::kdtree_order<CoordT>(tree);
    for (int i = 0; i < tree->max_node_index + 1; i++) {
      const int node_index = order[i];
      if (node_index != -1) {
        deduplicate_nodes(tree->nodes[node_index]);
      }
    }
  }
  else {
    for (uint i = 0; i < tree->nodes_len; i++) {
      deduplicate_nodes(tree->nodes[i]);
    }
  }
  return found;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name kdtree_calc_duplicates_cb
 * \{ */

/**
 * De-duplicate utility where the callback can evaluate duplicates and select the target
 * which other indices are merged into.
 *
 * \param tree: A tree, all indices *must* be unique.
 * \param has_self_index: When true, account for indices
 * in the `duplicates` array that reference themselves,
 * prioritizing them as targets before de-duplicating the remainder with each other.
 * \param duplicates_cb: A function which receives duplicate indices,
 * it must choose the "target" index to keep which is returned.
 * The return value is an index in the `cluster` array (a value from `0..cluster_num`).
 * The last item in `cluster` is the index from which the search began.
 *
 * \note ~1.1x-1.5x slower than `calc_duplicates_fast` depending on the distribution of points.
 *
 * \note The duplicate search is performed in an order defined by the tree-nodes index,
 * the index of the input (first to last) for predictability.
 */
template<typename CoordT, typename Func>
inline int kdtree_calc_duplicates_cb(const KDTree<CoordT> *tree,
                                     const typename KDTree<CoordT>::ValueType range,
                                     int *duplicates,
                                     const bool has_self_index,
                                     Func &&duplicates_cb)
{
  BLI_assert(tree->is_balanced);
  if (tree->root == detail::kd_node_unset) [[unlikely]] {
    return 0;
  }

  /* Use `index_to_node_index` so coordinates are looked up in order first to last. */
  const uint nodes_len = tree->nodes_len;
  Array<int> index_to_node_index(tree->max_node_index + 1);
  for (uint i = 0; i < nodes_len; i++) {
    index_to_node_index[tree->nodes[i].index] = int(i);
  }

  int found = 0;

  /* First pass, handle merging into self-index (if any exist). */
  if (has_self_index) {
    Array<typename KDTree<CoordT>::ValueType> duplicates_dist_sq(tree->max_node_index + 1);
    for (uint i = 0; i < nodes_len; i++) {
      const int node_index = tree->nodes[i].index;
      if (node_index != duplicates[node_index]) {
        continue;
      }
      const CoordT &search_co = tree->nodes[index_to_node_index[node_index]].co;
      auto accumulate_neighbors_fn =
          [&duplicates, &node_index, &duplicates_dist_sq, &found](
              int neighbor_index,
              const CoordT & /*co*/,
              const typename KDTree<CoordT>::ValueType dist_sq) -> bool {
        const int target_index = duplicates[neighbor_index];
        if (target_index == -1) {
          duplicates[neighbor_index] = node_index;
          duplicates_dist_sq[neighbor_index] = dist_sq;
          found += 1;
        }
        /* Don't steal from self references. */
        else if (target_index != neighbor_index) {
          typename KDTree<CoordT>::ValueType &dist_sq_best = duplicates_dist_sq[neighbor_index];
          /* Steal the target if it's closer. */
          if ((dist_sq < dist_sq_best) ||
              /* Pick the lowest index as a tie breaker for a deterministic result. */
              ((dist_sq == dist_sq_best) && (node_index < target_index)))
          {
            dist_sq_best = dist_sq;
            duplicates[neighbor_index] = node_index;
          }
        }
        return true;
      };

      kdtree_range_search_cb<CoordT>(tree, search_co, range, accumulate_neighbors_fn);
    }
  }

  /* Second pass, de-duplicate clusters that weren't handled in the first pass. */

  /* Could be inline, declare here to avoid re-allocation. */
  Vector<int> cluster;
  for (uint i = 0; i < nodes_len; i++) {
    const int node_index = tree->nodes[i].index;
    if (duplicates[node_index] != -1) {
      continue;
    }

    BLI_assert(cluster.is_empty());
    const CoordT &search_co = tree->nodes[index_to_node_index[node_index]].co;
    auto accumulate_neighbors_fn =
        [&duplicates, &cluster](int neighbor_index,
                                const CoordT & /*co*/,
                                const typename KDTree<CoordT>::ValueType /*dist_sq*/) -> bool {
      if (duplicates[neighbor_index] == -1) {
        cluster.append(neighbor_index);
      }
      return true;
    };

    kdtree_range_search_cb<CoordT>(tree, search_co, range, accumulate_neighbors_fn);
    if (cluster.is_empty()) {
      continue;
    }
    found += int(cluster.size());
    cluster.append(node_index);

    const int cluster_index = duplicates_cb(cluster.data(), int(cluster.size()));
    BLI_assert(uint(cluster_index) < uint(cluster.size()));
    const int target_index = cluster[cluster_index];
    for (const int cluster_node_index : cluster) {
      duplicates[cluster_node_index] = target_index;
    }
    cluster.clear();
  }

  return found;
}

/** \} */

}  // namespace blender
