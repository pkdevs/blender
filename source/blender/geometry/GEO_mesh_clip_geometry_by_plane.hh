/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "DNA_node_types.h"

#include "BKE_anonymous_attribute_id.hh"

struct Mesh;

namespace blender::geometry {

struct ClipByPlaneArgs {
  float plane[4];
  std::optional<std::string> plane_selection_attr_id;
};

enum ClipResult {
  /* Mesh data is discarded, returning empty output.
   */
  Discard,
  /* Entirity of the data is kept, returning the input mesh.
   */
  Keep,
  /* Mesh is split, returning a modified copy.
   */
  Clipped
};

std::pair<Mesh *, ClipResult> clip_by_plane(const Mesh &mesh,
                                            const ClipByPlaneArgs &args,
                                            const bke::AttributeFilter &attribute_filter);

}  // namespace blender::geometry
