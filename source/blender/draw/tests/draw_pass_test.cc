/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix.hh"
#include "GPU_context.hh"

#include "draw_cache.hh"
#include "draw_manager.hh"
#include "draw_pass.hh"
#include "draw_shader.hh"
#include "draw_testing.hh"

#include <bitset>

namespace blender::draw {

static void test_draw_pass_all_commands()
{
  Texture tex;
  tex.ensure_2d(gpu::TextureFormat::UNORM_16_16_16_16, int2(1));

  UniformBuffer<uint4> ubo;
  ubo.push_update();

  StorageBuffer<uint4> ssbo;
  ssbo.push_update();

  /* Won't be dereferenced. */
  gpu::VertBuf *vbo = reinterpret_cast<gpu::VertBuf *>(1);
  gpu::IndexBuf *ibo = reinterpret_cast<gpu::IndexBuf *>(1);
  gpu::FrameBuffer *fb = nullptr;

  float4 color(1.0f, 1.0f, 1.0f, 0.0f);
  int3 dispatch_size(1);

  PassSimple pass = {"test.all_commands"};
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_STENCIL);
  pass.clear_color_depth_stencil(float4(0.25f, 0.5f, 100.0f, -2000.0f), 0.5f, 0xF0);
  pass.state_stencil(0x80, 0x0F, 0x8F);
  gpu::Shader *sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR);
  const int color_location = GPU_shader_get_uniform(sh, "color");
  const int mvp_location = GPU_shader_get_uniform(sh, "ModelViewProjectionMatrix");
  pass.shader_set(sh);
  pass.framebuffer_set(&fb);
  pass.subpass_transition(GPU_ATTACHMENT_IGNORE, {GPU_ATTACHMENT_WRITE, GPU_ATTACHMENT_READ});
  pass.bind_texture("image", tex);
  pass.bind_texture("image", &tex);
  pass.bind_image("missing_image", tex);       /* Should not crash. */
  pass.bind_image("missing_image", &tex);      /* Should not crash. */
  pass.bind_ubo("missing_ubo", ubo);           /* Should not crash. */
  pass.bind_ubo("missing_ubo", &ubo);          /* Should not crash. */
  pass.bind_ssbo("missing_ssbo", ssbo);        /* Should not crash. */
  pass.bind_ssbo("missing_ssbo", &ssbo);       /* Should not crash. */
  pass.bind_ssbo("missing_vbo_as_ssbo", vbo);  /* Should not crash. */
  pass.bind_ssbo("missing_vbo_as_ssbo", &vbo); /* Should not crash. */
  pass.bind_ssbo("missing_ibo_as_ssbo", ibo);  /* Should not crash. */
  pass.bind_ssbo("missing_ibo_as_ssbo", &ibo); /* Should not crash. */
  pass.push_constant("color", color);
  pass.push_constant("color", &color);
  pass.push_constant("ModelViewProjectionMatrix", float4x4::identity());
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* Should not crash even if shader is not a compute. This is because we only serialize. */
  /* TODO(fclem): Use real compute shader. */
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR));
  pass.dispatch(dispatch_size);
  pass.dispatch(&dispatch_size);
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);

  /* Change references. */
  color[3] = 1.0f;
  dispatch_size = int3(2);

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.all_commands" << std::endl;
  expected << "  .state_set(2147483654)" << std::endl;
  expected << "  .clear(color=(0.25, 0.5, 100, -2000), depth=0.5, stencil=0b11110000))"
           << std::endl;
  expected
      << "  .stencil_set(write_mask=0b10000000, reference=0b00001111, compare_mask=0b10001111)"
      << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_color)" << std::endl;
  expected << "  .framebuffer_bind(nullptr)" << std::endl;
  expected << "  .subpass_transition(" << std::endl;
  expected << "depth=ignore," << std::endl;
  expected << "color0=write," << std::endl;
  expected << "color1=read," << std::endl;
  expected << "color2=ignore," << std::endl;
  expected << "color3=ignore," << std::endl;
  expected << "color4=ignore," << std::endl;
  expected << "color5=ignore," << std::endl;
  expected << "color6=ignore," << std::endl;
  expected << "color7=ignore" << std::endl;
  expected << ")" << std::endl;
  expected << "  .bind_texture(0, sampler=internal)" << std::endl;
  expected << "  .bind_texture_ref(0, sampler=internal)" << std::endl;
  expected << "  .bind_image(-1)" << std::endl;
  expected << "  .bind_image_ref(-1)" << std::endl;
  expected << "  .bind_uniform_buf(-1)" << std::endl;
  expected << "  .bind_uniform_buf_ref(-1)" << std::endl;
  expected << "  .bind_storage_buf(-1)" << std::endl;
  expected << "  .bind_storage_buf_ref(-1)" << std::endl;
  expected << "  .bind_vertbuf_as_ssbo(-1)" << std::endl;
  expected << "  .bind_vertbuf_as_ssbo_ref(-1)" << std::endl;
  expected << "  .bind_indexbuf_as_ssbo(-1)" << std::endl;
  expected << "  .bind_indexbuf_as_ssbo_ref(-1)" << std::endl;
  expected << "  .push_constant(" << color_location << ", data=(1, 1, 1, 0))" << std::endl;
  expected << "  .push_constant(" << color_location << ", data=(1, 1, 1, 1))" << std::endl;
  expected << "  .push_constant(" << mvp_location << ", data=(" << std::endl;
  expected << "(1, 0, 0, 0)," << std::endl;
  expected << "(0, 1, 0, 0)," << std::endl;
  expected << "(0, 0, 1, 0)," << std::endl;
  expected << "(0, 0, 0, 1)" << std::endl;
  expected << ")" << std::endl;
  expected << ")" << std::endl;
  expected << "  .draw(inst_len=1, vert_len=3, vert_first=0, res_id=0)" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_color)" << std::endl;
  expected << "  .dispatch(1, 1, 1)" << std::endl;
  expected << "  .dispatch_ref(2, 2, 2)" << std::endl;
  expected << "  .barrier(2)" << std::endl;

  EXPECT_EQ(result, expected.str());
}
DRAW_TEST(draw_pass_all_commands)

static void test_draw_pass_sub_ordering()
{
  PassSimple pass = {"test.sub_ordering"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR));
  pass.push_constant("test_pass", 1);

  PassSimple::Sub &sub1 = pass.sub("Sub1");
  sub1.push_constant("test_sub1", 11);

  PassSimple::Sub &sub2 = pass.sub("Sub2");
  sub2.push_constant("test_sub2", 21);

  /* Will execute after both sub. */
  pass.push_constant("test_pass", 2);

  /* Will execute after sub1. */
  sub2.push_constant("test_sub2", 22);

  /* Will execute before sub2. */
  sub1.push_constant("test_sub1", 12);

  /* Will execute before end of pass. */
  sub2.push_constant("test_sub2", 23);

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.sub_ordering" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_color)" << std::endl;
  expected << "  .push_constant(-1, data=1)" << std::endl;
  expected << "  .Sub1" << std::endl;
  expected << "    .push_constant(-1, data=11)" << std::endl;
  expected << "    .push_constant(-1, data=12)" << std::endl;
  expected << "  .Sub2" << std::endl;
  expected << "    .push_constant(-1, data=21)" << std::endl;
  expected << "    .push_constant(-1, data=22)" << std::endl;
  expected << "    .push_constant(-1, data=23)" << std::endl;
  expected << "  .push_constant(-1, data=2)" << std::endl;

  EXPECT_EQ(result, expected.str());
}
DRAW_TEST(draw_pass_sub_ordering)

static void test_draw_pass_simple_draw()
{
  PassSimple pass = {"test.simple_draw"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR));
  /* Each draw procedural type uses a different batch. Groups are drawn in correct order. */
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 10, 1, {1});
  pass.draw_procedural(GPU_PRIM_POINTS, 4, 20, 2, {2});
  pass.draw_procedural(GPU_PRIM_TRIS, 2, 30, 3, {3});
  pass.draw_procedural(GPU_PRIM_POINTS, 5, 40, 4, ResourceID(4, true));
  pass.draw_procedural(GPU_PRIM_LINES, 1, 50, 5, {5});
  pass.draw_procedural(GPU_PRIM_POINTS, 6, 60, 6, {5});
  pass.draw_procedural(GPU_PRIM_TRIS, 3, 70, 7, {6});

  PassSimple::Sub &sub = pass.sub("sub");
  sub.draw_procedural(GPU_PRIM_TRIS, 3, 80, 8, {8});

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.simple_draw" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_color)" << std::endl;
  expected << "  .draw(inst_len=1, vert_len=10, vert_first=1, res_id=1)" << std::endl;
  expected << "  .draw(inst_len=4, vert_len=20, vert_first=2, res_id=2)" << std::endl;
  expected << "  .draw(inst_len=2, vert_len=30, vert_first=3, res_id=3)" << std::endl;
  expected << "  .draw(inst_len=5, vert_len=40, vert_first=4, res_id=4)" << std::endl;
  expected << "  .draw(inst_len=1, vert_len=50, vert_first=5, res_id=5)" << std::endl;
  expected << "  .draw(inst_len=6, vert_len=60, vert_first=6, res_id=5)" << std::endl;
  expected << "  .draw(inst_len=3, vert_len=70, vert_first=7, res_id=6)" << std::endl;
  expected << "  .sub" << std::endl;
  expected << "    .draw(inst_len=3, vert_len=80, vert_first=8, res_id=8)" << std::endl;

  EXPECT_EQ(result, expected.str());
}
DRAW_TEST(draw_pass_simple_draw)

static void test_draw_pass_multi_draw()
{
  PassMain pass = {"test.multi_draw"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR));
  /* Each draw procedural type uses a different batch. Groups are drawn in reverse order. */
  pass.draw_procedural(GPU_PRIM_TRIS, 1, -1, -1, {1});
  pass.draw_procedural(GPU_PRIM_POINTS, 4, -1, -1, {2});
  pass.draw_procedural(GPU_PRIM_TRIS, 2, -1, -1, {3});
  pass.draw_procedural(GPU_PRIM_POINTS, 5, -1, -1, ResourceID(4, true));
  pass.draw_procedural(GPU_PRIM_LINES, 1, -1, -1, {5});
  pass.draw_procedural(GPU_PRIM_POINTS, 6, -1, -1, {5});
  pass.draw_procedural(GPU_PRIM_TRIS, 3, -1, -1, {6});
  /* Custom calls should use their own group and never be batched. */
  pass.draw_procedural(GPU_PRIM_TRIS, 2, 2, 2, {7});
  pass.draw_procedural(GPU_PRIM_TRIS, 2, 2, 2, {8});

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.multi_draw" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_color)" << std::endl;
  expected << "  .draw_multi(5)" << std::endl;
  expected << "    .group(id=4, len=2)" << std::endl;
  expected << "      .proto(instance_len=2, resource_id=8, front_face)" << std::endl;
  expected << "    .group(id=3, len=2)" << std::endl;
  expected << "      .proto(instance_len=2, resource_id=7, front_face)" << std::endl;
  expected << "    .group(id=2, len=1)" << std::endl;
  expected << "      .proto(instance_len=1, resource_id=5, front_face)" << std::endl;
  expected << "    .group(id=1, len=15)" << std::endl;
  expected << "      .proto(instance_len=5, resource_id=4, back_face)" << std::endl;
  expected << "      .proto(instance_len=6, resource_id=5, front_face)" << std::endl;
  expected << "      .proto(instance_len=4, resource_id=2, front_face)" << std::endl;
  expected << "    .group(id=0, len=6)" << std::endl;
  expected << "      .proto(instance_len=3, resource_id=6, front_face)" << std::endl;
  expected << "      .proto(instance_len=2, resource_id=3, front_face)" << std::endl;
  expected << "      .proto(instance_len=1, resource_id=1, front_face)" << std::endl;

  EXPECT_EQ(result, expected.str());
}
DRAW_TEST(draw_pass_multi_draw)

static void test_draw_pass_sortable()
{
  PassSortable pass = {"test.sortable"};
  pass.init();

  pass.sub("Sub3", 3.0f);
  pass.sub("Sub2", 2.0f);
  pass.sub("Sub5", 4.0f);
  pass.sub("Sub4", 3.0f);
  pass.sub("Sub1", 1.0f);

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.sortable" << std::endl;
  expected << "  .Sub1" << std::endl;
  expected << "  .Sub2" << std::endl;
  expected << "  .Sub3" << std::endl;
  expected << "  .Sub4" << std::endl;
  expected << "  .Sub5" << std::endl;

  EXPECT_EQ(result, expected.str());
}
DRAW_TEST(draw_pass_sortable)


static void test_meshlet_requested_batch()
{
  /* A batch in the `DRW_batch_request` state has no geometry: wrapping it must not read its
   * draw parameters, and the meshlet stays empty. */
  {
    Mesh mesh(GPU_batch_calloc());
    const Span<DrawMeshlet> meshlets = mesh.meshlets();
    EXPECT_EQ(meshlets.size(), 1);
    EXPECT_EQ(meshlets[0].vertex_count, 0);
  }

  /* Once the batch carries geometry, `meshlet_resync` reads its draw parameters. */
  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, 8);
  GPUIndexBufBuilder ibo_builder;
  GPU_indexbuf_init(&ibo_builder, GPU_PRIM_TRIS, 2, 8);
  for (uint i = 0; i < 6; i++) {
    GPU_indexbuf_add_generic_vert(&ibo_builder, i);
  }
  gpu::IndexBuf *ibo = GPU_indexbuf_build(&ibo_builder);
  gpu::Batch *batch = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
  {
    Mesh mesh(batch, /*owns_batch=*/false);
    mesh.meshlet_resync(float4(1.0f, 2.0f, 3.0f, 4.0f));
    const Span<DrawMeshlet> meshlets = mesh.meshlets();
    EXPECT_EQ(meshlets[0].vertex_start, 0);
    EXPECT_EQ(meshlets[0].vertex_count, 6);
    EXPECT_EQ(meshlets[0].base_index, 0);
    /* The meshlet spans the whole object, so every axis stores the full quantized range. */
    EXPECT_EQ(meshlets[0].packed_bounds, 0x3E0F83E0u);
  }
  GPU_batch_discard(batch);

  /* A procedural draw-mesh has no geometry of its own, so its meshlet is empty. */
  Mesh mesh_procedural(GPU_PRIM_TRIS);
  const Span<DrawMeshlet> meshlets = mesh_procedural.meshlets();
  EXPECT_EQ(meshlets.size(), 1);
  EXPECT_EQ(meshlets[0].vertex_count, 0);
}
DRAW_TEST(meshlet_requested_batch)

static gpu::Batch *make_meshlet_test_batch(int vertex_count)
{
  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, 8);
  GPUIndexBufBuilder ibo_builder;
  GPU_indexbuf_init(&ibo_builder, GPU_PRIM_TRIS, vertex_count / 3, 8);
  for (uint i = 0; i < vertex_count; i++) {
    GPU_indexbuf_add_generic_vert(&ibo_builder, i);
  }
  gpu::IndexBuf *ibo = GPU_indexbuf_build(&ibo_builder);
  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

static void test_meshlet_register_dedup()
{
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);
  float4x4 obmat = math::from_scale<float4x4>(float3(0.5f));

  gpu::Batch *batch_a = make_meshlet_test_batch(6);
  gpu::Batch *batch_b = make_meshlet_test_batch(3);

  Manager drw;
  gpu::Shader *sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_UNIFORM_COLOR);

  /* The mesh wrappers are the frame-to-frame identity the dedup rule relies on, as in the
   * caches that reuse them. */
  Mesh mesh_a(batch_a, /*owns_batch=*/false);
  mesh_a.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));
  Mesh mesh_b(batch_b, /*owns_batch=*/false);
  mesh_b.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));

  /* Frame 1: mesh A drawn by two resources. Its meshlet is stored once, and both resources
   * point at the same dense range. */
  {
    drw.begin_sync();
    ResourceHandleRange handle_a = drw.resource_handle(obmat);
    ResourceHandleRange handle_b = drw.resource_handle(obmat);
    drw.end_sync();

    PassSimple pass = {"test.meshlet_dedup"};
    pass.init();
    pass.shader_set(sh);
    pass.draw(&mesh_a, handle_a);
    pass.draw(&mesh_a, handle_b);

    drw.submit(pass);

    Manager::DataDebugOutput data = drw.data_debug();
    EXPECT_EQ(data.meshlets.size(), 1);
    EXPECT_EQ(data.meshlets[0].vertex_count, 6);
    const uint id_a = static_cast<ResourceHandle>(handle_a).index();
    const uint id_b = static_cast<ResourceHandle>(handle_b).index();
    EXPECT_EQ(data.meshlet_objects[id_a].meshlet_offset, 0);
    EXPECT_EQ(data.meshlet_objects[id_a].meshlet_count, 1);
    EXPECT_EQ(data.meshlet_objects[id_b].meshlet_offset, 0);
    EXPECT_EQ(data.meshlet_objects[id_b].meshlet_count, 1);
  }

  /* Frame 2: the dense array does not accumulate across frames — mesh A re-registers in slot 0
   * and mesh B takes slot 1 — and each resource points at its own mesh's range. */
  {
    drw.begin_sync();
    ResourceHandleRange handle_a = drw.resource_handle(obmat);
    ResourceHandleRange handle_b = drw.resource_handle(obmat);
    drw.end_sync();

    PassSimple pass = {"test.meshlet_dedup"};
    pass.init();
    pass.shader_set(sh);
    pass.draw(&mesh_a, handle_a);
    pass.draw(&mesh_b, handle_b);

    drw.submit(pass);

    Manager::DataDebugOutput data = drw.data_debug();
    EXPECT_EQ(data.meshlets.size(), 2);
    EXPECT_EQ(data.meshlets[0].vertex_count, 6);
    EXPECT_EQ(data.meshlets[1].vertex_count, 3);
    const uint id_a = static_cast<ResourceHandle>(handle_a).index();
    const uint id_b = static_cast<ResourceHandle>(handle_b).index();
    EXPECT_EQ(data.meshlet_objects[id_a].meshlet_offset, 0);
    EXPECT_EQ(data.meshlet_objects[id_a].meshlet_count, 1);
    EXPECT_EQ(data.meshlet_objects[id_b].meshlet_offset, 1);
    EXPECT_EQ(data.meshlet_objects[id_b].meshlet_count, 1);
  }

  GPU_batch_discard(batch_a);
  GPU_batch_discard(batch_b);
  DRW_shaders_free();
}
DRAW_TEST(meshlet_register_dedup)
static void test_draw_resource_id_gen()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);

  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  Manager drw;

  float4x4 obmat_1 = math::from_scale<float4x4>(float3(-0.5f));
  float4x4 obmat_2 = math::from_scale<float4x4>(float3(0.5f));

  drw.begin_sync();
  ResourceHandleRange handle1 = drw.resource_handle(obmat_1);
  ResourceHandleRange handle2 = drw.resource_handle(obmat_1);
  ResourceHandleRange handle3 = drw.resource_handle(obmat_2);
  drw.resource_handle(obmat_2, float3(2), float3(1));
  drw.end_sync();

  {
    /* Computed on CPU. */
    PassSimple pass = {"test.resource_id"};
    pass.init();
    pass.shader_set(
        GPU_shader_get_builtin_shader(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA));
    pass.draw_procedural(GPU_PRIM_TRIS, 1, -1, -1, handle2);
    pass.draw_procedural(GPU_PRIM_POINTS, 4, -1, -1, handle1);
    pass.draw_procedural(GPU_PRIM_TRIS, 2, -1, -1, handle3);
    pass.draw_procedural(GPU_PRIM_POINTS, 5, -1, -1, handle1);
    pass.draw_procedural(GPU_PRIM_LINES, 1, -1, -1, handle3);
    pass.draw_procedural(GPU_PRIM_POINTS, 6, -1, -1, handle2);
    pass.draw_procedural(GPU_PRIM_TRIS, 3, -1, -1, handle1);

    Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

    std::stringstream result;
    for (auto val : debug.resource_id) {
      result << val << " ";
    }

    StringRefNull expected_simple = "0 2 1 1 1 1 3 3 1 1 1 1 1 3 2 2 2 2 2 2 1 1 1 ";
    EXPECT_EQ(result.str(), expected_simple);
  }

  {
    /* Same thing with PassMain (computed on GPU) */
    PassMain pass = {"test.resource_id"};
    pass.init();
    pass.shader_set(
        GPU_shader_get_builtin_shader(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA));
    pass.draw_procedural(GPU_PRIM_TRIS, 1, -1, -1, handle2);
    pass.draw_procedural(GPU_PRIM_POINTS, 4, -1, -1, handle1);
    pass.draw_procedural(GPU_PRIM_TRIS, 2, -1, -1, handle3);
    pass.draw_procedural(GPU_PRIM_POINTS, 5, -1, -1, handle1);
    pass.draw_procedural(GPU_PRIM_LINES, 1, -1, -1, handle3);
    pass.draw_procedural(GPU_PRIM_POINTS, 6, -1, -1, handle2);
    pass.draw_procedural(GPU_PRIM_TRIS, 3, -1, -1, handle1);

    Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

    std::stringstream result;
    for (auto val : debug.resource_id) {
      result << val << " ";
    }

    /* When using PassMain the handles are sorted based on their handles and GPUBatches. Different
     * primitives use different batches.
     */
    StringRefNull expected_main = "2 3 3 1 1 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 3 ";
    EXPECT_EQ(result.str(), expected_main);
  }

  GPU_render_end();
  DRW_shaders_free();
}
DRAW_TEST(draw_resource_id_gen)

static void test_draw_visibility()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);

  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  Manager drw;

  float4x4 obmat_1 = math::from_scale<float4x4>(float3(-0.5f));
  float4x4 obmat_2 = math::from_scale<float4x4>(float3(0.5f));

  drw.begin_sync();                                   /* Default {0} always visible. */
  drw.resource_handle(obmat_1);                       /* No bounds, always visible. */
  drw.resource_handle(obmat_1, float3(7), float3(1)); /* Out of view. */
  drw.resource_handle(obmat_2, float3(0), float3(1)); /* Inside view. */
  drw.end_sync();

  Texture tex;
  tex.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1));

  PassMain pass = {"test.visibility"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR));
  pass.bind_texture("image", tex);
  pass.draw_procedural(GPU_PRIM_TRIS, 1, -1);

  Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);
  Vector<uint32_t> expected_visibility = {0};

  std::stringstream result;
  for (auto val : debug.visibility) {
    result << std::bitset<32>(val);
  }

  EXPECT_EQ(result.str(), "11111111111111111111111111111011");

  GPU_render_end();
  DRW_shaders_free();
}
DRAW_TEST(draw_visibility)

static void test_draw_visibility_meshlet()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);

  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  Manager drw;

  gpu::Batch *batch = make_meshlet_test_batch(6);
  Mesh mesh(batch, /*owns_batch=*/false);
  mesh.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));

  float4x4 obmat_1 = math::from_scale<float4x4>(float3(-0.5f));
  float4x4 obmat_2 = math::from_scale<float4x4>(float3(0.5f));

  drw.begin_sync();
  /* Resource 0 (default) is always visible. */
  drw.resource_handle(obmat_1);                                /* Resource 1: no bounds, always visible. */
  ResourceHandleRange handle_in = drw.resource_handle(obmat_2, float3(0), float3(1));  /* Resource 2: in view. */
  ResourceHandleRange handle_out = drw.resource_handle(obmat_1, float3(7), float3(1)); /* Resource 3: out of view. */
  drw.end_sync();

  Texture tex;
  tex.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1));

  PassMain pass = {"test.visibility_meshlet"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR));
  pass.bind_texture("image", tex);
  /* Both meshed resources carry exactly one full-span meshlet, so command generation gates on
   * the per-resource visibility bit: the in-view one survives, the out-of-view one is culled. */
  pass.draw(&mesh, handle_in);
  pass.draw(&mesh, handle_out);

  Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

  /* Resources 0, 1 and 2 visible; resource 3 (out of view) culled. */
  std::stringstream result;
  for (auto val : debug.visibility) {
    result << std::bitset<32>(val);
  }
  EXPECT_EQ(result.str(), "11111111111111111111111111110111");

  /* Both meshed resources were drawn: the in-view one is visible, the out-of-view one is
   * culled (its slot keeps the clear value). The buffer holds one slot per instance, so the
   * culled instance still occupies a slot. */
  EXPECT_EQ(debug.resource_id.size(), 2);
  bool found_visible = false, found_culled = false;
  for (auto val : debug.resource_id) {
    if (val == 2) {
      found_visible = true;
    }
    else if (val == 0xFFFFFFFFu) {
      found_culled = true;
    }
  }
  EXPECT_TRUE(found_visible);
  EXPECT_TRUE(found_culled);

  /* Both resources draw the same mesh, so they share one group (len 2). The matrix-based
   * resource handles are not marked inverted, so both instances are front-facing (back_len 0).
   * The out-of-view resource is culled by its resource bit, so only one instance is binned.
   * With one meshlet per resource the meshlet_max is 1, and the generated commands must be
   * byte-identical to the whole-range layout: one (facing, meshlet) pair per facing, the full
   * vertex range. */
  ASSERT_EQ(debug.commands.size(), 2);
  for (const auto &cmd : debug.commands) {
    EXPECT_EQ(cmd.indexed().vertex_len, 6);
    EXPECT_EQ(cmd.indexed().vertex_first, 0);
    EXPECT_EQ(cmd.indexed().base_index, 0);
  }
  EXPECT_EQ(debug.commands[0].indexed().instance_len, 0);   /* Back-facing (none). */
  EXPECT_EQ(debug.commands[0].indexed().instance_first, 0); /* Back-facing region. */
  EXPECT_EQ(debug.commands[1].indexed().instance_len, 1);   /* Front-facing, culled one skipped. */
  EXPECT_EQ(debug.commands[1].indexed().instance_first, 0); /* Front-facing region. */

  GPU_batch_discard(batch);
  GPU_render_end();
  DRW_shaders_free();
}
DRAW_TEST(draw_visibility_meshlet)

/** Exposes the view's UBOs so the visibility pass can be dispatched manually. */
class CullingTestView : public View {
 public:
  using View::View;
  gpu::UniformBuf **ubo_data()
  {
    return &data_;
  }
  gpu::UniformBuf **ubo_culling()
  {
    return &culling_;
  }
  void upload()
  {
    data_.push_update();
    culling_.push_update();
  }
};

static void test_draw_visibility_split()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  /* Dispatch the visibility pass directly with hand-built buffers (meshlet_stride == 2):
   * - resource 0 is split in two meshlets, one in-frustum and one out-of-frustum;
   * - resource 1 carries a single full-span meshlet and is in view.
   * The split resource must cull its out-of-frustum meshlet; the full-span resource's bit must
   * keep the clear value (1) — its verdict is the per-resource one, no test ran for it. */
  CullingTestView view("test_view");
  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);
  view.sync(float4x4::identity(), win_mat);
  view.upload();

  StorageArrayBuffer<ObjectBounds, 2> bounds_buf = {"bounds_buf"};
  StorageArrayBuffer<uint, 4> visibility_buf = {"visibility_buf"};
  StorageArrayBuffer<DrawMeshlet, 2> meshlet_buf = {"meshlet_buf"};
  StorageArrayBuffer<MeshletObject, 2> meshlet_object_buf = {"meshlet_object_buf"};
  StorageArrayBuffer<ObjectMatrices, 2> matrix_buf = {"matrix_buf"};
  StorageArrayBuffer<uint, 4> meshlet_visibility_buf = {"meshlet_visibility_buf"};

  {
    /* Bounds in the form the resource-finalize pass produces: `bounding_corners` hold the
     * origin, then the side vectors along x, y, z (not raw corner points); `bounding_sphere`
     * holds the center and radius; the inscribed-sphere radius of the quick test lives in
     * `bounding_corners[3].w`. */
    ObjectBounds &bounds_0 = bounds_buf[0];
    /* Box x [0, 4], y/z [-0.5, 0.5]: intersects the frustum, so it is not culled. */
    bounds_0.bounding_corners[0] = float4(0.0f, -0.5f, -0.5f, 0.0f);
    bounds_0.bounding_corners[1] = float4(4.0f, 0.0f, 0.0f, 0.0f);
    bounds_0.bounding_corners[2] = float4(0.0f, 1.0f, 0.0f, 0.0f);
    bounds_0.bounding_corners[3] = float4(0.0f, 0.0f, 1.0f, 0.0f);
    bounds_0._inner_sphere_radius = 1.0f;
    bounds_0.bounding_sphere = float4(2.0f, 0.0f, 0.0f, 2.1213203f);

    /* Resource 1: box [-1, 1]^3, in view. */
    ObjectBounds &bounds_1 = bounds_buf[1];
    bounds_1.bounding_corners[0] = float4(-1.0f, -1.0f, -1.0f, 0.0f);
    bounds_1.bounding_corners[1] = float4(2.0f, 0.0f, 0.0f, 0.0f);
    bounds_1.bounding_corners[2] = float4(0.0f, 2.0f, 0.0f, 0.0f);
    bounds_1.bounding_corners[3] = float4(0.0f, 0.0f, 2.0f, 0.0f);
    bounds_1._inner_sphere_radius = 2.0f;
    bounds_1.bounding_sphere = float4(0.0f, 0.0f, 0.0f, 1.7320508f);
    bounds_buf.push_update();
  }
  {
    /* Identity models: object space is world space. */
    matrix_buf[0].model = float4x4::identity();
    matrix_buf[1].model = float4x4::identity();
    matrix_buf.push_update();
  }
  {
    /* Resource 0's meshlets, quantized against its object box x [0, 4]: meshlet 0 inside the
     * frustum, meshlet 1 outside it. */
    const float3 object_min(0.0f, -0.5f, -0.5f);
    const float3 object_max(4.0f, 0.5f, 0.5f);
    DrawMeshlet &m0 = meshlet_buf[0];
    m0.vertex_start = 0;
    m0.vertex_count = 1;
    m0.base_index = 0;
    m0.packed_bounds = drw_meshlet_bounds_pack(float3(0.2f, -0.5f, -0.5f),
                                               float3(0.8f, 0.5f, 0.5f),
                                               object_min,
                                               object_max);
    DrawMeshlet &m1 = meshlet_buf[1];
    m1.vertex_start = 1;
    m1.vertex_count = 1;
    m1.base_index = 0;
    m1.packed_bounds = drw_meshlet_bounds_pack(float3(1.5f, -0.5f, -0.5f),
                                               float3(2.5f, 0.5f, 0.5f),
                                               object_min,
                                               object_max);
    meshlet_buf.push_update();
  }
  {
    meshlet_object_buf[0].bounds_min = float3(0.0f, -0.5f, -0.5f);
    meshlet_object_buf[0].meshlet_offset = 0;
    meshlet_object_buf[0].bounds_max = float3(4.0f, 0.5f, 0.5f);
    meshlet_object_buf[0].meshlet_count = 2;
    /* Full-span resource: one meshlet; its range is never read by the pass. */
    meshlet_object_buf[1].bounds_min = float3(-1.0f, -1.0f, -1.0f);
    meshlet_object_buf[1].meshlet_offset = 0;
    meshlet_object_buf[1].bounds_max = float3(1.0f, 1.0f, 1.0f);
    meshlet_object_buf[1].meshlet_count = 1;
    meshlet_object_buf.push_update();
  }

  GPU_storagebuf_clear(visibility_buf, 0xFFFFFFFFu);
  GPU_storagebuf_clear(meshlet_visibility_buf, 0xFFFFFFFFu);

  PassSimple pass("test.visibility_split");
  pass.init();
  gpu::Shader *sh = GPU_shader_create_from_info_name("draw_visibility_compute");
  pass.shader_set(sh);
  pass.bind_ssbo("bounds_buf", bounds_buf);
  pass.bind_ssbo("visibility_buf", visibility_buf);
  pass.bind_ssbo("meshlet_buf", meshlet_buf);
  pass.bind_ssbo("meshlet_object_buf", meshlet_object_buf);
  pass.bind_ssbo("matrix_buf", matrix_buf);
  pass.bind_ssbo("meshlet_visibility_buf", meshlet_visibility_buf);
  pass.bind_ubo(DRW_VIEW_UBO_SLOT, view.ubo_data());
  pass.bind_ubo(DRW_VIEW_CULLING_UBO_SLOT, view.ubo_culling());
  pass.push_constant("resource_len", 2);
  pass.push_constant("view_len", 1);
  pass.push_constant("visibility_word_per_draw", 1);
  pass.push_constant("meshlet_stride", 2);
  pass.dispatch(int3(1, 1, 1));
  pass.barrier(GPU_BARRIER_SHADER_STORAGE);

  Manager manager;
  manager.submit(pass);

  visibility_buf.read();
  meshlet_visibility_buf.read();

  /* Both resources intersect the frustum, so both per-resource bits survive. */
  EXPECT_EQ(visibility_buf[0] & 0b11u, 0b11u);

  /* Resource 0 (split): the in-frustum meshlet keeps its bit, the out-of-frustum one is
   * masked. Resource 1 (full-span): its bit keeps the clear value — no test ran for it. */
  EXPECT_EQ(meshlet_visibility_buf[0] & 0b1111u, 0b1101u);

  GPU_render_end();
  GPU_shader_unbind();
  GPU_shader_free(sh);
  DRW_shaders_free();
}
DRAW_TEST(draw_visibility_split)

static void test_draw_command_custom_ids()
{
  GPU_render_begin();
  Texture color_attachment;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  Framebuffer framebuffer;
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);

  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  Manager drw;

  gpu::Batch *batch = make_meshlet_test_batch(6);
  Mesh mesh(batch, /*owns_batch=*/false);
  mesh.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));

  float4x4 obmat = math::from_scale<float4x4>(float3(0.5f));
  drw.begin_sync();
  /* No bounds: always visible. */
  ResourceHandleRange handle_1 = drw.resource_handle(obmat);  /* Resource 1. */
  ResourceHandleRange handle_2 = drw.resource_handle(obmat);  /* Resource 2. */
  ResourceHandleRange handle_3 = drw.resource_handle(obmat);  /* Resource 3. */
  drw.end_sync();

  PassMain pass = {"test.command_custom_ids"};
  pass.init();
  pass.use_custom_ids = true;
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_COLOR));
  Texture tex;
  tex.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1));
  pass.bind_texture("image", tex);
  /* Every instance carries its own (resource_id, custom_id) pair. The resource_id buffer
   * must hold the pairs contiguously: slot k is instance k's pair, with no gaps. */
  pass.draw(&mesh, handle_1, 11);
  pass.draw(&mesh, handle_2, 22);
  pass.draw(&mesh, handle_3, 33);

  Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

  /* With custom ids each instance occupies two entries (resource id, custom id), so the span
   * holds 3 pairs. It covers the pair sequence starting at instance zero: that instance's
   * resource id, its custom id, then the next instance's resource id. A stride regression in
   * the pair write leaves the third slot at the clear value, which is caught here. The binning
   * order between instances is not guaranteed, but the first pair is always at instance zero. */
  ASSERT_EQ(debug.resource_id.size(), 6);
  const uint32_t resource_id = debug.resource_id[0];
  uint32_t expected_custom_id;
  if (resource_id == 1) {
    expected_custom_id = 11;
  }
  else if (resource_id == 2) {
    expected_custom_id = 22;
  }
  else if (resource_id == 3) {
    expected_custom_id = 33;
  }
  else {
    expected_custom_id = 0;
  }
  EXPECT_NE(resource_id, 0xFFFFFFFFu);
  EXPECT_EQ(debug.resource_id[1], expected_custom_id);
  EXPECT_NE(debug.resource_id[2], 0xFFFFFFFFu);
  EXPECT_NE(debug.resource_id[2], resource_id);

  /* All three instances bin into the single front-facing region. */
  ASSERT_EQ(debug.commands.size(), 2);
  EXPECT_EQ(debug.commands[0].indexed().instance_len, 0);
  EXPECT_EQ(debug.commands[0].indexed().instance_first, 0);
  EXPECT_EQ(debug.commands[1].indexed().instance_len, 3);
  EXPECT_EQ(debug.commands[1].indexed().instance_first, 0);

  GPU_batch_discard(batch);
  GPU_render_end();
  DRW_shaders_free();
}
DRAW_TEST(draw_command_custom_ids)

static gpu::Batch *make_screen_triangle_batch()
{
  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, 3);
  /* A single triangle covering the whole NDC view. */
  const float3 v0(-1.0f, -1.0f, 0.0f);
  const float3 v1(3.0f, -1.0f, 0.0f);
  const float3 v2(-1.0f, 3.0f, 0.0f);
  GPU_vertbuf_vert_set(vbo, 0, &v0);
  GPU_vertbuf_vert_set(vbo, 1, &v1);
  GPU_vertbuf_vert_set(vbo, 2, &v2);
  GPUIndexBufBuilder ibo_builder;
  GPU_indexbuf_init(&ibo_builder, GPU_PRIM_TRIS, 1, 3);
  GPU_indexbuf_add_tri_verts(&ibo_builder, 0, 1, 2);
  gpu::IndexBuf *ibo = GPU_indexbuf_build(&ibo_builder);
  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

static void test_draw_pass_multidraw_n1()
{
  GPU_render_begin();
  /* `meshlet_max == 1` invariant, end to end: when no resource carries more than one meshlet,
   * the command layout stays the two-commands-per-group whole-range one, and the indirect draw
   * issues through the pre-meshlet single-draw path. Render an indexed batch to a framebuffer
   * and check the pixel: any layout or offset regression in the N=1 path drops the triangle. */
  Texture color_attachment;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32,
                             int2(1),
                             GPU_TEXTURE_USAGE_HOST_READ | GPU_TEXTURE_USAGE_ATTACHMENT |
                             GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
  color_attachment.clear(float4(1.0f, 0.0f, 0.0f, 1.0f));
  Framebuffer framebuffer;
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);
  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  Manager drw;

  gpu::Batch *batch = make_screen_triangle_batch();
  Mesh mesh(batch, /*owns_batch=*/false);
  mesh.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));

  float4x4 obmat = math::from_scale<float4x4>(float3(0.5f));
  drw.begin_sync();
  /* No bounds: always visible. */
  ResourceHandleRange handle = drw.resource_handle(obmat);
  drw.end_sync();

  PassMain pass = {"test.multidraw_n1"};
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR);
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_UNIFORM_COLOR));
  float4 draw_color(0.0f, 1.0f, 0.0f, 1.0f);
  pass.push_constant("color", draw_color);
  pass.push_constant("ModelViewProjectionMatrix", float4x4::identity());
  pass.draw(&mesh, handle);

  Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

  /* One whole-batch meshlet per resource, so `meshlet_max == 1`: the front-facing command is the
   * full vertex range at the group start, byte-identical to the pre-meshlet layout. */
  ASSERT_EQ(debug.commands.size(), 2);
  EXPECT_EQ(debug.commands[0].indexed().instance_len, 0);
  EXPECT_EQ(debug.commands[0].indexed().instance_first, 0);
  EXPECT_EQ(debug.commands[1].indexed().vertex_len, 3);
  EXPECT_EQ(debug.commands[1].indexed().vertex_first, 0);
  EXPECT_EQ(debug.commands[1].indexed().base_index, 0);
  EXPECT_EQ(debug.commands[1].indexed().instance_len, 1);
  EXPECT_EQ(debug.commands[1].indexed().instance_first, 0);

  /* End to end: the indirect draw rendered the triangle over the clear color. */
  float4 *pixel = color_attachment.read<float4>(GPU_DATA_FLOAT);
  EXPECT_FLOAT_EQ(pixel->x, 0.0f);
  EXPECT_FLOAT_EQ(pixel->y, 1.0f);
  EXPECT_FLOAT_EQ(pixel->z, 0.0f);
  MEM_SAFE_DELETE(pixel);

  GPU_batch_discard(batch);
  GPU_render_end();
  DRW_shaders_free();
}
DRAW_TEST(draw_pass_multidraw_n1)

static void test_command_gen_meshlets()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  /* Dispatch `command_gen` directly with hand-built buffers: two groups of one mesh each, two
   * meshlets per resource (meshlet_stride == 2), covering per-meshlet culling, an empty meshlet,
   * and instance compaction of several prototypes into one meshlet command. */
  const uint meshlet_stride = 2u;

  StorageArrayBuffer<DrawGroup, 2> group_buf = {"group_buf"};
  StorageArrayBuffer<DrawPrototype, 3> prototype_buf = {"prototype_buf"};
  StorageArrayBuffer<DrawCommand, 8> command_buf = {"command_buf"};
  StorageArrayBuffer<uint, 4> meshlet_visibility_buf = {"meshlet_visibility_buf"};
  StorageArrayBuffer<DrawMeshlet, 4> meshlet_buf = {"meshlet_buf"};
  StorageArrayBuffer<uint, 4> visibility_buf = {"visibility_buf"};
  StorageArrayBuffer<uint, 8> resource_id_buf = {"resource_id_buf"};
  StorageArrayBuffer<uint, 8> instance_count_buf = {"instance_count_buf"};

  {
    DrawGroup &g0 = group_buf[0];
    g0.next = 1;
    g0.start = 0;
    g0.len = 2;
    g0.front_facing_len = 2;
    g0.vertex_len = 8;
    g0.vertex_first = 0;
    g0.base_index = 0;
    g0.meshlet_offset = 0;
    g0.meshlet_count = 2;
    g0.total_counter = g0.front_facing_counter = g0.back_facing_counter = 0;

    DrawGroup &g1 = group_buf[1];
    g1.next = uint(-1);
    g1.start = 2;
    g1.len = 1;
    g1.front_facing_len = 1;
    g1.base_index = 0;
    g1.meshlet_offset = 2;
    g1.meshlet_count = 2;
    g1.total_counter = g1.front_facing_counter = g1.back_facing_counter = 0;
    group_buf.push_update();
  }
  {
    /* Group 0: two front-facing instances of resource 0. Group 1: one of resource 1. */
    DrawPrototype &p0 = prototype_buf[0];
    p0.group_id = 0;
    p0.res_id = 0;
    p0.custom_id = 0;
    p0.instance_len = 1;
    DrawPrototype &p1 = prototype_buf[1];
    p1.group_id = 0;
    p1.res_id = 0;
    p1.custom_id = 0;
    p1.instance_len = 1;
    DrawPrototype &p2 = prototype_buf[2];
    p2.group_id = 1;
    p2.res_id = 1;
    p2.custom_id = 0;
    p2.instance_len = 1;
    prototype_buf.push_update();
  }
  {
    DrawMeshlet &m0 = meshlet_buf[0];
    m0.vertex_start = 0;
    m0.vertex_count = 4;
    m0.base_index = 0;
    DrawMeshlet &m1 = meshlet_buf[1];
    m1.vertex_start = 4;
    m1.vertex_count = 4;
    m1.base_index = 0;
    DrawMeshlet &m2 = meshlet_buf[2];
    m2.vertex_start = 0;
    m2.vertex_count = 8;
    m2.base_index = 0;
    DrawMeshlet &m3 = meshlet_buf[3];
    m3.vertex_start = 8;
    m3.vertex_count = 0; /* Empty meshlet: visible bit set, but draws nothing. */
    m3.base_index = 0;
    meshlet_buf.push_update();
  }
  {
    /* One bit per (resource, meshlet), stride 2. Resource 0: meshlet 0 visible, meshlet 1
     * culled. Resource 1: both visible (meshlet 1 is empty). */
    meshlet_visibility_buf[0] = 0b1101u;
    meshlet_visibility_buf.push_update();
  }
  {
    /* Not read on the meshlet path, but bound. */
    visibility_buf[0] = 0xFFFFFFFFu;
    visibility_buf.push_update();
  }

  GPU_storagebuf_clear(command_buf, 0);
  GPU_storagebuf_clear(resource_id_buf, 0xFFFFFFFFu);
  GPU_storagebuf_clear(instance_count_buf, 0);

  PassSimple pass("test.command_gen_meshlets");
  pass.init();
  gpu::Shader *sh = GPU_shader_create_from_info_name("draw_command_generate");
  pass.shader_set(sh);
  pass.bind_ssbo("group_buf", group_buf);
  pass.bind_ssbo("prototype_buf", prototype_buf);
  pass.bind_ssbo("command_buf", command_buf);
  pass.bind_ssbo("meshlet_visibility_buf", meshlet_visibility_buf);
  pass.bind_ssbo("meshlet_buf", meshlet_buf);
  pass.bind_ssbo("visibility_buf", visibility_buf);
  pass.bind_ssbo("resource_id_buf", resource_id_buf);
  pass.bind_ssbo("instance_count_buf", instance_count_buf);
  pass.push_constant("prototype_len", 3);
  pass.push_constant("visibility_word_per_draw", 0);
  pass.push_constant("view_len", 1);
  pass.push_constant("view_shift", 0);
  pass.push_constant("use_custom_ids", false);
  pass.push_constant("meshlet_stride", int(meshlet_stride));
  pass.dispatch(int3(3, 1, 1));
  pass.barrier(GPU_BARRIER_SHADER_STORAGE);

  Manager manager;
  manager.submit(pass);

  command_buf.read();
  resource_id_buf.read();

  /* Layout: group 0 owns slots [0, 4): meshlet 0 front at 0-1, meshlet 1 front at 2-3 (both
   * back-facing regions are empty). Group 1 owns slots [4, 6): meshlet 0 at 4, meshlet 1 at 5.
   * One command per (group, facing, meshlet): [0] g0 back m0, [1] g0 back m1, [2] g0 front m0,
   * [3] g0 front m1, [4] g1 back m0, [5] g1 back m1, [6] g1 front m0, [7] g1 front m1. */
  ASSERT_EQ(command_buf.size(), 8);

  {
    /* Group 0 back-facing: no instances, meshlet ranges still filled. */
    EXPECT_EQ(command_buf[0].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[0].indexed().vertex_first, 0);
    EXPECT_EQ(command_buf[0].indexed().base_index, 0);
    EXPECT_EQ(command_buf[0].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[0].indexed().instance_first, 0);
    EXPECT_EQ(command_buf[1].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[1].indexed().vertex_first, 4);
    EXPECT_EQ(command_buf[1].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[1].indexed().instance_first, 0);
  }
  {
    /* Group 0 front-facing: both instances compact into the meshlet 0 command, the culled
     * meshlet 1 command keeps its meshlet range but draws no instance. */
    EXPECT_EQ(command_buf[2].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[2].indexed().vertex_first, 0);
    EXPECT_EQ(command_buf[2].indexed().base_index, 0);
    EXPECT_EQ(command_buf[2].indexed().instance_len, 2);
    EXPECT_EQ(command_buf[2].indexed().instance_first, 0);
    EXPECT_EQ(command_buf[3].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[3].indexed().vertex_first, 4);
    EXPECT_EQ(command_buf[3].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[3].indexed().instance_first, 2);
  }
  {
    /* Group 1 front-facing: the whole-range meshlet draws its one instance, the empty meshlet
     * draws nothing although its visibility bit is set. */
    EXPECT_EQ(command_buf[4].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[4].indexed().instance_first, 4);
    EXPECT_EQ(command_buf[5].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[5].indexed().instance_first, 4);
    EXPECT_EQ(command_buf[6].indexed().vertex_len, 8);
    EXPECT_EQ(command_buf[6].indexed().vertex_first, 0);
    EXPECT_EQ(command_buf[6].indexed().base_index, 0);
    EXPECT_EQ(command_buf[6].indexed().instance_len, 1);
    EXPECT_EQ(command_buf[6].indexed().instance_first, 4);
    EXPECT_EQ(command_buf[7].indexed().vertex_len, 0);
    EXPECT_EQ(command_buf[7].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[7].indexed().instance_first, 5);
  }

  /* Culled and empty meshlet slots keep the clear value; drawn slots hold the resource id. */
  /* Six slots are used (group 0 owns [0,4), group 1 owns [4,6)); the buffer is padded to a
   * float4-aligned length. */
  EXPECT_EQ(resource_id_buf[0], 0);
  EXPECT_EQ(resource_id_buf[1], 0);
  EXPECT_EQ(resource_id_buf[2], 0xFFFFFFFFu);
  EXPECT_EQ(resource_id_buf[3], 0xFFFFFFFFu);
  EXPECT_EQ(resource_id_buf[4], 1);
  EXPECT_EQ(resource_id_buf[5], 0xFFFFFFFFu);

  GPU_render_end();
  GPU_shader_unbind();
  GPU_shader_free(sh);
  DRW_shaders_free();
}
DRAW_TEST(command_gen_meshlets)
static void test_command_gen_fallback()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  /* Dispatch `command_gen` directly with a resource that has no meshlets (meshlet_count == 0),
   * a non-indexed draw (base_index == -1) and object-level visibility: the fallback path. */
  StorageArrayBuffer<DrawGroup, 1> group_buf = {"group_buf"};
  StorageArrayBuffer<DrawPrototype, 3> prototype_buf = {"prototype_buf"};
  StorageArrayBuffer<DrawCommand, 2> command_buf = {"command_buf"};
  StorageArrayBuffer<uint, 4> meshlet_visibility_buf = {"meshlet_visibility_buf"};
  StorageArrayBuffer<DrawMeshlet, 1> meshlet_buf = {"meshlet_buf"};
  StorageArrayBuffer<uint, 4> visibility_buf = {"visibility_buf"};
  StorageArrayBuffer<uint, 4> resource_id_buf = {"resource_id_buf"};
  StorageArrayBuffer<uint, 4> instance_count_buf = {"instance_count_buf"};

  {
    DrawGroup &g0 = group_buf[0];
    g0.next = uint(-1);
    g0.start = 0;
    g0.len = 4;
    g0.front_facing_len = 4;
    g0.vertex_len = 8;
    g0.vertex_first = 2;
    g0.base_index = -1;
    g0.meshlet_offset = 0;
    g0.meshlet_count = 0;
    g0.total_counter = g0.front_facing_counter = g0.back_facing_counter = 0;
    group_buf.push_update();
  }
  {
    DrawPrototype &p0 = prototype_buf[0];
    p0.group_id = 0;
    p0.res_id = 0;
    p0.custom_id = 0;
    p0.instance_len = 1;
    DrawPrototype &p1 = prototype_buf[1];
    p1.group_id = 0;
    p1.res_id = 0;
    p1.custom_id = 0;
    p1.instance_len = 2;
    DrawPrototype &p2 = prototype_buf[2];
    p2.group_id = 0;
    p2.res_id = 0;
    p2.custom_id = 0;
    p2.instance_len = 1;
    prototype_buf.push_update();
  }
  {
    visibility_buf[0] = 0xFFFFFFFFu;
    visibility_buf.push_update();
  }
  {
    meshlet_visibility_buf[0] = 0u;
    meshlet_visibility_buf.push_update();
    meshlet_buf.push_update();
  }

  GPU_storagebuf_clear(command_buf, 0);
  GPU_storagebuf_clear(resource_id_buf, 0xFFFFFFFFu);
  GPU_storagebuf_clear(instance_count_buf, 0);

  PassSimple pass("test.command_gen_fallback");
  pass.init();
  gpu::Shader *sh = GPU_shader_create_from_info_name("draw_command_generate");
  pass.shader_set(sh);
  pass.bind_ssbo("group_buf", group_buf);
  pass.bind_ssbo("prototype_buf", prototype_buf);
  pass.bind_ssbo("command_buf", command_buf);
  pass.bind_ssbo("meshlet_visibility_buf", meshlet_visibility_buf);
  pass.bind_ssbo("meshlet_buf", meshlet_buf);
  pass.bind_ssbo("visibility_buf", visibility_buf);
  pass.bind_ssbo("resource_id_buf", resource_id_buf);
  pass.bind_ssbo("instance_count_buf", instance_count_buf);
  pass.push_constant("prototype_len", 3);
  pass.push_constant("visibility_word_per_draw", 0);
  pass.push_constant("view_len", 1);
  pass.push_constant("view_shift", 0);
  pass.push_constant("use_custom_ids", false);
  pass.push_constant("meshlet_stride", 1);
  pass.dispatch(int3(3, 1, 1));
  pass.barrier(GPU_BARRIER_SHADER_STORAGE);

  Manager manager;
  manager.submit(pass);

  command_buf.read();
  resource_id_buf.read();

  /* One command per facing (meshlet_stride == 1); all four front-facing instances compact into
   * the front command. */
  EXPECT_EQ(command_buf[0].array().vertex_len, 8);
  EXPECT_EQ(command_buf[0].array().instance_len, 0);
  EXPECT_EQ(command_buf[0].array().vertex_first, 2);
  EXPECT_EQ(command_buf[0].array().instance_first, 0);
  EXPECT_EQ(command_buf[1].array().vertex_len, 8);
  EXPECT_EQ(command_buf[1].array().instance_len, 4);
  EXPECT_EQ(command_buf[1].array().vertex_first, 2);
  EXPECT_EQ(command_buf[1].array().instance_first, 0);

  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(resource_id_buf[i], 0);
  }

  GPU_render_end();
  GPU_shader_unbind();
  GPU_shader_free(sh);
  DRW_shaders_free();
}
DRAW_TEST(command_gen_fallback)

static void test_command_gen_mixed()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  /* Dispatch `command_gen` directly with hand-built buffers (meshlet_stride == 2): one
   * full-span group (a single meshlet) and one split group (two meshlets). The full-span group
   * is gated on the per-resource bit — culled here even though its (stale) meshlet bit is still
   * set — while the split group is gated on its own meshlet bits. */
  const uint meshlet_stride = 2u;

  StorageArrayBuffer<DrawGroup, 2> group_buf = {"group_buf"};
  StorageArrayBuffer<DrawPrototype, 2> prototype_buf = {"prototype_buf"};
  StorageArrayBuffer<DrawCommand, 8> command_buf = {"command_buf"};
  StorageArrayBuffer<uint, 4> meshlet_visibility_buf = {"meshlet_visibility_buf"};
  StorageArrayBuffer<DrawMeshlet, 3> meshlet_buf = {"meshlet_buf"};
  StorageArrayBuffer<uint, 4> visibility_buf = {"visibility_buf"};
  StorageArrayBuffer<uint, 4> resource_id_buf = {"resource_id_buf"};
  StorageArrayBuffer<uint, 8> instance_count_buf = {"instance_count_buf"};

  {
    DrawGroup &g0 = group_buf[0];
    g0.next = 1;
    g0.start = 0;
    g0.len = 1;
    g0.front_facing_len = 1;
    g0.vertex_len = 4;
    g0.vertex_first = 0;
    g0.base_index = 0;
    g0.meshlet_offset = 0;
    g0.meshlet_count = 1; /* Full-span: one meshlet covering the whole batch. */
    g0.total_counter = g0.front_facing_counter = g0.back_facing_counter = 0;

    DrawGroup &g1 = group_buf[1];
    g1.next = uint(-1);
    g1.start = 1;
    g1.len = 1;
    g1.front_facing_len = 1;
    g1.vertex_len = 8;
    g1.vertex_first = 0;
    g1.base_index = 0;
    g1.meshlet_offset = 1;
    g1.meshlet_count = 2; /* Split: two meshlets, culled independently. */
    g1.total_counter = g1.front_facing_counter = g1.back_facing_counter = 0;
    group_buf.push_update();
  }
  {
    DrawPrototype &p0 = prototype_buf[0];
    p0.group_id = 0;
    p0.res_id = 0;
    p0.custom_id = 0;
    p0.instance_len = 1;
    DrawPrototype &p1 = prototype_buf[1];
    p1.group_id = 1;
    p1.res_id = 1;
    p1.custom_id = 0;
    p1.instance_len = 1;
    prototype_buf.push_update();
  }
  {
    DrawMeshlet &m0 = meshlet_buf[0]; /* g0 full-span. */
    m0.vertex_start = 0;
    m0.vertex_count = 4;
    m0.base_index = 0;
    DrawMeshlet &m1 = meshlet_buf[1]; /* g1 meshlet 0. */
    m1.vertex_start = 0;
    m1.vertex_count = 4;
    m1.base_index = 0;
    DrawMeshlet &m2 = meshlet_buf[2]; /* g1 meshlet 1. */
    m2.vertex_start = 4;
    m2.vertex_count = 4;
    m2.base_index = 0;
    meshlet_buf.push_update();
  }
  {
    /* Resource 0 (full-span group): culled — its resource bit is the gate and is cleared,
     * although its meshlet bit (index `0 * 2 + 0`) is still set. Resource 1 (split group):
     * meshlet 0 visible, meshlet 1 culled. */
    meshlet_visibility_buf[0] = 0b0101u;
    meshlet_visibility_buf.push_update();
  }
  {
    visibility_buf[0] = 0b0010u;
    visibility_buf.push_update();
  }

  GPU_storagebuf_clear(command_buf, 0);
  GPU_storagebuf_clear(resource_id_buf, 0xFFFFFFFFu);
  GPU_storagebuf_clear(instance_count_buf, 0);

  PassSimple pass("test.command_gen_mixed");
  pass.init();
  gpu::Shader *sh = GPU_shader_create_from_info_name("draw_command_generate");
  pass.shader_set(sh);
  pass.bind_ssbo("group_buf", group_buf);
  pass.bind_ssbo("prototype_buf", prototype_buf);
  pass.bind_ssbo("command_buf", command_buf);
  pass.bind_ssbo("meshlet_visibility_buf", meshlet_visibility_buf);
  pass.bind_ssbo("meshlet_buf", meshlet_buf);
  pass.bind_ssbo("visibility_buf", visibility_buf);
  pass.bind_ssbo("resource_id_buf", resource_id_buf);
  pass.bind_ssbo("instance_count_buf", instance_count_buf);
  pass.push_constant("prototype_len", 2);
  pass.push_constant("visibility_word_per_draw", 0);
  pass.push_constant("view_len", 1);
  pass.push_constant("view_shift", 0);
  pass.push_constant("use_custom_ids", false);
  pass.push_constant("meshlet_stride", int(meshlet_stride));
  pass.dispatch(int3(2, 1, 1));
  pass.barrier(GPU_BARRIER_SHADER_STORAGE);

  Manager manager;
  manager.submit(pass);

  command_buf.read();
  resource_id_buf.read();

  /* Layout (stride 2): [0] g0 back m0, [1] g0 back m1 (empty), [2] g0 front m0,
   * [3] g0 front m1 (empty), [4] g1 back m0, [5] g1 back m1, [6] g1 front m0, [7] g1 front m1. */
  ASSERT_EQ(command_buf.size(), 8);

  {
    /* Group 0: culled by its resource bit — its (stale) visible meshlet bit must not revive it.
     * The full-range command is still written for its single meshlet, with no instance. */
    EXPECT_EQ(command_buf[0].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[0].indexed().vertex_first, 0);
    EXPECT_EQ(command_buf[0].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[0].indexed().instance_first, 0);
    EXPECT_EQ(command_buf[1].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[2].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[2].indexed().vertex_first, 0);
    EXPECT_EQ(command_buf[2].indexed().base_index, 0);
    EXPECT_EQ(command_buf[2].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[2].indexed().instance_first, 0);
    EXPECT_EQ(command_buf[3].indexed().instance_len, 0);
    EXPECT_EQ(resource_id_buf[0], 0xFFFFFFFFu);
  }
  {
    /* Group 1: the visible meshlet draws its instance in its own region, the culled one keeps
     * its range but draws nothing. */
    EXPECT_EQ(command_buf[4].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[4].indexed().vertex_first, 0);
    EXPECT_EQ(command_buf[4].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[4].indexed().instance_first, 2);
    EXPECT_EQ(command_buf[5].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[5].indexed().vertex_first, 4);
    EXPECT_EQ(command_buf[5].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[5].indexed().instance_first, 2);
    EXPECT_EQ(command_buf[6].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[6].indexed().vertex_first, 0);
    EXPECT_EQ(command_buf[6].indexed().base_index, 0);
    EXPECT_EQ(command_buf[6].indexed().instance_len, 1);
    EXPECT_EQ(command_buf[6].indexed().instance_first, 2);
    EXPECT_EQ(command_buf[7].indexed().vertex_len, 4);
    EXPECT_EQ(command_buf[7].indexed().vertex_first, 4);
    EXPECT_EQ(command_buf[7].indexed().instance_len, 0);
    EXPECT_EQ(command_buf[7].indexed().instance_first, 3);
    EXPECT_EQ(resource_id_buf[2], 1);
    EXPECT_EQ(resource_id_buf[3], 0xFFFFFFFFu);
  }

  GPU_render_end();
  GPU_shader_unbind();
  GPU_shader_free(sh);
  DRW_shaders_free();
}
DRAW_TEST(command_gen_mixed)


static void test_meshlet_multi_mesh_resource()
{
  GPU_render_begin();
  /* One resource drawing two different meshes — the per-material batches of a multi-material
   * object. Each group must draw its own mesh's vertex range; the per-resource slot being
   * re-stamped with the last registered mesh must not clobber the other group's range. */
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);
  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  gpu::Batch *batch_a = make_meshlet_test_batch(6);
  gpu::Batch *batch_b = make_meshlet_test_batch(3);

  Manager drw;
  gpu::Shader *sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_UNIFORM_COLOR);

  /* The mesh wrappers are the frame-to-frame identity the dedup rule relies on. */
  Mesh mesh_a(batch_a, /*owns_batch=*/false);
  mesh_a.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));
  Mesh mesh_b(batch_b, /*owns_batch=*/false);
  mesh_b.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));

  float4x4 obmat = math::from_scale<float4x4>(float3(0.5f));
  drw.begin_sync();
  /* No bounds: always visible. */
  ResourceHandleRange handle = drw.resource_handle(obmat);
  drw.end_sync();

  PassMain pass = {"test.meshlet_multi_mesh_resource"};
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR);
  pass.shader_set(sh);
  pass.push_constant("color", float4(0.0f, 1.0f, 0.0f, 1.0f));
  pass.push_constant("ModelViewProjectionMatrix", float4x4::identity());
  pass.draw(&mesh_a, handle);
  pass.draw(&mesh_b, handle);

  Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

  /* Two groups (one per mesh), two facings, one meshlet each: 2 * 2 * 1 = 4 commands. Layout:
   * [0] mesh_a back, [1] mesh_a front, [2] mesh_b back, [3] mesh_b front. Each front command
   * must carry its own mesh's vertex range, not the last registered mesh's. */
  ASSERT_EQ(debug.commands.size(), 4);
  /* Front-facing mesh_a: its own full batch of 6 vertices. */
  EXPECT_EQ(debug.commands[1].indexed().vertex_len, 6);
  EXPECT_EQ(debug.commands[1].indexed().vertex_first, 0);
  EXPECT_EQ(debug.commands[1].indexed().instance_len, 1);
  /* Front-facing mesh_b: its own full batch of 3 vertices. */
  EXPECT_EQ(debug.commands[3].indexed().vertex_len, 3);
  EXPECT_EQ(debug.commands[3].indexed().vertex_first, 0);
  EXPECT_EQ(debug.commands[3].indexed().instance_len, 1);
  /* Both instances drawn, one per mesh, all under the same resource. */
  const uint res_id = static_cast<ResourceHandle>(handle).index();
  ASSERT_EQ(debug.resource_id.size(), 2);
  EXPECT_EQ(debug.resource_id[0], res_id);
  EXPECT_EQ(debug.resource_id[1], res_id);

  GPU_batch_discard(batch_a);
  GPU_batch_discard(batch_b);
  GPU_render_end();
  DRW_shaders_free();
}
/* Same setup as `test_meshlet_multi_mesh_resource` but the two meshes are recorded into two
 * separate draw commands (a state change splits the command stream), mirroring Workbench
 * material mode where each material slot is its own subpass/command under one resource. */
static void test_meshlet_multi_mesh_two_commands()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);
  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  gpu::Batch *batch_a = make_meshlet_test_batch(6);
  gpu::Batch *batch_b = make_meshlet_test_batch(3);

  Manager drw;
  gpu::Shader *sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_UNIFORM_COLOR);

  Mesh mesh_a(batch_a, /*owns_batch=*/false);
  mesh_a.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));
  Mesh mesh_b(batch_b, /*owns_batch=*/false);
  mesh_b.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));

  float4x4 obmat = math::from_scale<float4x4>(float3(0.5f));
  drw.begin_sync();
  ResourceHandleRange handle = drw.resource_handle(obmat);
  drw.end_sync();

  PassMain pass = {"test.meshlet_multi_mesh_two_commands"};
  pass.init();
  /* Material 0: its own command. */
  pass.state_set(DRW_STATE_WRITE_COLOR);
  pass.shader_set(sh);
  pass.push_constant("color", float4(0.0f, 1.0f, 0.0f, 1.0f));
  pass.push_constant("ModelViewProjectionMatrix", float4x4::identity());
  pass.draw(&mesh_a, handle);
  /* Material 1: a state change forces a second command, same as Workbench's per-material
   * subpass (different render state under the same resource). */
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_CULL_BACK);
  pass.draw(&mesh_b, handle);

  Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

  /* Two groups (one per command), two facings, one meshlet each: 2 * 2 * 1 = 4 commands.
   * Each front command must carry its own mesh's vertex range. */
  ASSERT_EQ(debug.commands.size(), 4);
  uint len_a = 0, len_b = 0;
  for (const auto &cmd : debug.commands) {
    if (cmd.indexed().instance_len == 0) {
      continue;
    }
    /* Identify which mesh each command draws by its vertex_first/len pair. */
    if (cmd.indexed().vertex_len == 6) {
      len_a = cmd.indexed().vertex_len;
    }
    else if (cmd.indexed().vertex_len == 3) {
      len_b = cmd.indexed().vertex_len;
    }
  }
  /* Both materials' batches must be drawn in full: mesh_a (6 verts) and mesh_b (3 verts). */
  EXPECT_EQ(len_a, 6);
  EXPECT_EQ(len_b, 3);

  GPU_batch_discard(batch_a);
  GPU_batch_discard(batch_b);
  GPU_render_end();
  DRW_shaders_free();
}
/* Reproduce the Workbench prepass shape: per-material batches under one resource, with
 * `use_custom_ids` set (the Workbench `MeshPass` enables it). The per-instance slot must carry
 * a valid resource id **and** the per-material `custom_id`; a cleared slot (0xFFFFFFFF) is what
 * the lost material shows in the report. */
static void test_meshlet_multi_mesh_custom_ids()
{
  GPU_render_begin();
  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  float4x4 win_mat;
  orthographic_m4(win_mat.ptr(), -1, 1, -1, 1, -1, 1);
  View view("test_view");
  view.sync(float4x4::identity(), win_mat);

  gpu::Batch *batch_a = make_meshlet_test_batch(6);
  gpu::Batch *batch_b = make_meshlet_test_batch(3);

  Manager drw;
  gpu::Shader *sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_UNIFORM_COLOR);

  Mesh mesh_a(batch_a, /*owns_batch=*/false);
  mesh_a.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));
  Mesh mesh_b(batch_b, /*owns_batch=*/false);
  mesh_b.meshlet_resync(float4(1.0f, 1.0f, 1.0f, 1.0f));

  float4x4 obmat = math::from_scale<float4x4>(float3(0.5f));
  drw.begin_sync();
  ResourceHandleRange handle = drw.resource_handle(obmat);
  drw.end_sync();
  const uint res_id = static_cast<ResourceHandle>(handle).index();

  PassMain pass = {"test.meshlet_multi_mesh_custom_ids"};
  pass.init();
  pass.use_custom_ids = true; /* Workbench `MeshPass` shape. */
  pass.state_set(DRW_STATE_WRITE_COLOR);
  pass.shader_set(sh);
  pass.push_constant("color", float4(0.0f, 1.0f, 0.0f, 1.0f));
  pass.push_constant("ModelViewProjectionMatrix", float4x4::identity());
  /* Per-material `custom_id` (the Workbench material index). */
  pass.draw(&mesh_a, handle, /*custom_id=*/0);
  pass.draw(&mesh_b, handle, /*custom_id=*/1);

  Manager::SubmitDebugOutput debug = drw.submit_debug(pass, view);

  ASSERT_EQ(debug.commands.size(), 4);
  /* Both front commands draw their own mesh's full range. */
  EXPECT_EQ(debug.commands[1].indexed().vertex_len, 6);
  EXPECT_EQ(debug.commands[1].indexed().instance_len, 1);
  EXPECT_EQ(debug.commands[3].indexed().vertex_len, 3);
  EXPECT_EQ(debug.commands[3].indexed().instance_len, 1);

  /* Each material's instance base must point at its own per-instance slot in the two-entry
   * (`use_custom_ids`) buffer: material 0 -> slot 0, material 1 -> slot 1. A base of 2 for
   * material 1 reads out of bounds of the 2-slot buffer, so the shader reads back a cleared slot
   * (the reported lost material). */
  EXPECT_EQ(debug.commands[1].indexed().instance_first, 0u);
  EXPECT_EQ(debug.commands[3].indexed().instance_first, 1u);

  /* Each per-instance slot is [resource_id, custom_id]. Both must be valid; a 0xFFFFFFFF slot is
   * the cleared, never-written state reported for the lost material. */
  ASSERT_EQ(debug.resource_id.size() >= 4, true);
  EXPECT_EQ(debug.resource_id[0], res_id);
  EXPECT_EQ(debug.resource_id[1], 0u);
  /* Locate the second material's slot by its custom_id (1). */
  bool found_b = false;
  for (size_t i = 0; i + 1 < debug.resource_id.size(); i += 2) {
    if (debug.resource_id[i + 1] == 1u) {
      found_b = true;
      EXPECT_EQ(debug.resource_id[i], res_id);
    }
  }
  EXPECT_TRUE(found_b);

  GPU_batch_discard(batch_a);
  GPU_batch_discard(batch_b);
  GPU_render_end();
  DRW_shaders_free();
}
DRAW_TEST(meshlet_multi_mesh_custom_ids)
DRAW_TEST(meshlet_multi_mesh_two_commands)
DRAW_TEST(meshlet_multi_mesh_resource)


static void test_draw_manager_sync()
{
  float4x4 obmat_1 = math::from_scale<float4x4>(float3(-0.5f));
  float4x4 obmat_2 = math::from_scale<float4x4>(float3(0.5f));

  /* TODO find a way to create a minimum object to test resource handle creation on it. */
  Manager drw;

  drw.begin_sync();
  drw.resource_handle(obmat_1);
  drw.resource_handle(obmat_2, float3(2), float3(1));
  drw.end_sync();

  Manager::DataDebugOutput debug = drw.data_debug();

  std::stringstream result;
  for (const auto &val : debug.matrices) {
    result << val;
  }
  for (const auto &val : debug.bounds) {
    result << val;
  }
  for (const auto &val : debug.infos) {
    result << val;
  }

  std::stringstream expected;
  expected << "ObjectMatrices(" << std::endl;
  expected << "model=(" << std::endl;
  expected << "(1, 0, 0, 0)," << std::endl;
  expected << "(0, 1, 0, 0)," << std::endl;
  expected << "(0, 0, 1, 0)," << std::endl;
  expected << "(0, 0, 0, 1)" << std::endl;
  expected << ")" << std::endl;
  expected << ", " << std::endl;
  expected << "model_inverse=(" << std::endl;
  expected << "(1, -0, 0, -0)," << std::endl;
  expected << "(-0, 1, -0, 0)," << std::endl;
  expected << "(0, -0, 1, -0)," << std::endl;
  expected << "(-0, 0, -0, 1)" << std::endl;
  expected << ")" << std::endl;
  expected << ")" << std::endl;
  expected << "ObjectMatrices(" << std::endl;
  expected << "model=(" << std::endl;
  expected << "(-0.5, 0, 0, 0)," << std::endl;
  expected << "(0, -0.5, 0, 0)," << std::endl;
  expected << "(0, 0, -0.5, 0)," << std::endl;
  expected << "(0, 0, 0, 1)" << std::endl;
  expected << ")" << std::endl;
  expected << ", " << std::endl;
  expected << "model_inverse=(" << std::endl;
  expected << "(-2, -0, -0, 0)," << std::endl;
  expected << "(-0, -2, 0, -0)," << std::endl;
  expected << "(-0, 0, -2, 0)," << std::endl;
  expected << "(0, -0, 0, 1)" << std::endl;
  expected << ")" << std::endl;
  expected << ")" << std::endl;
  expected << "ObjectMatrices(" << std::endl;
  expected << "model=(" << std::endl;
  expected << "(0.5, 0, 0, 0)," << std::endl;
  expected << "(0, 0.5, 0, 0)," << std::endl;
  expected << "(0, 0, 0.5, 0)," << std::endl;
  expected << "(0, 0, 0, 1)" << std::endl;
  expected << ")" << std::endl;
  expected << ", " << std::endl;
  expected << "model_inverse=(" << std::endl;
  expected << "(2, -0, 0, -0)," << std::endl;
  expected << "(-0, 2, -0, 0)," << std::endl;
  expected << "(0, -0, 2, -0)," << std::endl;
  expected << "(-0, 0, -0, 1)" << std::endl;
  expected << ")" << std::endl;
  expected << ")" << std::endl;
  expected << "ObjectBounds(skipped)" << std::endl;
  expected << "ObjectBounds(skipped)" << std::endl;
  expected << "ObjectBounds(" << std::endl;
  expected << ".bounding_corners[0](1.5, 0.5, 0.5)" << std::endl;
  expected << ".bounding_corners[1](-1, -0, -0)" << std::endl;
  expected << ".bounding_corners[2](0, 1, 0)" << std::endl;
  expected << ".bounding_corners[3](0, 0, 1)" << std::endl;
  expected << ".sphere=(pos=(1, 1, 1), rad=0.866025" << std::endl;
  expected << ")" << std::endl;
  expected << "ObjectInfos(skipped)" << std::endl;
  expected << "ObjectInfos(skipped)" << std::endl;
  expected << "ObjectInfos(skipped)" << std::endl;

  EXPECT_EQ(result.str(), expected.str());

  DRW_shaders_free();
}
DRAW_TEST(draw_manager_sync)

static void test_draw_submit_only()
{
  float4x4 projmat = math::projection::orthographic(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
  float4x4 viewmat = float4x4::identity();

  Texture color_attachment;
  Framebuffer framebuffer;
  color_attachment.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1));
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_attachment));
  framebuffer.bind();

  Manager manager;
  View view = {"Test"};
  View view_other = {"Test"};
  PassSimple pass = {"Test"};
  PassMain pass_main = {"Test"};
  PassMain pass_manual = {"Test"};

  manager.begin_sync();
  manager.end_sync();
  view.sync(viewmat, projmat);
  view_other.sync(viewmat, projmat);

  /* Add some draws to prevent empty pass optimization. */
  gpu::Shader *sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_UNIFORM_COLOR);
  pass.init();
  pass.shader_set(sh);
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  pass_main.init();
  pass_main.shader_set(sh);
  pass_main.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  pass_manual.init();
  pass_manual.shader_set(sh);
  pass_manual.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* Auto command and visibility computation. */
  manager.submit(pass);
  manager.submit(pass_main, view);

  /* Update manager. */
  manager.begin_sync();
  manager.end_sync();

  /* Auto command and visibility computation. */
  manager.submit(pass);
  manager.submit(pass_main, view);

  /* Update view. */
  view.sync(viewmat, projmat);

  /* Auto command and visibility computation. */
  manager.submit(pass);
  manager.submit(pass_main, view);

  /* Update both. */
  manager.begin_sync();
  manager.end_sync();
  view.sync(viewmat, projmat);

  /* Auto command and visibility computation. */
  manager.submit(pass);
  manager.submit(pass_main, view);

  /* Update both. */
  manager.begin_sync();
  manager.end_sync();
  view.sync(viewmat, projmat);

  {
    /* Manual command and visibility computation. */
    manager.compute_visibility(view);
    manager.generate_commands(pass_manual, view);
    manager.submit_only(pass_manual, view);

    /* Redundant updates. */
    EXPECT_BLI_ASSERT(manager.compute_visibility(view),
                      "Resources did not changed, no need to update");
    EXPECT_BLI_ASSERT(manager.generate_commands(pass_manual, view),
                      "Resources and view did not changed no need to update");
  }
  {
    /* Update view. */
    view.sync(viewmat, projmat);

    /* Submit before visibility. */
    EXPECT_BLI_ASSERT(manager.submit_only(pass_manual, view),
                      "compute_visibility was not called on this view");
    /* Update commands before visibility. */
    EXPECT_BLI_ASSERT(manager.generate_commands(pass_manual, view),
                      "Resources or view changed, but compute_visibility was not called");

    manager.compute_visibility(view);

    /* Submit before command generation. */
    EXPECT_BLI_ASSERT(manager.submit_only(pass_manual, view),
                      "View have changed since last generate_commands");

    manager.generate_commands(pass_manual, view);
    manager.submit_only(pass_manual, view);
  }
  {
    /* Update manager. */
    manager.begin_sync();
    manager.end_sync();

    /* Update commands before visibility. */
    EXPECT_BLI_ASSERT(manager.generate_commands(pass_manual, view),
                      "Resources or view changed, but compute_visibility was not called");
    /* Submit before visibility. */
    EXPECT_BLI_ASSERT(manager.submit_only(pass_manual, view),
                      "Resources changed since last compute_visibility");

    manager.compute_visibility(view);

    /* Submit with stale commands. */
    EXPECT_BLI_ASSERT(manager.submit_only(pass_manual, view),
                      "Resources changed since last generate_command");

    manager.generate_commands(pass_manual, view);
    manager.submit_only(pass_manual, view);
  }
  {
    /* Add some draws to prevent empty pass optimization. */
    pass_manual.init();
    pass_manual.shader_set(sh);
    pass_manual.draw_procedural(GPU_PRIM_TRIS, 1, 3);

    /* Submit before command generation. */
    EXPECT_BLI_ASSERT(manager.submit_only(pass_manual, view),
                      "generate_command was not called on this pass");
    manager.generate_commands(pass_manual, view);
    manager.submit_only(pass_manual, view);
  }
  {
    manager.compute_visibility(view_other);

    /* Submit with a different view before command generation. */
    EXPECT_BLI_ASSERT(manager.submit_only(pass_manual, view_other),
                      "submitting with a different view");
    manager.generate_commands(pass_manual, view_other);
    manager.submit_only(pass_manual, view_other);
  }

  DRW_shaders_free();
}
DRAW_TEST(draw_submit_only)

}  // namespace blender::draw
