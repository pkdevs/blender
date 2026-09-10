/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_pipeline.hh"

#include "gpu_context_private.hh"
#include "vk_context.hh"
#include "vk_index_buffer.hh"
#include "vk_ray_tracing.hh"
#include "vk_storage_buffer.hh"
#include "vk_texture.hh"
#include "vk_uniform_buffer.hh"
#include "vk_vertex_buffer.hh"

namespace blender::gpu {

VKPipeline *vk_pipeline_get()
{
  return static_cast<VKPipeline *>(Context::get()->pipeline_get());
}

void VKPipeline::resolve_draw(GPUPrimType /*prim_type*/, int64_t /*vertex_input_key*/)
{
  sync_context();
  /* Vulkan bakes fixed-function state into the VkPipeline at render-graph
   * creation; `update_pipeline_data` / `VKShader::ensure_and_get_graphics_pipeline`
   * read the state directly from the pipeline (`pipeline_get()->state_get()`).
   * The old "mirror into VKStateManager then read it back" is gone. */
  clear_dirty();
}

void VKPipeline::resolve_compute()
{
  sync_context();
  clear_dirty();
}

void VKPipeline::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  textures_.bind(BindSpaceTextures::Type::Texture, tex, sampler, unit);
  descriptor_dirty_ = true;
}

void VKPipeline::texture_unbind(Texture *tex)
{
  textures_.unbind(tex);
  descriptor_dirty_ = true;
}

void VKPipeline::texture_unbind_all()
{
  textures_.unbind_all();
  descriptor_dirty_ = true;
}

void VKPipeline::image_bind(Texture *tex, int unit)
{
  images_.bind(unwrap(tex), unit);
  descriptor_dirty_ = true;
}

void VKPipeline::image_unbind(Texture *tex)
{
  images_.unbind(unwrap(tex));
  descriptor_dirty_ = true;
}

void VKPipeline::image_unbind_all()
{
  images_.unbind_all();
  descriptor_dirty_ = true;
}

void VKPipeline::uniformbuf_bind(UniformBuf *ubo, int slot)
{
  uniform_buffer_bind(static_cast<VKUniformBuffer *>(ubo), slot);
}

void VKPipeline::uniformbuf_unbind(UniformBuf *ubo)
{
  uniform_buffer_unbind(static_cast<VKUniformBuffer *>(ubo));
}

void VKPipeline::storagebuf_bind(StorageBuf *ssbo, int slot)
{
  storage_buffer_bind(BindSpaceStorageBuffers::Type::StorageBuffer, ssbo, slot);
}

void VKPipeline::storagebuf_unbind(StorageBuf *ssbo)
{
  storage_buffer_unbind(ssbo);
}

void VKPipeline::tlas_bind(TopLevelAS *tlas, int slot)
{
  toplevelas_bind(static_cast<VKTopLevelAS *>(tlas), slot);
}

void VKPipeline::tlas_unbind(TopLevelAS *tlas)
{
  toplevelas_unbind(tlas);
}

void VKPipeline::texel_buffer_bind(VKVertexBuffer &vertex_buffer, int binding)
{
  textures_.bind(BindSpaceTextures::Type::VertexBuffer,
                 &vertex_buffer,
                 GPUSamplerState::default_sampler(),
                 binding);
  descriptor_dirty_ = true;
}

void VKPipeline::texel_buffer_unbind(VKVertexBuffer &vertex_buffer)
{
  textures_.unbind(&vertex_buffer);
  descriptor_dirty_ = true;
}

void VKPipeline::uniform_buffer_bind(VKUniformBuffer *uniform_buffer, int binding)
{
  uniform_buffers_.bind(uniform_buffer, binding);
  descriptor_dirty_ = true;
}

void VKPipeline::uniform_buffer_unbind(VKUniformBuffer *uniform_buffer)
{
  uniform_buffers_.unbind(uniform_buffer);
  descriptor_dirty_ = true;
}

void VKPipeline::uniform_buffer_unbind_all()
{
  uniform_buffers_.unbind_all();
  descriptor_dirty_ = true;
}

void VKPipeline::storage_buffer_bind(BindSpaceStorageBuffers::Type resource_type,
                                     void *resource,
                                     int binding,
                                     VkDeviceSize offset)
{
  storage_buffers_.bind(resource_type, resource, binding, offset);
  descriptor_dirty_ = true;
}

void VKPipeline::storage_buffer_unbind(void *resource)
{
  storage_buffers_.unbind(resource);
  descriptor_dirty_ = true;
}

void VKPipeline::storage_buffer_unbind_all()
{
  storage_buffers_.unbind_all();
  descriptor_dirty_ = true;
}

void VKPipeline::toplevelas_bind(VKTopLevelAS *tlas, int slot)
{
  acceleration_structures_.bind(tlas, slot);
  descriptor_dirty_ = true;
}

void VKPipeline::toplevelas_unbind(void *resource)
{
  acceleration_structures_.unbind(resource);
  descriptor_dirty_ = true;
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
