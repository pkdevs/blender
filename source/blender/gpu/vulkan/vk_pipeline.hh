/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_pipeline.hh"

#include "BLI_array.hh"

#include "vk_common.hh"

namespace blender::gpu {
class VKContext;
class VKTexture;
class VKUniformBuffer;
class VKVertexBuffer;
class VKStorageBuffer;
class VKIndexBuffer;
class VKTopLevelAS;
class VKBuffer;

/**
 * Offset when searching for bindings.
 *
 * When shaders combine images and samplers, the images have to be offset to find the correct
 * shader input. Both textures and images are stored in the uniform list and their ID can be
 * overlapping.
 */
static constexpr int BIND_SPACE_IMAGE_OFFSET = 512;

/** Bind space for a uniform buffers. */
class BindSpaceUniformBuffers {
 public:
  Vector<VKUniformBuffer *> bound_resources;

  void bind(VKUniformBuffer *resource, int binding)
  {
    if (bound_resources.size() <= binding) {
      bound_resources.resize(binding + 1);
    }
    bound_resources[binding] = resource;
  }

  VKUniformBuffer *get(int binding) const
  {
    return bound_resources[binding];
  }

  VKUniformBuffer *get_or_null(int binding) const
  {
    if (binding >= (int)bound_resources.size()) {
      return nullptr;
    }
    return bound_resources[binding];
  }

  void unbind(void *resource)
  {
    for (int index : IndexRange(bound_resources.size())) {
      if (bound_resources[index] == resource) {
        bound_resources[index] = nullptr;
      }
    }
  }

  void unbind_all()
  {
    bound_resources.clear();
  }
};

/**
 * Bind space for image resources.
 */
template<int Offset> class BindSpaceImages {
 public:
  Vector<VKTexture *> bound_resources;

  void bind(VKTexture *resource, int binding)
  {
    if (binding >= Offset) {
      binding -= Offset;
    }
    if (bound_resources.size() <= binding) {
      bound_resources.resize(binding + 1);
    }
    bound_resources[binding] = resource;
  }

  VKTexture *get(int binding) const
  {
    if (binding >= Offset) {
      binding -= Offset;
    }
    return bound_resources[binding];
  }

  VKTexture *get_or_null(int binding) const
  {
    if (binding >= Offset) {
      binding -= Offset;
    }
    if (binding >= (int)bound_resources.size()) {
      return nullptr;
    }
    return bound_resources[binding];
  }

  void unbind(void *resource)
  {
    for (int index : IndexRange(bound_resources.size())) {
      if (bound_resources[index] == resource) {
        bound_resources[index] = nullptr;
      }
    }
  }

  void unbind_all()
  {
    bound_resources.clear();
  }
};

/** Bind space for storage buffers. */
class BindSpaceStorageBuffers {
 public:
  enum class Type {
    Unused,
    UniformBuffer,
    VertexBuffer,
    IndexBuffer,
    StorageBuffer,
    Buffer,
  };
  struct Elem {
    Type resource_type;
    void *resource;
    VkDeviceSize offset;
  };
  Vector<Elem> bound_resources;

  void bind(Type resource_type, void *resource, int binding, VkDeviceSize offset)
  {
    if (bound_resources.size() <= binding) {
      bound_resources.resize(binding + 1);
    }
    bound_resources[binding].resource_type = resource_type;
    bound_resources[binding].resource = resource;
    bound_resources[binding].offset = offset;
  }

  const Elem &get(int binding) const
  {
    return bound_resources[binding];
  }

  const Elem *get_or_null(int binding) const
  {
    if (binding >= (int)bound_resources.size()) {
      return nullptr;
    }
    return &bound_resources[binding];
  }

  void unbind(void *resource)
  {
    for (int index : IndexRange(bound_resources.size())) {
      if (bound_resources[index].resource == resource) {
        bound_resources[index].resource = nullptr;
        bound_resources[index].resource_type = Type::Unused;
        bound_resources[index].offset = 0u;
      }
    }
  }

  void unbind_all()
  {
    bound_resources.clear();
  }
};

/** Bind space for textures. */
class BindSpaceTextures {
 public:
  enum class Type {
    Unused,
    Texture,
    VertexBuffer,
  };
  struct Elem {
    Type resource_type;
    void *resource;
    GPUSamplerState sampler;
  };
  Vector<Elem> bound_resources;

  void bind(Type resource_type, void *resource, GPUSamplerState sampler, int binding)
  {
    if (bound_resources.size() <= binding) {
      bound_resources.resize(binding + 1, {});
    }
    bound_resources[binding].resource_type = resource_type;
    bound_resources[binding].resource = resource;
    bound_resources[binding].sampler = sampler;
  }

  const Elem *get(int binding) const
  {
    if (binding >= bound_resources.size()) {
      /* TODO: Check with @Jeroen-Bakker.
       * Could we ensure state_manager adds default initialized bindings for each ShaderInterface
       * resource? (See #142097). */
      return nullptr;
    }
    return &bound_resources[binding];
  }

  void unbind(void *resource)
  {
    for (int index : IndexRange(bound_resources.size())) {
      if (bound_resources[index].resource == resource) {
        bound_resources[index].resource = nullptr;
        bound_resources[index].resource_type = Type::Unused;
        bound_resources[index].sampler = GPUSamplerState::default_sampler();
      }
    }
  }

  void unbind_all()
  {
    bound_resources.clear();
  }
};

class BindSpaceTopLevelAS {
 public:
  Vector<VKTopLevelAS *> bound_resources;

  void bind(VKTopLevelAS *resource, int binding)
  {
    if (bound_resources.size() <= binding) {
      bound_resources.resize(binding + 1);
    }
    bound_resources[binding] = resource;
  }

  VKTopLevelAS *get(int binding) const
  {
    return bound_resources[binding];
  }

  VKTopLevelAS *get_or_null(int binding) const
  {
    if (binding >= (int)bound_resources.size()) {
      return nullptr;
    }
    return bound_resources[binding];
  }

  void unbind(void *resource)
  {
    for (int index : IndexRange(bound_resources.size())) {
      if (bound_resources[index] == resource) {
        bound_resources[index] = nullptr;
      }
    }
  }

  void unbind_all()
  {
    bound_resources.clear();
  }
};

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

 public:
  /* Bind-space storage (formerly VKStateManager, now owned by the pipeline).
   * Public because the descriptor-set upload reads these directly at draw
   * time — same as the old `VKStateManager` layout. */
  BindSpaceTextures textures_;
  BindSpaceImages<BIND_SPACE_IMAGE_OFFSET> images_;
  BindSpaceUniformBuffers uniform_buffers_;
  BindSpaceStorageBuffers storage_buffers_;
  BindSpaceTopLevelAS acceleration_structures_;

  /**
   * True when a resource binding changed since the last descriptor-set upload
   * for any pipeline (mirrors the old `VKStateManager::is_dirty`).
   */
  bool descriptor_dirty_ = false;

   /* Texture / image binding overrides — now stored directly on this pipeline
    * in VK's bind spaces above. */
   void texture_bind(Texture *tex, GPUSamplerState sampler, int unit) override;
   void texture_unbind(Texture *tex) override;
   void texture_unbind_all() override;
   void image_bind(Texture *tex, int unit) override;
   void image_unbind(Texture *tex) override;
   void image_unbind_all() override;
   void uniformbuf_bind(UniformBuf *ubo, int slot) override;
   void uniformbuf_unbind(UniformBuf *ubo) override;
   void storagebuf_bind(StorageBuf *ssbo, int slot) override;
   void storagebuf_unbind(StorageBuf *ssbo) override;
   void tlas_bind(TopLevelAS *tlas, int slot) override;
   void tlas_unbind(TopLevelAS *tlas) override;

   /* Resource-object bind helpers. These are the VK-specific entrypoints used
    * by `VKUniformBuffer`, `VKStorageBuffer`, `VKVertexBuffer`,
    * `VKIndexBuffer`, `VKTopLevelAS` and `VKContext` (back-buffer blit). */
   void texel_buffer_bind(VKVertexBuffer &vertex_buffer, int binding);
   void texel_buffer_unbind(VKVertexBuffer &vertex_buffer);
   void uniform_buffer_bind(VKUniformBuffer *uniform_buffer, int binding);
   void uniform_buffer_unbind(VKUniformBuffer *uniform_buffer);
   void uniform_buffer_unbind_all();
   void storage_buffer_bind(BindSpaceStorageBuffers::Type resource_type,
                            void *resource,
                            int binding)
   {
     storage_buffer_bind(resource_type, resource, binding, 0);
   }
   void storage_buffer_bind(BindSpaceStorageBuffers::Type resource_type,
                            void *resource,
                            int binding,
                            VkDeviceSize offset);
   void storage_buffer_unbind(void *resource);
   void storage_buffer_unbind_all();
   void toplevelas_bind(VKTopLevelAS *tlas, int slot);
   void toplevelas_unbind(void *resource);
};

/**
 * Return the pipeline that legacy (non-pipeline-bound) Vulkan state setters
 * should configure: the explicitly bound `active_pipeline_`, or the
 * per-context fallback pipeline when none is set. Implemented in vk_pipeline.cc.
 */
VKPipeline *vk_pipeline_get();

}  // namespace blender::gpu
