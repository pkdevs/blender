/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_string_ref.hh"

namespace blender {

struct Mesh;

namespace geometry {

Mesh *create_ico_sphere_mesh(const int subdivisions,
                                      const float radius,
                                      const std::optional<StringRef> &uv_map_id);

}  // namespace geometry
}  // namespace blender
