/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_cache_mutex.hh"

namespace blender {

void CacheMutex::ensure_impl(const FunctionRef<void()> compute_cache)
{
  threading::collaborative_call_once(once_flag_, [&]() {
    compute_cache();
    cache_valid_.store(true, std::memory_order_release);
  });
}

void CacheMutex::tag_dirty()
{
  cache_valid_.store(false);
  std::destroy_at(&once_flag_);
  new (&once_flag_) threading::CollaborativeOnceFlag();
}

}  // namespace blender
