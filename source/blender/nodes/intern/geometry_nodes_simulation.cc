/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "NOD_geo_tag_filter.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_simulation.hh"

namespace blender::nodes::simulation {

Vector<GeometrySetData> gather_geometry_set_data(Bundle &world,
                                                 const StringRef geometry_tag_filter)
{
  /* Gather geometry sets to process from the world. */
  Vector<std::string> paths = gather_bundle_paths_by_data_type(world, SOCK_GEOMETRY);
  /* Ensure the order is deterministic. */
  std::ranges::sort(paths);

  Vector<GeometrySetData> geometry_sets;
  for (const StringRef path : paths) {
    GeometrySetData geo_set_data;
    geo_set_data.path = path;
    geo_set_data.geometry = std::move(*world.lookup_path_for_write_ptr<GeometrySet>(path));
    if (geo_set_data.geometry.has_bundle()) {
      const Bundle &bundle_in_geo = *geo_set_data.geometry.bundle();
      if (const std::optional<GListPtr> tags_list_ptr = bundle_in_geo.lookup_path<GListPtr>(
              "tags"))
      {
        if (*tags_list_ptr) {
          const GList &tags_list = **tags_list_ptr;
          if (tags_list.cpp_type().is<std::string>()) {
            tags_list.typed<std::string>().foreach(
                [&](const std::string &tag) { geo_set_data.tags.add(tag); });
          }
        }
      }
    }
    if (!tag_filter_matches(geometry_tag_filter, geo_set_data.tags)) {
      continue;
    }
    geometry_sets.append(std::move(geo_set_data));
  }

  return geometry_sets;
}

}  // namespace blender::nodes::simulation
