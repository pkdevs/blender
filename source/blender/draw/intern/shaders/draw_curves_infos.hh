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

GPU_SHADER_CREATE_INFO(draw_curves_topology)
LOCAL_GROUP_SIZE(CURVES_PER_THREADGROUP)
/* Offsets giving the start and end of the curve. */
STORAGE_BUF(0, read, int, evaluated_offsets_buf[])
STORAGE_BUF(1, read, uint, curves_cyclic_buf[]) /* Actually bool (1 byte). */
STORAGE_BUF(2, write, int, indirection_buf[])
PUSH_CONSTANT(int, curves_start)
PUSH_CONSTANT(int, curves_count)
PUSH_CONSTANT(bool, is_ribbon_topology)
PUSH_CONSTANT(bool, use_cyclic)
COMPUTE_SOURCE("draw_curves_topology_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_test)
STORAGE_BUF(0, write, float, result_pos_buf[])
STORAGE_BUF(1, write, int4, result_indices_buf[])
VERTEX_SOURCE("draw_curves_test.glsl")
FRAGMENT_SOURCE("draw_curves_test.glsl")
ADDITIONAL_INFO(draw_curves_infos)
ADDITIONAL_INFO(draw_curves)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
