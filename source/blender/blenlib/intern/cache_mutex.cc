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
    BLI_assert(this->is_dirty());
    compute_cache();
  });
  BLI_assert(this->is_cached());
}

void CacheMutex::tag_dirty()
{
  threading::collaborative_once_flag_reset(once_flag_);
}

}  // namespace blender
