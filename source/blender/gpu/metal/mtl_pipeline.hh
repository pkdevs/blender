/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_pipeline.hh"

namespace blender::gpu {

/**
 * Metal pipeline. Metal is a semi-dynamic API: depth/stencil/blend state is
 * baked into the MTLRenderPipelineState created by `MTLContext::ensure_render_pipeline_state`.
 * `prepare_draw` stages the pipeline's state into the StateManager (same as
 * OpenGL) so it can be polled when the render pipeline state is (re)created.
 */
class MTLPipeline : public Pipeline {
 protected:
  /* Metal bakes state into MTLRenderPipelineState / depth-stencil, which the
   * context re-creates from the StateManager. `resolve_draw` stages the
   * pipeline's state there, mirroring the OpenGL path.
   */
  void resolve_draw(GPUPrimType prim_type, int64_t vertex_input_key) override;
  void resolve_compute() override;
  void issue_barrier(GPUBarrier barrier_bits) override;
};

}  // namespace blender::gpu
