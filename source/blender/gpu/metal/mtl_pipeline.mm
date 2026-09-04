/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "mtl_pipeline.hh"

#include "gpu_context_private.hh"
#include "mtl_command_buffer.hh"
#include "mtl_context.hh"
#include "mtl_state.hh"

namespace blender::gpu {

void MTLPipeline::resolve_draw(GPUPrimType /*prim_type*/, int64_t /*vertex_input_key*/)
{
  Context *ctx = Context::get();
  sync_context();
  sync_state_manager();
  ctx->state_manager->apply_state();
  clear_dirty();
}

void MTLPipeline::resolve_compute()
{
  sync_context();
  clear_dirty();
}

void MTLPipeline::issue_barrier(GPUBarrier barrier_bits)
{
  /* NOTE(Metal): The Metal API implicitly tracks dependencies between resources.
   * Memory barriers and execution barriers (Fences/Events) can be used to coordinate
   * this explicitly, however, in most cases, the driver will be able to
   * resolve these dependencies automatically.
   * For untracked resources, such as MTLHeap's, explicit barriers are necessary. */
  GPUStageBarrierBits before_stages = GPU_BARRIER_STAGE_ANY;
  GPUStageBarrierBits after_stages = GPU_BARRIER_STAGE_ANY;

  MTLContext *mtl_ctx = MTLContext::get();
  BLI_assert(mtl_ctx);

  mtl_ctx->main_command_buffer.insert_memory_barrier(
      barrier_bits, before_stages, after_stages);
}

}  // namespace blender::gpu
