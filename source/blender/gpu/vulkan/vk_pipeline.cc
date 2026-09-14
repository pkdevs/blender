/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_pipeline.hh"

#include <cstring>

#include "gpu_context_private.hh"
#include "vk_context.hh"
#include "vk_framebuffer.hh"
#include "vk_index_buffer.hh"
#include "vk_ray_tracing.hh"
#include "vk_shader.hh"
#include "vk_storage_buffer.hh"
#include "vk_texture.hh"
#include "vk_uniform_buffer.hh"
#include "vk_vertex_buffer.hh"

namespace blender::gpu {

VKPipeline *vk_pipeline_get()
{
  return static_cast<VKPipeline *>(Context::get()->pipeline_get());
}

void VKPipeline::resolve_draw(GPUPrimType prim_type, int64_t vertex_input_key)
{
  sync_context();
  /* Resolve the VkPipeline once into `resolved_vk_pipeline_`; subsequent draws
   * reuse it (see `update_pipeline_data`) until this pipeline is re-tagged
   * dirty by a shader / state / specialization-constants / framebuffer / prim /
   * vertex-input change.
   *
   * Defensive: `Pipeline::apply_state` (base, non-virtual) unconditionally
   * calls `resolve_draw` on legacy / state-only contexts where there is no
   * active framebuffer, no active shader, and no valid prim type. The actual
   * steady-state resolve happens in `update_pipeline_data` after the tags are
   * set and the pipeline is dirty, so we early-return here if any of the
   * resolve preconditions are missing. */
  VKContext *context = VKContext::get();
  if (context == nullptr) {
    return;
  }
  const VKFrameBuffer *framebuffer = context->active_framebuffer_get();
  Shader *shader = shader_ ? shader_ : context->shader;
  if (framebuffer == nullptr || shader == nullptr) {
    /* No real draw is about to happen. `apply_state` / debug / state-only
     * callers still expect us to clear the flag so future draws can
     * proceed normally. */
    clear_dirty();
    return;
  }
  VKShader &vk_shader = unwrap(*shader);

  GPUState state = state_get();
  state.clip_control = 0; /* not a vulkan flag; avoids needless recompilations. */

  resolved_vk_pipeline_ = vk_shader.ensure_and_get_graphics_pipeline(
      prim_type, vertex_input_key, state, *framebuffer, context->constants_state_);
  clear_dirty();
}

void VKPipeline::tag_framebuffer_state(VkFormat depth_format,
                                       VkFormat stencil_format,
                                       Span<VkFormat> color_formats,
                                       uint32_t viewport_count)
{
  const bool changed = (tagged_depth_format_ != depth_format) ||
                       (tagged_stencil_format_ != stencil_format) ||
                       (tagged_viewport_count_ != viewport_count) ||
                       (tagged_color_count_ != color_formats.size()) ||
                       (std::memcmp(tagged_color_formats_,
                                    color_formats.data(),
                                    (size_t)color_formats.size() * sizeof(VkFormat)) != 0);
  if (changed) {
    tagged_depth_format_ = depth_format;
    tagged_stencil_format_ = stencil_format;
    tagged_viewport_count_ = viewport_count;
    tagged_color_count_ = color_formats.size();
    std::memcpy(tagged_color_formats_,
                color_formats.data(),
                (size_t)color_formats.size() * sizeof(VkFormat));
    mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
  }
}

void VKPipeline::tag_vertex_input_key(int64_t key)
{
  if (tagged_vertex_input_key_ != key) {
    tagged_vertex_input_key_ = key;
    mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
  }
}

void VKPipeline::resolve_compute()
{
  sync_context();
  clear_dirty();
}

void VKPipeline::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  const BindSpaceTextures::Elem *elem = textures_.get(unit);
  if (elem && elem->resource_type == BindSpaceTextures::Type::Texture && elem->resource == tex &&
      elem->sampler == sampler)
  {
    /* Same texture + sampler already bound at this unit: no descriptor change. */
    return;
  }
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
  if (images_.get_or_null(unit) == unwrap(tex)) {
    /* Same image already bound at this unit: no descriptor change. */
    return;
  }
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
  const BindSpaceTextures::Elem *elem = textures_.get(binding);
  if (elem && elem->resource_type == BindSpaceTextures::Type::VertexBuffer &&
      elem->resource == &vertex_buffer)
  {
    /* Same texel buffer already bound at this slot: no descriptor change. */
    return;
  }
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
  if (uniform_buffers_.get_or_null(binding) == uniform_buffer) {
    /* Same uniform buffer already bound at this slot: no descriptor change. */
    return;
  }
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
  const BindSpaceStorageBuffers::Elem *elem = storage_buffers_.get_or_null(binding);
  if (elem != nullptr && elem->resource_type == resource_type && elem->resource == resource &&
      elem->offset == offset)
  {
    /* Same storage buffer already bound at this slot: no descriptor change. */
    return;
  }
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
  if (acceleration_structures_.get_or_null(slot) == tlas) {
    /* Same acceleration structure already bound at this slot: no descriptor change. */
    return;
  }
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
