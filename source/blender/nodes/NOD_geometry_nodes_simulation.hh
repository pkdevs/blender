/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "NOD_geometry_exec.hh"
#include "NOD_geometry_nodes_bundle_fwd.hh"

namespace blender::nodes::simulation {

struct GeometrySetData {
  std::string path;
  /**
   * The geometry is moved out of the world bundle for local processing and is moved back in the
   * end.
   */
  GeometrySet geometry;
  Set<std::string> tags;
};

/* Gather geometry sets to process from the world. */
Vector<GeometrySetData> gather_geometry_set_data(Bundle &world, StringRef geometry_tag_filter);

}  // namespace blender::nodes::simulation
