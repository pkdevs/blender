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
 * OpenGL pipeline. OpenGL is a dynamic-API: fixed-function state is applied
 * imperatively through the GL state manager, so `prepare_draw` pushes the
 * pipeline's state into the StateManager and forces `apply_state()`.
 */
class GLPipeline : public Pipeline {
 public:
  void prepare_draw(GPUPrimType prim_type, int64_t vertex_input_key = 0) override;
  void prepare_compute() override;
};

}  // namespace blender::gpu
