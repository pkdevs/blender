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
  /* Push the pipeline's fixed-function state into the StateManager so MTL
   * can read it when building its PSO / depth-stencil / RPS. */
  MTLStateManager &sm = static_cast<MTLStateManager &>(*ctx->state_manager);
  sm.state = this->state_;
  sm.mutable_state = this->mutable_state_;
  sm.apply_state();
  clear_dirty();
}

void MTLPipeline::resolve_compute()
{
  sync_context();
  clear_dirty();
}

void MTLPipeline::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  MTLContext::get()->state_manager->texture_bind(tex, sampler, unit);
}

void MTLPipeline::texture_unbind(Texture *tex)
{
  MTLContext::get()->state_manager->texture_unbind(tex);
}

void MTLPipeline::texture_unbind_all()
{
  MTLContext::get()->state_manager->texture_unbind_all();
}

void MTLPipeline::image_bind(Texture *tex, int unit)
{
  MTLContext::get()->state_manager->image_bind(tex, unit);
}

void MTLPipeline::image_unbind(Texture *tex)
{
  MTLContext::get()->state_manager->image_unbind(tex);
}

void MTLPipeline::image_unbind_all()
{
  MTLContext::get()->state_manager->image_unbind_all();
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
