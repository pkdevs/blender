/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BKE_image_wrappers.hh"
#include "BKE_paint.hh"
#include "BLI_kdtree_types.hh"

#include "IMB_colormanagement.hh"

namespace blender::ed::sculpt_paint::image {
/* -------------------------------------------------------------------- */
/** \name 3D Texture Paint (Experimental)
 * \{ */

/**
 * Uniquely identifies scratch data for a given stroke step.
 *
 * Similar to image_undo.cc, however since this is explicitly only scoped to a single UDIM being
 * used, we don't need to identify the image or image buffer here.
 */
struct ScratchDataKey {
  bke::image::TileNumber tile_number;
  short tile_x;
  short tile_y;

  uint64_t hash() const
  {
    return get_default_hash(tile_number, tile_x, tile_y);
  }

  bool operator==(const ScratchDataKey &other) const
  {
    return tile_number == other.tile_number && tile_x == other.tile_x && tile_y == other.tile_y;
  }
};

/** Data that is allocated / deallocated on a per-tile basis for each stroke step. */
struct ScratchDataTile {
  /** Assumed to be scene-linear */
  Vector<float4, 0> pixel_data;
  Vector<float, 0> factors;
};

struct ScratchData {
  Map<ScratchDataKey, ScratchDataTile> tiles;
  /* TODO: Might be able to do something smarter here and use a flat array */
  Map<bke::image::TileNumber, Array<uint32_t>> data_created;
};

struct TileColorspaceProcessor : NonCopyable {
  ColormanageProcessor buffer_to_linear_processor = {};
  ColormanageProcessor linear_to_buffer_processor = {};
  bool is_noop = true;
  bool is_srgb_byte = false;
};

using BufferType = std::variant<std::monostate, MutableSpan<float4>, MutableSpan<uchar4>>;
using ScratchBufferType = std::variant<std::monostate, MutableSpan<float4>>;

struct ImageData : NonCopyable {
  Image *image = nullptr;
  CanvasImageUser image_user;

  Map<bke::image::TileNumber, ImBuf *> image_buffers = {};
  Map<bke::image::TileNumber, BufferType> data_buffers = {};

  /* TODO: Remove these entries */
  /* Used for algorithms which need a state prior to the stroke (e.g. Blur).
   * Assumed to store scene linear float data. */
  Map<bke::image::TileNumber, ImBuf *> scratch_image_buffers = {};
  Map<bke::image::TileNumber, ScratchBufferType> scratch_data_buffers = {};

  Map<bke::image::TileNumber, TileColorspaceProcessor> processors = {};

  /** Per undo tile, to quickly check if it was already pushed. */
  Map<bke::image::TileNumber, Array<uint32_t>> undo_tile_pushed = {};

  /** Per seam tile modified state, to only do seam bleeding where needed. */
  Map<bke::image::TileNumber, Array<uint8_t>> seam_tile_modified = {};

  ~ImageData();

  static std::unique_ptr<ImageData> init_active_image(Object &ob, ImagePaintSettings &settings);
  const ImageUser &image_user_get() const
  {
    if (std::holds_alternative<ImageUser *>(image_user)) {
      return *std::get<ImageUser *>(image_user);
    }

    return std::get<ImageUser>(image_user);
  }
};

KDTree<float3> *build_kd_tree(Object &ob,
                              const ImageData &data,
                              bke::image::TileNumber tile_number);

void do_3d_image_paint_brush(const Depsgraph &depsgraph,
                             const Paint &paint,
                             const Brush &brush,
                             Object &ob,
                             ImageData &image_data,
                             const IndexMask &node_mask);
void do_3d_image_blur_brush(const Depsgraph &depsgraph,
                            const Paint &paint,
                            const Brush &brush,
                            Object &ob,
                            ImageData &image_data,
                            const KDTree<float3> &tree,
                            const IndexMask &node_mask);
/** \} */
}  // namespace blender::ed::sculpt_paint::image
