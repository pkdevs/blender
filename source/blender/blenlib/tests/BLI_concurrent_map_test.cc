/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_concurrent_map.hh"

#include "testing/testing.h"

namespace blender::tests {

TEST(concurrent_map, Clear)
{
  ConcurrentMap<int, int> map;
  {
    ConcurrentMap<int, int>::MutableAccessor accessor;
    EXPECT_TRUE(map.add(accessor, 1));
    accessor->second = 42;
  }

  map.clear();

  ConcurrentMap<int, int>::ConstAccessor accessor;
  EXPECT_FALSE(map.lookup(accessor, 1));
}

}  // namespace blender::tests
