/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_pipeline.hh"

#include "gpu_context_private.hh"
#include "vk_context.hh"
#include "vk_state_manager.hh"

namespace blender::gpu {

void VKPipeline::resolve_draw(GPUPrimType /*prim_type*/, int64_t /*vertex_input_key*/)
{
  sync_context();
  sync_state_manager();
  clear_dirty();
}

void VKPipeline::resolve_compute()
{
  sync_context();
  clear_dirty();
}

void VKPipeline::issue_barrier(GPUBarrier barrier_bits)
{
  /**
   * Workaround for EEVEE ThicknessFromShadow shader.
   *
   * EEVEE light evaluation uses layered sub-pass tracking. Currently, the tracking supports
   * transitioning a layer to a different layout once per rendering scope. When using the
   * thickness from shadow, the layers need to be transitioned twice: once to image load/store
   * for the thickness from shadow shader and then to a sampler for the light evaluation shader.
   * We work around this limitation by suspending the rendering.
   *
   * The reason we need to suspend the rendering is that Vulkan, by default, doesn't support layout
   * transitions between the begin and end of rendering. By suspending the render, the graph will
   * create a new node group that allows the necessary image layout transition.
   *
   * This limitation could also be addressed in the render graph scheduler, but that would be quite
   * a hassle to track and might not be worth the effort.
   */
  if (bool(barrier_bits & GPU_BARRIER_SHADER_IMAGE_ACCESS)) {
    VKContext &context = *VKContext::get();
    context.rendering_end();
  }
}

}  // namespace blender::gpu
