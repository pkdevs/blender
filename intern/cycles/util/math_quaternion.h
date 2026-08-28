/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/math_base.h"
#include "util/types_quaternion.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline Quaternion operator*(const float a, const Quaternion q)
{
  return make_quaternion(a * q.w, a * q.x, a * q.y, a * q.z);
}
ccl_device_inline Quaternion operator*(const Quaternion q, const float a)
{
  return make_quaternion(q.w * a, q.x * a, q.y * a, q.z * a);
}

ccl_device_inline Quaternion operator+(const Quaternion a, const Quaternion b)
{
  return make_quaternion(a.w + b.w, a.x + b.x, a.y + b.y, a.z + b.z);
}

ccl_device_inline float dot(const Quaternion a, const Quaternion b)
{
  return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

ccl_device_inline Quaternion normalized(const Quaternion q)
{
  const float len = sqrtf(dot(q, q));
  if (len != 0.0f) {
    const float len_inv = 1.0f / len;
    return make_quaternion(q.w * len_inv, q.x * len_inv, q.y * len_inv, q.z * len_inv);
  }
  return identity_quaternion();
}

ccl_device_inline Quaternion lerp(const Quaternion q1, const Quaternion q2, const float t)
{
  return normalized((1.0f - t) * q1 + t * q2);
}

CCL_NAMESPACE_END
