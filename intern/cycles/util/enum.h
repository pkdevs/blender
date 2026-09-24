/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/defines.h"

/* Bitwise operators for enum flags. The underlying integer type is passed explicitly,
 * as `std::underlying_type` is not available for all GPU backends.
 *
 * Operators with other types are deleted, so mixing flags of different enums is a
 * compile error. */

#define CCL_ENUM_OPERATORS(_type, _int_type) \
  ccl_device_inline constexpr _type operator|(const _type a, const _type b) \
  { \
    return _type(_int_type(a) | _int_type(b)); \
  } \
  ccl_device_inline constexpr _type operator&(const _type a, const _type b) \
  { \
    return _type(_int_type(a) & _int_type(b)); \
  } \
  ccl_device_inline constexpr _type operator^(const _type a, const _type b) \
  { \
    return _type(_int_type(a) ^ _int_type(b)); \
  } \
  ccl_device_inline constexpr _type operator~(const _type a) \
  { \
    return _type(~_int_type(a)); \
  } \
  ccl_device_inline void operator|=(ccl_private _type &a, const _type b) \
  { \
    a = a | b; \
  } \
  ccl_device_inline void operator&=(ccl_private _type &a, const _type b) \
  { \
    a = a & b; \
  } \
  ccl_device_inline void operator^=(ccl_private _type &a, const _type b) \
  { \
    a = a ^ b; \
  } \
  template<typename T> void operator|(_type, T) = delete; \
  template<typename T> void operator|(T, _type) = delete; \
  template<typename T> void operator&(_type, T) = delete; \
  template<typename T> void operator&(T, _type) = delete; \
  template<typename T> void operator^(_type, T) = delete; \
  template<typename T> void operator^(T, _type) = delete;
