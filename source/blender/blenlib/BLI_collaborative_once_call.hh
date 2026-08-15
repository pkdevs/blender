/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#ifdef WITH_TBB
#  include <cstddef>
#  include <tbb/collaborative_call_once.h>
#else
#  include <atomic>

#  include "BLI_mutex.hh"
#  include "BLI_task.hh"
#  include "BLI_utility_mixins.hh"
#endif

namespace blender::threading {

#ifdef WITH_TBB

/**
 * #CollaborativeOnceFlag allows a function to be called just once in a thread-safe manner. With
 * it, a thread-safe cache can be implemented. Importantly, it's "collaborative" in the sense that
 * threads waiting for the cache to be computed can do other work while waiting.
 */
using CollaborativeOnceFlag = tbb::collaborative_once_flag;
using tbb::collaborative_call_once;

/**
 * Returns true when the flag is in the done state. Importantly, this does not allow checking
 * whether the function is being computed currently, only whether it is done.
 */
inline bool collaborative_once_flag_is_done(const CollaborativeOnceFlag &flag)
{
  /* This is a copy of the start of #tbb::collaborative_call_once. There is otherwise no public
   * API for it. It relies on TBB internals. Asserts in #CacheMutex::ensure_impl should fail if
   * tbb internals changed in an incompatible way. */
  static_assert(sizeof(CollaborativeOnceFlag) == sizeof(std::atomic<uintptr_t>));
  const std::atomic<uintptr_t> &state = *reinterpret_cast<const std::atomic<uintptr_t> *>(&flag);
  /* Matches tbb::collaborative_once_flag::state::done. */
  constexpr uintptr_t done_state = 1;
  const bool is_done = state.load(std::memory_order_acquire) == done_state;
  return is_done;
}

/**
 * Set the flag back to its initial state. This should never be done while the flag is in use by
 * #collaborative_call_once.
 */
inline void collaborative_once_flag_reset(CollaborativeOnceFlag &flag)
{
  /* This relies on the same TBB internals as #collaborative_once_flag_is_done. */
  std::atomic<uintptr_t> &state = *reinterpret_cast<std::atomic<uintptr_t> *>(&flag);
  constexpr uintptr_t init_state = 0;
  state.store(init_state, std::memory_order_release);
}

#else

/**
 * This is just a fallback when building without TBB. It's not actually collaborative but should
 * still be fine correctness-wise.
 */
struct CollaborativeOnceFlag : NonCopyable, NonMovable {
  Mutex mutex;
  std::atomic<bool> has_run = false;
};

template<typename Fn, typename... Args>
inline void collaborative_call_once(CollaborativeOnceFlag &flag, Fn &&fn, Args &&...args)
{
  if (flag.has_run.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard lock(flag.mutex);
  if (flag.has_run.load(std::memory_order_relaxed)) {
    return;
  }
  /* Isolate because mutex is locked. */
  threading::isolate_task([&]() { fn(std::forward<Args>(args)...); });
  flag.has_run.store(true, std::memory_order_release);
}

inline bool collaborative_once_flag_is_done(const CollaborativeOnceFlag &flag)
{
  return flag.has_run.load(std::memory_order_acquire);
}

inline void collaborative_once_flag_reset(CollaborativeOnceFlag &flag)
{
  flag.has_run.store(false, std::memory_order_release);
}

#endif

}  // namespace blender::threading
