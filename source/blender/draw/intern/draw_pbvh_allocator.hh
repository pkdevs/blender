/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include <algorithm>

#include "BLI_vector.hh"

#include "DRW_pbvh.hh"

namespace blender::draw::pbvh {

/**
 * Allocates sub-ranges inside a GPU buffer of a known (but growing) size.
 *
 * - #acquire: a node requests a range sized to its geometry. The allocator either reuses space
 *   that was previously #release'd, or grows by extending its size.
 * - #release: a node no longer needs its range. The space is tracked as free so it can be reused
 *   by a later #acquire.
 *
 * Growing the buffer is an expensive event, since the GPU buffers backing the space need to be
 * reallocated. The caller should compare #size between frames to detect whether this happened.
 */
class RangeAllocator {
  int64_t size_ = 0;
  bool should_optimize_ = false;
  Vector<IndexRange> released_ranges_;

 public:
  bool is_empty() const
  {
    return size_ == 0;
  }

  int64_t size() const
  {
    return size_;
  }

  /**
   * \brief Acquire space for the given buffer range.
   *
   * Reuses previously released space when a large enough range is available, otherwise grows the
   * virtual buffer. Marks the range as acquired and fills in its offset.
   */
  void acquire(NodeInfo::BufferRange &buffer_info)
  {
    BLI_assert(!buffer_info.is_space_acquired);
    if (buffer_info.len <= 0) {
      buffer_info.is_space_acquired = true;
      buffer_info.offset = 0;
      return;
    }

    /* Ensure sequential free space are merged. */
    if (should_optimize_) {
      this->optimize();
      should_optimize_ = false;
    }

    /* Check for space inside existing unassigned space. */
    for (int64_t space_index : released_ranges_.index_range()) {
      IndexRange &index_range = released_ranges_[space_index];
      if (index_range.size() >= buffer_info.len) {
        buffer_info.offset = index_range.start();
        index_range = index_range.drop_front(buffer_info.len);
        if (index_range.is_empty()) {
          released_ranges_.remove(space_index);
        }
        buffer_info.is_space_acquired = true;
        break;
      }
    }

    /* If not found we need to grow space. This would invalidate all existing VBO's. */
    if (!buffer_info.is_space_acquired) {
      buffer_info.offset = size_;
      size_ += buffer_info.len;
      buffer_info.is_space_acquired = true;
    }
  }

  /**
   * Release the space previously acquired for the given buffer range.
   *
   * The space becomes available for reuse by a later #acquire. Resets the range to an unacquired
   * state.
   */
  void release(NodeInfo::BufferRange &buffer_info)
  {
    BLI_assert(buffer_info.is_space_acquired);
    const IndexRange range = buffer_info.index_range();
    buffer_info.is_space_acquired = false;
    buffer_info.len = -1;
    buffer_info.offset = -1;

    if (range.is_empty()) {
      return;
    }
    released_ranges_.append(range);
    should_optimize_ = true;
  }

 private:
  /**
   * Merge adjacent free ranges in the released-space list.
   *
   * Should be called after a batch of releases, before the next acquisitions, so that released
   * space can be reused by requests that are larger than the individual released ranges.
   */
  void optimize()
  {
    this->optimize_sort();
    this->optimize_merge();
    this->optimize_remove_empty();
#ifndef NDEBUG
    this->validate();
#endif
  }

  void optimize_sort()
  {
    std::ranges::sort(released_ranges_, [](const IndexRange &a, const IndexRange &b) {
      return a.begin() < b.begin();
    });
  }

  void optimize_merge()
  {
    int last_value = -2;
    int last_index = -1;
    for (int index : released_ranges_.index_range()) {
      IndexRange &range = released_ranges_[index];
      if (range.is_empty()) {
        continue;
      }
      if (range.first() == last_value + 1) {
        /* Merge with previous */
        IndexRange &prev_range = released_ranges_[last_index];
        range = IndexRange::from_begin_size(prev_range.first(), prev_range.size() + range.size());
        prev_range = IndexRange::from_begin_size(prev_range.first(), 0);
      }
      last_value = range.last();
      last_index = index;
    }
  }

  void optimize_remove_empty()
  {
    released_ranges_.remove_if([](const IndexRange &space) { return space.is_empty(); });
  }

#ifndef NDEBUG
  void validate() const
  {
    int last_value = -2;
    for (const IndexRange &range : released_ranges_) {
      BLI_assert_msg(range.start() != last_value + 1, "Found two ranges that has to be merged.");
      BLI_assert_msg(range.start() > last_value, "Ranges are unordered");
      BLI_assert_msg(!range.is_empty(), "Found empty range");
      last_value = range.last();
    }
  }
#endif
};

}  // namespace blender::draw::pbvh
