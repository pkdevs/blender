/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_color_types.hh"
#include "BLI_string_ref.hh"

#include "NOD_bundle_type_fwd.hh"

namespace blender::nodes::viewer_bundles {

/**
 * Draws meshes, curves and point clouds in the "geometry" item with a flat color from their
 * "viewer_color" attribute. Loose edges and curves are drawn as lines with a screen space width
 * from the "edge_width" item.
 */
class GeometryOverlayBundle {
 public:
  static constexpr StringRefNull name = "Blender.GeometryOverlay";
  static constexpr StringRefNull color_attribute_name = "viewer_color";
  static constexpr ColorGeometry4f default_color = ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f);
  static constexpr float default_edge_width = 1.0f;
  static constexpr StringRefNull edge_width_instance_attribute_name = ".viewer_edge_width";
  static const FlatBundleTypePtr &get_bundle_type();
};

}  // namespace blender::nodes::viewer_bundles
