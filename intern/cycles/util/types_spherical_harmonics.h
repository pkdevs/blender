/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/defines.h"
#include "util/math_float3.h"
#include "util/types_base.h"
#include "util/types_float3.h"

#if !defined(__KERNEL_GPU__)
#  include <cstring> /* for memset(). */
#endif

CCL_NAMESPACE_BEGIN

/* A packed representation of spherical harmonics.
 * Supports spherical harmonics up to degree of 3.  The values are quantized to 8 bit.
 *
 * The order is: <L=1 M=-1>, <L=1 M=0>, <L=1 M=1>, <L=2 M=-2> ...
 * where L is the band index, M is the  basis function.
 *
 * NOTE: There is an utility spherical_harmonics_get<L, M> to access bands in a more semantic way.
 */
struct PackedSphericalHarmonics {
  /* Some formats (like SPZ) define the maximum degree as 4.
   * However, in practice it is hard to run across dataset that is actually trained using such
   * high degree, and the difference that the higher degree brings is probably barely perceivable.
   * To keep logic simple, assume the most commonly used degree of 3, and allocate coefficients for
   * it. */
  ccl_static_constexpr int MAX_DEGREES = 3;
  ccl_static_constexpr int MAX_COEFFICIENTS = 15;

  int8_t coefficients[3 * MAX_COEFFICIENTS];

  /* NOTE: Can not have methods here, as the kernel references data from global memory, which makes
   * it tricky on Metal that either doesn't support it or requires some non-trivial way, as calling
   * methods from a `ccl_global PackedSphericalHarmonics *` conflicts with the address space for
   * `this`. */
};

/* Set all spherical harmonics values to 0. */
ccl_device_forceinline void spherical_harmonics_fill_zero(
    ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
#if !defined(__KERNEL_GPU__)
  memset(
      packed_spherical_harmonics.coefficients, 0, sizeof(packed_spherical_harmonics.coefficients));
#else
  /* Metal does not support memset() call. */
  for (int i = 0; i < 3 * PackedSphericalHarmonics::MAX_COEFFICIENTS; ++i) {
    packed_spherical_harmonics.coefficients[i] = 0;
  }
#endif
}

/* Set the spherical harmonic coefficient by its flat index. */
ccl_device_forceinline void spherical_harmonics_set_coefficient(
    ccl_global PackedSphericalHarmonics &packed_spherical_harmonics,
    const int index,
    const float3 value)
{
  util_assert(index >= 0);
  util_assert(index < PackedSphericalHarmonics::MAX_COEFFICIENTS);
  const int i = index * 3;
  const float3 clamped_scaled = clamp(value, make_float3(-1.0f), make_float3(1.0f)) * 127.0f;
  packed_spherical_harmonics.coefficients[i + 0] = int8_t(clamped_scaled.x);
  packed_spherical_harmonics.coefficients[i + 1] = int8_t(clamped_scaled.y);
  packed_spherical_harmonics.coefficients[i + 2] = int8_t(clamped_scaled.z);
}

/* Get unquantized spherical harmonics coefficient by its flat index. */
ccl_device_forceinline float3 spherical_harmonics_get_coefficient(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics, const int index)
{
  util_assert(index >= 0);
  util_assert(index < PackedSphericalHarmonics::MAX_COEFFICIENTS);
  const int i = index * 3;
  return make_float3(packed_spherical_harmonics.coefficients[i + 0],
                     packed_spherical_harmonics.coefficients[i + 1],
                     packed_spherical_harmonics.coefficients[i + 2]) /
         127.0f;
}

/* Get unquantized spherical harmonics coefficient by the band L and the basis function M. */
template<int L, int M>
ccl_device_inline float3
spherical_harmonics_get(const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics);

/* Band L=1. */
ccl_device_template_spec float3 spherical_harmonics_get<1, -1>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 0);
}
ccl_device_template_spec float3 spherical_harmonics_get<1, 0>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 1);
}
ccl_device_template_spec float3 spherical_harmonics_get<1, 1>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 2);
}

/* Band L=2. */
ccl_device_template_spec float3 spherical_harmonics_get<2, -2>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 3);
}
ccl_device_template_spec float3 spherical_harmonics_get<2, -1>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 4);
}
ccl_device_template_spec float3 spherical_harmonics_get<2, 0>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 5);
}
ccl_device_template_spec float3 spherical_harmonics_get<2, 1>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 6);
}
ccl_device_template_spec float3 spherical_harmonics_get<2, 2>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 7);
}

/* Band L=3. */
ccl_device_template_spec float3 spherical_harmonics_get<3, -3>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 8);
}
ccl_device_template_spec float3 spherical_harmonics_get<3, -2>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 9);
}
ccl_device_template_spec float3 spherical_harmonics_get<3, -1>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 10);
}
ccl_device_template_spec float3 spherical_harmonics_get<3, 0>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 11);
}
ccl_device_template_spec float3 spherical_harmonics_get<3, 1>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 12);
}
ccl_device_template_spec float3 spherical_harmonics_get<3, 2>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 13);
}
ccl_device_template_spec float3 spherical_harmonics_get<3, 3>(
    const ccl_global PackedSphericalHarmonics &packed_spherical_harmonics)
{
  return spherical_harmonics_get_coefficient(packed_spherical_harmonics, 14);
}

CCL_NAMESPACE_END
