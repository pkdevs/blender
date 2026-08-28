/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include "BLI_enum_flags.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DRW_pbvh.hh"
#include "draw_handle.hh"

namespace blender {

struct GPUMaterial;
struct Object;
namespace gpu {
class Batch;
}
namespace draw {
using pbvh::SculptBatch;
}  // namespace draw

enum SculptBatchFeature {
  SCULPT_BATCH_DEFAULT = 0,
  SCULPT_BATCH_WIREFRAME = 1 << 0,
  SCULPT_BATCH_MASK = 1 << 1,
  SCULPT_BATCH_FACE_SET = 1 << 2,
  SCULPT_BATCH_VERTEX_COLOR = 1 << 3,
  SCULPT_BATCH_UV = 1 << 4
};
ENUM_OPERATORS(SculptBatchFeature);

namespace draw {

/**
 * Used by engines that don't use GPUMaterials (Workbench, Overlay, External).
 * Returns one shared batch with one record per visible leaf node.
 * `node_handles` is obtained from `Manager::sculpt_node_handles(ref)`.
 */
SculptBatch sculpt_batches_get(const Object *ob,
                               SculptBatchFeature features,
                               const ResourceHandleRange &node_handles,
                               Span<int> slot_to_custom = {});

/**
 * Used by EEVEE. Returns one SculptBatch per material slot (index = slot).
 * `shared_batch` is nullptr for slots with no visible geometry.
 * `node_handles` is obtained from `Manager::sculpt_node_handles(ref)`.
 */
Vector<SculptBatch> sculpt_batches_per_material_get(const Object *ob,
                                                    Span<const GPUMaterial *> materials,
                                                    const ResourceHandleRange &node_handles,
                                                    Span<int> slot_to_custom = {});

}  // namespace draw
}  // namespace blender
