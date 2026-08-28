/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include <variant>

#include "BLI_index_mask_fwd.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "BKE_paint_bvh.hh"

#include "DNA_customdata_types.h"

#include "draw_command_shared.hh"

namespace blender {

namespace gpu {
class Batch;
class IndexBuf;
class VertBuf;
}  // namespace gpu
struct Object;
namespace bke {
enum class AttrDomain : int8_t;
namespace pbvh {
class Node;
class DrawCache;
class Tree;
}  // namespace pbvh
}  // namespace bke

namespace draw::pbvh {

using GenericRequest = std::string;

enum class CustomRequest : int8_t {
  Position,
  Normal,
  Mask,
  FaceSet,
};

using AttributeRequest = std::variant<CustomRequest, GenericRequest>;

struct ViewportRequest {
  Vector<AttributeRequest> attributes;
  bool use_coarse_grids;
  friend bool operator==(const ViewportRequest &a, const ViewportRequest &b) = default;
  uint64_t hash() const;
};

/**
 * A sculpt batch is one shared batch plus one draw record per PBVH leaf node.
 * `records[k]` corresponds to the k-th leaf node (in `all_leaf_nodes` order); its
 * `vertex_len` is 0 if the node has no visible geometry (discarded by append_records).
 * The records are left unstampet: `res_id` and `custom_id` are set by the caller.
 */
struct SculptBatch {
  gpu::Batch *shared_batch = nullptr;
  Vector<command::DrawRecord> records;
};

/**
 * \brief Information on where each node stores its data in the vertex/index buffers.
 */
struct NodeInfo {
  struct BufferRange {
    /** \brief Does this node already have space acquired inside GPU buffers. */
    bool is_space_acquired = false;
    int offset = -1;
    int len = -1;
    IndexRange index_range() const
    {
      return IndexRange::from_begin_size(offset, len);
    }
  };

  bool is_dirty = true;

  BufferRange vbo;
  BufferRange ibo_lines;
  BufferRange ibo_lines_coarse;
  BufferRange ibo_tris;
  BufferRange ibo_tris_coarse;
};

class DrawCache : public bke::pbvh::DrawCache {
 public:
  ~DrawCache() override = default;
  /**
   * Recalculate and copy data as necessary to prepare a shared batch and per-node draw records
   * for drawing triangles for a specific combination of attributes.
   */
  virtual SculptBatch ensure_tris_batch(const Object &object,
                                        const ViewportRequest &request,
                                        const IndexMask &leaf_nodes) = 0;
  /**
   * Recalculate and copy data as necessary to prepare a shared batch and per-node draw records
   * for drawing wireframe geometry.
   */
  virtual SculptBatch ensure_lines_batch(const Object &object,
                                         const ViewportRequest &request,
                                         const IndexMask &leaf_nodes) = 0;

  /**
   * Return the material index for each node (all faces in a node should have the same material
   * index, as ensured by the BVH building process).
   */
  virtual Span<int> ensure_material_indices(const Object &object) = 0;

  /**
   * \brief Return the node info of each node.
   *
   * After invoking each node will have a space inside the vertex buffers/index buffers.
   *
   * \note Should be called before requesting other information.
   */
  virtual Span<NodeInfo> ensure_node_infos(const Object &object,
                                           const IndexMask &nodes_to_update) = 0;
};

DrawCache &ensure_draw_data(std::unique_ptr<bke::pbvh::DrawCache> &ptr);

}  // namespace draw::pbvh
}  // namespace blender
