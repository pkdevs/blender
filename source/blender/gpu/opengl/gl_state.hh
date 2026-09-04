/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * GL-specific utilities that used to live alongside the (now-removed)
 * `GLStateManager`: the GL fence and a handful of GPU→GL enum conversions.
 */

#pragma once

#include "MEM_guardedalloc.h"

#include "gpu_state_private.hh"

#include <epoxy/gl.h>

namespace blender::gpu {

/* Fence synchronization primitive. */
class GLFence : public Fence {
 private:
  GLsync gl_sync_ = 0;

 public:
  GLFence() : Fence() {};
  ~GLFence();

  void signal() override;
  void wait() override;

  MEM_CXX_CLASS_ALLOC_FUNCS("GLFence")
};

static inline GLbitfield to_gl(GPUBarrier barrier_bits)
{
  GLbitfield barrier = 0;
  if (barrier_bits & GPU_BARRIER_SHADER_IMAGE_ACCESS) {
    barrier |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_SHADER_STORAGE) {
    barrier |= GL_SHADER_STORAGE_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_TEXTURE_FETCH) {
    barrier |= GL_TEXTURE_FETCH_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_TEXTURE_UPDATE) {
    barrier |= GL_TEXTURE_UPDATE_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_COMMAND) {
    barrier |= GL_COMMAND_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_FRAMEBUFFER) {
    barrier |= GL_FRAMEBUFFER_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_VERTEX_ATTRIB_ARRAY) {
    barrier |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_ELEMENT_ARRAY) {
    barrier |= GL_ELEMENT_ARRAY_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_UNIFORM) {
    barrier |= GL_UNIFORM_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_BUFFER_UPDATE) {
    barrier |= GL_BUFFER_UPDATE_BARRIER_BIT;
  }
  return barrier;
}

}  // namespace blender::gpu
