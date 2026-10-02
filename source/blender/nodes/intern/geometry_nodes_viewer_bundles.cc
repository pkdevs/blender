/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "NOD_bundle_type.hh"
#include "NOD_geometry_nodes_viewer_bundles.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

namespace blender::nodes::viewer_bundles {

const FlatBundleTypePtr &GeometryOverlayBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(GeometryOverlayBundle::name);
    b.add<decl::Geometry>("geometry"_ustr)
        .description(
            "Meshes, curves and point clouds drawn with a flat color from the "
            "\"viewer_color\" attribute");
    b.add<decl::Float>("edge_width"_ustr)
        .default_value(GeometryOverlayBundle::default_edge_width)
        .min(0.0f)
        .description("Width of loose edges and curves in pixels");
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

}  // namespace blender::nodes::viewer_bundles
