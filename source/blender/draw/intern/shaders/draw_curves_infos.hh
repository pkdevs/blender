/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_attribute_shader_shared.hh"
#  include "draw_object_infos_infos.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define DRW_HAIR_INFO
#endif

#include "draw_curves_defines.hh"

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(draw_curves_test)
STORAGE_BUF(0, write, float, result_pos_buf[])
STORAGE_BUF(1, write, int4, result_indices_buf[])
VERTEX_SOURCE("draw_curves_test.glsl")
FRAGMENT_SOURCE("draw_curves_test.glsl")
ADDITIONAL_INFO(draw_curves_infos)
ADDITIONAL_INFO(draw_curves)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
