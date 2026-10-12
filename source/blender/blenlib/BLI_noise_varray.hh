/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * This contains various utilities to evaluate noise functions on many elements efficiently.
 */

#include "BLI_color_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_virtual_array.hh"

namespace blender::noise {

void hash_float_to_float(const IndexMask &mask,
                         const VArray<float> &ks,
                         MutableSpan<float> r_values);
void hash_float_to_float(const IndexMask &mask,
                         const VArray<float3> &ks,
                         MutableSpan<float> r_values);
void hash_float_to_color(const IndexMask &mask,
                         const VArray<float> &ks,
                         MutableSpan<ColorGeometry4f> r_values);
void hash_float_to_color(const IndexMask &mask,
                         const VArray<float3> &ks,
                         MutableSpan<ColorGeometry4f> r_values);

/**
 * NOTE: Currently the float2 are passed in as float3, but only the x and y components are used.
 */
void hash_float2_to_float(const IndexMask &mask,
                          const VArray<float3> &ks,
                          MutableSpan<float> r_values);
void hash_float2_to_color(const IndexMask &mask,
                          const VArray<float3> &ks,
                          MutableSpan<ColorGeometry4f> r_values);

/**
 * NOTE: Currently the float4 is passed in as float3 + float.
 */
void hash_float4_to_float(const IndexMask &mask,
                          const VArray<float3> &vectors,
                          const VArray<float> &ws,
                          MutableSpan<float> r_values);
void hash_float4_to_color(const IndexMask &mask,
                          const VArray<float3> &vectors,
                          const VArray<float> &ws,
                          MutableSpan<ColorGeometry4f> r_values);

}  // namespace blender::noise
