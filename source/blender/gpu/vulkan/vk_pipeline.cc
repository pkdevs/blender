/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_pipeline.hh"

#include "gpu_context_private.hh"
#include "vk_context.hh"
#include "vk_state_manager.hh"

namespace blender::gpu {

void VKPipeline::prepare_draw(GPUPrimType /*prim_type*/, int64_t /*vertex_input_key*/)
{
  sync_context();
  sync_state_manager();
  /* Vulkan bakes state into VkPipeline; the render graph polls the
   * StateManager when creating the pipeline for this draw. No command stream
   * work to do here (VKStateManager::apply_state is intentionally empty). */
  clear_dirty();
}

void VKPipeline::prepare_compute()
{
  sync_context();
  sync_state_manager();
  clear_dirty();
}

}  // namespace blender::gpu
