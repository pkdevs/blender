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
#include "BLI_span.hh"
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
  GPU_PIPELINE_DIRTY_ALL = GPU_PIPELINE_DIRTY_PIPELINE | GPU_PIPELINE_DIRTY_DESCRIPTOR |
                           GPU_PIPELINE_DIRTY_DEPTH_STENCIL | GPU_PIPELINE_DIRTY_PUSH_CONSTANTS,
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

  /* Resource bindings (abstract on base). Each backend owns its own storage +
   * fast path. See `GLPipeline`, `VKPipeline`, `MTLPipeline`. */
  virtual void texture_bind(Texture *tex, GPUSamplerState sampler, int unit) = 0;
  virtual void texture_unbind(Texture *tex) = 0;
  virtual void texture_unbind_all() = 0;
  virtual void image_bind(Texture *tex, int unit) = 0;
  virtual void image_unbind(Texture *tex) = 0;
  virtual void image_unbind_all() = 0;
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
   Span<uint8_t> push_constants_data_get() const
   {
     return push_constants_data_;
   }
   uint32_t push_constants_size_get() const
   {
     return (uint32_t)push_constants_data_.size();
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
    * Orchestration (common to all backends)
    * ---------------------------------------------------------- */
   /**
    * Mark this pipeline as the active one on the context and mark it dirty so
    * the first `prepare_*` after binding re-resolves. Overriding is optional;
    * backends that want to skip the resolve when state is unchanged can compare
    * against their own "last applied" cache and only mark dirty when there's
    * a real diff (see `GLPipeline::bind`).
    */
   virtual void bind();
   virtual void unbind();
   /**
    * Called by the draw path. Skips work when the pipeline is not dirty
    * (nothing has changed since the last resolve on this pipeline). Otherwise
    * delegates to the backend's `resolve_draw`, which applies the pipeline's
    * state to the device and clears the dirty flag at the end.
    */
   void prepare_draw(GPUPrimType prim_type, int64_t vertex_input_key = 0);
   void prepare_compute();
   /** Backend hook: apply/resolve this pipeline's state for a draw call. */
    virtual void resolve_draw(GPUPrimType prim_type, int64_t vertex_input_key) = 0;
    /** Backend hook: apply/resolve this pipeline's state for a compute dispatch. */
    virtual void resolve_compute() = 0;
    /** Issue a memory barrier on the active context. Backend-specific. */
    virtual void issue_barrier(GPUBarrier barrier_bits) = 0;
    /**
     * Legacy entrypoint: unconditionally (re)apply this pipeline's state to the
     * device, regardless of whether a `prepare_draw` already ran. Used by the
     * legacy (non-pipeline-bound) draw paths to force a resolve.
     */
    void apply_state()
    {
      mark_dirty(GPU_PIPELINE_DIRTY_ALL);
      resolve_draw(prim_type_, 0);
    }

  protected:
  void mark_dirty(GPUPipelineDirtyFlag flag);
  void clear_dirty();

  /**
   * Propagate this pipeline's shader + framebuffer to the context's global state
   * (only when set, so we don't clobber context state the pipeline didn't touch).
   */
  void sync_context();

  /* ------------------------------------------------------------
   * State storage (fixed-function shared - backends own device objects/bindings)
   * ---------------------------------------------------------- */
  Shader *shader_ = nullptr;
  uint64_t shader_interface_id_ = 0;
  const shader::SpecializationConstants *specialization_constants_ = nullptr;
  FrameBuffer *framebuffer_ = nullptr;
  GPUPrimType prim_type_ = GPU_PRIM_TRIS;

  GPUState state_ = {};
  GPUStateMutable mutable_state_ = {};

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
