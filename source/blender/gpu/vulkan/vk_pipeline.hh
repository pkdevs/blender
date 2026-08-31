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
 public:
  void prepare_draw(GPUPrimType prim_type, int64_t vertex_input_key = 0) override;
  void prepare_compute() override;
};

}  // namespace blender::gpu
