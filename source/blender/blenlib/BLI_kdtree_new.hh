/* SPDX-FileCopyrightText: 2008-2009 Marius Muja, David G. Lowe.
 * SPDX-FileCopyrightText: 2011-2026 Jose Luis Blanco-Claraco.
 * SPDX-FileCopyrightText: 2026 Blender Authors.
 *
 * SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <algorithm>
#include <limits>

#include "BLI_array.hh"
#include "BLI_bounds_types.hh"
#include "BLI_index_mask_fwd.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_utility_mixins.hh"
#include "BLI_vector.hh"

/**
 * The searches are the hot part of this file and benefit from a higher optimization level than
 * the rest of Blender is built with.
 */
#if defined(__GNUC__) && !defined(__clang__)
#  define KDTREE_SEARCH_OPTIMIZE [[gnu::optimize("O3")]]
#else
#  define KDTREE_SEARCH_OPTIMIZE
#endif

namespace blender {

namespace kdtree {

/**
 * A node of a #KDTreeNew, which a search walks and the build produces. Only the coordinate
 * type matters to it, so trees over points of any dimension share one.
 */
template<typename ValueT> struct Node {
  /**
   * The two children, which are allocated next to each other so that one pointer reaches both
   * and they share a cache line. Null for a leaf node.
   */
  Node *children;
  union {
    /** Leaf: the range of the tree's indices the node covers. */
    struct {
      int start;
      int end;
    } leaf;
    /**
     * Interior: the highest coordinate of the left child and the lowest of the right one, in
     * #dim. Points fall in neither child in between, which lets a search skip that gap.
     */
    struct {
      ValueT low;
      ValueT high;
    } split;
  };
  /** Dimension an interior node splits. */
  int dim;
};

/**
 * A search reports the points it visits to a result set, which keeps what it wants and tells the
 * search how far it still has to look through #max_distance_sq. Returning false from #add_point
 * ends the search.
 * \{
 */

template<typename ValueT> class NearestResult {
 private:
  int index_ = -1;
  ValueT distance_sq_ = std::numeric_limits<ValueT>::max();

 public:
  int index() const
  {
    return index_;
  }
  ValueT distance_sq() const
  {
    return distance_sq_;
  }

  bool add_point(const ValueT distance_sq, const int index)
  {
    if (distance_sq < distance_sq_) {
      distance_sq_ = distance_sq;
      index_ = index;
    }
    return true;
  }

  ValueT max_distance_sq() const
  {
    return distance_sq_;
  }
};

/** Like #NearestResult, but only points accepted by a filter shrink the search radius. */
template<typename ValueT, typename Filter> class FilteredNearestResult {
 private:
  Filter filter_;
  int index_ = -1;
  ValueT distance_sq_ = std::numeric_limits<ValueT>::max();

 public:
  explicit FilteredNearestResult(Filter filter) : filter_(filter) {}

  int index() const
  {
    return index_;
  }
  ValueT distance_sq() const
  {
    return distance_sq_;
  }

  bool add_point(const ValueT distance_sq, const int index)
  {
    if (distance_sq < distance_sq_ && filter_(index)) {
      distance_sq_ = distance_sq;
      index_ = index;
    }
    return true;
  }

  ValueT max_distance_sq() const
  {
    return distance_sq_;
  }
};

/** Keeps the closest points, in order, up to the size of the span it was given. */
template<typename NearestT, typename ValueT> class NearestNResult {
 private:
  MutableSpan<NearestT> nearest_;
  int count_ = 0;

 public:
  explicit NearestNResult(MutableSpan<NearestT> nearest) : nearest_(nearest) {}

  int count() const
  {
    return count_;
  }

  bool add_point(const ValueT distance_sq, const int index)
  {
    const int capacity = int(nearest_.size());
    int i;
    for (i = count_; i > 0; i--) {
      if (nearest_[i - 1].distance_sq <= distance_sq) {
        break;
      }
      if (i < capacity) {
        nearest_[i] = nearest_[i - 1];
      }
    }
    if (i < capacity) {
      nearest_[i] = {index, distance_sq};
    }
    if (count_ < capacity) {
      count_++;
    }
    return true;
  }

  ValueT max_distance_sq() const
  {
    if (count_ < int(nearest_.size())) {
      return std::numeric_limits<ValueT>::max();
    }
    return nearest_[count_ - 1].distance_sq;
  }
};

/**
 * Passes every point in the radius to a callback.
 * \note The radius is inclusive, matching #blender::kdtree_range_search_cb.
 */
template<typename ValueT, typename Fn> class RadiusResult {
 private:
  ValueT radius_sq_;
  Fn fn_;
  int count_ = 0;

 public:
  RadiusResult(const ValueT radius_sq, Fn fn) : radius_sq_(radius_sq), fn_(fn) {}

  int count() const
  {
    return count_;
  }

  bool add_point(const ValueT distance_sq, const int index)
  {
    if (distance_sq <= radius_sq_) {
      count_++;
      fn_(index, distance_sq);
    }
    return true;
  }

  /** The search never narrows, since every point within the radius is wanted. */
  ValueT max_distance_sq() const
  {
    return radius_sq_;
  }
};

/** \} */

}  // namespace kdtree

/**
 * A KD-tree for nearest neighbor search. References a subset of values in a #Span. Results are
 * indices into that span, so data attached to the points can be referenced sparsely with this
 * level of indirection.
 *
 * Searching is defined here so that the callbacks it takes can be inlined into it, which for a
 * filter as cheap as skipping one index is most of the cost of a search over a small tree.
 * Building is much more code, is not a template on anything the caller chooses, and drags in
 * threading and bounds headers, so it stays in the implementation file, which instantiates the
 * class for #float2 and #float3. Those are the only coordinate types a tree can be built for.
 *
 * Differences from #blender::KDTree in `BLI_kdtree.hh`:
 * - The tree references the caller's coordinate array instead of storing a copy of it. The array
 *   must stay alive and unchanged for as long as the tree is used.
 * - Points are added when the tree is built, there is no separate insert and balance step.
 * - Distances are squared, since that's what the searches compute anyway.
 * - Results don't include coordinates which are instead found in #points with the returned index.
 * - Performance and memory usage are much better.
 */
template<typename T> class KDTreeNew : NonMovable, NonCopyable {
 public:
  using ValueT = typename T::base_type;
  static constexpr int dimensions = T::type_length;

  /** A single search result. */
  struct Nearest {
    /** Index of the point in #points. */
    int index;
    /** Squared distance between the point and the search position. */
    ValueT distance_sq;
  };

  /**
   * Indices in a leaf node are searched linearly. Larger values build faster and use less memory
   * whereas smaller values make queries visit fewer points.
   *
   * 32 was measured to be the best tradeoff between build time and memory usage for float3 point
   * clouds, building 1.3-2x faster than a leaf size of 10 and halving the memory the nodes use
   * (6.5 MB vs. 12.6 MB for a million points). See `BLI_kdtree_performance_test`.
   */
  static constexpr int LEAF_SIZE_DEFAULT = 32;

 private:
  using Node = kdtree ::Node<ValueT>;

  Span<T> points_;
  /** Indices into #points_, in the order the tree visits them. */
  Array<int> indices_;
  /** Bounds of every point in the tree, which a search starts from. */
  Bounds<T> bounds_ = Bounds<T>(T(0));
  Node *root_ = nullptr;
  int leaf_size_;
  int size_ = 0;
  /** The buffers every node in the tree lives in. */
  LinearAllocator<> allocator_;

 public:
  /**
   * Build a tree containing every point in \a points.
   * \note \a points must outlive the tree.
   */
  explicit KDTreeNew(Span<T> points, int leaf_size = LEAF_SIZE_DEFAULT);

  /**
   * Build a tree containing the points selected by \a mask. Results are still indices into
   * \a points rather than positions in the mask.
   * \note \a points must outlive the tree, \a mask does not have to.
   */
  KDTreeNew(Span<T> points, const IndexMask &mask, int leaf_size = LEAF_SIZE_DEFAULT);

  /**
   * Build a tree containing the points at \a indices. Results are indices into \a points, not
   * positions in \a indices.
   * \note \a points must outlive the tree, \a indices does not have to.
   */
  KDTreeNew(Span<T> points, Span<int> indices, int leaf_size = LEAF_SIZE_DEFAULT);

  /** The coordinates the tree was built from. Returned indices are indices into this span. */
  Span<T> points() const
  {
    return points_;
  }

  /** The number of points, which may be less than `points().size()` for a masked tree. */
  int size() const
  {
    return size_;
  }

  bool is_empty() const
  {
    return size_ == 0;
  }

  /**
   * Find the point closest to \a position.
   * \return Its index, or -1 if the tree is empty.
   */
  KDTREE_SEARCH_OPTIMIZE int find_nearest(const T &position, ValueT *r_distance_sq = nullptr) const
  {
    kdtree ::NearestResult<ValueT> result;
    this->find_neighbors(result, position);
    if (result.index() != -1 && r_distance_sq) {
      *r_distance_sq = result.distance_sq();
    }
    return result.index();
  }

  /**
   * Find the point closest to \a position for which \a filter returns true. Useful to skip the
   * search position itself when finding the neighbors of points that are in the tree.
   *
   * \note \a filter is called for candidate points in an unspecified order.
   *
   * \return The index of the closest accepted point, or -1 if there is none.
   */
  template<typename Filter>
  KDTREE_SEARCH_OPTIMIZE int find_nearest_filtered(const T &position,
                                                   Filter &&filter,
                                                   ValueT *r_distance_sq = nullptr) const
  {
    kdtree ::FilteredNearestResult<ValueT, std::decay_t<Filter>> result(
        std::forward<Filter>(filter));
    this->find_neighbors(result, position);
    if (result.index() != -1 && r_distance_sq) {
      *r_distance_sq = result.distance_sq();
    }
    return result.index();
  }

  /**
   * Find the closest points to \a position, in order of increasing distance.
   * \param r_nearest: Filled with at most `r_nearest.size()` results.
   * \return The number of results written, which is smaller when the tree has fewer points.
   */
  KDTREE_SEARCH_OPTIMIZE int find_nearest_n(const T &position,
                                            MutableSpan<Nearest> r_nearest) const
  {
    if (r_nearest.is_empty()) {
      return 0;
    }
    kdtree ::NearestNResult<Nearest, ValueT> result(r_nearest);
    this->find_neighbors(result, position);
    return result.count();
  }

  /** Call \a fn for every point within \a radius of \a position, in an unspecified order. */
  template<typename Fn>
  KDTREE_SEARCH_OPTIMIZE void foreach_in_radius(const T &position,
                                                const ValueT radius,
                                                Fn &&fn) const
  {
    kdtree ::RadiusResult<ValueT, std::decay_t<Fn>> result(radius * radius, std::forward<Fn>(fn));
    this->find_neighbors(result, position);
  }

  /**
   * Find every point within \a radius of \a position.
   * \param r_nearest: Cleared, then filled with the results in order of increasing distance.
   * Passing in the same vector for many searches avoids reallocating it every time.
   * \return The number of results.
   */
  KDTREE_SEARCH_OPTIMIZE int find_in_radius(const T &position,
                                            const ValueT radius,
                                            Vector<Nearest> &r_nearest) const
  {
    r_nearest.clear();
    this->foreach_in_radius(position, radius, [&](const int index, const ValueT distance_sq) {
      r_nearest.append({index, distance_sq});
    });
    std::sort(r_nearest.begin(), r_nearest.end(), [](const Nearest &a, const Nearest &b) {
      return a.distance_sq < b.distance_sq ||
             (a.distance_sq == b.distance_sq && a.index < b.index);
    });
    return int(r_nearest.size());
  }

  /** The number of points within \a radius of \a position. */
  KDTREE_SEARCH_OPTIMIZE int count_in_radius(const T &position, const ValueT radius) const
  {
    kdtree ::RadiusResult<ValueT, decltype([](int, ValueT) {})> result(radius * radius, {});
    this->find_neighbors(result, position);
    return result.count();
  }

  /**
   * Group points that are within \a range of each other, favoring speed over quality: each point
   * is merged into the first point that claims it rather than into its closest neighbor.
   *
   * Which point of a cluster the others merge into is not defined, only the properties described
   * below: points are visited in the order the tree stores them, which is an internal detail that
   * can change, though it is deterministic.
   *
   * \param duplicates: Indexed by point index, so it must be large enough for the largest index
   * in the tree. Points with a value of -1 are candidates to be merged into another point.
   * Presetting a point to its own index keeps it from being merged into anything, though it can
   * still become the target other points merge into. When the function returns, a merged point
   * holds the index of the point it was merged into, and a point that others were merged into
   * holds its own index. Points that are neither are left at -1.
   *
   * \return The number of points that were merged into another point.
   *
   * \note Merging is a single step: a point is never merged into a point that is itself merged
   * into a third point.
   */
  KDTREE_SEARCH_OPTIMIZE int calc_duplicates(const ValueT range, MutableSpan<int> duplicates) const
  {
    int found = 0;
    /* The order the tree visits the points in rather than the order they were added. Which point
     * of a cluster the others merge into depends on the order either way, and this one keeps each
     * search near the one before it. */
    for (const int index : indices_) {
      BLI_assert(index < duplicates.size());
      /* Skip points that are already merged into another point. Points that were preset to their
       * own index are not merged themselves, but can still be merged into. */
      if (duplicates[index] != -1 && duplicates[index] != index) {
        continue;
      }
      const int found_before = found;
      this->foreach_in_radius(
          points_[index], range, [&](const int other, const ValueT /*distance_sq*/) {
            if (other != index && duplicates[other] == -1) {
              duplicates[other] = index;
              found++;
            }
          });
      if (found != found_before) {
        /* Mark as a target so the merged points don't form chains. */
        duplicates[index] = index;
      }
    }
    return found;
  }

  /**
   * Visit the points that could be closer to \a position than the result set has accepted so
   * far, reporting each of them to it. The result set decides what to keep and how far the
   * search still has to look. See #kdtree
   ::NearestResult and the others beside it.
   */
  template<typename ResultSet>
  KDTREE_SEARCH_OPTIMIZE void find_neighbors(ResultSet &result, const T &position) const
  {
    if (root_ == nullptr) {
      return;
    }
    /* Squared distance to the bounds of the tree, per dimension, which the walk below replaces
     * one dimension at a time rather than recomputing. */
    T offsets(ValueT(0));
    ValueT distance_sq = 0;
    for (const int dim : IndexRange(dimensions)) {
      if (position[dim] < bounds_.min[dim]) {
        offsets[dim] = math::square(bounds_.min[dim] - position[dim]);
      }
      else if (position[dim] > bounds_.max[dim]) {
        offsets[dim] = math::square(position[dim] - bounds_.max[dim]);
      }
      distance_sq += offsets[dim];
    }
    this->search_node(result, position, root_, distance_sq, offsets);
  }

 private:
  /**
   * Build the tree over the points at \a indices, or over every point when \a indices is empty.
   * Defined in the implementation file, see the note on the class.
   */
  void build(Span<int> indices);

  /**
   * \param distance_sq: Squared distance from \a position to the part of space \a node covers.
   * \param offsets: Squared distance to that part per dimension, which this restores before it
   * returns so that the caller can keep using it.
   * \return False when the result set has seen enough and the search should stop.
   */
  template<typename ResultSet>
  KDTREE_SEARCH_OPTIMIZE bool search_node(ResultSet &result,
                                          const T &position,
                                          const Node *node,
                                          const ValueT distance_sq,
                                          T &offsets) const
  {
    if (node->children == nullptr) {
      const int *indices = indices_.data();
      const T *points = points_.data();
      for (int i = node->leaf.start; i < node->leaf.end; i++) {
        const int index = indices[i];
        const ValueT point_distance_sq = math::distance_squared(position, points[index]);
        if (point_distance_sq < result.max_distance_sq()) {
          if (!result.add_point(point_distance_sq, index)) {
            return false;
          }
        }
      }
      return true;
    }

    /* Both children are loaded before it is known which one to visit, so that the loads do not
     * wait on the comparison below. Picking one first makes the walk down the tree a chain of
     * dependent loads. */
    const Node *low_child = &node->children[0];
    const Node *high_child = &node->children[1];

    /* Visit the side of the split the position is on first, since what it finds there is what
     * lets the other side be skipped. */
    const int dim = node->dim;
    const ValueT value = position[dim];
    const ValueT to_low = value - node->split.low;
    const ValueT to_high = value - node->split.high;

    /* Written as a branch rather than as a choice between two pointers, so that the processor
     * predicts which child comes next and starts loading it before the comparison has resolved.
     * Selecting the pointer instead makes the walk down the tree wait at every level. */
    const Node *far_child;
    ValueT far_offset;
    if ((to_low + to_high) < 0) {
      if (!this->search_node(result, position, low_child, distance_sq, offsets)) {
        return false;
      }
      far_child = high_child;
      far_offset = math::square(to_high);
    }
    else {
      if (!this->search_node(result, position, high_child, distance_sq, offsets)) {
        return false;
      }
      far_child = low_child;
      far_offset = math::square(to_low);
    }

    /* Swap in the distance to the far side for this dimension, keeping the others. */
    const ValueT previous = offsets[dim];
    const ValueT far_distance_sq = distance_sq + far_offset - previous;
    if (far_distance_sq > result.max_distance_sq()) {
      return true;
    }
    offsets[dim] = far_offset;
    const bool keep_searching = this->search_node(
        result, position, far_child, far_distance_sq, offsets);
    offsets[dim] = previous;
    return keep_searching;
  }
};

}  // namespace blender
