/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gl_pipeline.hh"

#include "gpu_context_private.hh"
#include "gl_context.hh"
#include "gl_state.hh"

namespace blender::gpu {

void GLPipeline::prepare_draw(GPUPrimType /*prim_type*/, int64_t /*vertex_input_key*/)
{
  sync_context();
  sync_state_manager();
  /* OpenGL state is dynamic: apply it imperatively every time the pipeline is
   * bound to a draw. `apply_state()` is cheap since the state manager diffs
   * against the current GL state internally. */
  Context *ctx = Context::get();
  ctx->state_manager->apply_state();
  clear_dirty();
}

void GLPipeline::prepare_compute()
{
  sync_context();
  sync_state_manager();
  Context *ctx = Context::get();
  ctx->state_manager->apply_state();
  clear_dirty();
}

}  // namespace blender::gpu
