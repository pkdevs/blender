/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 */

#pragma once

#include <optional>

#include "BLI_virtual_array.hh"

#include "BKE_attribute_filter.hh"

namespace blender {

struct Mesh;
namespace fn {
template<typename T> class Field;
}
namespace bke {
enum class AttrDomain : int8_t;
}  // namespace bke

namespace geometry {

/**
 * Create a copy containing all of the masked elements. Caller must ensure dependent masks match
 * (e.g. all vertices related to kept edges/faces are included).
 */
std::optional<Mesh *> mesh_copy_by_mask(const Mesh &src_mesh,
                                        IndexMask vert_mask,
                                        IndexMask edge_mask,
                                        IndexMask face_mask,
                                        const bke::AttributeFilter &attribute_filter);

/**
 * Create a copy that includes all elements fully defined by the domain selection filter.
 * (e.g. faces will only be kept if all vertices are retained when filtering the vertex domain).
 */
std::optional<Mesh *> mesh_copy_selection(const Mesh &src_mesh,
                                          const VArray<bool> &selection,
                                          bke::AttrDomain selection_domain,
                                          const bke::AttributeFilter &attribute_filter = {});

std::optional<Mesh *> mesh_copy_selection_keep_verts(
    const Mesh &src_mesh,
    const VArray<bool> &selection,
    bke::AttrDomain selection_domain,
    const bke::AttributeFilter &attribute_filter = {});

std::optional<Mesh *> mesh_copy_selection_keep_edges(
    const Mesh &mesh,
    const VArray<bool> &selection,
    bke::AttrDomain selection_domain,
    const bke::AttributeFilter &attribute_filter = {});

}  // namespace geometry
}  // namespace blender
