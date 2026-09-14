/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "GPU_batch_utils.hh"
#include "testing/testing.h"

#include "gpu_testing.hh"

#include "GPU_batch.hh"
#include "GPU_batch_presets.hh"
#include "GPU_framebuffer.hh"
#include "GPU_pipeline.hh"
#include "GPU_state.hh"

#include "BLI_math_vector.hh"

#include "intern/draw_cache.hh"

namespace blender::gpu::tests {

static void test_pipeline_create_discard()
{
  Pipeline *pipeline = GPU_pipeline_create();
  if (pipeline == nullptr) {
    GTEST_SKIP();
    return;
  }
  EXPECT_EQ(pipeline->shader_get(), nullptr);
  EXPECT_EQ(pipeline->blend_get(), GPU_BLEND_NONE);
  EXPECT_EQ(pipeline->write_mask_get(), GPU_WRITE_COLOR);
  EXPECT_FALSE(pipeline->is_dirty());
  GPU_pipeline_discard(pipeline);
}
GPU_TEST(pipeline_create_discard)

static void test_pipeline_state_flags()
{
  Pipeline *pipeline = GPU_pipeline_create();
  if (pipeline == nullptr) {
    GTEST_SKIP();
    return;
  }

  EXPECT_FALSE(pipeline->is_dirty());

  pipeline->set_blend(GPU_BLEND_ALPHA);
  EXPECT_TRUE(pipeline->is_dirty());
  EXPECT_TRUE(pipeline->is_pipeline_dirty());
  EXPECT_EQ(pipeline->blend_get(), GPU_BLEND_ALPHA);

  pipeline->set_depth_test(GPU_DEPTH_LESS_EQUAL);
  EXPECT_TRUE(pipeline->is_depth_stencil_dirty());

  pipeline->set_face_culling(GPU_CULL_BACK);
  EXPECT_EQ(pipeline->state_get().culling_test, GPU_CULL_BACK);
  EXPECT_TRUE(pipeline->is_depth_stencil_dirty());

  pipeline->uniform_float(0, 1, 1, nullptr);
  EXPECT_TRUE(pipeline->is_push_constants_dirty());

  pipeline->unbind();
  GPU_pipeline_discard(pipeline);
}
GPU_TEST(pipeline_state_flags)

static void test_pipeline_independent_state()
{
  Pipeline *pipeline_a = GPU_pipeline_create();
  if (pipeline_a == nullptr) {
    GTEST_SKIP();
    return;
  }
  Pipeline *pipeline_b = GPU_pipeline_create();

  pipeline_a->state_set(GPU_WRITE_COLOR | GPU_WRITE_ALPHA,
                        GPU_BLEND_ADDITIVE,
                        GPU_CULL_NONE,
                        GPU_DEPTH_NONE,
                        GPU_STENCIL_NONE,
                        GPU_STENCIL_OP_NONE,
                        GPU_VERTEX_LAST);
  pipeline_a->set_point_size(2.0f);
  pipeline_a->set_line_width(3.0f);
  EXPECT_TRUE(pipeline_a->is_dirty());

  /* A second pipeline is independent - it starts from the default state. */
  EXPECT_FALSE(pipeline_b->is_dirty());
  EXPECT_EQ(pipeline_b->blend_get(), GPU_BLEND_NONE);
  EXPECT_EQ(pipeline_b->state_get().write_mask, GPU_WRITE_COLOR);
  EXPECT_FLOAT_EQ(pipeline_b->mutable_state_get().point_size, -1.0f);
  EXPECT_FLOAT_EQ(pipeline_b->mutable_state_get().line_width, 1.0f);

  GPU_pipeline_discard(pipeline_b);
  GPU_pipeline_discard(pipeline_a);
}
GPU_TEST(pipeline_independent_state)

template<GPUBlend blend_type>
void pipeline_blend_test(float4 source_a, float4 source_b, float4 expected_result)
{
  GPUOffScreen *offscreen = GPU_offscreen_create(1,
                                                 1,
                                                 false,
                                                 TextureFormat::SFLOAT_16_16_16_16,
                                                 GPU_TEXTURE_USAGE_ATTACHMENT |
                                                     GPU_TEXTURE_USAGE_HOST_READ,
                                                 false,
                                                 nullptr);
  BLI_assert(offscreen != nullptr);
  GPU_offscreen_bind(offscreen, false);
  gpu::Texture *color_texture = GPU_offscreen_color_texture(offscreen);
  GPU_texture_clear(color_texture, GPU_DATA_FLOAT, source_a);

  Batch *batch = GPU_batch_preset_quad();
  GPU_batch_program_set_builtin(batch, GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_batch_uniform_4fv(batch, "color", source_b);

  Pipeline *pipeline = GPU_pipeline_create();
  if (pipeline == nullptr) {
    GPU_offscreen_unbind(offscreen, false);
    GPU_offscreen_free(offscreen);
    GTEST_SKIP();
    return;
  }
  /* The batch's built-in shader is bound at draw time; the pipeline only
   * carries the fixed-function state. */
  pipeline->set_blend(blend_type);
  pipeline->bind();

  GPU_batch_draw(batch);
  pipeline->unbind();

  GPU_offscreen_unbind(offscreen, false);
  GPU_flush();

  float4 read_back;
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  GPU_offscreen_read_color(offscreen, GPU_DATA_FLOAT, &read_back);
  EXPECT_EQ(read_back, expected_result);

  GPU_offscreen_free(offscreen);

  /* Reset default. */
  GPU_blend(GPU_BLEND_NONE);
}

static void test_pipeline_blend_alpha()
{
  pipeline_blend_test<GPU_BLEND_ALPHA>(float4(1.0f, 0.0f, 1.0f, 1.0f),
                                       float4(0.0f, 1.0f, 0.0f, 0.5f),
                                       float4(0.5f, 0.5f, 0.5f, 1.0f));
}
GPU_TEST(pipeline_blend_alpha)

static void test_pipeline_blend_additive()
{
  pipeline_blend_test<GPU_BLEND_ADDITIVE>(float4(1.0f, 0.0f, 1.0f, 1.0f),
                                          float4(0.0f, 1.0f, 0.0f, 0.5f),
                                          float4(1.0f, 0.5f, 1.0f, 1.0f));
}
GPU_TEST(pipeline_blend_additive)

static void test_pipeline_blend_none()
{
  pipeline_blend_test<GPU_BLEND_NONE>(float4(1.0f, 0.0f, 1.0f, 1.0f),
                                      float4(0.0f, 1.0f, 0.0f, 0.5f),
                                      float4(0.0f, 1.0f, 0.0f, 0.5f));
}
GPU_TEST(pipeline_blend_none)

}  // namespace blender::gpu::tests
