/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gl_pipeline.hh"

#include "BLI_math_base_c.hh"
#include "BLI_utildefines.hh"

#include "BKE_global.hh"

#include "GPU_capabilities.hh"

#include "gl_context.hh"
#include "gl_framebuffer.hh"
#include "gl_state.hh"
#include "gl_texture.hh"

namespace blender::gpu {

/* ---------- Static member definitions ---------- */

GLuint GLPipeline::targets_[64] = {0};
GLuint GLPipeline::textures_[64] = {0};
GLuint GLPipeline::samplers_[64] = {0};
uint64_t GLPipeline::dirty_texture_binds_ = 0;
GLuint GLPipeline::images_[8] = {0};
GLenum GLPipeline::formats_[8] = {0};
uint8_t GLPipeline::dirty_image_binds_ = 0;
std::array<TextureWriteFormat, GPU_MAX_IMAGE> GLPipeline::image_formats_ = [] {
  std::array<TextureWriteFormat, GPU_MAX_IMAGE> f;
  f.fill(TextureWriteFormat::Invalid);
  return f;
}();
GPUState GLPipeline::current_ = {};
GPUStateMutable GLPipeline::current_mutable_ = {};
float GLPipeline::line_width_range_[2] = {1.f, 1.f};
bool GLPipeline::line_width_range_init_ = false;

void GLPipeline::ensure_init()
{
  if (!line_width_range_init_) {
    line_width_range_init_ = true;
    line_width_range_[0] = 1.f;
    line_width_range_[1] = 1.f;
    targets_[0] = GL_TEXTURE_2D;
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, line_width_range_);
    current_ = ~GPUState();
    current_mutable_ = ~GPUStateMutable();
  }
}

/* =================================================================== */
/** \name Fixed-function state setters (GL-specific)
 * \{ */

static void gl_set_write_mask(GPUWriteMask value)
{
  glDepthMask((value & GPU_WRITE_DEPTH) != 0);
  glColorMask((value & GPU_WRITE_RED) != 0,
              (value & GPU_WRITE_GREEN) != 0,
              (value & GPU_WRITE_BLUE) != 0,
              (value & GPU_WRITE_ALPHA) != 0);

  if (value == GPU_WRITE_NONE) {
    glEnable(GL_RASTERIZER_DISCARD);
  }
  else {
    glDisable(GL_RASTERIZER_DISCARD);
  }
}

static void gl_set_depth_test(GPUDepthTest value)
{
  GLenum func;
  switch (value) {
    case GPU_DEPTH_LESS:
      func = GL_LESS;
      break;
    case GPU_DEPTH_LESS_EQUAL:
      func = GL_LEQUAL;
      break;
    case GPU_DEPTH_EQUAL:
      func = GL_EQUAL;
      break;
    case GPU_DEPTH_GREATER:
      func = GL_GREATER;
      break;
    case GPU_DEPTH_GREATER_EQUAL:
      func = GL_GEQUAL;
      break;
    case GPU_DEPTH_ALWAYS:
    default:
      func = GL_ALWAYS;
      break;
  }

  if (value != GPU_DEPTH_NONE) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(func);
  }
  else {
    glDisable(GL_DEPTH_TEST);
  }
}

static void gl_set_stencil_test(const GPUStencilTest test, const GPUStencilOp operation)
{
  switch (operation) {
    case GPU_STENCIL_OP_REPLACE:
      glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_PASS:
      glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
      glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_FAIL:
      glStencilOpSeparate(GL_BACK, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
      glStencilOpSeparate(GL_FRONT, GL_INCR_WRAP, GL_KEEP, GL_KEEP);
      break;
    case GPU_STENCIL_OP_NONE:
    default:
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  }

  if (test != GPU_STENCIL_NONE) {
    glEnable(GL_STENCIL_TEST);
  }
  else {
    glDisable(GL_STENCIL_TEST);
  }
}

static void gl_set_stencil_mask(const GPUStencilTest test, const GPUStateMutable &state)
{
  GLenum func;
  switch (test) {
    case GPU_STENCIL_NEQUAL:
      func = GL_NOTEQUAL;
      break;
    case GPU_STENCIL_EQUAL:
      func = GL_EQUAL;
      break;
    case GPU_STENCIL_ALWAYS:
      func = GL_ALWAYS;
      break;
    case GPU_STENCIL_NONE:
    default:
      glStencilMask(0x00);
      glStencilFunc(GL_ALWAYS, 0x00, 0x00);
      return;
  }

  glStencilMask(state.stencil_write_mask);
  glStencilFunc(func, state.stencil_reference, state.stencil_compare_mask);
}

static void gl_set_clip_distances(const int new_dist_len, const int old_dist_len)
{
  for (int i = 0; i < new_dist_len; i++) {
    glEnable(GL_CLIP_DISTANCE0 + i);
  }
  for (int i = new_dist_len; i < old_dist_len; i++) {
    glDisable(GL_CLIP_DISTANCE0 + i);
  }
}

static void gl_set_facing(const bool invert)
{
  glFrontFace((invert) ? GL_CW : GL_CCW);
}

static void gl_set_backface_culling(const GPUFaceCullTest test)
{
  if (test != GPU_CULL_NONE) {
    glEnable(GL_CULL_FACE);
    glCullFace((test == GPU_CULL_FRONT) ? GL_FRONT : GL_BACK);
  }
  else {
    glDisable(GL_CULL_FACE);
  }
}

static void gl_set_provoking_vert(const GPUProvokingVertex vert)
{
  GLenum value = (vert == GPU_VERTEX_FIRST) ? GL_FIRST_VERTEX_CONVENTION :
                                             GL_LAST_VERTEX_CONVENTION;
  glProvokingVertex(value);
}

static void gl_set_clip_control(const bool enable)
{
  if (enable) {
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
  }
  else {
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
  }
}

static void gl_set_blend(const GPUBlend value)
{
  GLenum src_rgb, src_alpha, dst_rgb, dst_alpha;
  switch (value) {
    default:
    case GPU_BLEND_ALPHA: {
      src_rgb = GL_SRC_ALPHA;
      dst_rgb = GL_ONE_MINUS_SRC_ALPHA;
      src_alpha = GL_ONE;
      dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
      break;
    }
    case GPU_BLEND_ALPHA_PREMULT: {
      src_rgb = GL_ONE;
      dst_rgb = GL_ONE_MINUS_SRC_ALPHA;
      src_alpha = GL_ONE;
      dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
      break;
    }
    case GPU_BLEND_ADDITIVE: {
      src_rgb = GL_SRC_ALPHA;
      dst_rgb = GL_ONE;
      src_alpha = GL_ZERO;
      dst_alpha = GL_ONE;
      break;
    }
    case GPU_BLEND_MIN:
    case GPU_BLEND_MAX:
    case GPU_BLEND_SUBTRACT:
    case GPU_BLEND_ADDITIVE_PREMULT: {
      src_rgb = GL_ONE;
      dst_rgb = GL_ONE;
      src_alpha = GL_ONE;
      dst_alpha = GL_ONE;
      break;
    }
    case GPU_BLEND_MULTIPLY: {
      src_rgb = GL_DST_COLOR;
      dst_rgb = GL_ZERO;
      src_alpha = GL_DST_ALPHA;
      dst_alpha = GL_ZERO;
      break;
    }
    case GPU_BLEND_INVERT: {
      src_rgb = GL_ONE_MINUS_DST_COLOR;
      dst_rgb = GL_ZERO;
      src_alpha = GL_ZERO;
      dst_alpha = GL_ONE;
      break;
    }
    case GPU_BLEND_OIT: {
      src_rgb = GL_ONE;
      dst_rgb = GL_ONE;
      src_alpha = GL_ZERO;
      dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
      break;
    }
    case GPU_BLEND_BACKGROUND: {
      src_rgb = GL_ONE_MINUS_DST_ALPHA;
      dst_rgb = GL_SRC_ALPHA;
      src_alpha = GL_ZERO;
      dst_alpha = GL_SRC_ALPHA;
      break;
    }
    case GPU_BLEND_ALPHA_UNDER_PREMUL: {
      src_rgb = GL_ONE_MINUS_DST_ALPHA;
      dst_rgb = GL_ONE;
      src_alpha = GL_ONE_MINUS_DST_ALPHA;
      dst_alpha = GL_ONE;
      break;
    }
    case GPU_BLEND_CUSTOM: {
      src_rgb = GL_ONE;
      dst_rgb = GL_SRC1_COLOR;
      src_alpha = GL_ONE;
      dst_alpha = GL_SRC1_ALPHA;
      break;
    }
    case GPU_BLEND_OVERLAY_MASK_FROM_ALPHA: {
      src_rgb = GL_ZERO;
      dst_rgb = GL_ONE_MINUS_SRC_ALPHA;
      src_alpha = GL_ZERO;
      dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
      break;
    }
    case GPU_BLEND_TRANSPARENCY: {
      src_rgb = GL_ONE;
      dst_rgb = GL_SRC_ALPHA;
      src_alpha = GL_ZERO;
      dst_alpha = GL_SRC_ALPHA;
      break;
    }
  }

  if (value == GPU_BLEND_MIN) {
    glBlendEquation(GL_MIN);
  }
  else if (value == GPU_BLEND_MAX) {
    glBlendEquation(GL_MAX);
  }
  else if (value == GPU_BLEND_SUBTRACT) {
    glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
  }
  else {
    glBlendEquation(GL_FUNC_ADD);
  }

  glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
  if (value != GPU_BLEND_NONE) {
    glEnable(GL_BLEND);
  }
  else {
    glDisable(GL_BLEND);
  }
}

/** \} */

/* =================================================================== */
/** \name State apply
 * \{ */

void GLPipeline::apply_fixed_state()
{
  ensure_init();
  GPUState target = Context::get()->pipeline_get()->state_get();
  GPUStateMutable mutable_target = Context::get()->pipeline_get()->mutable_state_get();

  GPUState changed = target ^ current_;

  if (changed.blend != 0) {
    gl_set_blend(GPUBlend(target.blend));
  }
  if (changed.write_mask != 0) {
    gl_set_write_mask(GPUWriteMask(target.write_mask));
  }
  if (changed.depth_test != 0) {
    gl_set_depth_test(GPUDepthTest(target.depth_test));
  }
  if (changed.stencil_test != 0 || changed.stencil_op != 0) {
    gl_set_stencil_test(GPUStencilTest(target.stencil_test), GPUStencilOp(target.stencil_op));
    gl_set_stencil_mask(GPUStencilTest(target.stencil_test), mutable_target);
  }
  if (changed.clip_distances != 0) {
    gl_set_clip_distances(target.clip_distances, current_.clip_distances);
  }
  if (changed.culling_test != 0) {
    gl_set_backface_culling(GPUFaceCullTest(target.culling_test));
  }
  if (changed.invert_facing != 0) {
    gl_set_facing(target.invert_facing);
  }
  if (changed.provoking_vert != 0) {
    gl_set_provoking_vert(GPUProvokingVertex(target.provoking_vert));
  }
  if (changed.clip_control != 0) {
    gl_set_clip_control(target.clip_control);
  }
  if (changed.polygon_smooth) {
    if (target.polygon_smooth) {
      glEnable(GL_POLYGON_SMOOTH);
    }
    else {
      glDisable(GL_POLYGON_SMOOTH);
    }
  }
  if (changed.line_smooth) {
    if (target.line_smooth) {
      glEnable(GL_LINE_SMOOTH);
    }
    else {
      glDisable(GL_LINE_SMOOTH);
    }
  }

  current_ = target;

  GPUStateMutable changed_mutable = mutable_target ^ current_mutable_;

  if (float_as_uint(changed_mutable.point_size) != 0) {
    if (mutable_target.point_size > 0.0f) {
      glEnable(GL_PROGRAM_POINT_SIZE);
    }
    else {
      glDisable(GL_PROGRAM_POINT_SIZE);
      glPointSize(fabsf(mutable_target.point_size));
    }
  }

  if (float_as_uint(changed_mutable.line_width) != 0) {
    glLineWidth(clamp_f(mutable_target.line_width, line_width_range_[0], line_width_range_[1]));
  }

  if (changed_mutable.stencil_compare_mask != 0 || changed_mutable.stencil_reference != 0 ||
      changed_mutable.stencil_write_mask != 0)
  {
    gl_set_stencil_mask(GPUStencilTest(current_.stencil_test), mutable_target);
  }

  current_mutable_ = mutable_target;
}

int GLPipeline::get_line_width_range(float &min, float &max)
{
  ensure_init();
  min = line_width_range_[0];
  max = line_width_range_[1];
  return 2;
}

/** \} */

/* =================================================================== */
/** \name Texture binding
 * \{ */

void GLPipeline::bind_texture(Texture *tex_, GPUSamplerState sampler_state, int unit)
{
  BLI_assert(unit < GPU_max_textures());
  ensure_init();
  GLTexture *tex = static_cast<GLTexture *>(tex_);
  if (G.debug & G_DEBUG_GPU) {
    tex->check_feedback_loop();
  }
  if ((textures_[unit] == tex->tex_id_) &&
      (samplers_[unit] == GLTexture::get_sampler(sampler_state)))
  {
    return;
  }
  targets_[unit] = tex->target_;
  textures_[unit] = tex->tex_id_;
  samplers_[unit] = GLTexture::get_sampler(sampler_state);
  tex->is_bound_ = true;
  dirty_texture_binds_ |= 1ULL << unit;
}

void GLPipeline::texture_bind_temp(GLTexture *tex)
{
  ensure_init();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(tex->target_, tex->tex_id_);
  dirty_texture_binds_ |= 1ULL;
}

void GLPipeline::unbind_texture(Texture *tex_)
{
  GLTexture *tex = static_cast<GLTexture *>(tex_);
  if (!tex->is_bound_) {
    return;
  }

  GLuint tex_id = tex->tex_id_;
  for (int i = 0; i < ARRAY_SIZE(textures_); i++) {
    if (textures_[i] == tex_id) {
      textures_[i] = 0;
      samplers_[i] = 0;
      dirty_texture_binds_ |= 1ULL << i;
    }
  }
  tex->is_bound_ = false;
}

void GLPipeline::unbind_all_textures()
{
  for (int i = 0; i < ARRAY_SIZE(textures_); i++) {
    if (textures_[i] != 0) {
      textures_[i] = 0;
      samplers_[i] = 0;
      dirty_texture_binds_ |= 1ULL << i;
    }
  }
  texture_bind_apply();
}

void GLPipeline::texture_bind_apply()
{
  if (dirty_texture_binds_ == 0) {
    return;
  }
  uint64_t dirty_bind = dirty_texture_binds_;
  dirty_texture_binds_ = 0;

  int first = bitscan_forward_uint64(dirty_bind);
  int last = 64 - bitscan_reverse_uint64(dirty_bind);
  int count = last - first;

  if (GLContext::multi_bind_support) {
    glBindTextures(first, count, textures_ + first);
    glBindSamplers(first, count, samplers_ + first);
  }
  else {
    for (int unit = first; unit < last; unit++) {
      if ((dirty_bind >> unit) & 1UL) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(targets_[unit], textures_[unit]);
        glBindSampler(unit, samplers_[unit]);
      }
    }
  }
}

uint64_t GLPipeline::bound_texture_slots()
{
  uint64_t bound_slots = 0;
  for (int i = 0; i < ARRAY_SIZE(textures_); i++) {
    if (textures_[i] != 0) {
      bound_slots |= 1ULL << i;
    }
  }
  return bound_slots;
}

/** \} */

/* =================================================================== */
/** \name Image binding
 * \{ */

void GLPipeline::bind_image(Texture *tex_, int unit)
{
  ensure_init();
  BLI_assert(unit < 8);
  GLTexture *tex = static_cast<GLTexture *>(tex_);
  if (G.debug & G_DEBUG_GPU) {
    tex->check_feedback_loop();
  }
  images_[unit] = tex->tex_id_;
  formats_[unit] = to_gl_internal_format(tex->format_);
  image_formats_[unit] = TextureWriteFormat(tex->format_get());
  tex->is_bound_image_ = true;
  dirty_image_binds_ |= 1U << unit;
}

void GLPipeline::unbind_image(Texture *tex_)
{
  GLTexture *tex = static_cast<GLTexture *>(tex_);
  if (!tex->is_bound_image_) {
    return;
  }

  GLuint tex_id = tex->tex_id_;
  for (int i = 0; i < ARRAY_SIZE(images_); i++) {
    if (images_[i] == tex_id) {
      images_[i] = 0;
      image_formats_[i] = TextureWriteFormat::Invalid;
      dirty_image_binds_ |= 1U << i;
    }
  }
  tex->is_bound_image_ = false;
}

void GLPipeline::unbind_all_images()
{
  for (int i = 0; i < ARRAY_SIZE(images_); i++) {
    if (images_[i] != 0) {
      images_[i] = 0;
      dirty_image_binds_ |= 1U << i;
    }
  }
  image_formats_.fill(TextureWriteFormat::Invalid);
  image_bind_apply();
}

void GLPipeline::image_format_set_unit(int unit, TextureWriteFormat format)
{
  ensure_init();
  image_formats_[unit] = format;
}

TextureWriteFormat GLPipeline::image_format_get_unit(int unit)
{
  ensure_init();
  return image_formats_[unit];
}

void GLPipeline::image_bind_apply()
{
  if (dirty_image_binds_ == 0) {
    return;
  }
  uint32_t dirty_bind = dirty_image_binds_;
  dirty_image_binds_ = 0;

  int first = bitscan_forward_uint(dirty_bind);
  int last = 32 - bitscan_reverse_uint(dirty_bind);
  int count = last - first;

  if (GLContext::multi_bind_image_support) {
    glBindImageTextures(first, count, images_ + first);
  }
  else {
    for (int unit = first; unit < last; unit++) {
      if ((dirty_bind >> unit) & 1U) {
        glBindImageTexture(unit, images_[unit], 0, GL_TRUE, 0, GL_READ_WRITE, formats_[unit]);
      }
    }
  }
}

uint8_t GLPipeline::bound_image_slots()
{
  uint8_t bound_slots = 0;
  for (int i = 0; i < ARRAY_SIZE(images_); i++) {
    if (images_[i] != 0) {
      bound_slots |= 1U << i;
    }
  }
  return bound_slots;
}

/** \} */

/* =================================================================== */
/** \name Orchestration
 * \{ */

void GLPipeline::bind()
{
  Context *ctx = Context::get();
  ctx->active_pipeline_ = this;
  if (ctx->state_ever_applied_ &&
      ctx->applied_state_ == this->state_ &&
      ctx->applied_mutable_state_ == this->mutable_state_ &&
      ctx->applied_shader_ == this->shader_)
  {
    return;
  }
  this->mark_dirty(GPU_PIPELINE_DIRTY_ALL);
}

void GLPipeline::unbind()
{
  Context *ctx = Context::get();
  if (ctx->active_pipeline_ == this) {
    ctx->active_pipeline_ = nullptr;
  }
}

void GLPipeline::resolve_draw(GPUPrimType /*prim_type*/, int64_t /*vertex_input_key*/)
{
  Context *ctx = Context::get();
  sync_context();
  apply_fixed_state();
  texture_bind_apply();
  image_bind_apply();

  if (ctx->active_fb) {
    /* This is needed by gpu_py_offscreen. */
    static_cast<GLFrameBuffer *>(ctx->active_fb)->apply_state();
  }

  ctx->applied_state_ = this->state_;
  ctx->applied_mutable_state_ = this->mutable_state_;
  ctx->applied_shader_ = this->shader_;
  ctx->state_ever_applied_ = true;
  this->clear_dirty();
}

void GLPipeline::resolve_compute()
{
  Context *ctx = Context::get();
  sync_context();
  if (this->shader_ != nullptr) {
    ctx->shader = this->shader_;
    ctx->applied_shader_ = this->shader_;
  }
  this->clear_dirty();
}

void GLPipeline::issue_barrier(GPUBarrier barrier_bits)
{
  glMemoryBarrier(to_gl(barrier_bits));
}

/** \} */

}  // namespace blender::gpu
