/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * GLStateManager is now a thin compatibility shim over GLPipeline. All slot
 * storage + fixed-function state application live on GLPipeline (context-
 * scoped, shared). State setters (GPUState / GPUStateMutable) were absorbed
 * into Pipeline in Step 1; GLStateManager::apply_state now delegates to
 * GLPipeline::apply_fixed_state.
 *
 * GLFence and the `to_gl(barrier)` helper remain here since they're GL-
 * specific utilities that don't cleanly fit in `Pipeline`.
 */

#include "gl_state.hh"

#include "gl_context.hh"
#include "gl_framebuffer.hh"
#include "gl_pipeline.hh"
#include "gl_texture.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name GLStateManager (thin shim over GLPipeline)
 * \{ */

GLStateManager::GLStateManager()
{
  /* One-time GL init that used to live in the `GLStateManager` constructor.
   * `GLPipeline::ensure_init` handles the slot + line-width-range setup on
   * first use; here we only need the "never-changing" GL enables. */
  glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
  glEnable(GL_MULTISAMPLE);
  glDisable(GL_DITHER);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  /* Takes precedence over #GL_PRIMITIVE_RESTART.
   * Sets restart index correctly following the IBO type. */
  glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
}

void GLStateManager::apply_state()
{
  GLPipeline::apply_fixed_state();
  GLPipeline::texture_bind_apply();
  GLPipeline::image_bind_apply();

  /* This is needed by gpu_py_offscreen. */
  static_cast<GLFrameBuffer *>(GLContext::get()->active_fb)->apply_state();
}

void GLStateManager::force_state()
{
  GLPipeline::apply_fixed_state();
}

void GLStateManager::issue_barrier(GPUBarrier barrier_bits)
{
  glMemoryBarrier(to_gl(barrier_bits));
}

void GLStateManager::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  GLPipeline::bind_texture(tex, sampler, unit);
}

void GLStateManager::texture_bind_temp(GLTexture *tex)
{
  GLPipeline::texture_bind_temp(tex);
}

void GLStateManager::texture_unbind(Texture *tex)
{
  GLPipeline::unbind_texture(tex);
}

void GLStateManager::texture_unbind_all()
{
  GLPipeline::unbind_all_textures();
}

void GLStateManager::image_bind(Texture *tex, int unit)
{
  GLPipeline::bind_image(tex, unit);
}

void GLStateManager::image_unbind(Texture *tex)
{
  GLPipeline::unbind_image(tex);
}

void GLStateManager::image_unbind_all()
{
  GLPipeline::unbind_all_images();
}

uint64_t GLStateManager::bound_texture_slots()
{
  return GLPipeline::bound_texture_slots();
}

uint8_t GLStateManager::bound_image_slots()
{
  return GLPipeline::bound_image_slots();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GLFence
 * \{ */

GLFence::~GLFence()
{
  if (gl_sync_ != nullptr) {
    glDeleteSync(gl_sync_);
    gl_sync_ = nullptr;
  }
}

void GLFence::signal()
{
  if (gl_sync_) {
    glDeleteSync(gl_sync_);
  }
  gl_sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  signalled_ = true;
}

void GLFence::wait()
{
  if (gl_sync_ == nullptr) {
    return;
  }
  glWaitSync(gl_sync_, 0, GL_TIMEOUT_IGNORED);
  signalled_ = false;
}

/** \} */

}  // namespace blender::gpu
