/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Implementation for `GLFence`. The slot/binding storage + fixed-function
 * apply logic that used to live here is now on `GLPipeline`
 * (see `gl_pipeline.hh` / `gl_pipeline.cc`).
 */

#include "gl_state.hh"

namespace blender::gpu {

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

}  // namespace blender::gpu
