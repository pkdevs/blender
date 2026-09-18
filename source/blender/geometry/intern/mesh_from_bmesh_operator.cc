/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdarg>

#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"

#include "bmesh.hh"

#include "GEO_mesh_from_bmesh_operator.hh"
#include "GEO_randomize.hh"

namespace blender::geometry {

Mesh *create_mesh_from_bmesh_operator(const std::optional<StringRef> uv_map_id,
                                      const float4x4 &transform,
                                      const char *fmt,
                                      ...)
{
  BMeshCreateParams bmesh_create_params{};
  bmesh_create_params.use_toolflags = true;
  const BMAllocTemplate allocsize = {0, 0, 0, 0};
  BMesh *bm = BM_mesh_create(&allocsize, &bmesh_create_params);
  if (uv_map_id) {
    BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, *uv_map_id);
    /* The pin layer is normally added along with the UV layer (via Python or Mesh conversion),
     * adding the layer directly means it must be added manually. */
    BM_uv_map_attr_pin_ensure_named(bm, *uv_map_id);
  }

  BMOperator bmop;
  va_list args;
  va_start(args, fmt);
  const bool ok = BMO_op_vinitf(bm, &bmop, BMO_FLAG_DEFAULTS, fmt, args);
  va_end(args);
  BLI_assert(ok);
  UNUSED_VARS_NDEBUG(ok);

  BLI_assert(BMO_slot_exists(bmop.slots_in, "matrix"));
  BLI_assert(BMO_slot_exists(bmop.slots_in, "calc_uvs"));
  BMO_slot_mat_set(&bmop, bmop.slots_in, "matrix", transform.base_ptr(), 4);
  BMO_slot_bool_set(bmop.slots_in, "calc_uvs", bool(uv_map_id));
  BMO_op_exec(bm, &bmop);
  BMO_op_finish(bm, &bmop);

  BMeshToMeshParams params{};
  params.calc_object_remap = false;
  Mesh *mesh = BKE_id_new_nomain<Mesh>(nullptr);
  BKE_id_material_eval_ensure_default_slot(&mesh->id);
  BM_mesh_bm_to_me(nullptr, bm, mesh, &params);
  BM_mesh_free(bm);

  debug_randomize_mesh_order(mesh);

  return mesh;
}

}  // namespace blender::geometry
