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

Mesh::Mesh(gpu::Batch *batch) : batch_(batch), owns_batch_(true) {}

Mesh::Mesh(gpu::Batch *batch, bool owns_batch) : batch_(batch), owns_batch_(owns_batch) {}

Mesh::Mesh(GPUPrimType batch_prim) : batch_prim_(batch_prim)
{
  BLI_assert(batch_prim != GPU_PRIM_NONE);
}

gpu::Batch *Mesh::batch_get() const
{
  if (batch_prim_ != GPU_PRIM_NONE) {
    return procedural_batch_get(batch_prim_);
  }
  return batch_;
}

}  // namespace blender::draw
