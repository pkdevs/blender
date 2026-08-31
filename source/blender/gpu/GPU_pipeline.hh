/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * A gpu::Pipeline encapsulates shader + fixed-function state + resource bindings.
 * It is an explicit, named object that owns the state needed to draw.
 *
 * State is stored here to track the consequence of a state change for the specific
 * backend/device being used. In the end this should reduce the overhead inside Metal
 * Vulkan.
 */

#pragma once

#include <algorithm>
#include <memory>

#include "BLI_enum_flags.hh"
#include "BLI_sys_types.hh"
#include "BLI_vector.hh"

#include "GPU_common_types.hh"
#include "GPU_primitive.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "gpu_state_private.hh"

namespace blender {
namespace gpu {
class FrameBuffer;
class Pipeline;
class Shader;
class TopLevelAS;
class Texture;
class UniformBuf;
class StorageBuf;
namespace shader {
struct SpecializationConstants;
}  // namespace shader
}  // namespace gpu
}  // namespace blender

namespace blender {

/**
 * Flags for each category of pipeline state that may have changed since it was last
 * resolved to a backend device object. Backends use these to decide what to rebuild
 * (e.g. Vulkan pipeline, Metal depth/stencil state, descriptor set, GL state).
 */
enum GPUPipelineDirtyFlag {
  GPU_PIPELINE_DIRTY_NONE = 0,
  /* State baked into the pipeline object (shader, blend, write mask, clip, ...). */
  GPU_PIPELINE_DIRTY_PIPELINE = (1 << 0),
  /* Resource bindings (textures, UBOs, SSBOs, images, TLAS). */
  GPU_PIPELINE_DIRTY_DESCRIPTOR = (1 << 1),
  /* Depth/stencil test and ops (Metal depth/stencil state, Vulkan baked depth/stencil). */
  GPU_PIPELINE_DIRTY_DEPTH_STENCIL = (1 << 2),
  /* Push constants (per-draw uniform data). */
  GPU_PIPELINE_DIRTY_PUSH_CONSTANTS = (1 << 3),
};
ENUM_OPERATORS(GPUPipelineDirtyFlag)

namespace gpu {

/** Deleter for use with std::unique_ptr. */
class PipelineDeleter {
 public:
  void operator()(Pipeline *p);
};
using PipelinePtr = std::unique_ptr<Pipeline, PipelineDeleter>;

/**
 * A pipeline encapsulates shader + fixed-function state + resource bindings.
 */
class Pipeline {
 public:
  /** A texture (or image) binding: the texture plus the sampler/format used. */
  struct TextureBinding {
    Texture *tex = nullptr;
    GPUSamplerState sampler = GPUSamplerState::default_sampler();
    TextureWriteFormat format = TextureWriteFormat::Invalid;
  };

  Pipeline();
  virtual ~Pipeline();

  /** Bind a shader (and its specialization constants). */
  virtual void shader_set(Shader *shader);
  virtual void specialization_set(const shader::SpecializationConstants *constants);
  virtual void framebuffer_set(FrameBuffer *fb);

  /* Pipeline state (PSO-affecting). */
  virtual void set_blend(GPUBlend blend);
  virtual void set_write_mask(GPUWriteMask mask);
  virtual void set_clip_distances(int count);
  virtual void set_prim_type(GPUPrimType type);
  virtual void set_point_size(float size);
  virtual void set_clip_control(bool unit_range);
  virtual void set_polygon_smooth(bool enable);
  virtual void set_line_smooth(bool enable);

  /* Depth/stencil / cull / facing (may or may not be baked, backend decides). */
  virtual void set_depth_test(GPUDepthTest test);
  virtual void set_stencil_test(GPUStencilTest test);
  virtual void set_stencil_op(GPUStencilOp op);
  virtual void set_face_culling(GPUFaceCullTest cull);
  virtual void set_provoking_vertex(GPUProvokingVertex vert);
  virtual void set_front_facing(bool invert);
  virtual void set_line_width(float width);
  virtual void set_stencil_write_mask(uint8_t mask);
  virtual void set_stencil_compare_mask(uint8_t mask);
  virtual void set_stencil_reference(uint8_t ref);

  /* Push constants (per-draw data). */
  virtual void uniform_float(int location, int comp_len, int array_size, const float *data);
  virtual void uniform_int(int location, int comp_len, int array_size, const int *data);

  /* Resource bindings. */
  virtual void texture_bind(Texture *tex, GPUSamplerState sampler, int unit);
  virtual void texture_unbind(Texture *tex);
  virtual void texture_unbind_all();
  virtual void image_bind(Texture *tex, int unit);
  virtual void image_unbind(Texture *tex);
  virtual void image_unbind_all();
  virtual void uniformbuf_bind(UniformBuf *ubo, int slot);
  virtual void uniformbuf_unbind(UniformBuf *ubo);
  virtual void storagebuf_bind(StorageBuf *ssbo, int slot);
  virtual void storagebuf_unbind(StorageBuf *ssbo);
  virtual void tlas_bind(TopLevelAS *tlas, int slot);
  virtual void tlas_unbind(TopLevelAS *tlas);
  virtual void image_format_set(int unit, TextureWriteFormat format);

  /** Convenience: set several state values at once. */
  void state_set(GPUWriteMask write_mask,
                 GPUBlend blend,
                 GPUFaceCullTest culling_test,
                 GPUDepthTest depth_test,
                 GPUStencilTest stencil_test,
                 GPUStencilOp stencil_op,
                 GPUProvokingVertex provoking_vert);

  /* ------------------------------------------------------------
   * Query
   * ---------------------------------------------------------- */
  Shader *shader_get() const
  {
    return shader_;
  }
  uint64_t shader_interface_id_get() const
  {
    return shader_interface_id_;
  }
  const shader::SpecializationConstants *specialization_get() const
  {
    return specialization_constants_;
  }
  FrameBuffer *framebuffer_get() const
  {
    return framebuffer_;
  }
  GPUBlend blend_get() const
  {
    return GPUBlend(state_.blend);
  }
  GPUWriteMask write_mask_get() const
  {
    return GPUWriteMask(state_.write_mask);
  }
  GPUPrimType prim_type_get() const
  {
    return prim_type_;
  }
  const GPUState &state_get() const
  {
    return state_;
  }
  const GPUStateMutable &mutable_state_get() const
  {
    return mutable_state_;
  }
  const TextureBinding *texture_bind_get(int unit) const;
  const Texture *image_bind_get(int unit) const;
  UniformBuf *uniformbuf_bind_get(int slot) const;
  StorageBuf *storagebuf_bind_get(int slot) const;
  TopLevelAS *tlas_bind_get(int slot) const;
  const Vector<uint8_t> &push_constants_data_get() const
  {
    return push_constants_data_;
  }

  /* Dirty state. */
  GPUPipelineDirtyFlag dirty_flags_get() const
  {
    return dirty_flags_;
  }
  bool is_dirty() const { return dirty_flags_ != GPU_PIPELINE_DIRTY_NONE; }
  bool is_pipeline_dirty() const { return (dirty_flags_ & GPU_PIPELINE_DIRTY_PIPELINE) != 0; }
  bool is_descriptor_dirty() const { return (dirty_flags_ & GPU_PIPELINE_DIRTY_DESCRIPTOR) != 0; }
  bool is_depth_stencil_dirty() const
  {
    return (dirty_flags_ & GPU_PIPELINE_DIRTY_DEPTH_STENCIL) != 0;
  }
  bool is_push_constants_dirty() const
  {
    return (dirty_flags_ & GPU_PIPELINE_DIRTY_PUSH_CONSTANTS) != 0;
  }

  /* ------------------------------------------------------------
   * Orchestration (common to all backends in this phase)
   * ---------------------------------------------------------- */
  /** Mark this pipeline as the active one on the context. */
  void bind();
  /** Clear the active pipeline if it is this one. */
  void unbind();
  /**
   * Apply this pipeline's state to the context's StateManager and (if anything
   * is dirty) force the backend to re-resolve. Called by the draw path.
   * Backends that want to cache device state can override this in a later phase.
   */
   virtual void prepare_draw(GPUPrimType prim_type, int64_t vertex_input_key = 0) = 0;
   virtual void prepare_compute() = 0;

  protected:
  void mark_dirty(GPUPipelineDirtyFlag flag);
  void clear_dirty();

  /** Propagate shader + framebuffer to the context's global state. */
  void sync_context();
  /** Copy the pipeline's fixed-function state into the StateManager (no apply). */
  void sync_state_manager();

  /* ------------------------------------------------------------
   * State storage (shared - backends own their device objects in a later phase)
   * ---------------------------------------------------------- */
  Shader *shader_ = nullptr;
  uint64_t shader_interface_id_ = 0;
  const shader::SpecializationConstants *specialization_constants_ = nullptr;
  FrameBuffer *framebuffer_ = nullptr;
  GPUPrimType prim_type_ = GPU_PRIM_TRIS;

  GPUState state_ = {};
  GPUStateMutable mutable_state_ = {};

  /* Resource bindings (dynamic, sized as needed). */
  Vector<TextureBinding> texture_bindings_;
  Vector<TextureBinding> image_bindings_;
  Vector<UniformBuf *> ubo_bindings_;
  Vector<StorageBuf *> ssbo_bindings_;
  Vector<TopLevelAS *> tlas_bindings_;

  /* Push constants (raw byte buffer). */
  Vector<uint8_t> push_constants_data_;

  GPUPipelineDirtyFlag dirty_flags_ = GPU_PIPELINE_DIRTY_NONE;
};  /* end of Pipeline */

}  // namespace gpu

/** Create a pipeline for the current backend. */
gpu::Pipeline *GPU_pipeline_create();
/** Free a pipeline created with GPU_pipeline_create. */
void GPU_pipeline_discard(gpu::Pipeline *pipeline);

}  // namespace blender
