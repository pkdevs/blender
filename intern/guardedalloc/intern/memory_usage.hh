/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup intern_mem
 *
 * Memory usage counting. The common case is inline, as it runs for every allocation and free.
 */

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mem_guarded::internal::memory_usage {

struct Global;

/**
 * This is stored per thread. Align to cache line size to avoid false sharing.
 */
struct alignas(128) Local {
  /**
   * Retain shared ownership of #Global to make sure that it is not destructed.
   */
  std::shared_ptr<Global> global;

  /** Helps to find bugs during program shutdown. */
  bool destructed = false;
  /**
   * This is the first created #Local and on the main thread. When the main local data is
   * destructed, we know that Blender is quitting and that we can't rely on thread locals being
   * available still.
   */
  bool is_main = false;
  /**
   * Number of bytes. This can be negative when e.g. one thread allocates a lot of memory, and
   * another frees it. It has to be an atomic, because it may be accessed by other threads when the
   * total memory usage is counted.
   */
  std::atomic<int64_t> mem_in_use = 0;
  /**
   * Number of allocated blocks. Can be negative and is atomic for the same reason as above.
   */
  std::atomic<int64_t> blocks_num = 0;
  /**
   * Amount of memory used when the peak was last updated. This is used so that we don't have to
   * update the peak memory usage after every memory allocation. Instead it's only updated when "a
   * lot" of new memory has been allocated. This makes the peak memory usage a little bit less
   * accurate, but it's still good enough for practical purposes.
   */
  std::atomic<int64_t> mem_in_use_during_peak_update = 0;

  Local();
  ~Local();
};

/**
 * This is true for most of the lifetime of the program. Only when it starts exiting this becomes
 * false indicating that global counters should be used for correctness.
 */
extern std::atomic<bool> use_local_counters;

/**
 * When a thread allocated this amount of memory, the peak memory usage is updated. An alternative
 * would be to update the global peak memory after every allocation, but that would cause much more
 * overhead with little benefit.
 */
inline constexpr int64_t peak_update_threshold = 1024 * 1024;

inline Local &get_local_data()
{
  static thread_local Local local;
  assert(!local.destructed);
  return local;
}

/** Check if the current memory usage is higher than the peak and update it if yes. */
void update_global_peak();

/** Uncommon case of #memory_usage_block_alloc and #memory_usage_block_free. */
void block_alloc_slow(size_t size);
void block_free_slow(size_t size);

}  // namespace mem_guarded::internal::memory_usage

inline void memory_usage_block_alloc(const size_t size)
{
  using namespace mem_guarded::internal::memory_usage;
  if (use_local_counters.load(std::memory_order_relaxed)) [[likely]] {
    Local &local = get_local_data();
    /* Increase local memory counts. This does not cause thread synchronization in the majority of
     * cases, because each thread has these counters on a separate cache line. It may only cause
     * synchronization if another thread is computing the total current memory usage at the same
     * time, which is very rare compared to doing allocations. */
    local.blocks_num.fetch_add(1, std::memory_order_relaxed);
    local.mem_in_use.fetch_add(int64_t(size), std::memory_order_relaxed);

    /* If a certain amount of new memory has been allocated, update the peak. */
    if (local.mem_in_use - local.mem_in_use_during_peak_update > peak_update_threshold) {
      update_global_peak();
    }
  }
  else {
    block_alloc_slow(size);
  }
}

inline void memory_usage_block_free(const size_t size)
{
  using namespace mem_guarded::internal::memory_usage;
  if (use_local_counters) [[likely]] {
    /* Decrease local memory counts. See comment in #memory_usage_block_alloc for details regarding
     * thread synchronization. */
    Local &local = get_local_data();
    local.mem_in_use.fetch_sub(int64_t(size), std::memory_order_relaxed);
    local.blocks_num.fetch_sub(1, std::memory_order_relaxed);
  }
  else {
    block_free_slow(size);
  }
}
