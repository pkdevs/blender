/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "mtl_pipeline.hh"

#include "gpu_context_private.hh"
#include "mtl_context.hh"
#include "mtl_state.hh"

namespace blender::gpu {

void MTLPipeline::prepare_draw(GPUPrimType /*prim_type*/, int64_t /*vertex_input_key*/)
{
  sync_context();
  sync_state_manager();
  /* Metal state is baked into the MTLRenderPipelineState; the context's
   * ensure_render_pipeline_state polls the StateManager at (re)creation time.
   * apply_state() on Metal only sets encoder-level (mutable) state. */
  Context *ctx = Context::get();
  ctx->state_manager->apply_state();
  clear_dirty();
}

void MTLPipeline::prepare_compute()
{
  sync_context();
  sync_state_manager();
  Context *ctx = Context::get();
  ctx->state_manager->apply_state();
  clear_dirty();
}

}  // namespace blender::gpu
