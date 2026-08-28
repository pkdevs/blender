/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "draw_sculpt.hh"

#include "BKE_paint_bvh.hh"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "draw_attributes.hh"
#include "draw_context_private.hh"
#include "draw_handle.hh"
#include "draw_view.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_customdata.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "BLI_math_matrix.hh"

#include "bmesh_class.hh"

#include "DRW_pbvh.hh"
#include "DRW_render.hh"

namespace blender::draw {

namespace {

/**
 * \brief "Fast navigate" flag: show the coarse multires level while the user is navigating the
 * viewport. Only relevant when the paint settings enable it.
 * \note Coarse "fast navigate" geometry only exists for multires (grid) sculpting, so this is
 * false for any other PBVH type.
 */
bool fast_navigate_enabled(const bke::pbvh::Tree &pbvh)
{
  if (pbvh.type() != bke::pbvh::Type::Grids) {
    return false;
  }
  const DRWContext *drwctx = DRW_context_get();
  const RegionView3D *rv3d = drwctx->rv3d;
  const bool navigating = rv3d && (rv3d->rflag & RV3D_NAVIGATING);
  Paint *paint = nullptr;
  if (drwctx->evil_C != nullptr) {
    paint = BKE_paint_get_active_from_context(drwctx->evil_C);
  }
  return navigating && paint != nullptr && (paint->flags & PAINT_FAST_NAVIGATE) != 0;
}

/**
 * Build and return the sculpt batch (one shared batch + one draw record per leaf) for the
 * requested attribute set and coarse / wire settings.
 * \returns batch; empty if the object has no sculpt session.
 * \note `leaf_nodes` receives the leaf mask; record `k` corresponds to leaf `leaf_nodes[k]`.
 */

SculptBatch sculpt_batch_get(const Object *ob,
                             IndexMaskMemory &memory,
                             IndexMask &leaf_nodes,
                             const bool use_wire,
                             Span<pbvh::AttributeRequest> attrs)
{
  bke::pbvh::Tree *pbvh = ob->runtime->sculpt_session ?
                              const_cast<bke::pbvh::Tree *>(bke::object::pbvh_get(*ob)) :
                              nullptr;
  if (!pbvh) {
    leaf_nodes = IndexMask();
    return {};
  }

  bke::pbvh::update_normals_from_eval(*const_cast<Object *>(ob), *pbvh);

  pbvh::DrawCache &draw_data = pbvh::ensure_draw_data(pbvh->draw_data);
  leaf_nodes = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  draw_data.ensure_node_infos(*ob, leaf_nodes);

  const pbvh::ViewportRequest request{attrs, fast_navigate_enabled(*pbvh)};
  if (use_wire) {
    return draw_data.ensure_lines_batch(*ob, request, leaf_nodes);
  }
  return draw_data.ensure_tris_batch(*ob, request, leaf_nodes);
}

/**
 * Stamp `res_id` and `custom_id` onto every record with visible geometry.
 * \param leaf_nodes  The leaf mask; record `k` corresponds to leaf `leaf_nodes[k]`.
 * \param node_handles `node_handles.sub_handle(k)` is the resource handle of leaf `k`.
 * \param slot_to_custom `custom_id` to use for a per-leaf material slot; empty means `custom_id`
 *                       is `0` for every record. `slot_to_custom[slot]` is looked up only when
 *                       `material_indices` is non-empty and `slot` is in range.
 */
void sculpt_batch_stamp(SculptBatch &batch,
                        const IndexMask &leaf_nodes,
                        const ResourceHandleRange &node_handles,
                        Span<int> material_indices,
                        Span<int> slot_to_custom)
{
  if (batch.shared_batch == nullptr || batch.records.is_empty()) {
    return;
  }
  const int n = int(batch.records.size());
  Vector<command::DrawRecord> kept;
  kept.reserve(n);
  for (int k = 0; k < n; k++) {
    const command::DrawRecord &src = batch.records[k];
    if (src.vertex_len == 0 || src.instance_len == 0) {
      continue;
    }
    command::DrawRecord dst = src;
    dst.res_id = node_handles.sub_handle(k).raw();
    if (slot_to_custom.size() > 0) {
      int slot = 0;
      if (material_indices.size() > 0) {
        const int node_index = int(leaf_nodes[k]);
        slot = std::clamp(material_indices[node_index], 0, int(material_indices.size()) - 1);
      }
      if (slot < int(slot_to_custom.size())) {
        dst.custom_id = uint(slot_to_custom[slot]);
      }
    }
    kept.append(std::move(dst));
  }
  batch.records = std::move(kept);
  if (batch.records.is_empty()) {
    batch.shared_batch = nullptr;
  }
}

}  // namespace

SculptBatch sculpt_batches_get(const Object *ob,
                               SculptBatchFeature features,
                               const ResourceHandleRange &node_handles,
                               Span<int> slot_to_custom)
{
  Vector<pbvh::AttributeRequest, 16> attrs;
  attrs.append(pbvh::CustomRequest::Position);
  attrs.append(pbvh::CustomRequest::Normal);
  if (features & SCULPT_BATCH_MASK) {
    attrs.append(pbvh::CustomRequest::Mask);
  }
  if (features & SCULPT_BATCH_FACE_SET) {
    attrs.append(pbvh::CustomRequest::FaceSet);
  }
  const Mesh *mesh = BKE_object_get_original_mesh(ob);
  if (features & SCULPT_BATCH_VERTEX_COLOR) {
    if (const char *name = mesh->active_color_attribute) {
      attrs.append(pbvh::GenericRequest(name));
    }
  }
  if (features & SCULPT_BATCH_UV) {
    const StringRef uv_name = mesh->active_uv_map_name();
    if (!uv_name.is_empty()) {
      attrs.append(pbvh::GenericRequest(uv_name));
    }
  }

  IndexMaskMemory memory;
  IndexMask leaf_nodes;
  SculptBatch batch = sculpt_batch_get(
      ob, memory, leaf_nodes, features & SCULPT_BATCH_WIREFRAME, attrs.as_span());

  /* The material slot per leaf (non-empty only when `slot_to_custom` is provided). */
  Span<int> material_indices;
  if (slot_to_custom.size() > 0) {
    bke::pbvh::Tree *pbvh = const_cast<bke::pbvh::Tree *>(bke::object::pbvh_get(*ob));
    if (pbvh) {
      pbvh::DrawCache &draw_data = pbvh::ensure_draw_data(pbvh->draw_data);
      material_indices = draw_data.ensure_material_indices(*ob);
    }
  }
  sculpt_batch_stamp(batch, leaf_nodes, node_handles, material_indices, slot_to_custom);
  return batch;
}

Vector<SculptBatch> sculpt_batches_per_material_get(const Object *ob,
                                                    Span<const GPUMaterial *> materials,
                                                    const ResourceHandleRange &node_handles,
                                                    Span<int> slot_to_custom)
{
  BLI_assert(ob->type == OB_MESH);
  bke::pbvh::Tree *pbvh = const_cast<bke::pbvh::Tree *>(bke::object::pbvh_get(*ob));
  if (!pbvh) {
    return {};
  }
  pbvh::DrawCache &draw_data = pbvh::ensure_draw_data(pbvh->draw_data);

  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob);
  VectorSet<std::string> draw_attrs;
  DRW_MeshCDMask cd_needed;
  DRW_mesh_get_attributes(*ob, mesh, materials, &draw_attrs, &cd_needed);

  Vector<pbvh::AttributeRequest, 16> attrs;
  attrs.append(pbvh::CustomRequest::Position);
  attrs.append(pbvh::CustomRequest::Normal);
  for (const StringRef name : draw_attrs) {
    attrs.append(pbvh::GenericRequest(name));
  }
  for (const StringRef name : cd_needed.uv) {
    attrs.append(pbvh::GenericRequest(name));
  }

  IndexMaskMemory memory;
  IndexMask leaf_nodes;
  const SculptBatch base = sculpt_batch_get(
      ob, memory, leaf_nodes, /*use_wire*/ false, attrs.as_span());
  const Span<int> material_indices = draw_data.ensure_material_indices(*ob);

  /* Split the records by material slot, so each slot's sub_pass can draw its own geometry.
   * `base.records[k]` corresponds to leaf `leaf_nodes[k]`, hence `res_id` must use the original
   * position `k`, not the position inside the filtered subset. */
  Vector<SculptBatch> result(materials.size());
  const bool per_material = material_indices.size() > 0;
  const int custom_max = int(slot_to_custom.size());
  for (uint slot = 0; slot < materials.size(); slot++) {
    SculptBatch rec;
    Vector<command::DrawRecord> kept;
    kept.reserve(base.records.size());
    for (uint k = 0; k < base.records.size(); k++) {
      const command::DrawRecord &src = base.records[k];
      if (src.vertex_len == 0 || src.instance_len == 0) {
        continue;
      }
      int slot_node = 0;
      if (per_material) {
        const int node_index = int(leaf_nodes[k]);
        slot_node = std::clamp(material_indices[node_index], 0, int(material_indices.size()) - 1);
      }
      if (slot_node != int(slot)) {
        continue;
      }
      command::DrawRecord dst = src;
      dst.res_id = node_handles.sub_handle(k).raw();
      dst.custom_id = (slot < custom_max) ? uint(slot_to_custom[slot]) : uint(slot);
      kept.append(std::move(dst));
    }
    rec.records = std::move(kept);
    rec.shared_batch = rec.records.is_empty() ? nullptr : base.shared_batch;
    result[slot] = std::move(rec);
  }
  return result;
}

}  // namespace blender::draw
