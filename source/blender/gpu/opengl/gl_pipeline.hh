/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <array>

#include "epoxy/gl.h"

#include "GPU_pipeline.hh"
#include "GPU_common_types.hh"

// Forward declarations.
class GLFrameBuffer;
namespace blender::gpu {
class GLTexture;
}
namespace blender::gpu {

/**
 * OpenGL pipeline.
 *
 * Uses the unified `Pipeline` API. Fixed-function state lives on the pipeline;
 * GL binding/state application is delegated to the `GLStateManager` (the GL
 * state machine), which owns the slot state + multi-bind fast path. So GL
 * performance is unchanged.
 *
 * `bind`/`unbind` mark the pipeline dirty only when it differs from the
 * context's last-applied snapshot (the old `GLStateManager::current_` diff), so
 * re-binding an already-active pipeline is a no-op. `resolve_*` push the
 * pipeline's state into the context and apply the delta.
 *
 * GL's slot storage (textures/samplers/images) is context-scoped, matching
 * the API (one context at a time). The members below are static so that
 * legacy call sites (GLTexture::update, gl_debug.cc, GLFrameBuffer, etc.) can
 * access them without a pipeline pointer.
 */
class GLPipeline : public Pipeline {
 public:
  /* Context-scoped fixed-function state diff (last-applied snapshot). Kept here
   * (static, shared) to mirror the old GLStateManager::current_ model. */
  static void apply_fixed_state();
  static int get_line_width_range(float &min, float &max);

  /* Slot storage context. */
  static GLuint &texture_ref(int unit)
  {
    return textures_[unit];
  }
  static GLuint &sampler_ref(int unit)
  {
    return samplers_[unit];
  }
  static GLuint &target_ref(int unit)
  {
    return targets_[unit];
  }
  static GLuint &image_ref(int unit)
  {
    return images_[unit];
  }
  static GLenum &image_format_ref(int unit)
  {
    return formats_[unit];
  }
  static uint64_t &dirty_texture_binds_ref()
  {
    return dirty_texture_binds_;
  }
  static uint8_t &dirty_image_binds_ref()
  {
    return dirty_image_binds_;
  }

  /* Bind/unbind entry points (called by legacy `GPU_texture_bind` etc. and by
   * `GLTexture::update` via `texture_bind_temp`). */
  static void bind_texture(Texture *tex, GPUSamplerState sampler_state, int unit);
  static void texture_bind_temp(GLTexture *tex);
  static void unbind_texture(Texture *tex);
  static void unbind_all_textures();
  static void bind_image(Texture *tex, int unit);
  static void unbind_image(Texture *tex);
  static void unbind_all_images();
  static void image_format_set_unit(int unit, TextureWriteFormat format);
  static TextureWriteFormat image_format_get_unit(int unit);
  static int image_format_unit_count()
  {
    return 8;
  }
  static uint64_t bound_texture_slots();
  static uint8_t bound_image_slots();

  /** Return the per-image-unit write formats currently bound. */
  Span<const TextureWriteFormat> image_formats_get() const override
  {
    return Span<const TextureWriteFormat>(image_formats_.data(),
                                          (size_t)GLPipeline::image_format_unit_count());
  }

  /* Apply the pending binds (called from `resolve_draw`). */
  static void texture_bind_apply();
  static void image_bind_apply();

  void bind() override;
  void unbind() override;

  /* Texture / image binding overrides — store into GLPipeline's static slot
   * arrays and mark the descriptor dirty. */
  void texture_bind(Texture *tex, GPUSamplerState sampler, int unit) override;
  void texture_unbind(Texture *tex) override;
  void texture_unbind_all() override;
  void image_bind(Texture *tex, int unit) override;
  void image_unbind(Texture *tex) override;
  void image_unbind_all() override;

 protected:
  void resolve_draw(GPUPrimType prim_type, int64_t vertex_input_key) override;
  void resolve_compute() override;
  void issue_barrier(GPUBarrier barrier_bits) override;

  private:
  /* Ensure static state (line width range, initial `~state` for first diff) is set up.
   * Called before the first fixed-function apply and from the slot accessors. */
  static void ensure_init();

  /* Static (context-scoped) storage. */
  static GLuint targets_[64];
  static GLuint textures_[64];
  static GLuint samplers_[64];
  static uint64_t dirty_texture_binds_;
  static GLuint images_[8];
  static GLenum formats_[8];
  static uint8_t dirty_image_binds_;
  static std::array<TextureWriteFormat, GPU_MAX_IMAGE> image_formats_;
  static GPUState current_;
  static GPUStateMutable current_mutable_;
  static float line_width_range_[2];
  static bool line_width_range_init_;
};

}  // namespace blender::gpu
