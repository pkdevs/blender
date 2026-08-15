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

using CollaborativeOnceFlag = tbb::collaborative_once_flag;
using tbb::collaborative_call_once;

#else

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

#endif

}  // namespace blender::threading
