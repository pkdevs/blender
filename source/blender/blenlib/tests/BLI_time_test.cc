/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_time.hh"

namespace blender {

TEST(time, WallClock)
{
  /* The wall-clock time should be after 2023-01-01. */
  const int64_t unix_ms = BLI_time_wall_clock_ms();
  EXPECT_GT(unix_ms, int64_t(1672531200000));

  /* The exact local fields depend on the system timezone, so only check that they are within
   * valid ranges. */
  const DateTime dt = BLI_time_local_from_unix_ms(unix_ms);
  EXPECT_GE(dt.year, 2023);
  EXPECT_GE(dt.month, 1);
  EXPECT_LE(dt.month, 12);
  EXPECT_GE(dt.day, 1);
  EXPECT_LE(dt.day, 31);
  EXPECT_GE(dt.hour, 0);
  EXPECT_LE(dt.hour, 23);
  EXPECT_GE(dt.minute, 0);
  EXPECT_LE(dt.minute, 59);
  EXPECT_GE(dt.second, 0);
  EXPECT_LE(dt.second, 59);
}

}  // namespace blender
