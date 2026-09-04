/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#ifndef GPU_STANDALONE
#  include "DNA_userdef_types.h"
#  define PIXELSIZE (U.pixelsize)
#else
#  define PIXELSIZE (1.0f)
#endif

#include "BLI_math_vector_c.hh"
#include "BLI_utildefines.hh"

#include "GPU_state.hh"

#include "gpu_backend.hh"
#include "gpu_context_private.hh"

#include "gpu_state_private.hh"

namespace blender {

using namespace blender::gpu;

/* -------------------------------------------------------------------- */
/** \name Immutable state Setters
 * \{ */

void GPU_blend(GPUBlend blend)
{
  Context::get()->pipeline_get()->set_blend(blend);
}

void GPU_face_culling(GPUFaceCullTest culling)
{
  Context::get()->pipeline_get()->set_face_culling(culling);
}

GPUFaceCullTest GPU_face_culling_get()
{
  const GPUState &state = Context::get()->pipeline_get()->state_get();
  return GPUFaceCullTest(state.culling_test);
}

void GPU_front_facing(bool invert)
{
  Context::get()->pipeline_get()->set_front_facing(invert);
}

void GPU_provoking_vertex(GPUProvokingVertex vert)
{
  Context::get()->pipeline_get()->set_provoking_vertex(vert);
}

void GPU_depth_test(GPUDepthTest test)
{
  Context::get()->pipeline_get()->set_depth_test(test);
}

void GPU_stencil_test(GPUStencilTest test)
{
  Context::get()->pipeline_get()->set_stencil_test(test);
}

void GPU_line_smooth(bool enable)
{
  Context::get()->pipeline_get()->set_line_smooth(enable);
}

void GPU_polygon_smooth(bool enable)
{
  Context::get()->pipeline_get()->set_polygon_smooth(enable);
}

void GPU_write_mask(GPUWriteMask mask)
{
  Context::get()->pipeline_get()->set_write_mask(mask);
}

void GPU_color_mask(bool r, bool g, bool b, bool a)
{
  Pipeline *pl = Context::get()->pipeline_get();
  uint32_t write_mask = uint32_t(pl->write_mask_get());
  SET_FLAG_FROM_TEST(write_mask, r, uint32_t(GPU_WRITE_RED));
  SET_FLAG_FROM_TEST(write_mask, g, uint32_t(GPU_WRITE_GREEN));
  SET_FLAG_FROM_TEST(write_mask, b, uint32_t(GPU_WRITE_BLUE));
  SET_FLAG_FROM_TEST(write_mask, a, uint32_t(GPU_WRITE_ALPHA));
  pl->set_write_mask(GPUWriteMask(write_mask));
}

void GPU_depth_mask(bool depth)
{
  Pipeline *pl = Context::get()->pipeline_get();
  uint32_t write_mask = uint32_t(pl->write_mask_get());
  SET_FLAG_FROM_TEST(write_mask, depth, uint32_t(GPU_WRITE_DEPTH));
  pl->set_write_mask(GPUWriteMask(write_mask));
}

void GPU_clip_distances(int distances_enabled)
{
  Context::get()->pipeline_get()->set_clip_distances(distances_enabled);
}

void GPU_state_set(GPUWriteMask write_mask,
                   GPUBlend blend,
                   GPUFaceCullTest culling_test,
                   GPUDepthTest depth_test,
                   GPUStencilTest stencil_test,
                   GPUStencilOp stencil_op,
                   GPUProvokingVertex provoking_vert)
{
  Context::get()->pipeline_get()->state_set(
      write_mask, blend, culling_test, depth_test, stencil_test, stencil_op, provoking_vert);
}

void GPU_clip_control_unit_range(bool enable)
{
  Context::get()->pipeline_get()->set_clip_control(enable);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mutable State Setters
 * \{ */

void GPU_line_width(float width)
{
  width = max_ff(1.0f, width * PIXELSIZE);
  Context::get()->pipeline_get()->set_line_width(width);
}

void GPU_point_size(float size)
{
  Pipeline *pl = Context::get()->pipeline_get();
  float current = pl->mutable_state_get().point_size;
  /* Keep the sign of point_size since it represents the enable state. */
  pl->set_point_size(size * ((current > 0.0) ? 1.0f : -1.0f));
}

void GPU_program_point_size(bool enable)
{
  Pipeline *pl = Context::get()->pipeline_get();
  float current = pl->mutable_state_get().point_size;
  /* Set point size sign negative to disable. */
  pl->set_point_size(fabsf(current) * (enable ? 1 : -1));
}

void GPU_scissor_test(bool enable)
{
  Context::get()->active_fb->scissor_test_set(enable);
}

void GPU_scissor(int x, int y, int width, int height)
{
  int scissor_rect[4] = {x, y, width, height};
  Context::get()->active_fb->scissor_set(scissor_rect);
}

void GPU_viewport(int x, int y, int width, int height)
{
  int viewport_rect[4] = {x, y, width, height};
  Context::get()->active_fb->viewport_set(viewport_rect);
}

void GPU_stencil_reference_set(uint reference)
{
  Context::get()->pipeline_get()->set_stencil_reference(uint8_t(reference));
}

void GPU_stencil_write_mask_set(uint write_mask)
{
  Context::get()->pipeline_get()->set_stencil_write_mask(uint8_t(write_mask));
}

void GPU_stencil_compare_mask_set(uint compare_mask)
{
  Context::get()->pipeline_get()->set_stencil_compare_mask(uint8_t(compare_mask));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name State Getters
 * \{ */

GPUBlend GPU_blend_get()
{
  return Context::get()->pipeline_get()->blend_get();
}

GPUWriteMask GPU_write_mask_get()
{
  return Context::get()->pipeline_get()->write_mask_get();
}

uint GPU_stencil_mask_get()
{
  const GPUStateMutable &state = Context::get()->pipeline_get()->mutable_state_get();
  return state.stencil_write_mask;
}

GPUDepthTest GPU_depth_test_get()
{
  const GPUState &state = Context::get()->pipeline_get()->state_get();
  return GPUDepthTest(state.depth_test);
}

GPUStencilTest GPU_stencil_test_get()
{
  const GPUState &state = Context::get()->pipeline_get()->state_get();
  return GPUStencilTest(state.stencil_test);
}

float GPU_line_width_get()
{
  const GPUStateMutable &state = Context::get()->pipeline_get()->mutable_state_get();
  return state.line_width;
}

bool GPU_line_smooth_get()
{
  const GPUState &state = Context::get()->pipeline_get()->state_get();
  return bool(state.line_smooth);
}

void GPU_scissor_get(int coords[4])
{
  Context::get()->active_fb->scissor_get(coords);
}

void GPU_viewport_size_get_f(float coords[4])
{
  int viewport[4];
  Context::get()->active_fb->viewport_get(viewport);
  for (int i = 0; i < 4; i++) {
    coords[i] = viewport[i];
  }
}

void GPU_viewport_size_get_i(int coords[4])
{
  Context::get()->active_fb->viewport_get(coords);
}

bool GPU_depth_mask_get()
{
  return (Context::get()->pipeline_get()->write_mask_get() & GPU_WRITE_DEPTH) != GPU_WRITE_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context Utils
 * \{ */

void GPU_flush()
{
  Context::get()->flush();
}

void GPU_finish()
{
  Context::get()->finish();
}

void GPU_apply_state()
{
  Context::get()->pipeline_get()->apply_state();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Synchronization Utils
 * \{ */

void GPU_memory_barrier(GPUBarrier barrier)
{
  Context::get()->pipeline_get()->issue_barrier(barrier);
}

GPUFence *GPU_fence_create()
{
  Fence *fence = GPUBackend::get()->fence_alloc();
  return wrap(fence);
}

void GPU_fence_free(GPUFence *fence)
{
  delete unwrap(fence);
}

void GPU_fence_signal(GPUFence *fence)
{
  unwrap(fence)->signal();
}

void GPU_fence_wait(GPUFence *fence)
{
  unwrap(fence)->wait();
}

/** \} */

}  // namespace blender
