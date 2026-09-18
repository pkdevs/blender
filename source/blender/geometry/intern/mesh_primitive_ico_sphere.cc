/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BKE_mesh.hh"
#include "BLI_lazy_threading.hh"
#include "BLI_math_constants.hh"
#include "BLI_string_ref.hh"

#include "GEO_mesh_from_bmesh_operator.hh"
#include "GEO_mesh_primitive_ico_sphere.hh"

namespace blender::geometry {

static Bounds<float3> calculate_bounds_ico_sphere(const float radius, const int subdivisions)
{
  const float delta_phi = (2.0f * M_PI) / 5.0f;
  const float theta = std::cos(std::atan(0.5f));
  const float ro = radius * std::sin(delta_phi);

  float x_max = radius;
  float x_min = -radius;
  float y_max = radius;
  float y_min = -radius;

  if (subdivisions == 1) {
    x_max = radius * theta;
    x_min = -x_max;
    y_max = ro * theta;
    y_min = -y_max;
  }
  else if (subdivisions == 2) {
    x_max = ro;
    x_min = -x_max;
  }

  const float3 bounds_min(x_min, y_min, -radius);
  const float3 bounds_max(x_max, y_max, radius);

  return {bounds_min, bounds_max};
}

Mesh *create_ico_sphere_mesh(const int subdivisions,
                             const float radius,
                             const std::optional<StringRef> &uv_map_id)
{
  if (subdivisions >= 3) {
    /* Most nodes don't need this because they internally use multi-threading which triggers
     * lazy-threading without any extra code. */
    lazy_threading::send_hint();
  }

  Mesh *mesh = create_mesh_from_bmesh_operator(uv_map_id,
                                               float4x4::identity(),
                                               "create_icosphere subdivisions=%i radius=%f",
                                               subdivisions,
                                               std::abs(radius));

  mesh->bounds_set_eager(calculate_bounds_ico_sphere(radius, subdivisions));

  return mesh;
}
}  // namespace blender::geometry
