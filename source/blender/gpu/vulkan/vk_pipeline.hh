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
 * Vulkan pipeline. Vulkan bakes state into VkPipeline (created by the render
 * graph when the draw is recorded). The pipeline's state is staged into the
 * StateManager, from which the render graph polls it during pipeline creation;
 * `apply_state()` is intentionally a no-op on this backend.
 */
class VKPipeline : public Pipeline {
 protected:
  /* Vulkan bakes state into VkPipeline (created by the render graph when the
   * draw is recorded). `resolve_draw` stages the pipeline's state into the
   * StateManager, from which the render graph polls it during pipeline
   * creation; `apply_state()` is a no-op here.
   */
  void resolve_draw(GPUPrimType prim_type, int64_t vertex_input_key) override;
  void resolve_compute() override;
  void issue_barrier(GPUBarrier barrier_bits) override;
};

}  // namespace blender::gpu
