/* SPDX-FileCopyrightText: 2008-2009 Marius Muja, David G. Lowe.
 * SPDX-FileCopyrightText: 2011-2026 Jose Luis Blanco-Claraco.
 * SPDX-FileCopyrightText: 2026 Blender Authors.
 *
 * SPDX-License-Identifier: BSD-2-Clause */

/** \file
 * \ingroup bli
 *
 * A KD-tree for nearest neighbor search, which started from the nanoflann library and has since
 * been rewritten around Blender's own containers, threading and math. The node layout and the way
 * a search walks it are still recognizably nanoflann's, which is where the copyright above comes
 * from.
 *
 * The build reorders a copy of the coordinates rather than an array of indices. That makes every
 * pass over a node sequential, and lets subtrees become cache resident as soon as they are small
 * enough. Each point carries its index in the element after its coordinates, so reordering the
 * points reorders the indices with them, in the same cache lines rather than in a second array.
 *
 * Splitting a node is the hot part of the build, and the obvious way to write it, a conditional
 * swap per point, mispredicts on about every other point of an evenly split node. That costs more
 * than everything else put together, so the split and the bounds it needs are both written
 * without branching on the comparison. See #partition_points.
 */

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdtree_nanoflann.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_threads.hh"
#include "BLI_utildefines.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Build Tuning
 * \{ */

/** Tree size that makes building with more than one thread worthwhile. */
static constexpr int PARALLEL_BUILD_THRESHOLD = 10000;

/**
 * Building the tree repeatedly reorders the coordinates, so it is bottlenecked by memory
 * bandwidth. Past this level, extra threads won't be able to do any useful work.
 *
 * Measured on a 12 core Apple M3 Max, a million point build takes 33 ms on one thread, 8 ms on
 * eight and 7 ms on 12, using half again as much CPU time. See `BLI_kdtree_performance_test`.
 */
static constexpr int PARALLEL_BUILD_MAX_THREADS = 16;

/**
 * Experimentally derived value to increase building thread count from 4 to 8 as point count
 * increases from 10'000 to 100'000, balancing threading overhead with increased parallelism
 * opportunity. See `BLI_kdtree_performance_test`.
 */
static constexpr int PARALLEL_BUILD_POINTS_PER_THREAD = 2500;

/** Subtrees smaller than this are built in one task, which is cheaper than handing them over. */
static constexpr int PARALLEL_SUBTREE_GRAIN = 4096;

/** Points that make a block worth giving a thread of its own within a single node. */
static constexpr int NODE_BLOCK_SIZE = 8192;

/** Upper bound on those blocks, so that the bookkeeping fits on the stack. */
static constexpr int NODE_MAX_BLOCKS = 64;

/**
 * Nodes holding more than this fraction of the tree are split a block at a time, which is what a
 * threaded build spreads over its threads. Below that there are enough subtrees to keep the
 * threads busy on their own, and the blocks and the fix up they need are only extra work.
 *
 * \note A build on a single thread splits the same nodes into the same blocks, which costs it a
 * little, so that a tree does not depend on the number of threads that built it. See
 * #partition_points_in_blocks.
 */
static constexpr int NODE_BLOCK_FRACTION = 8;

/**
 * Points a partition works on at a time. An offset within a block has to fit in a #uint8_t, and
 * two blocks have to fit in a range for the block path to be used at all.
 */
static constexpr int PARTITION_BLOCK_SIZE = 128;

static int parallel_build_threads(const int64_t points_num)
{
  return int(std::clamp<int64_t>(
      points_num / PARALLEL_BUILD_POINTS_PER_THREAD, 1, PARALLEL_BUILD_MAX_THREADS));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tree
 * \{ */

/**
 * Nodes over the points of a #Span, referenced by index so that data attached to the points can
 * be looked up sparsely. The points of a node are a contiguous range of #indices_.
 */
template<typename T> class KDTree {
 public:
  using ValueT = typename T::base_type;
  static constexpr int dimensions = T::type_length;

 private:
  /**
   * A point as the build holds it: its coordinates, and its index in #points_ in the element
   * after them. For three dimensional points that element is padding that a point would be
   * rounded up to anyway.
   */
  using BuildPoint = VecBase<ValueT, dimensions + 1>;

  struct Node {
    /**
     * The two children, which are allocated next to each other so that one pointer reaches both
     * and they share a cache line. Null for a leaf node.
     */
    Node *children;
    union {
      /** Leaf: the range of #indices_ the node covers. */
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
   * Nodes handed out from large batches, so that a walk down the tree stays within a few
   * allocations. A linear allocator packs small allocations into buffers of a few kilobytes at
   * most, which for a tree of any size means nodes scattered over many of them.
   */
  struct NodeAllocator {
    /** Nodes per batch, and so the most that can go unused per thread that built a subtree. */
    static constexpr int batch_size = 256;

    LinearAllocator<> buffers;
    MutableSpan<Node> batch;
    /* Equal to the (empty) batch size, so that the first allocation starts a batch. */
    int used = 0;

    /** A node, or the two children of one, which have to be next to each other. */
    Node *allocate(const int num)
    {
      if (used + num > batch.size()) {
        batch = buffers.allocate_array<Node>(batch_size);
        used = 0;
      }
      Node *nodes = &batch[used];
      used += num;
      return nodes;
    }
  };

  /** What the build needs beyond the tree it produces. */
  struct BuildState {
    /** The points being reordered, which #indices_ is taken from once the tree is built. */
    MutableSpan<BuildPoint> points;
    /** The pool of a build that stays on one thread. */
    NodeAllocator allocator;
    /**
     * A pool per thread that takes part in a threaded build. One pool behind a mutex would put
     * every node in the tree through it, while a pool per subtree leaves a partly used batch
     * behind for each of them and spreads the nodes of a subtree over several pools. Only built
     * when there are threads to give pools to: reaching into thread local storage costs more than
     * building a tree of a few points does.
     */
    std::optional<threading::EnumerableThreadSpecific<NodeAllocator>> thread_allocators;
  };

  Span<T> points_;
  /** Indices into #points_, in the order the tree visits them. */
  Array<int> indices_;
  /** Bounds of every point in the tree, which a search starts from. */
  Bounds<T> bounds_ = Bounds<T>(T(0));
  Node *root_ = nullptr;
  int leaf_size_;

  /**
   * The buffers every node in the tree lives in. The build fills the pools of #BuildState, which
   * hand their buffers over to this one once the nodes are where they will stay.
   */
  LinearAllocator<> allocator_;

 public:
  /**
   * Build a tree over the points at \a indices, or over every point when \a indices is empty.
   * \note \a points must outlive the tree, \a indices does not have to.
   */
  KDTree(const Span<T> points, const Span<int> indices, const int leaf_size)
      : points_(points), leaf_size_(std::max(leaf_size, 1))
  {
    const int size = indices.is_empty() ? int(points.size()) : int(indices.size());
    indices_.reinitialize(size);
    if (size == 0) {
      return;
    }

    /* A tree that is a single leaf has nothing to split, and copying the coordinates aside would
     * cost more than the rest of its build. */
    if (size <= leaf_size_) {
      this->build_single_leaf(indices);
      return;
    }

    /* Splitting the build into tasks costs something even when there is no second thread to run
     * them, so only ask for it when there is one. */
    const bool threaded = size >= PARALLEL_BUILD_THRESHOLD && BLI_system_thread_count() > 1;
    if (threaded) {
      threading::max_threads_task(parallel_build_threads(size),
                                  [&]() { this->build(indices, true); });
    }
    else {
      this->build(indices, false);
    }
  }

  /** The points of the tree, in the order it visits them. */
  Span<int> indices() const
  {
    return indices_;
  }

  /**
   * Visit the points that could be closer to \a position than the result set has accepted so
   * far, reporting each of them to it. The result set decides what to keep and how far the
   * search still has to look. See #NearestResult and the others below.
   */
  template<typename ResultSet> void find_neighbors(ResultSet &result, const T &position) const
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
  /* -------------------------------------------------------------------- */
  /** \name Build
   * \{ */

  void build_single_leaf(const Span<int> indices)
  {
    if (indices.is_empty()) {
      std::iota(indices_.begin(), indices_.end(), 0);
    }
    else {
      indices_.as_mutable_span().copy_from(indices);
    }

    bounds_ = Bounds<T>(points_[indices_.first()]);
    for (const int index : indices_) {
      math::min_max(points_[index], bounds_.min, bounds_.max);
    }

    root_ = allocator_.allocate<Node>();
    root_->children = nullptr;
    root_->leaf.start = 0;
    root_->leaf.end = int(indices_.size());
  }

  void build(const Span<int> indices, const bool threaded)
  {
    const int size = int(indices_.size());
    Array<BuildPoint> build_points(size);

    BuildState state{build_points.as_mutable_span()};
    const Bounds<BuildPoint> bounds = this->init_points(state, indices, threaded);

    if (threaded) {
      state.thread_allocators.emplace();
    }
    NodeAllocator &allocator = threaded ? state.thread_allocators->local() : state.allocator;
    root_ = allocator.allocate(1);
    this->build_node(state, root_, 0, size, bounds, allocator, threaded);

    allocator_.transfer_ownership_from(state.allocator.buffers);
    if (state.thread_allocators.has_value()) {
      for (NodeAllocator &allocator : *state.thread_allocators) {
        allocator_.transfer_ownership_from(allocator.buffers);
      }
    }

    for (const int dim : IndexRange(dimensions)) {
      bounds_.min[dim] = bounds.min[dim];
      bounds_.max[dim] = bounds.max[dim];
    }

    /* The points are in the order the tree visits them now, so the indices they carry are the
     * order to keep. The coordinates are not, since a search reads #points_. */
    this->collect_indices(state, threaded);
  }

  /** Fill \a state with the coordinates and indices of the points, and measure them. */
  Bounds<BuildPoint> init_points(BuildState &state, const Span<int> indices, const bool threaded)
  {
    const int size = int(indices_.size());
    const auto fill = [&](const int start, const int end) {
      Bounds<BuildPoint> bounds(this->build_point(indices, start));
      for (const int i : IndexRange(start, end - start)) {
        const BuildPoint point = this->build_point(indices, i);
        state.points[i] = point;
        bounds.min = math::min(bounds.min, point);
        bounds.max = math::max(bounds.max, point);
      }
      return bounds;
    };

    const int blocks = threaded ? block_num(size) : 0;
    if (blocks < 2) {
      return fill(0, size);
    }

    Array<Bounds<BuildPoint>> block_bounds(blocks);
    threading::parallel_for(IndexRange(blocks), 1, [&](const IndexRange range) {
      for (const int block : range) {
        block_bounds[block] = fill(block_begin(size, blocks, block),
                                   block_begin(size, blocks, block + 1));
      }
    });
    return merge_bounds(block_bounds);
  }

  BuildPoint build_point(const Span<int> indices, const int i) const
  {
    const int index = indices.is_empty() ? i : indices[i];
    BuildPoint point;
    for (const int dim : IndexRange(dimensions)) {
      point[dim] = points_[index][dim];
    }
    point[dimensions] = std::bit_cast<ValueT>(index);
    return point;
  }

  /** Take the index each point carries, in the order the build left them in. */
  void collect_indices(BuildState &state, const bool threaded)
  {
    const int size = int(indices_.size());
    const auto fill = [&](const int start, const int end) {
      for (const int i : IndexRange(start, end - start)) {
        indices_[i] = std::bit_cast<int>(state.points[i][dimensions]);
      }
    };

    const int blocks = threaded ? block_num(size) : 0;
    if (blocks < 2) {
      fill(0, size);
      return;
    }
    threading::parallel_for(IndexRange(blocks), 1, [&](const IndexRange range) {
      for (const int block : range) {
        fill(block_begin(size, blocks, block), block_begin(size, blocks, block + 1));
      }
    });
  }

  /**
   * Build the node covering [start, end), of whose points \a bounds must be the exact bounds.
   * Splitting around the middle of those rather than of a cell that can be much larger keeps the
   * tree shallow where the points are unevenly spread.
   */
  void build_node(BuildState &state,
                  Node *node,
                  const int start,
                  const int end,
                  const Bounds<BuildPoint> &bounds,
                  NodeAllocator &allocator,
                  const bool threaded)
  {
    if (end - start <= leaf_size_) {
      node->children = nullptr;
      node->leaf.start = start;
      node->leaf.end = end;
      return;
    }

    /* Split the widest extent of the points in half. */
    const BuildPoint extent = bounds.max - bounds.min;
    int dim = 0;
    for (const int other : IndexRange(1, dimensions - 1)) {
      if (extent[other] > extent[dim]) {
        dim = other;
      }
    }
    const ValueT split_value = bounds.min[dim] + extent[dim] / 2;

    /* Split the node a block at a time, which a threaded build spreads over its threads. Which
     * blocks the range is cut into follows from its size alone, so the points end up in the same
     * order however many threads run it, or none at all. That matters beyond reproducible trees:
     * #calc_duplicates walks the points in this order, and the blocks leave them in a different
     * one than a single pass over the whole range does. Below this size there are enough subtrees
     * to keep the threads busy on their own, and the blocks would only be extra work. */
    const bool split_blocks = int64_t(end - start) * NODE_BLOCK_FRACTION > state.points.size();

    int middle = split_blocks ?
                     this->partition_points_in_blocks(state, start, end, dim, split_value) :
                     partition_points(state.points, start, end, dim, split_value);
    if (middle == start || middle == end) {
      /* Everything ended up on one side, which means the coordinates of the points are within
       * rounding distance of each other in every dimension. Split them in the middle so that the
       * recursion still terminates. */
      middle = start + (end - start) / 2;
    }

    Bounds<BuildPoint> left_bounds, right_bounds;
    if (split_blocks) {
      left_bounds = this->compute_bounds_in_blocks(state, start, middle);
      right_bounds = this->compute_bounds_in_blocks(state, middle, end);
    }
    else {
      left_bounds = compute_bounds(state.points, start, middle);
      right_bounds = compute_bounds(state.points, middle, end);
    }

    node->dim = dim;
    node->children = allocator.allocate(2);
    if (threaded && end - start > PARALLEL_SUBTREE_GRAIN) {
      threading::parallel_invoke(
          [&]() {
            this->build_node(state,
                             &node->children[0],
                             start,
                             middle,
                             left_bounds,
                             state.thread_allocators->local(),
                             true);
          },
          [&]() {
            this->build_node(state,
                             &node->children[1],
                             middle,
                             end,
                             right_bounds,
                             state.thread_allocators->local(),
                             true);
          });
    }
    else {
      this->build_node(state, &node->children[0], start, middle, left_bounds, allocator, false);
      this->build_node(state, &node->children[1], middle, end, right_bounds, allocator, false);
    }

    node->split.low = left_bounds.max[dim];
    node->split.high = right_bounds.min[dim];
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Bounds
   * \{ */

  /** Exact bounds of the points in [start, end), in one sequential pass. */
  static Bounds<BuildPoint> compute_bounds(const Span<BuildPoint> points,
                                           const int start,
                                           const int end)
  {
    /* Several accumulators, so that the comparisons of one point do not wait on those of the
     * point before it. */
    constexpr int lanes = 4;
    const int full = start + ((end - start) & ~(lanes - 1));

    BuildPoint lo[lanes], hi[lanes];
    for (const int lane : IndexRange(lanes)) {
      lo[lane] = hi[lane] = points[start];
    }
    for (int i = start; i < full; i += lanes) {
      for (const int lane : IndexRange(lanes)) {
        const BuildPoint point = points[i + lane];
        lo[lane] = math::min(lo[lane], point);
        hi[lane] = math::max(hi[lane], point);
      }
    }

    Bounds<BuildPoint> bounds(lo[0], hi[0]);
    for (const int lane : IndexRange(1, lanes - 1)) {
      bounds.min = math::min(bounds.min, lo[lane]);
      bounds.max = math::max(bounds.max, hi[lane]);
    }
    for (int i = full; i < end; i++) {
      bounds.min = math::min(bounds.min, points[i]);
      bounds.max = math::max(bounds.max, points[i]);
    }
    return bounds;
  }

  /** #compute_bounds with the range measured a block at a time, which threads can share. */
  Bounds<BuildPoint> compute_bounds_in_blocks(BuildState &state, const int start, const int end)
  {
    const int count = end - start;
    const int blocks = block_num(count);
    if (blocks < 2) {
      return compute_bounds(state.points, start, end);
    }

    Array<Bounds<BuildPoint>> block_bounds(blocks);
    threading::parallel_for(IndexRange(blocks), 1, [&](const IndexRange range) {
      for (const int block : range) {
        block_bounds[block] = compute_bounds(state.points,
                                             start + block_begin(count, blocks, block),
                                             start + block_begin(count, blocks, block + 1));
      }
    });
    return merge_bounds(block_bounds);
  }

  static Bounds<BuildPoint> merge_bounds(const Span<Bounds<BuildPoint>> block_bounds)
  {
    Bounds<BuildPoint> bounds = block_bounds.first();
    for (const Bounds<BuildPoint> &other : block_bounds.drop_front(1)) {
      bounds.min = math::min(bounds.min, other.min);
      bounds.max = math::max(bounds.max, other.max);
    }
    return bounds;
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Partitioning
   * \{ */

  /**
   * Reorder the points in [start, end) so that those below \a split_value in \a dim come first,
   * and return where the rest starts.
   *
   * Deciding per point whether to swap it is a branch on the result of the comparison, which for
   * an evenly split node is unpredictable and so mispredicts on about every other point. This
   * collects the offsets of the misplaced points of a block without branching on the comparison,
   * and only then swaps them, which is a loop over exactly as many iterations as there are swaps.
   * Same approach as the partition in BlockQuicksort.
   */
  static int partition_points(const MutableSpan<BuildPoint> points,
                              const int start,
                              const int end,
                              const int dim,
                              const ValueT split_value)
  {
    constexpr int block = PARTITION_BLOCK_SIZE;

    uint8_t offsets_l[block], offsets_r[block];
    int num_l = 0, num_r = 0, first_l = 0, first_r = 0;
    int l = start, r = end;

    while (r - l > 2 * block) {
      /* Offsets of the points of the left block that belong on the right, and the other way
       * around. The store always happens, only the counter advances conditionally. */
      if (num_l == 0) {
        first_l = 0;
        for (const int i : IndexRange(block)) {
          offsets_l[num_l] = uint8_t(i);
          num_l += int(!(points[l + i][dim] < split_value));
        }
      }
      if (num_r == 0) {
        first_r = 0;
        for (const int i : IndexRange(block)) {
          offsets_r[num_r] = uint8_t(i);
          num_r += int(points[r - 1 - i][dim] < split_value);
        }
      }

      const int num = std::min(num_l, num_r);
      for (const int i : IndexRange(num)) {
        std::swap(points[l + offsets_l[first_l + i]], points[r - 1 - offsets_r[first_r + i]]);
      }
      num_l -= num;
      num_r -= num;
      first_l += num;
      first_r += num;

      /* A block is done once none of its points are misplaced any more. */
      if (num_l == 0) {
        l += block;
      }
      if (num_r == 0) {
        r -= block;
      }
    }

    return partition_points_small(points, l, r, dim, split_value);
  }

  /**
   * #partition_points for a range that fits in a temporary buffer, which is the case for the
   * last levels of the tree. Copying the points out and back in is cheaper than the swaps that
   * staying in place needs, and keeps those levels branchless as well.
   */
  static int partition_points_small(const MutableSpan<BuildPoint> points,
                                    const int start,
                                    const int end,
                                    const int dim,
                                    const ValueT split_value)
  {
    const int count = end - start;
    BLI_assert(count <= 2 * PARTITION_BLOCK_SIZE);

    BuildPoint buffer[2 * PARTITION_BLOCK_SIZE];
    /* Points below the split fill the buffer from the front, the others from the back. Only the
     * position depends on the comparison, so there is nothing to mispredict. */
    int l = 0, r = count;
    for (const int i : IndexRange(start, count)) {
      /* Read the coordinate from the array rather than from a copy of the point: indexing a
       * vector by a value only known at runtime means it has to be in memory, and a copy would
       * have to be spilled there every iteration. */
      const bool less = points[i][dim] < split_value;
      const BuildPoint point = points[i];
      /* Written to both ends, of which only one is kept. Picking the position instead lets a
       * compiler turn it back into a branch on the comparison, which is the whole thing this
       * avoids, and both ends are a few cache lines apart at most. */
      buffer[l] = point;
      buffer[r - 1] = point;
      l += int(less);
      r -= int(!less);
    }

    MutableSpan<BuildPoint> range = points.slice(start, count);
    for (const int i : IndexRange(count)) {
      range[i] = buffer[i];
    }
    return start + l;
  }

  /**
   * #partition_points with the range split a block at a time, which can be spread over threads.
   * Which blocks the range is cut into follows from its size alone, so this leaves the points in
   * the same order however many threads run it.
   *
   * Each block is partitioned on its own, which leaves every block split but the blocks
   * themselves interleaved: points above the split sit in blocks that belong entirely below it,
   * and the other way around. There are as many of one as of the other, so the fix up is to swap
   * them pairwise, which spreads over threads as well. This is how embree partitions primitives
   * when building a BVH.
   */
  int partition_points_in_blocks(
      BuildState &state, const int start, const int end, const int dim, const ValueT split_value)
  {
    const MutableSpan<BuildPoint> points = state.points;
    const int count = end - start;
    const int blocks = block_num(count);
    if (blocks < 2) {
      return partition_points(points, start, end, dim, split_value);
    }

    int begin[NODE_MAX_BLOCKS + 1];
    int middle[NODE_MAX_BLOCKS];
    for (const int block : IndexRange(blocks + 1)) {
      begin[block] = start + block_begin(count, blocks, block);
    }

    threading::parallel_for(IndexRange(blocks), 1, [&](const IndexRange range) {
      for (const int block : range) {
        middle[block] = partition_points(points, begin[block], begin[block + 1], dim, split_value);
      }
    });

    /* Where the split ends up once the blocks are put in order. */
    int split = start;
    for (const int block : IndexRange(blocks)) {
      split += middle[block] - begin[block];
    }

    /* The points that are on the wrong side of it. A block lies either before or after the
     * split, so it contributes to one of the two lists at most. */
    IndexRange high[NODE_MAX_BLOCKS], low[NODE_MAX_BLOCKS];
    int high_num = 0, low_num = 0;
    for (const int block : IndexRange(blocks)) {
      if (middle[block] < split) {
        const int last = std::min(begin[block + 1], split);
        if (middle[block] < last) {
          high[high_num++] = IndexRange::from_begin_end(middle[block], last);
        }
      }
      else if (middle[block] > split) {
        const int first = std::max(begin[block], split);
        if (first < middle[block]) {
          low[low_num++] = IndexRange::from_begin_end(first, middle[block]);
        }
      }
    }

    /* Pair the two lists up into chunks that can be swapped independently. Each step finishes a
     * range on one side or the other, so there are fewer chunks than there are ranges. */
    struct Swap {
      int from;
      int to;
      int num;
    };
    Swap swaps[2 * NODE_MAX_BLOCKS];
    int swaps_num = 0;
    for (int h = 0, l = 0, h_done = 0, l_done = 0; h < high_num && l < low_num;) {
      const int num = std::min(int(high[h].size()) - h_done, int(low[l].size()) - l_done);
      swaps[swaps_num++] = {int(high[h].start()) + h_done, int(low[l].start()) + l_done, num};
      h_done += num;
      l_done += num;
      if (h_done == int(high[h].size())) {
        h++;
        h_done = 0;
      }
      if (l_done == int(low[l].size())) {
        l++;
        l_done = 0;
      }
    }

    threading::parallel_for(IndexRange(swaps_num), 1, [&](const IndexRange range) {
      for (const int i : range) {
        const Swap &swap = swaps[i];
        for (const int offset : IndexRange(swap.num)) {
          std::swap(points[swap.from + offset], points[swap.to + offset]);
        }
      }
    });

    return split;
  }

  static int block_num(const int count)
  {
    return std::min(count / NODE_BLOCK_SIZE, NODE_MAX_BLOCKS);
  }

  static int block_begin(const int count, const int blocks, const int block)
  {
    return int(int64_t(count) * block / blocks);
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Search
   * \{ */

  /**
   * \param distance_sq: Squared distance from \a position to the part of space \a node covers.
   * \param offsets: Squared distance to that part per dimension, which this restores before it
   * returns so that the caller can keep using it.
   * \return False when the result set has seen enough and the search should stop.
   */
  template<typename ResultSet>
  bool search_node(ResultSet &result,
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

  /** \} */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Result Sets
 *
 * A search reports the points it visits to a result set, which keeps what it wants and tells the
 * search how far it still has to look through #max_distance_sq. Returning false from #add_point
 * ends the search.
 * \{ */

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
template<typename ValueT> class FilteredNearestResult {
 private:
  FunctionRef<bool(int index)> filter_;
  int index_ = -1;
  ValueT distance_sq_ = std::numeric_limits<ValueT>::max();

 public:
  explicit FilteredNearestResult(FunctionRef<bool(int index)> filter) : filter_(filter) {}

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

/* -------------------------------------------------------------------- */
/** \name #KDTreeNanoflann
 * \{ */

template<typename T> static const KDTree<T> &get_tree(const void *impl)
{
  return *static_cast<const KDTree<T> *>(impl);
}

template<typename T>
KDTreeNanoflann<T>::KDTreeNanoflann(const Span<T> points, const int leaf_size)
    : points_(points), size_(int(points.size()))
{
  static_assert(sizeof(KDTree<T>) <= IMPL_SIZE);
  static_assert(alignof(KDTree<T>) <= IMPL_ALIGNMENT);

  new (impl_.ptr()) KDTree<T>(points, Span<int>(), leaf_size);
}

template<typename T>
KDTreeNanoflann<T>::KDTreeNanoflann(const Span<T> points,
                                    const IndexMask &mask,
                                    const int leaf_size)
    : points_(points), size_(int(mask.size()))
{
  const std::optional<IndexRange> range = mask.to_range();
  if (range && *range == points.index_range()) {
    /* Avoid the indirection when the mask contains every point anyway. */
    new (impl_.ptr()) KDTree<T>(points, Span<int>(), leaf_size);
    return;
  }
  /* Only needed to build the tree, which copies the order it ends up visiting the points in. */
  Array<int> indices(mask.size());
  mask.to_indices<int>(indices);
  new (impl_.ptr()) KDTree<T>(points, indices.as_span(), leaf_size);
}

template<typename T>
KDTreeNanoflann<T>::KDTreeNanoflann(const Span<T> points,
                                    const Span<int> indices,
                                    const int leaf_size)
    : points_(points), size_(int(indices.size()))
{
  new (impl_.ptr()) KDTree<T>(points, indices, leaf_size);
}

template<typename T> KDTreeNanoflann<T>::~KDTreeNanoflann()
{
  std::destroy_at(static_cast<KDTree<T> *>(impl_.ptr()));
}

template<typename T>
int KDTreeNanoflann<T>::find_nearest(const T &position, ValueT *r_distance_sq) const
{
  if (this->is_empty()) {
    return -1;
  }
  NearestResult<ValueT> result;
  get_tree<T>(impl_.ptr()).find_neighbors(result, position);
  if (result.index() == -1) {
    return -1;
  }
  if (r_distance_sq) {
    *r_distance_sq = result.distance_sq();
  }
  return result.index();
}

template<typename T>
int KDTreeNanoflann<T>::find_nearest_filtered(const T &position,
                                              FunctionRef<bool(int index)> filter,
                                              ValueT *r_distance_sq) const
{
  if (this->is_empty()) {
    return -1;
  }
  FilteredNearestResult<ValueT> result(filter);
  get_tree<T>(impl_.ptr()).find_neighbors(result, position);
  if (result.index() != -1 && r_distance_sq) {
    *r_distance_sq = result.distance_sq();
  }
  return result.index();
}

template<typename T>
int KDTreeNanoflann<T>::find_nearest_n(const T &position, MutableSpan<Nearest> r_nearest) const
{
  if (this->is_empty() || r_nearest.is_empty()) {
    return 0;
  }
  NearestNResult<Nearest, ValueT> result(r_nearest);
  get_tree<T>(impl_.ptr()).find_neighbors(result, position);
  return result.count();
}

template<typename T>
void KDTreeNanoflann<T>::foreach_in_radius(
    const T &position,
    const ValueT radius,
    FunctionRef<void(int index, ValueT distance_sq)> fn) const
{
  if (this->is_empty()) {
    return;
  }
  const auto call_fn = [&](const int index, const ValueT distance_sq) { fn(index, distance_sq); };
  RadiusResult<ValueT, decltype(call_fn)> result(radius * radius, call_fn);
  get_tree<T>(impl_.ptr()).find_neighbors(result, position);
}

template<typename T>
int KDTreeNanoflann<T>::find_in_radius(const T &position,
                                       const ValueT radius,
                                       Vector<Nearest> &r_nearest) const
{
  r_nearest.clear();
  if (this->is_empty()) {
    return 0;
  }
  const auto collect_fn = [&](const int index, const ValueT distance_sq) {
    r_nearest.append({index, distance_sq});
  };
  RadiusResult<ValueT, decltype(collect_fn)> result(radius * radius, collect_fn);
  get_tree<T>(impl_.ptr()).find_neighbors(result, position);

  std::sort(r_nearest.begin(), r_nearest.end(), [](const Nearest &a, const Nearest &b) {
    return a.distance_sq < b.distance_sq || (a.distance_sq == b.distance_sq && a.index < b.index);
  });
  return int(r_nearest.size());
}

template<typename T>
int KDTreeNanoflann<T>::count_in_radius(const T &position, const ValueT radius) const
{
  if (this->is_empty()) {
    return 0;
  }
  const auto count_fn = [](const int /*index*/, const ValueT /*distance_sq*/) {};
  RadiusResult<ValueT, decltype(count_fn)> result(radius * radius, count_fn);
  get_tree<T>(impl_.ptr()).find_neighbors(result, position);
  return result.count();
}

template<typename T>
int KDTreeNanoflann<T>::calc_duplicates(const ValueT range, MutableSpan<int> duplicates) const
{
  if (this->is_empty()) {
    return 0;
  }
  const KDTree<T> &tree = get_tree<T>(impl_.ptr());
  const ValueT range_sq = range * range;
  int found = 0;
  /* The order the tree visits the points in rather than the order they were added. Which point of
   * a cluster the others merge into depends on the order either way, and this one keeps each
   * search near the one before it. */
  for (const int index : tree.indices()) {
    BLI_assert(index < duplicates.size());
    /* Skip points that are already merged into another point. Points that were preset to their
     * own index are not merged themselves, but can still be merged into. */
    if (duplicates[index] != -1 && duplicates[index] != index) {
      continue;
    }
    const int found_before = found;
    const auto claim_fn = [&](const int other, const ValueT /*distance_sq*/) {
      if (other != index && duplicates[other] == -1) {
        duplicates[other] = index;
        found++;
      }
    };
    RadiusResult<ValueT, decltype(claim_fn)> result(range_sq, claim_fn);
    tree.find_neighbors(result, points_[index]);
    if (found != found_before) {
      /* Mark as a target so the merged points don't form chains. */
      duplicates[index] = index;
    }
  }
  return found;
}

template class KDTreeNanoflann<float2>;
template class KDTreeNanoflann<float3>;

/** \} */

}  // namespace blender
