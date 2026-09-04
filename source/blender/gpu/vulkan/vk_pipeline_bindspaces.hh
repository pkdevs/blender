/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Vulkan descriptor binding storage for `VKPipeline`. Previously lived on
 * `VKStateManager`; the pipeline is the single source of truth now.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_utildefines.hh"

#include "gpu_state_private.hh"  // TextureWriteFormat (until moved into GPU common)
#include "GPU_state.hh"

#include "render_graph/vk_resource_access_info.hh"

namespace blender::gpu {

static constexpr int BIND_SPACE_IMAGE_OFFSET = 512;

class VKTexture;
class VKUniformBuffer;
class VKVertexBuffer;
class VKStorageBuffer;
class VKIndexBuffer;
class VKTopLevelAS;

/** VK bind space for uniform buffers. */
class VKBindSpaceUniformBuffers {
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

/** VK bind space for image resources. */
template<int Offset> class VKBindSpaceImages {
 public:
  Vector<VKTexture *> bound_resources;
  Vector<TextureWriteFormat> formats;  // per-slot write format, parallel

  void bind(VKTexture *resource, int binding, TextureWriteFormat format)
  {
    if (binding >= Offset) {
      binding -= Offset;
    }
    if (bound_resources.size() <= binding) {
      bound_resources.resize(binding + 1);
      formats.resize(binding + 1, TextureWriteFormat::Invalid);
    }
    bound_resources[binding] = resource;
    formats[binding] = format;
  }

  VKTexture *get(int binding) const
  {
    if (binding >= Offset) {
      binding -= Offset;
    }
    return bound_resources[binding];
  }

  TextureWriteFormat format_get(int binding) const
  {
    return formats[binding];
  }

  void unbind(void *resource)
  {
    for (int index : IndexRange(bound_resources.size())) {
      if (bound_resources[index] == resource) {
        bound_resources[index] = nullptr;
        formats[index] = TextureWriteFormat::Invalid;
      }
    }
  }

  void unbind_all()
  {
    bound_resources.clear();
    formats.clear();
  }
};

/** VK bind space for storage buffers (UBOs, SSBOs, vertex/index, texel buffers). */
class VKBindSpaceBuffers {
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

/** VK bind space for textures (includes texel/vertex buffers via `Type`). */
class VKBindSpaceTextures {
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
       * Could we ensure the pipeline adds default initialized bindings for each ShaderInterface
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

/** VK bind space for top-level acceleration structures. */
class VKBindSpaceTLAS {
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
 * Descriptor binding storage for VKPipeline. Owned by the pipeline, not by
 * a StateManager. `is_dirty` is the "some binding changed, rebuild the
 * descriptor set" flag the descriptor-set code relies on.
 */
struct VKPipelineBindings {
  VKBindSpaceTextures textures_;
  VKBindSpaceImages<BIND_SPACE_IMAGE_OFFSET> images_;
  VKBindSpaceUniformBuffers uniform_buffers_;
  VKBindSpaceBuffers storage_buffers_;
  VKBindSpaceTLAS acceleration_structures_;

  bool is_dirty = false;

  /* --- Texture (incl. texel buffers, stored in the same slot space) --- */
  void texture_bind(void *res, GPUSamplerState sampler, int binding)
  {
    textures_.bind(VKBindSpaceTextures::Type::Texture, res, sampler, binding);
    is_dirty = true;
  }
  void texel_buffer_bind(VKVertexBuffer &vb, int binding)
  {
    textures_.bind(VKBindSpaceTextures::Type::VertexBuffer,
                   &vb,
                   GPUSamplerState::default_sampler(),
                   binding);
    is_dirty = true;
  }
  void texture_unbind(void *res)
  {
    textures_.unbind(res);
    is_dirty = true;
  }
  void texture_unbind_all()
  {
    textures_.unbind_all();
    is_dirty = true;
  }

  /* --- Uniform buffers --- */
  void uniform_buffer_bind(VKUniformBuffer *res, int binding)
  {
    uniform_buffers_.bind(res, binding);
    is_dirty = true;
  }
  void uniform_buffer_unbind(VKUniformBuffer *res)
  {
    uniform_buffers_.unbind(res);
    is_dirty = true;
  }
  void uniform_buffer_unbind_all()
  {
    uniform_buffers_.unbind_all();
    is_dirty = true;
  }

  /* --- Storage buffers (UB, SSBO, vertex/index, plain) --- */
  void storage_buffer_bind(VKBindSpaceBuffers::Type type, void *res, int binding,
                           VkDeviceSize offset = 0)
  {
    storage_buffers_.bind(type, res, binding, offset);
    is_dirty = true;
  }
  void storage_buffer_unbind(void *res)
  {
    storage_buffers_.unbind(res);
    is_dirty = true;
  }
  void storage_buffer_unbind_all()
  {
    storage_buffers_.unbind_all();
    is_dirty = true;
  }

  /* --- Images (writes) --- */
  void image_bind(void *res, int binding, TextureWriteFormat format)
  {
    images_.bind(static_cast<VKTexture *>(res), binding, format);
    is_dirty = true;
  }
  void image_unbind(void *res)
  {
    images_.unbind(res);
    is_dirty = true;
  }
  void image_unbind_all()
  {
    images_.unbind_all();
    is_dirty = true;
  }

  /* --- Ray-tracing top-level acceleration structures --- */
  void tlas_bind(VKTopLevelAS *res, int binding)
  {
    acceleration_structures_.bind(res, binding);
    is_dirty = true;
  }
  void tlas_unbind(void *res)
  {
    acceleration_structures_.unbind(res);
    is_dirty = true;
  }
};

}  // namespace blender::gpu
