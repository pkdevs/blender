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
#include <cstddef>
#include <cstdint>

namespace mem_guarded::internal::memory_usage {

enum class LocalState {
  Unregistered, /** Thread has not registered #Local yet. */
  Registered,   /** Local counters can be used. */
  Exited,       /** Thread is exiting, need to use global counters. */
};

/**
 * This is stored per thread. Align to cache line size to avoid false sharing.
 *
 * Constant initialized and trivially destructible, to avoid the compiler generating
 * expensive initialization check on every access.
 */
struct alignas(128) Local {
  LocalState state = LocalState::Unregistered;
  /**
   * Number of bytes. This can be negative when e.g. one thread allocates a lot of memory, and
   * another frees it. It has to be an atomic, because it may be read by other threads when the
   * total memory usage is counted. Only the owning thread writes it.
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
};

extern constinit thread_local Local local_data;

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
  return local_data;
}

/**
 * Add to atomic counter only written by the owning thread and read by other threads. This means we
 * can use cheaper separate load and store instructions rather than requiring them to be done in
 * one atomic operation.
 */
inline int64_t local_counter_add(std::atomic<int64_t> &counter, const int64_t value)
{
  const int64_t new_value = counter.load(std::memory_order_relaxed) + value;
  counter.store(new_value, std::memory_order_relaxed);
  return new_value;
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
  Local &local = get_local_data();
  if (local.state == LocalState::Registered) [[likely]] {
    /* Increase local memory counts. This does not cause thread synchronization in the majority of
     * cases, because each thread has these counters on a separate cache line. It may only cause
     * synchronization if another thread is computing the total current memory usage at the same
     * time, which is very rare compared to doing allocations. */
    local_counter_add(local.blocks_num, 1);
    const int64_t mem_in_use = local_counter_add(local.mem_in_use, int64_t(size));

    /* If a certain amount of new memory has been allocated, update the peak. */
    if (mem_in_use - local.mem_in_use_during_peak_update.load(std::memory_order_relaxed) >
        peak_update_threshold)
    {
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
  Local &local = get_local_data();
  if (local.state == LocalState::Registered) [[likely]] {
    /* Decrease local memory counts. See comment in #memory_usage_block_alloc for details regarding
     * thread synchronization. */
    local_counter_add(local.mem_in_use, -int64_t(size));
    local_counter_add(local.blocks_num, -1);
  }
  else {
    block_free_slow(size);
  }
}
