/* SPDX-FileCopyrightText: 2008-2009 Marius Muja, David G. Lowe.
 * SPDX-FileCopyrightText: 2011-2026 Jose Luis Blanco-Claraco.
 * SPDX-FileCopyrightText: 2026 Blender Authors.
 *
 * SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_index_mask_fwd.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_utils.hh"
#include "BLI_span.hh"
#include "BLI_utility_mixins.hh"
#include "BLI_vector.hh"

namespace blender {

/**
 * A KD-tree for nearest neighbor search. References a subset of values in a #Span. Results are
 * indices into that span, so data attached to the points can be referenced sparsely with this
 * level of indirection.
 *
 * The tree itself lives in the implementation file, which keeps it out of every file that uses
 * one, so the class is explicitly instantiated for the coordinate types listed at the bottom of
 * this file.
 *
 * Differences from #blender::KDTree in `BLI_kdtree.hh`:
 * - The tree references the caller's coordinate array instead of storing a copy of it. The array
 *   must stay alive and unchanged for as long as the tree is used.
 * - Points are added when the tree is built, there is no separate insert and balance step.
 * - Distances are squared, since that's what the searches compute anyway.
 * - Results don't include coordinates which are instead found in #points with the returned index.
 * - Performance and memory usage are much better.
 */
template<typename T> class KDTreeNanoflann : NonMovable, NonCopyable {
 public:
  using ValueT = typename T::base_type;

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

  /**
   * Build a tree containing every point in \a points.
   * \note \a points must outlive the tree.
   */
  explicit KDTreeNanoflann(Span<T> points, int leaf_size = LEAF_SIZE_DEFAULT);

  /**
   * Build a tree containing the points selected by \a mask. Results are still indices into
   * \a points rather than positions in the mask.
   * \note \a points must outlive the tree, \a mask does not have to.
   */
  KDTreeNanoflann(Span<T> points, const IndexMask &mask, int leaf_size = LEAF_SIZE_DEFAULT);

  /**
   * Build a tree containing the points at \a indices. Results are indices into \a points, not
   * positions in \a indices.
   * \note Both \a points and \a indices must outlive the tree.
   */
  KDTreeNanoflann(Span<T> points, Span<int> indices, int leaf_size = LEAF_SIZE_DEFAULT);

  ~KDTreeNanoflann();

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
  int find_nearest(const T &position, ValueT *r_distance_sq = nullptr) const;

  /**
   * Find the point closest to \a position for which \a filter returns true. Useful to skip the
   * search position itself when finding the neighbors of points that are in the tree.
   *
   * \note \a filter is called for candidate points in an unspecified order, and is called through
   * a function pointer, so this is slower than #find_nearest.
   *
   * \return The index of the closest accepted point, or -1 if there is none.
   */
  int find_nearest_filtered(const T &position,
                            FunctionRef<bool(int index)> filter,
                            ValueT *r_distance_sq = nullptr) const;

  /**
   * Find the closest points to \a position, in order of increasing distance.
   * \param r_nearest: Filled with at most `r_nearest.size()` results.
   * \return The number of results written, which is smaller when the tree has fewer points.
   */
  int find_nearest_n(const T &position, MutableSpan<Nearest> r_nearest) const;

  /**
   * Call \a fn for every point within \a radius of \a position, in an unspecified order.
   * \note \a fn is called through a function pointer, prefer #find_in_radius when the results are
   * collected anyway.
   */
  void foreach_in_radius(const T &position,
                         ValueT radius,
                         FunctionRef<void(int index, ValueT distance_sq)> fn) const;

  /**
   * Find every point within \a radius of \a position.
   * \param r_nearest: Cleared, then filled with the results in order of increasing distance.
   * Passing in the same vector for many searches avoids reallocating it every time.
   * \return The number of results.
   */
  int find_in_radius(const T &position, ValueT radius, Vector<Nearest> &r_nearest) const;

  /** The number of points within \a radius of \a position. */
  int count_in_radius(const T &position, ValueT radius) const;

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
  int calc_duplicates(ValueT range, MutableSpan<int> duplicates) const;

 private:
  /**
   * Storage for the tree, whose type is only known to the implementation file, which checks that
   * it fits. The size is larger than the tree needs so that it doesn't have to change for every
   * compiler and build configuration.
   */
  static constexpr size_t IMPL_SIZE = 320;
  static constexpr size_t IMPL_ALIGNMENT = 16;
  AlignedBuffer<IMPL_SIZE, IMPL_ALIGNMENT> impl_;

  Span<T> points_;
  int size_ = 0;
};

extern template class KDTreeNanoflann<float2>;
extern template class KDTreeNanoflann<float3>;

}  // namespace blender
