/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_pipeline.hh"

#include "BLI_utildefines.hh"

#include "gpu_backend.hh"
#include "gpu_context_private.hh"
#include "gpu_state_private.hh"

namespace blender {

namespace gpu {

Pipeline::Pipeline()
{
  /* Match the StateManager default state so a fresh pipeline is safe to apply. */
  state_.write_mask = GPU_WRITE_COLOR;
  state_.blend = GPU_BLEND_NONE;
  state_.culling_test = GPU_CULL_NONE;
  state_.depth_test = GPU_DEPTH_NONE;
  state_.stencil_test = GPU_STENCIL_NONE;
  state_.stencil_op = GPU_STENCIL_OP_NONE;
  state_.provoking_vert = GPU_VERTEX_LAST;
  state_.invert_facing = false;
  state_.clip_distances = 0;
  state_.clip_control = false;
  state_.polygon_smooth = false;
  state_.line_smooth = false;

  mutable_state_.point_size = -1.0f; /* Negative = fixed (program size disabled). */
  mutable_state_.line_width = 1.0f;
  mutable_state_.stencil_write_mask = 0x00;
  mutable_state_.stencil_compare_mask = 0x00;
  mutable_state_.stencil_reference = 0x00;
}

Pipeline::~Pipeline()
{
}

/* ------------------------------------------------------------ */
/** \name Setters
 * \{ */

void Pipeline::shader_set(Shader *shader)
{
  if (shader_ == shader) {
    return;
  }
  shader_ = shader;
  shader_interface_id_ = (shader != nullptr && shader->interface != nullptr) ?
                                 shader->interface->id :
                                 0;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE | GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::specialization_set(const shader::SpecializationConstants *constants)
{
  if (specialization_constants_ == constants) {
    return;
  }
  specialization_constants_ = constants;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::framebuffer_set(FrameBuffer *fb)
{
  if (framebuffer_ == fb) {
    return;
  }
  framebuffer_ = fb;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_blend(GPUBlend blend)
{
  if (state_.blend == blend) {
    return;
  }
  state_.blend = blend;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_write_mask(GPUWriteMask mask)
{
  if (GPUWriteMask(state_.write_mask) == mask) {
    return;
  }
  state_.write_mask = mask;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_clip_distances(int count)
{
  if (int(state_.clip_distances) == count) {
    return;
  }
  state_.clip_distances = count;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_prim_type(GPUPrimType type)
{
  if (prim_type_ == type) {
    return;
  }
  prim_type_ = type;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_point_size(float size)
{
  if (mutable_state_.point_size == size) {
    return;
  }
  mutable_state_.point_size = size;
  /* Point size is mutable state - not a PSO change. (No dirty flag needed.) */
}

void Pipeline::set_clip_control(bool unit_range)
{
  if (bool(state_.clip_control) == unit_range) {
    return;
  }
  state_.clip_control = unit_range;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_polygon_smooth(bool enable)
{
  if (bool(state_.polygon_smooth) == enable) {
    return;
  }
  state_.polygon_smooth = enable;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_line_smooth(bool enable)
{
  if (bool(state_.line_smooth) == enable) {
    return;
  }
  state_.line_smooth = enable;
  mark_dirty(GPU_PIPELINE_DIRTY_PIPELINE);
}

void Pipeline::set_depth_test(GPUDepthTest test)
{
  if (GPUDepthTest(state_.depth_test) == test) {
    return;
  }
  state_.depth_test = test;
  mark_dirty(GPU_PIPELINE_DIRTY_DEPTH_STENCIL);
}

void Pipeline::set_stencil_test(GPUStencilTest test)
{
  if (GPUStencilTest(state_.stencil_test) == test) {
    return;
  }
  state_.stencil_test = test;
  mark_dirty(GPU_PIPELINE_DIRTY_DEPTH_STENCIL);
}

void Pipeline::set_stencil_op(GPUStencilOp op)
{
  if (GPUStencilOp(state_.stencil_op) == op) {
    return;
  }
  state_.stencil_op = op;
  mark_dirty(GPU_PIPELINE_DIRTY_DEPTH_STENCIL);
}

void Pipeline::set_face_culling(GPUFaceCullTest cull)
{
  if (GPUFaceCullTest(state_.culling_test) == cull) {
    return;
  }
  state_.culling_test = cull;
  /* Cull/facing may be baked on some backends; treat as a depth-stencil category
   * change so backends that bake them know to rebuild. */
  mark_dirty(GPU_PIPELINE_DIRTY_DEPTH_STENCIL);
}

void Pipeline::set_provoking_vertex(GPUProvokingVertex vert)
{
  if (GPUProvokingVertex(state_.provoking_vert) == vert) {
    return;
  }
  state_.provoking_vert = vert;
  mark_dirty(GPU_PIPELINE_DIRTY_DEPTH_STENCIL);
}

void Pipeline::set_front_facing(bool invert)
{
  if (bool(state_.invert_facing) == invert) {
    return;
  }
  state_.invert_facing = invert;
  mark_dirty(GPU_PIPELINE_DIRTY_DEPTH_STENCIL);
}

void Pipeline::set_line_width(float width)
{
  /* Line width is mutable state (encoder-level), no dirty flag needed. */
  mutable_state_.line_width = width;
}

void Pipeline::set_stencil_write_mask(uint8_t mask)
{
  /* Mutable state - no dirty flag. */
  mutable_state_.stencil_write_mask = mask;
}

void Pipeline::set_stencil_compare_mask(uint8_t mask)
{
  /* Mutable state - no dirty flag. */
  mutable_state_.stencil_compare_mask = mask;
}

void Pipeline::set_stencil_reference(uint8_t ref)
{
  /* Mutable state - no dirty flag. */
  mutable_state_.stencil_reference = ref;
}

void Pipeline::uniform_float(int location, int comp_len, int array_size, const float *data)
{
  /* Push the per-draw value through the shader (GL uniform, Vulkan or Metal push
   * constant as appropriate to the backend). The shader is the single source of
   * truth for how to encode a uniform, so this is backend-agnostic. */
  if (shader_ != nullptr) {
    shader_->uniform_float(location, comp_len, array_size, data);
  }
  mark_dirty(GPU_PIPELINE_DIRTY_PUSH_CONSTANTS);
}

void Pipeline::uniform_int(int location, int comp_len, int array_size, const int *data)
{
  if (shader_ != nullptr) {
    shader_->uniform_int(location, comp_len, array_size, data);
  }
  mark_dirty(GPU_PIPELINE_DIRTY_PUSH_CONSTANTS);
}

void Pipeline::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  texture_bindings_.resize(std::max<int>(unit + 1, (int)texture_bindings_.size()));
  texture_bindings_[unit] = TextureBinding{tex, sampler, TextureWriteFormat::Invalid};
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::texture_unbind(Texture *tex)
{
  for (TextureBinding &b : texture_bindings_) {
    if (b.tex == tex) {
      b = TextureBinding{};
    }
  }
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::texture_unbind_all()
{
  texture_bindings_.clear();
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::image_bind(Texture *tex, int unit)
{
  image_bindings_.resize(std::max<int>(unit + 1, (int)image_bindings_.size()));
  image_bindings_[unit] = TextureBinding{
      tex, GPUSamplerState::default_sampler(), TextureWriteFormat(tex->format_get())};
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::image_unbind(Texture *tex)
{
  for (TextureBinding &b : image_bindings_) {
    if (b.tex == tex) {
      b = TextureBinding{};
    }
  }
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::image_unbind_all()
{
  image_bindings_.clear();
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::uniformbuf_bind(UniformBuf *ubo, int slot)
{
  ubo_bindings_.resize(std::max<int>(slot + 1, (int)ubo_bindings_.size()));
  ubo_bindings_[slot] = ubo;
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::uniformbuf_unbind(UniformBuf *ubo)
{
  for (int i = 0; i < (int)ubo_bindings_.size(); i++) {
    if (ubo_bindings_[i] == ubo) {
      ubo_bindings_[i] = nullptr;
    }
  }
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::storagebuf_bind(StorageBuf *ssbo, int slot)
{
  ssbo_bindings_.resize(std::max<int>(slot + 1, (int)ssbo_bindings_.size()));
  ssbo_bindings_[slot] = ssbo;
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::storagebuf_unbind(StorageBuf *ssbo)
{
  for (int i = 0; i < (int)ssbo_bindings_.size(); i++) {
    if (ssbo_bindings_[i] == ssbo) {
      ssbo_bindings_[i] = nullptr;
    }
  }
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::tlas_bind(TopLevelAS *tlas, int slot)
{
  tlas_bindings_.resize(std::max<int>(slot + 1, (int)tlas_bindings_.size()));
  tlas_bindings_[slot] = tlas;
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::tlas_unbind(TopLevelAS *tlas)
{
  for (int i = 0; i < (int)tlas_bindings_.size(); i++) {
    if (tlas_bindings_[i] == tlas) {
      tlas_bindings_[i] = nullptr;
    }
  }
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::image_format_set(int unit, TextureWriteFormat format)
{
  image_bindings_.resize(std::max<int>(unit + 1, (int)image_bindings_.size()));
  image_bindings_[unit].format = format;
  /* Image format affects descriptor / shader binding - mark dirty. */
  mark_dirty(GPU_PIPELINE_DIRTY_DESCRIPTOR);
}

void Pipeline::state_set(GPUWriteMask write_mask,
                         GPUBlend blend,
                         GPUFaceCullTest culling_test,
                         GPUDepthTest depth_test,
                         GPUStencilTest stencil_test,
                         GPUStencilOp stencil_op,
                         GPUProvokingVertex provoking_vert)
{
  set_write_mask(write_mask);
  set_blend(blend);
  set_face_culling(culling_test);
  set_depth_test(depth_test);
  set_stencil_test(stencil_test);
  set_stencil_op(stencil_op);
  set_provoking_vertex(provoking_vert);
}

/** \} */

/* ------------------------------------------------------------ */
/** \name Query
 * \{ */

const Pipeline::TextureBinding *Pipeline::texture_bind_get(int unit) const
{
  if (unit < 0 || unit >= (int)texture_bindings_.size()) {
    return nullptr;
  }
  return &texture_bindings_[unit];
}

const Texture *Pipeline::image_bind_get(int unit) const
{
  if (unit < 0 || unit >= (int)image_bindings_.size()) {
    return nullptr;
  }
  return image_bindings_[unit].tex;
}

UniformBuf *Pipeline::uniformbuf_bind_get(int slot) const
{
  if (slot < 0 || slot >= (int)ubo_bindings_.size()) {
    return nullptr;
  }
  return ubo_bindings_[slot];
}

StorageBuf *Pipeline::storagebuf_bind_get(int slot) const
{
  if (slot < 0 || slot >= (int)ssbo_bindings_.size()) {
    return nullptr;
  }
  return ssbo_bindings_[slot];
}

TopLevelAS *Pipeline::tlas_bind_get(int slot) const
{
  if (slot < 0 || slot >= (int)tlas_bindings_.size()) {
    return nullptr;
  }
  return tlas_bindings_[slot];
}

/** \} */

/* ------------------------------------------------------------ */
/** \name Orchestration
 * \{ */

void Pipeline::bind()
{
  Context::get()->active_pipeline_ = this;
}

void Pipeline::unbind()
{
  Context *ctx = Context::get();
  if (ctx->active_pipeline_ == this) {
    ctx->active_pipeline_ = nullptr;
  }
}

void Pipeline::sync_context()
{
  Context *ctx = Context::get();
  /* Propagate the pipeline's shader/framebuffer to the context so the
   * state-manager-based draw path uses them. Only update if we actually have
   * a value - avoid clobbering context state that the pipeline didn't set. */
  if (shader_ != nullptr) {
    ctx->shader = shader_;
  }
  if (framebuffer_ != nullptr) {
    ctx->active_fb = framebuffer_;
  }
}

void Pipeline::sync_state_manager()
{
  StateManager *sm = Context::get()->state_manager;
  /* Cheap bitfield copies; the backend decides whether to (re)apply. */
  if (sm->state != state_) {
    sm->state = state_;
  }
  if (sm->mutable_state != mutable_state_) {
    sm->mutable_state = mutable_state_;
  }
}

void Pipeline::mark_dirty(GPUPipelineDirtyFlag flag)
{
  dirty_flags_ |= flag;
}

void Pipeline::clear_dirty()
{
  dirty_flags_ = GPU_PIPELINE_DIRTY_NONE;
}

/** \} */

void PipelineDeleter::operator()(Pipeline *p)
{
  GPU_pipeline_discard(p);
}

}  // namespace gpu

gpu::Pipeline *GPU_pipeline_create()
{
  return gpu::GPUBackend::get()->pipeline_alloc();
}

void GPU_pipeline_discard(gpu::Pipeline *pipeline)
{
  if (pipeline != nullptr) {
    delete pipeline;
  }
}

}  // namespace blender
