/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define SAMPLER_FUNCTION texture
#endif

#include "gpu_shader_create_info.hh"

/* -------
 * Common.
 * ------- */

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_shared)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float4x4, transformation)
PUSH_CONSTANT(float2, wh)
COMPUTE_SOURCE("compositor_realize_on_domain.glsl")
GPU_SHADER_CREATE_END()

/* -----------------------
 * Float Nearest/Bilinear.
 * ----------------------- */

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_float4)
ADDITIONAL_INFO(compositor_realize_on_domain_shared)
SAMPLER(0, sampler2D, input_tx)
COMPUTE_FUNCTION("realize_on_domain_texture")
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* --------
 * Float4x4
 * -------- */

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_float4x4)
ADDITIONAL_INFO(compositor_realize_on_domain_shared)
SAMPLER(0, sampler2DArray, input_tx)
COMPUTE_FUNCTION("realize_on_domain_float4x4")
IMAGE(0, SFLOAT_16_16_16_16, write, image2DArray, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* float4 Anisotropic */

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_anisotropic_float4)
ADDITIONAL_INFO(compositor_realize_on_domain_shared)
SAMPLER(0, sampler2D, input_tx)
COMPUTE_FUNCTION("realize_on_domain_anisotropic")
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* ----
 * Int.
 * ---- */

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_int_shared)
ADDITIONAL_INFO(compositor_realize_on_domain_shared)
SAMPLER(0, isampler2D, input_tx)
COMPUTE_FUNCTION("realize_on_domain_texture")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_int)
ADDITIONAL_INFO(compositor_realize_on_domain_int_shared)
IMAGE(0, SINT_16, write, iimage2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_int2)
ADDITIONAL_INFO(compositor_realize_on_domain_int_shared)
IMAGE(0, SINT_16_16, write, iimage2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_int4)
ADDITIONAL_INFO(compositor_realize_on_domain_int_shared)
IMAGE(0, SINT_16_16_16_16, write, iimage2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_sint8)
ADDITIONAL_INFO(compositor_realize_on_domain_int_shared)
IMAGE(0, SINT_8, write, iimage2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* --------------------------------
 * Rectangular, uv and wh in texels
 * -------------------------------- */

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_wh_shared)
ADDITIONAL_INFO(compositor_realize_on_domain_shared)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, domain_img)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_box_float4)
ADDITIONAL_INFO(compositor_realize_on_domain_wh_shared)
COMPUTE_FUNCTION("realize_on_domainTSampler_Box")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_bspline_float4)
ADDITIONAL_INFO(compositor_realize_on_domain_wh_shared)
COMPUTE_FUNCTION("realize_on_domainTSampler_Bspline")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
