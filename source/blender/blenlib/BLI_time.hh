/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * \brief Platform independent time functions.
 */

#pragma once

#include <cstdint>

namespace blender {

/**
 * Return an indication of time, expressed as seconds since some fixed point.
 * Successive calls are guaranteed to generate values greater than or equal to the last call.
 */
double BLI_time_now_seconds();

/** Integer version of #BLI_time_now_seconds. */
int64_t BLI_time_now_seconds_i();

/**
 * Platform-independent sleep function.
 * \param ms: Number of milliseconds to sleep
 */
void BLI_time_sleep_ms(int ms);

/**
 * Platform-independent high-resolution sleep function.
 * Using this function can have advantages over \see BLI_time_sleep_ms on Windows due to a default
 * non-precise sleep resolution of 15.25ms.
 * \param us: Number of microseconds to sleep
 */
void BLI_time_sleep_precise_us(int us);

/**
 * Local wall-clock date and time fields, accurate to the second.
 *
 * \note This is intentionally separate from #BLI_time_now_seconds(), which uses a monotonic clock
 * and is only meaningful for measuring elapsed time, not for producing a calendar date.
 */
struct DateTime {
  int year;   /* e.g. 2026 */
  int month;  /* 1-12 */
  int day;    /* 1-31 */
  int hour;   /* 0-23 */
  int minute; /* 0-59 */
  int second; /* 0-59 */
};

/**
 * The current wall-clock time as milliseconds since the Unix epoch (UTC).
 *
 * The value is timezone-independent, so it can be stored and passed around, then converted to
 * local fields with #BLI_time_local_from_unix_ms() when needed.
 */
int64_t BLI_time_wall_clock_ms();

/**
 * Convert milliseconds since the Unix epoch (UTC) to local wall-clock fields.
 */
DateTime BLI_time_local_from_unix_ms(int64_t unix_ms);

}  // namespace blender
