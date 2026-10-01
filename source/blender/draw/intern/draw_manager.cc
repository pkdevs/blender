/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "DNA_userdef_types.h"

#include "BKE_image.hh"
#include "BKE_image_gpu.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLI_math_base_c.hh"
#include "BLI_task.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "draw_common.hh"
#include "draw_context_private.hh"
#include "draw_debug.hh"
#include "draw_defines.hh"
#include "draw_manager.hh"
#include "draw_meshlet.hh"
#include "draw_pass.hh"
#include "draw_shader.hh"

namespace blender::draw {

std::atomic<uint32_t> Manager::global_sync_counter_ = 1;

Manager::~Manager()
{
  for (gpu::Texture *texture : acquired_textures) {
    /* Decrease refcount and free if 0. */
    GPU_texture_free(texture);
  }
}

void Manager::begin_sync(Object *object_active)
{
  /* Add 2 to always have a non-null number even in case of overflow. */
  sync_counter_ = (global_sync_counter_ += 2);

  matrix_buf.swap();
  bounds_buf.swap();
  infos_buf.swap();
  meshlet_buf.swap();
  meshlet_object_buf.swap();

  matrix_buf.current().trim_to_next_power_of_2(resource_len_);
  bounds_buf.current().trim_to_next_power_of_2(resource_len_);
  infos_buf.current().trim_to_next_power_of_2(resource_len_);
  meshlet_buf.current().trim_to_next_power_of_2(meshlet_len_);
  meshlet_object_buf.current().trim_to_next_power_of_2(resource_len_);
  /* Reset meshlet counts; only resources drawn with a mesh (`meshlet_register`) set a non-zero
   * count. Left zero for every other resource so the per-meshlet pass never loops over them. */
  for (uint i = 0; i < meshlet_object_buf.current().size(); i++) {
    meshlet_object_buf.current().data()[i].meshlet_count = 0;
  }
  attributes_buf.trim_to_next_power_of_2(attribute_len_);

  /* TODO: This means the reference is kept until further redraw or manager tear-down. Instead,
   * they should be released after each draw loop. But for now, mimics old DRW behavior. */
  for (gpu::Texture *texture : acquired_textures) {
    /* Decrease refcount and free if 0. */
    GPU_texture_free(texture);
  }

  acquired_textures.clear();
  deferred_textures_.clear();
  layer_attributes.clear();

/* For some reason, if this uninitialized data pattern was enabled (ie release asserts enabled),
 * The viewport just gives up rendering objects on ARM64 devices. Possibly Mesa GLOn12-related. */
#if !defined(NDEBUG) && !defined(_M_ARM64)
  /* Detect uninitialized data. */
  memset(matrix_buf.current().data(),
         0xF0,
         matrix_buf.current().size() * sizeof(*matrix_buf.current().data()));
  memset(bounds_buf.current().data(),
         0xF0,
         matrix_buf.current().size() * sizeof(*bounds_buf.current().data()));
  memset(infos_buf.current().data(),
         0xF0,
         matrix_buf.current().size() * sizeof(*infos_buf.current().data()));
  memset(meshlet_buf.current().data(),
         0xF0,
         meshlet_buf.current().size() * sizeof(*meshlet_buf.current().data()));
  memset(meshlet_object_buf.current().data(),
         0xF0,
         meshlet_object_buf.current().size() * sizeof(*meshlet_object_buf.current().data()));
#endif
  resource_len_ = 0;
  attribute_len_ = 0;
  meshlet_len_ = 0;
  meshlet_count_max_ = 0;
  /* TODO(fclem): Resize buffers if too big, but with an hysteresis threshold. */

  this->object_active = object_active;

  /* Init the 0 resource. */
  resource_handle(float4x4::identity());
}

void Manager::sync_layer_attributes()
{
  /* Sort the attribute IDs - the shaders use binary search. */
  Vector<uint32_t> id_list;

  id_list.reserve(layer_attributes.size());

  for (uint32_t id : layer_attributes.keys()) {
    id_list.append(id);
  }

  std::ranges::sort(id_list);

  /* Look up the attributes. */
  int count = 0, size = layer_attributes_buf.end() - layer_attributes_buf.begin();

  for (uint32_t id : id_list) {
    if (layer_attributes_buf[count].sync(
            drw_get().scene, drw_get().view_layer, layer_attributes.lookup(id)))
    {
      /* Check if the buffer is full. */
      if (++count == size) {
        break;
      }
    }
  }

  layer_attributes_buf[0].buffer_length = count;
}

void Manager::load_deferred_textures()
{
  if (deferred_textures_.is_empty()) {
    return;
  }

  GPU_debug_group_begin("Texture Loading");

  /* Load files from disk in a multithreaded manner. Allow better parallelism. */
  threading::parallel_for(deferred_textures_.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      DeferredTexture &deferred = *deferred_textures_[i];
      BKE_image_get_tile(deferred.image, 0);
      threading::isolate_task([&]() {
        ImBuf *imbuf = BKE_image_acquire_ibuf(deferred.image, deferred.image_user, nullptr);
        BKE_image_release_ibuf(deferred.image, imbuf, nullptr);
      });
    }
  });

  /* Avoid any leftover bind before GPU texture creation which could cause assert
   * about missing specialization constants. */
  GPU_shader_unbind();

  /* Upload to the GPU (create gpu::Texture). This part still requires a valid GPU context and
   * is not easily parallelized. */
  for (std::unique_ptr<DeferredTexture> &deferred : deferred_textures_) {
    const ImageGPUTextures textures = BKE_image_acquire_gpu_material_texture(
        deferred->image, deferred->image_user, deferred->use_tile_mapping, false);
    deferred->texture = textures.texture;
    deferred->tile_mapping = textures.tile_mapping;
    hold_texture(textures.texture);
    hold_texture(textures.tile_mapping);
  }

  GPU_debug_group_end();
}

void Manager::end_sync()
{
  GPU_debug_group_begin("Manager.end_sync");

  load_deferred_textures();

  sync_layer_attributes();

  matrix_buf.current().push_update();
  bounds_buf.current().push_update();
  infos_buf.current().push_update();
  /* The visibility pass reads every resource's `MeshletObject` (for the per-meshlet bit stride),
   * so the GPU copy must be valid even when no meshlet is registered this frame: every resource
   * was initialized with `meshlet_count == 0` above, which keeps the per-meshlet stage out. */
  meshlet_object_buf.current().push_update();
  attributes_buf.push_update();
  layer_attributes_buf.push_update();

  /* Useful for debugging the following resource finalize. But will trigger the drawing of the GPU
   * debug draw/print buffers for every frame. Not nice for performance. */
  // debug_bind();

  DRW_submission_start();

  /* Dispatch compute to finalize the resources on GPU. Save a bit of CPU time. */
  uint thread_groups = divide_ceil_u(resource_len_, DRW_FINALIZE_GROUP_SIZE);
  gpu::Shader *shader = DRW_shader_draw_resource_finalize_get();
  GPU_shader_bind(shader);
  GPU_shader_uniform_1i(shader, "resource_len", resource_len_);
  GPU_storagebuf_bind(matrix_buf.current(), GPU_shader_get_ssbo_binding(shader, "matrix_buf"));
  GPU_storagebuf_bind(bounds_buf.current(), GPU_shader_get_ssbo_binding(shader, "bounds_buf"));
  GPU_storagebuf_bind(infos_buf.current(), GPU_shader_get_ssbo_binding(shader, "infos_buf"));
  GPU_compute_dispatch(shader, thread_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  DRW_submission_end();

  GPU_debug_group_end();
}

void Manager::debug_bind()
{
  gpu::StorageBuf *gpu_buf = DebugDraw::get().gpu_draw_buf_get();
  if (gpu_buf == nullptr) {
    return;
  }
  GPU_storagebuf_bind(gpu_buf, DRW_DEBUG_DRAW_SLOT);
#ifndef DISABLE_DEBUG_SHADER_PRINT_BARRIER
  /* Add a barrier to allow multiple shader writing to the same buffer. */
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
#endif
}

void Manager::resource_bind()
{
  GPU_storagebuf_bind(matrix_buf.current(), DRW_OBJ_MAT_SLOT);
  GPU_storagebuf_bind(infos_buf.current(), DRW_OBJ_INFOS_SLOT);
  GPU_storagebuf_bind(attributes_buf, DRW_OBJ_ATTR_SLOT);
  GPU_uniformbuf_bind(layer_attributes_buf, DRW_LAYER_ATTR_UBO_SLOT);
}

uint64_t Manager::fingerprint_get()
{
  /* Covers a new sync cycle, added/removed resources, and the max meshlet count — the latter
   * is the visibility pass's bit stride, so a split mesh registered after the pass ran must
   * invalidate the view.
   *
   * Exact bit-packing (no hash): 16 bits for the sync cycle, 24 bits per count (16.7M — beyond
   * any scene or sculpt workload). The counter is `+2` per sync (init 1), so its low bit is
   * constant; the field stores the dense event index `(sync_counter_ - 1) / 2`. The counter
   * only needs to differ between two checks of the same view, a few sync events apart;
   * aliasing (65536 sync events with unchanged counts) costs one stale frame, corrected by
   * the next check. */
  return uint64_t(((sync_counter_ - 1u) / 2u) & 0xFFFFu) |
         (uint64_t(resource_len_ & 0xFFFFFFu) << 16) |
         (uint64_t(meshlet_count_max_ & 0xFFFFFFu) << 40);
}

ResourceHandleRange Manager::unique_handle_for_sculpt(const ObjectRef &ref)
{
  if (ref.sculpt_handle_.is_valid()) {
    return ref.sculpt_handle_;
  }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*ref.object);
  const Bounds<float3> bounds = bke::pbvh::bounds_get(pbvh);
  const float3 center = math::midpoint(bounds.min, bounds.max);
  const float3 half_extent = bounds.max - center;
  /* WORKAROUND: Instead of breaking const correctness everywhere, we only break it for this. */
  const_cast<ObjectRef &>(ref).sculpt_handle_ = resource_handle(
      ref, nullptr, &center, &half_extent);
  return ref.sculpt_handle_;
}

void Manager::compute_visibility(View &view)
{
  bool freeze_culling = (USER_DEVELOPER_TOOL_TEST(&U, use_viewport_debug) && drw_get().v3d &&
                         (drw_get().v3d->debug_flag & V3D_DEBUG_FREEZE_CULLING) != 0);

  BLI_assert_msg(view.manager_fingerprint_ != this->fingerprint_get(),
                 "Resources did not changed, no need to update");

  view.manager_fingerprint_ = this->fingerprint_get();

  view.bind();
  view.compute_visibility(bounds_buf.current(),
                          infos_buf.current(),
                          resource_len_,
                          freeze_culling,
                          matrix_buf.current(),
                          meshlet_buf.current(),
                          meshlet_object_buf.current(),
                          max_uu(1, meshlet_count_max_));
}

void Manager::ensure_visibility(View &view)
{
  if (view.manager_fingerprint_ != this->fingerprint_get()) {
    compute_visibility(view);
  }
}

void Manager::generate_commands(PassMain &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  BLI_assert_msg((pass.manager_fingerprint_ != this->fingerprint_get()) ||
                     (pass.view_fingerprint_ != view.fingerprint_get()),
                 "Resources and view did not changed no need to update");
  BLI_assert_msg((view.manager_fingerprint_ == this->fingerprint_get()) &&
                     (view.fingerprint_get() != 0),
                 "Resources or view changed, but compute_visibility was not called");

  pass.manager_fingerprint_ = this->fingerprint_get();
  pass.view_fingerprint_ = view.fingerprint_get();

  pass.draw_commands_buf_.generate_commands(pass.headers_,
                                            pass.commands_,
                                            view.get_visibility_buffer(),
                                            view.visibility_word_per_draw(),
                                            view.view_len_,
                                            pass.use_custom_ids,
                                            meshlet_buf.current(),
                                            view.get_meshlet_visibility_buffer(),
                                            max_uu(1, meshlet_count_max_));
}

void Manager::generate_commands(PassSortable &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  pass.sort();
  generate_commands(static_cast<PassMain &>(pass), view);
}

void Manager::generate_commands(PassSimple &pass)
{
  if (pass.is_empty()) {
    return;
  }

  BLI_assert_msg(pass.manager_fingerprint_ != this->fingerprint_get(),
                 "Resources did not changed since last generate_command, no need to update");
  pass.manager_fingerprint_ = this->fingerprint_get();

  pass.draw_commands_buf_.generate_commands(pass.headers_, pass.commands_, pass.sub_passes_);
}

void Manager::warm_shader_specialization(PassMain &pass)
{
  if (pass.is_empty()) {
    return;
  }
  command::RecordingState state;
  pass.warm_shader_specialization(state);
}

void Manager::warm_shader_specialization(PassSimple &pass)
{
  if (pass.is_empty()) {
    return;
  }
  command::RecordingState state;
  pass.warm_shader_specialization(state);
}

void Manager::submit_only(PassMain &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }
  /* Register the meshes drawn by this pass before any visibility computation reads the meshlet
   * buffers. Idempotent for passes already registered this frame. */
  meshlets_register_pass(pass);

  BLI_assert_msg(view.manager_fingerprint_ != 0, "compute_visibility was not called on this view");
  BLI_assert_msg(view.manager_fingerprint_ == this->fingerprint_get(),
                 "Resources changed since last compute_visibility");
  BLI_assert_msg(pass.manager_fingerprint_ != 0, "generate_command was not called on this pass");
  BLI_assert_msg(pass.manager_fingerprint_ == this->fingerprint_get(),
                 "Resources changed since last generate_command");
  /* The function generate_commands needs to be called for each view this pass is going to be
   * submitted with. This is because the commands are stored inside the pass and not per view. */
  BLI_assert_msg(pass.view_fingerprint_ == view.fingerprint_get(),
                 "View have changed since last generate_commands or "
                 "submitting with a different view");

  debug_bind();

  command::RecordingState state;
  state.inverted_view = view.is_inverted();

  view.bind();
  pass.draw_commands_buf_.bind(state);

  resource_bind();

  pass.submit(state);

  state.cleanup();
}

void Manager::submit(PassMain &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  /* Register the pass meshes before compute_visibility reads the meshlet buffers. */
  meshlets_register_pass(pass);

  if (view.manager_fingerprint_ != this->fingerprint_get()) {
    compute_visibility(view);
  }

  if (pass.manager_fingerprint_ != this->fingerprint_get() ||
      pass.view_fingerprint_ != view.fingerprint_get())
  {
    generate_commands(pass, view);
  }

  this->submit_only(pass, view);
}

void Manager::submit(PassSortable &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  pass.sort();

  this->submit(static_cast<PassMain &>(pass), view);
}

void Manager::submit(PassSimple &pass, bool inverted_view)
{
  if (pass.is_empty()) {
    return;
  }

  meshlets_register_pass(pass);

  if (!pass.has_generated_commands()) {
    generate_commands(pass);
  }

  debug_bind();

  command::RecordingState state;
  state.inverted_view = inverted_view;

  pass.draw_commands_buf_.bind(state);

  resource_bind();

  pass.submit(state);

  state.cleanup();
}

void Manager::submit(PassSimple &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  debug_bind();

  view.bind();

  this->submit(pass, view.is_inverted());
}

Manager::SubmitDebugOutput Manager::submit_debug(PassSimple &pass, View &view)
{
  submit(pass, view);

  pass.draw_commands_buf_.resource_id_buf_.read();

  Manager::SubmitDebugOutput output;
  output.resource_id = {pass.draw_commands_buf_.resource_id_buf_.data(),
                        pass.draw_commands_buf_.resource_id_count_};
  /* There is no command data for PassSimple. */
  output.commands = {nullptr, 0};
  /* There is no visibility data for PassSimple. */
  output.visibility = {static_cast<uint *>(view.get_visibility_buffer().data()), 0};
  return output;
}

Manager::SubmitDebugOutput Manager::submit_debug(PassMain &pass, View &view)
{
  submit(pass, view);

  GPU_memory_barrier(GPU_BARRIER_BUFFER_UPDATE);

  pass.draw_commands_buf_.resource_id_buf_.read();
  view.get_visibility_buffer().read();
  pass.draw_commands_buf_.command_buf_.read();

  Manager::SubmitDebugOutput output;
  output.resource_id = {pass.draw_commands_buf_.resource_id_buf_.data(),
                        pass.draw_commands_buf_.resource_id_entry_count_};
  output.visibility = {static_cast<uint *>(view.get_visibility_buffer().data()),
                       divide_ceil_u(resource_len_, 32)};
  output.commands = {pass.draw_commands_buf_.command_buf_.data(),
                     pass.draw_commands_buf_.group_count_ * 2 *
                         pass.draw_commands_buf_.meshlet_max_};
  return output;
}

Manager::DataDebugOutput Manager::data_debug()
{
  matrix_buf.current().read();
  bounds_buf.current().read();
  infos_buf.current().read();
  meshlet_buf.current().read();
  meshlet_object_buf.current().read();

  Manager::DataDebugOutput output;
  output.matrices = {matrix_buf.current().data(), resource_len_};
  output.bounds = {bounds_buf.current().data(), resource_len_};
  output.infos = {infos_buf.current().data(), resource_len_};
  output.meshlets = {meshlet_buf.current().data(), meshlet_len_};
  output.meshlet_objects = {meshlet_object_buf.current().data(), resource_len_};
  return output;
}

MeshletID Manager::meshlet_register(Mesh *mesh, ResourceID resource_id)
{
  /* Procedural draw-meshes are whole-batch instanced draws whose batch is a per-context dummy;
   * they have no extracted meshlets and are culled at object level, so they register nothing. */
  if (mesh->batch_prim_ != GPU_PRIM_NONE) {
    return 0;
  }
  const Span<DrawMeshlet> meshlets = mesh->meshlets();
  if (mesh->meshlet_registered_sync_ != sync_counter_) {
    /* Not registered this frame yet: copy the meshlets into the dense array once. */
    mesh->meshlet_registered_sync_ = sync_counter_;
    mesh->meshlet_id_ = meshlet_len_;
    for (const DrawMeshlet &draw_meshlet : meshlets) {
      meshlet_buf.current().get_or_resize(meshlet_len_) = draw_meshlet;
      meshlet_len_++;
    }
  }
  /* The resource points at the shared meshlet range. */
  MeshletObject &object = meshlet_object_buf.current().get_or_resize(resource_id.index());
  object.meshlet_offset = mesh->meshlet_id_;
  object.meshlet_count = uint(meshlets.size());
  meshlet_count_max_ = max_uu(meshlet_count_max_, uint(meshlets.size()));
  /* The meshlets' bounds are quantized against the resource's object-space AABB, which the
   * visibility pass decodes them against. The CPU copy of the bounds buffer still holds the
   * local-space corners (the GPU finalize pass only rewrites the GPU-side buffer). */
  const ObjectBounds &bounds = bounds_buf.current().data()[resource_id.index()];
  const float3 corner0 = *reinterpret_cast<const float3 *>(&bounds.bounding_corners[0]);
  const float3 corner1 = *reinterpret_cast<const float3 *>(&bounds.bounding_corners[1]);
  const float3 corner2 = *reinterpret_cast<const float3 *>(&bounds.bounding_corners[2]);
  const float3 corner3 = *reinterpret_cast<const float3 *>(&bounds.bounding_corners[3]);
  object.bounds_min = corner0;
  object.bounds_max = corner0 + (corner1 - corner0) + (corner2 - corner0) + (corner3 - corner0);
  return mesh->meshlet_id_;
}

bool Manager::meshlets_register_commands(detail::PassBase<command::DrawCommandBuf> &pass_base)
{
  bool registered = false;
  for (const command::Header &header : pass_base.headers_) {
    if (header.type == command::Type::SubPass) {
      registered = this->meshlets_register_commands(pass_base.sub_passes_[int64_t(header.index)]) ||
                   registered;
    }
    else if (header.type == command::Type::Draw) {
      const command::Draw &cmd = pass_base.commands_[int64_t(header.index)].draw;
      if (cmd.mesh != nullptr && cmd.res_id.raw != 0) {
        this->meshlet_register(cmd.mesh, cmd.res_id);
        registered = true;
      }
    }
  }
  return registered;
}

void Manager::meshlets_register_pass(PassSimple &pass)
{
  if (this->meshlets_register_commands(pass)) {
    meshlet_buf.current().push_update();
    meshlet_object_buf.current().push_update();
  }
}

void Manager::meshlets_register_pass(PassMain &pass)
{
  command::DrawMultiBuf &buf = pass.draw_commands_buf_;
  bool registered = false;
  for (uint i = 0; i < buf.prototype_count_; i++) {
    const command::DrawPrototype &prototype = buf.prototype_buf_[i];
    if (prototype.res_id == 0) {
      continue;
    }
    command::DrawGroup &group = buf.group_buf_[prototype.group_id];
    if (group.desc.mesh != nullptr) {
      Mesh *mesh = group.desc.mesh;
      meshlet_register(mesh, prototype.res_id);
      /* Mirror the range `meshlet_register` just stamped into the per-resource slot onto the
       * group, so each group draws its own mesh's range. Procedural meshes register nothing and
       * keep the zeroed whole-batch fallback. */
      if (mesh->batch_prim_ == GPU_PRIM_NONE) {
        group.meshlet_offset = mesh->meshlet_id_;
        group.meshlet_count = uint(mesh->meshlets().size());
      }
      registered = true;
    }
  }
  if (registered) {
    meshlet_buf.current().push_update();
    meshlet_object_buf.current().push_update();
  }
}

}  // namespace blender::draw
