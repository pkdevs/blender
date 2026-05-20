/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BLI_string_ref.hh"

#include "bmesh.hh"

#include "GEO_randomize.hh"
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

  const float4x4 transform = float4x4::identity();

  const bool create_uv_map = bool(uv_map_id);

  BMeshCreateParams bmesh_create_params{};
  bmesh_create_params.use_toolflags = true;
  const BMAllocTemplate allocsize = {0, 0, 0, 0};
  BMesh *bm = BM_mesh_create(&allocsize, &bmesh_create_params);
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "UVMap");
  /* Make sure the associated boolean layers exists as well. Normally this would be done when
    * adding a UV layer via python or when copying from Mesh, but when we 'manually' create the UV
    * layer we need to make sure the boolean layers exist as well. */
  BM_uv_map_attr_pin_ensure_for_all_layers(bm);

  BMO_op_callf(bm,
               BMO_FLAG_DEFAULTS,
               "create_icosphere subdivisions=%i radius=%f matrix=%m4 calc_uvs=%b",
               subdivisions,
               std::abs(radius),
               transform.ptr(),
               create_uv_map);

  BMeshToMeshParams params{};
  params.calc_object_remap = false;
  Mesh *mesh = BKE_id_new_nomain<Mesh>(nullptr);
  BKE_id_material_eval_ensure_default_slot(&mesh->id);
  BM_mesh_bm_to_me(nullptr, bm, mesh, &params);
  BM_mesh_free(bm);

  /* The code above generates a "UVMap" attribute. The code below renames that attribute, we don't
    * have a simple utility for that yet though so there is some overhead right now. */
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  if (create_uv_map) {
    const VArraySpan orig_uv_map = *attributes.lookup<float2>("UVMap");
    bke::SpanAttributeWriter<float2> uv_map = attributes.lookup_or_add_for_write_only_span<float2>(
        *uv_map_id, bke::AttrDomain::Corner);
    uv_map.span.copy_from(orig_uv_map);
    uv_map.finish();
  }

  attributes.remove("UVMap");

  geometry::debug_randomize_mesh_order(mesh);

  mesh->bounds_set_eager(calculate_bounds_ico_sphere(radius, subdivisions));

  return mesh;
}
}
