/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_dual_kawase_downsample)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float2, uv_step)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_dual_kawase_downsample.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_dual_kawase_upsample)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float2, uv_step)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_dual_kawase_upsample.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_dual_kawase_mix)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float, input_weight)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16_16_16_16, read_write, image2D, output_img)
COMPUTE_SOURCE("compositor_dual_kawase_mix.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
