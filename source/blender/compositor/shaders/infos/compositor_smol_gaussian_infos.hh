/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_smol_gaussian_downsample)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int4, source_bounds)
PUSH_CONSTANT(int4, destination_bounds)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_smol_gaussian_downsample.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_smol_gaussian_filter)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float2, uv_step)
PUSH_CONSTANT(float, center_weight)
PUSH_CONSTANT(int, tap_count)
PUSH_CONSTANT_ARRAY(float4, taps, 12)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_smol_gaussian_filter.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_smol_gaussian_reconstruct)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float2, weights)
PUSH_CONSTANT(int4, base_bounds)
PUSH_CONSTANT(int4, neighbor_bounds)
PUSH_CONSTANT(int4, diagonal_bounds)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, neighbor_tx)
SAMPLER(2, sampler2D, diagonal_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_smol_gaussian_reconstruct.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
