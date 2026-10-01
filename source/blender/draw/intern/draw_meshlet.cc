/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "BLI_assert.hh"

#include "draw_meshlet.hh"

namespace blender::draw {

namespace {
/** Resolve the procedural batch for \a primitive from the active GPU context. */
gpu::Batch *procedural_batch_get(GPUPrimType primitive)
{
  switch (primitive) {
    case GPU_PRIM_POINTS:
      return GPU_batch_procedural_points_get();
    case GPU_PRIM_LINES:
      return GPU_batch_procedural_lines_get();
    case GPU_PRIM_TRIS:
      return GPU_batch_procedural_triangles_get();
    case GPU_PRIM_TRI_STRIP:
      return GPU_batch_procedural_triangle_strips_get();
    default:
      BLI_assert_unreachable();
      return nullptr;
  }
}
}  // namespace

Mesh::Mesh(gpu::Batch *batch, bool owns_batch, float4 bounding_sphere)
    : batch_(batch), owns_batch_(owns_batch)
{
  meshlet_resync(bounding_sphere);
}

Mesh::Mesh(GPUPrimType batch_prim) : batch_prim_(batch_prim)
{
  BLI_assert(batch_prim != GPU_PRIM_NONE);
  /* The procedural batch is a per-context dummy: the draw count is supplied per call, not by
   * the batch, so the meshlet is left empty. */
  meshlet_ = DrawMeshlet();
}

gpu::Batch *Mesh::batch_get() const
{
  if (batch_prim_ != GPU_PRIM_NONE) {
    return procedural_batch_get(batch_prim_);
  }
  return batch_;
}

void Mesh::meshlet_resync(float4 bounding_sphere)
{
  gpu::Batch *batch = batch_get();
  if (batch == nullptr) {
    meshlet_ = DrawMeshlet();
    return;
  }
  /* A batch in the `DRW_batch_request` state has neither vertex nor index buffer, and reading
   * its draw parameters would dereference null. Leave the meshlet empty; the cache owner
   * refills it after the batch is filled with geometry. */
  if (batch->procedural_vertices < 0 && batch->elem == nullptr && batch->verts[0] == nullptr) {
    meshlet_ = DrawMeshlet();
    return;
  }
  int vertex_count, vertex_first, base_index, instance_count;
  GPU_batch_draw_parameter_get(batch, &vertex_count, &vertex_first, &base_index, &instance_count);
  meshlet_.vertex_start = vertex_first;
  meshlet_.vertex_count = vertex_count;
  meshlet_.base_index = base_index;
  /* The meshlet spans the whole object; encode the object bounds (full-span placeholder). */
  float3 object_min = float3(bounding_sphere.x - bounding_sphere.w,
                             bounding_sphere.y - bounding_sphere.w,
                             bounding_sphere.z - bounding_sphere.w);
  float3 object_max = float3(bounding_sphere.x + bounding_sphere.w,
                             bounding_sphere.y + bounding_sphere.w,
                             bounding_sphere.z + bounding_sphere.w);
  meshlet_.packed_bounds = drw_meshlet_bounds_pack(object_min, object_max, object_min, object_max);
}

}  // namespace blender::draw
