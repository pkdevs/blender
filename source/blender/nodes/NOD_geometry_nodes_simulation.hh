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

namespace attribute_names {

constexpr StringRefNull position = "position";
constexpr StringRefNull velocity = "velocity";
constexpr StringRefNull instance_transform = "instance_transform";
constexpr StringRefNull rotation = "rotation";
constexpr StringRefNull angular_velocity = "angular_velocity";
constexpr StringRefNull external_force = "external_force";
constexpr StringRefNull external_torque = "external_torque";
constexpr StringRefNull mass = "mass";
constexpr StringRefNull moment_of_inertia = "moment_of_inertia";
constexpr StringRefNull static_friction = "static_friction";
constexpr StringRefNull dynamic_friction = "dynamic_friction";
constexpr StringRefNull radius = "radius";

}  // namespace attribute_names

struct GeometrySetData {
  std::string path;
  /**
   * The geometry is moved out of the world bundle for local processing and is moved back in the
   * end.
   */
  GeometrySet geometry;
  Set<std::string> tags;
};

/* Uniquely identifies a geometry in a world data bundle. */
struct DataKey {
  int geo_bundle_i;
  bke::GeometryComponent::Type type;
  std::optional<int> layer_i;

  uint64_t hash() const
  {
    return get_default_hash(this->geo_bundle_i, this->type, this->layer_i.value_or(0));
  }

  friend bool operator==(const DataKey &a, const DataKey &b) = default;
};

/* Gather geometry sets to process from the world. */
Vector<GeometrySetData> gather_geometry_set_data(Bundle &world, StringRef geometry_tag_filter);

template<typename T>
bke::SpanAttributeWriter<T> ensure_attribute(bke::MutableAttributeAccessor attributes,
                                             const StringRef name,
                                             const bke::AttrDomain domain)
{
  const std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(name);
  if (meta_data) {
    if (meta_data->domain != domain ||
        meta_data->data_type != bke::cpp_type_to_attribute_type(CPPType::get<T>()))
    {
      attributes.remove(name);
    }
  }
  return attributes.lookup_or_add_for_write_span<T>(name, domain);
}

}  // namespace blender::nodes::simulation
