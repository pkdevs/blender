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
 * \brief Information on where each node stores its data in the vertex/index buffers.
 */
struct NodeInfo {
  struct BufferRange {
    /** \brief Does this node already have space inside GPU buffers. */
    bool is_space_assigned = false;
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
   * Recalculate and copy data as necessary to prepare batches for drawing triangles for a
   * specific combination of attributes.
   */
  virtual Span<gpu::Batch *> ensure_tris_batches(const Object &object,
                                                 const ViewportRequest &request,
                                                 const IndexMask &nodes_to_update) = 0;
  /**
   * Recalculate and copy data as necessary to prepare batches for drawing wireframe geometry for a
   * specific combination of attributes.
   */
  virtual Span<gpu::Batch *> ensure_lines_batches(const Object &object,
                                                  const ViewportRequest &request,
                                                  const IndexMask &nodes_to_update) = 0;

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
