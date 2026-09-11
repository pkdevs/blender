/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "BLI_task.hh"

/* Build subtrees with Blender's task pool rather than the threads nanoflann would create itself.
 * Must be defined before the header is included. */
#define NANOFLANN_PARALLEL_INVOKE(fn_a, fn_b) blender::threading::parallel_invoke(fn_a, fn_b)

#include "nanoflann.hpp"

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdtree_nanoflann.hh"
#include "BLI_utildefines.hh"

namespace blender {

namespace nf = ::nanoflann;

/* -------------------------------------------------------------------- */
/** \name Nanoflann Index
 * \{ */

/**
 * The data nanoflann reads coordinates from. There are two versions so that the common case of a
 * tree referencing a contiguous portion of the span doesn't pay for the index indirection.
 */
template<typename T, bool Masked> struct DatasetAdaptor {
  using ValueT = typename T::base_type;

  Span<T> points;
  /** Indices of the points in the tree, used when #Masked is true. */
  Span<int> indices;

  size_t kdtree_get_point_count() const
  {
    return size_t(Masked ? indices.size() : points.size());
  }

  ValueT kdtree_get_pt(const size_t i, const size_t dim) const
  {
    if constexpr (Masked) {
      return points[indices[int64_t(i)]][int(dim)];
    }
    else {
      return points[int64_t(i)][int(dim)];
    }
  }

  /** Let nanoflann compute the bounding box itself. */
  template<class BBox> bool kdtree_get_bbox(BBox & /*bbox*/) const
  {
    return false;
  }
};

template<typename T, bool Masked>
using NanoflannIndex =
    nf::KDTreeSingleIndexAdaptor<nf::L2_Simple_Adaptor<typename T::base_type,
                                                       DatasetAdaptor<T, Masked>,
                                                       typename T::base_type,
                                                       int>,
                                 DatasetAdaptor<T, Masked>,
                                 T::type_length,
                                 int>;

/**
 * Tree size needed to make building in parallel worthwhile. See the "tiny tree" tests in
 * `BLI_kdtree_performance_test`.
 */
static constexpr int64_t PARALLEL_BUILD_THRESHOLD = 10000;

/**
 * Building the tree repeatedly partitions the coordinates, so it is bottlenecked by memory
 * bandwidth. Past this level, extra threads won't be able to do any useful work.
 *
 * Measured on a Ryzen 7950X, a million point build takes 176 ms on one thread and 44 ms on 8, but
 * 52 ms on 16 while using twice as much CPU time. See `BLI_kdtree_performance_test`.
 */
static constexpr int PARALLEL_BUILD_MAX_THREADS = 8;

/**
 * Experimentally derived value to increase building thread count from 4 to 8 as point count
 * increases from 10'000 to 100'000, balancing threading overhead with increased parallelism
 * opportunity. See `BLI_kdtree_performance_test`.s
 */
static constexpr int64_t PARALLEL_BUILD_POINTS_PER_THREAD = 2500;

static int parallel_build_threads(const int64_t points_num)
{
  return int(std::clamp<int64_t>(
      points_num / PARALLEL_BUILD_POINTS_PER_THREAD, 1, PARALLEL_BUILD_MAX_THREADS));
}

template<typename T, bool Masked> struct TreeImpl {
  static constexpr bool is_masked = Masked;

  /** Optional storage for IndexMask construction. */
  Array<int> indices_storage;
  DatasetAdaptor<T, Masked> adaptor;
  NanoflannIndex<T, Masked> index;

  TreeImpl(const Span<T> points, Array<int> indices, const int leaf_size)
      : indices_storage(std::move(indices)),
        adaptor{points, this->indices_storage},
        index(T::type_length, adaptor, build_params(adaptor, leaf_size))
  {
    this->build();
  }

  TreeImpl(const Span<T> points, const Span<int> indices, const int leaf_size)
      : adaptor{points, indices}, index(T::type_length, adaptor, build_params(adaptor, leaf_size))
  {
    this->build();
  }

 private:
  static nf::KDTreeSingleIndexAdaptorParams build_params(const DatasetAdaptor<T, Masked> &adaptor,
                                                         const int leaf_size)
  {
    const int64_t points_num = int64_t(adaptor.kdtree_get_point_count());
    return nf::KDTreeSingleIndexAdaptorParams(
        size_t(leaf_size),
        /* Build separately instead, so that it can be limited to fewer threads. */
        nf::KDTreeSingleIndexAdaptorFlags::SkipInitialBuildIndex,
        /* Anything but one makes nanoflann use #NANOFLANN_PARALLEL_INVOKE, which leaves the
         * number of threads up to the task scheduler. */
        points_num < PARALLEL_BUILD_THRESHOLD ? 1 : 0);
  }

  void build()
  {
    const int64_t points_num = int64_t(adaptor.kdtree_get_point_count());
    if (points_num < PARALLEL_BUILD_THRESHOLD) {
      index.buildIndex();
    }
    else {
      threading::max_threads_task(parallel_build_threads(points_num),
                                  [&]() { index.buildIndex(); });
    }
  }
};

/** Calls fn with the tree implementation matching the way the tree was built. */
template<typename T, typename Fn>
static auto dispatch(const void *impl, const bool is_masked, Fn &&fn)
{
  if (is_masked) {
    return fn(*static_cast<const TreeImpl<T, true> *>(impl));
  }
  return fn(*static_cast<const TreeImpl<T, false> *>(impl));
}

/** Translate an index used by nanoflann into an index into the original coordinate span. */
template<typename ImplT> static int point_index(const ImplT &impl, const int index)
{
  if constexpr (ImplT::is_masked) {
    return impl.adaptor.indices[index];
  }
  else {
    UNUSED_VARS(impl);
    return index;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Result Sets
 *
 * Nanoflann reports the points it visits to a "result set" which is expected to provide #addPoint,
 * which returns false to stop the search early, and #worstDist, which is the squared distance
 * beyond which the search can prune nodes.
 * \{ */

template<typename ValueT> class NearestResultSet {
 public:
  using DistanceType = ValueT;
  using IndexType = int;

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

  bool addPoint(const ValueT distance_sq, const int index)
  {
    if (distance_sq < distance_sq_) {
      distance_sq_ = distance_sq;
      index_ = index;
    }
    return true;
  }

  ValueT worstDist() const
  {
    return distance_sq_;
  }

  size_t size() const
  {
    return index_ == -1 ? 0 : 1;
  }
  bool empty() const
  {
    return index_ == -1;
  }
  bool full() const
  {
    return index_ != -1;
  }
  void sort() {}
};

/** Like #NearestResultSet, but only points accepted by a filter shrink the search radius. */
template<typename ImplT, typename ValueT> class FilteredNearestResultSet {
 public:
  using DistanceType = ValueT;
  using IndexType = int;

 private:
  const ImplT &impl_;
  FunctionRef<bool(int index)> filter_;
  int index_ = -1;
  ValueT distance_sq_ = std::numeric_limits<ValueT>::max();

 public:
  FilteredNearestResultSet(const ImplT &impl, FunctionRef<bool(int index)> filter)
      : impl_(impl), filter_(filter)
  {
  }

  /** \note Already an index into the original coordinate span. */
  int index() const
  {
    return index_;
  }
  ValueT distance_sq() const
  {
    return distance_sq_;
  }

  bool addPoint(const ValueT distance_sq, const int index)
  {
    if (distance_sq < distance_sq_) {
      const int point = point_index(impl_, index);
      if (filter_(point)) {
        distance_sq_ = distance_sq;
        index_ = point;
      }
    }
    return true;
  }

  ValueT worstDist() const
  {
    return distance_sq_;
  }

  size_t size() const
  {
    return index_ == -1 ? 0 : 1;
  }
  bool empty() const
  {
    return index_ == -1;
  }
  bool full() const
  {
    return index_ != -1;
  }
  void sort() {}
};

/** Keeps the closest results in the caller's array, sorted by increasing distance. */
template<typename NearestT, typename ValueT> class NearestNResultSet {
 public:
  using DistanceType = ValueT;
  using IndexType = int;

 private:
  MutableSpan<NearestT> nearest_;
  int count_ = 0;

 public:
  explicit NearestNResultSet(MutableSpan<NearestT> nearest) : nearest_(nearest) {}

  int count() const
  {
    return count_;
  }

  bool addPoint(const ValueT distance_sq, const int index)
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

  ValueT worstDist() const
  {
    if (count_ < int(nearest_.size())) {
      return std::numeric_limits<ValueT>::max();
    }
    return nearest_[count_ - 1].distance_sq;
  }

  size_t size() const
  {
    return size_t(count_);
  }
  bool empty() const
  {
    return count_ == 0;
  }
  bool full() const
  {
    return count_ == int(nearest_.size());
  }
  /** Results are inserted in order already. */
  void sort() {}
};

/**
 * Passes every point in the radius to a callback.
 * \note The radius is inclusive, matching #blender::kdtree_range_search_cb.
 */
template<typename ImplT, typename ValueT, typename Fn> class RadiusResultSet {
 public:
  using DistanceType = ValueT;
  using IndexType = int;

 private:
  const ImplT &impl_;
  ValueT radius_sq_;
  Fn fn_;
  int count_ = 0;

 public:
  RadiusResultSet(const ImplT &impl, const ValueT radius_sq, Fn fn)
      : impl_(impl), radius_sq_(radius_sq), fn_(fn)
  {
  }

  int count() const
  {
    return count_;
  }

  bool addPoint(const ValueT distance_sq, const int index)
  {
    if (distance_sq <= radius_sq_) {
      count_++;
      fn_(point_index(impl_, index), distance_sq);
    }
    return true;
  }

  ValueT worstDist() const
  {
    return radius_sq_;
  }

  size_t size() const
  {
    return size_t(count_);
  }
  bool empty() const
  {
    return count_ == 0;
  }
  bool full() const
  {
    return true;
  }
  void sort() {}
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #KDTreeNanoflann
 * \{ */

template<typename T>
KDTreeNanoflann<T>::KDTreeNanoflann(const Span<T> points, const int leaf_size)
    : points_(points), size_(int(points.size()))
{
  static_assert(sizeof(TreeImpl<T, false>) <= IMPL_SIZE);
  static_assert(sizeof(TreeImpl<T, true>) <= IMPL_SIZE);
  static_assert(alignof(TreeImpl<T, false>) <= IMPL_ALIGNMENT);
  static_assert(alignof(TreeImpl<T, true>) <= IMPL_ALIGNMENT);

  new (impl_.ptr()) TreeImpl<T, false>(points, Array<int>(), leaf_size);
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
    new (impl_.ptr()) TreeImpl<T, false>(points, Array<int>(), leaf_size);
    return;
  }
  Array<int> indices(mask.size());
  mask.to_indices<int>(indices);
  is_masked_ = true;
  new (impl_.ptr()) TreeImpl<T, true>(points, std::move(indices), leaf_size);
}

template<typename T>
KDTreeNanoflann<T>::KDTreeNanoflann(const Span<T> points,
                                    const Span<int> indices,
                                    const int leaf_size)
    : points_(points), size_(int(indices.size())), is_masked_(true)
{
  new (impl_.ptr()) TreeImpl<T, true>(points, indices, leaf_size);
}

template<typename T> KDTreeNanoflann<T>::~KDTreeNanoflann()
{
  if (is_masked_) {
    std::destroy_at(static_cast<TreeImpl<T, true> *>(impl_.ptr()));
  }
  else {
    std::destroy_at(static_cast<TreeImpl<T, false> *>(impl_.ptr()));
  }
}

template<typename T>
int KDTreeNanoflann<T>::find_nearest(const T &position, ValueT *r_distance_sq) const
{
  if (this->is_empty()) {
    return -1;
  }
  return dispatch<T>(impl_.ptr(), is_masked_, [&](auto &impl) {
    NearestResultSet<ValueT> result;
    impl.index.findNeighbors(result, static_cast<const ValueT *>(position));
    if (result.index() == -1) {
      return -1;
    }
    if (r_distance_sq) {
      *r_distance_sq = result.distance_sq();
    }
    return point_index(impl, result.index());
  });
}

template<typename T>
int KDTreeNanoflann<T>::find_nearest_filtered(const T &position,
                                              FunctionRef<bool(int index)> filter,
                                              ValueT *r_distance_sq) const
{
  if (this->is_empty()) {
    return -1;
  }
  return dispatch<T>(impl_.ptr(), is_masked_, [&](auto &impl) {
    FilteredNearestResultSet<std::decay_t<decltype(impl)>, ValueT> result(impl, filter);
    impl.index.findNeighbors(result, static_cast<const ValueT *>(position));
    if (result.index() != -1 && r_distance_sq) {
      *r_distance_sq = result.distance_sq();
    }
    return result.index();
  });
}

template<typename T>
int KDTreeNanoflann<T>::find_nearest_n(const T &position, MutableSpan<Nearest> r_nearest) const
{
  if (this->is_empty() || r_nearest.is_empty()) {
    return 0;
  }
  return dispatch<T>(impl_.ptr(), is_masked_, [&](auto &impl) {
    NearestNResultSet<Nearest, ValueT> result(r_nearest);
    impl.index.findNeighbors(result, static_cast<const ValueT *>(position));
    const int count = result.count();
    if constexpr (std::decay_t<decltype(impl)>::is_masked) {
      for (Nearest &nearest : r_nearest.take_front(count)) {
        nearest.index = point_index(impl, nearest.index);
      }
    }
    return count;
  });
}

template<typename T>
void KDTreeNanoflann<T>::foreach_in_radius(const T &position,
                                           const ValueT radius,
                                           FunctionRef<void(int, ValueT)> fn) const
{
  if (this->is_empty()) {
    return;
  }
  dispatch<T>(impl_.ptr(), is_masked_, [&](auto &impl) {
    using ImplT = std::decay_t<decltype(impl)>;
    RadiusResultSet<ImplT, ValueT, FunctionRef<void(int, ValueT)>> result(
        impl, radius * radius, fn);
    impl.index.findNeighbors(result, static_cast<const ValueT *>(position));
  });
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
  dispatch<T>(impl_.ptr(), is_masked_, [&](auto &impl) {
    using ImplT = std::decay_t<decltype(impl)>;
    const auto append_fn = [&](const int index, const ValueT distance_sq) {
      r_nearest.append({index, distance_sq});
    };
    RadiusResultSet<ImplT, ValueT, decltype(append_fn)> result(impl, radius * radius, append_fn);
    impl.index.findNeighbors(result, static_cast<const ValueT *>(position));
  });
  /* Sorting by index as well keeps the result deterministic when distances are equal. */
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
  return dispatch<T>(impl_.ptr(), is_masked_, [&](auto &impl) {
    using ImplT = std::decay_t<decltype(impl)>;
    const auto count_fn = [](const int /*index*/, const ValueT /*distance_sq*/) {};
    RadiusResultSet<ImplT, ValueT, decltype(count_fn)> result(impl, radius * radius, count_fn);
    impl.index.findNeighbors(result, static_cast<const ValueT *>(position));
    return result.count();
  });
}

template<typename T>
int KDTreeNanoflann<T>::calc_duplicates(const ValueT range, MutableSpan<int> duplicates) const
{
  if (this->is_empty()) {
    return 0;
  }
  const ValueT range_sq = range * range;
  int found = 0;
  dispatch<T>(impl_.ptr(), is_masked_, [&](auto &impl) {
    using ImplT = std::decay_t<decltype(impl)>;
    for (const int i : IndexRange(size_)) {
      const int index = point_index(impl, i);
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
      RadiusResultSet<ImplT, ValueT, decltype(claim_fn)> result(impl, range_sq, claim_fn);
      impl.index.findNeighbors(result, static_cast<const ValueT *>(points_[index]));
      if (found != found_before) {
        /* Mark as a target so the merged points don't form chains. */
        duplicates[index] = index;
      }
    }
  });
  return found;
}

template class KDTreeNanoflann<float2>;
template class KDTreeNanoflann<float3>;

/** \} */

}  // namespace blender
