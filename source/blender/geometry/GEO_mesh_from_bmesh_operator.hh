/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup geo
 */

#include <optional>

#include "BLI_math_matrix_types.hh"
#include "BLI_string_ref.hh"

namespace blender {

struct Mesh;

namespace geometry {

/**
 * Run a BMesh `create_*` operator on an empty BMesh, returning the result as a mesh.
 * The `matrix` & `calc_uvs` slots are set from the arguments, so `fmt` must not include them.
 */
Mesh *create_mesh_from_bmesh_operator(std::optional<StringRef> uv_map_id,
                                      const float4x4 &transform,
                                      const char *fmt,
                                      ...);

}  // namespace geometry
}  // namespace blender
