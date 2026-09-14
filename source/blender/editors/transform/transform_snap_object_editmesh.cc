/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "BKE_attribute.hh"
#include "BKE_editmesh.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_transform_snap_object_context.hh"

#include "transform_snap_object.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Snap Object Data
 * \{ */

static const Mesh *get_mesh_ref(const Object *ob_eval)
{
  if (const Mesh *me = BKE_object_get_editmesh_eval_final(ob_eval)) {
    return me;
  }

  if (const Mesh *me = BKE_object_get_editmesh_eval_cage(ob_eval)) {
    return me;
  }

  return id_cast<const Mesh *>(ob_eval->data);
}

/**
 * Edit mesh snap cache.
 *
 * \note Linked duplicates share the same original mesh data, but each object receives
 * its own evaluated data.
 * When the original object enters edit mode, all linked duplicates are also flagged
 * as being in edit mode.
 *
 * Because `snap_object_data_editmesh_get` uses the original data pointer as the
 * key, all linked duplicates map to the same cache slot. However,`has_mesh_updated`
 * compares the evaluated data pointers, which differ between duplicates. This causes
 * each object evaluation to invalidate the cache entry written by the previous object,
 * freeing the cached mesh on every lookup.
 *
 * This is causes a use-after-free as the snap result may still reference
 * a mesh that the SnapCache has already freed.
 *
 * To prevent this, the cached mesh is wrapped in a shared pointer. When a snap
 * hit is registered, a copy of the shared pointer is stored in `sctx->ret.data_ref`,
 * incrementing the reference count. Even when the SnapCache later frees its own
 * reference, the mesh is not freed when the cache is invalidated because the reference
 * count remains above zero.
 * See: #148788.
 */
struct SnapCache_EditMesh : public SnapObjectContext::SnapCache {
  /* Owns the snap cache mesh so other references can keep it alive. */
  std::shared_ptr<Mesh> shared_mesh;
  /* Mesh created from the edited mesh. */
  Mesh *mesh;

  /* Reference to pointers that change when the mesh is changed. It is used to detect updates. */
  const Mesh *mesh_ref;
  bke::MeshRuntime *runtime_ref;
  bke::EditMeshData *edit_data_ref;

  bool has_mesh_updated(const Mesh *mesh)
  {
    if (mesh != this->mesh_ref || mesh->runtime != this->runtime_ref ||
        mesh->runtime->edit_data.get() != this->edit_data_ref)
    {
      return true;
    }

    return false;
  }

  void clear()
  {
    this->mesh = nullptr;
  }

  ~SnapCache_EditMesh() override
  {
    this->clear();
  }

  MEM_CXX_CLASS_ALLOC_FUNCS("SnapCache_EditMesh")
};

static Mesh *create_mesh(SnapObjectContext *sctx,
                         const Object *ob_eval,
                         eSnapEditType /*edit_mode_type*/)
{
  Mesh *mesh = BKE_id_new_nomain<Mesh>(nullptr);
  const BMEditMesh *em = BKE_editmesh_from_object(const_cast<Object *>(DEG_get_original(ob_eval)));
  BMesh *bm = em->bm;
  BM_mesh_bm_to_me_compact(*bm, *mesh, nullptr, false);

  bke::MutableAttributeAccessor attrs = mesh->attributes_for_write();
  bke::SpanAttributeWriter<bool> hide_vert = attrs.lookup_or_add_for_write_only_span<bool>(
      ".hide_vert", bke::AttrDomain::Point);
  bke::SpanAttributeWriter<bool> hide_edge = attrs.lookup_or_add_for_write_only_span<bool>(
      ".hide_edge", bke::AttrDomain::Edge);
  bke::SpanAttributeWriter<bool> hide_poly = attrs.lookup_or_add_for_write_only_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  /* Loop over all elements in parallel to choose which elements will participate in the snap.
   * Hidden elements are ignored for snapping. */
  const bool use_threading = (mesh->faces_num + mesh->edges_num) > 1024;
  threading::parallel_invoke(
      use_threading,
      [&]() {
        BMIter iter;
        BMVert *v;
        int i;
        BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
          if (sctx->callbacks.edit_mesh.test_vert_fn) {
            hide_vert.span[i] = !sctx->callbacks.edit_mesh.test_vert_fn(
                v, sctx->callbacks.edit_mesh.user_data);
          }
          else {
            hide_vert.span[i] = BM_elem_flag_test_bool(v, BM_ELEM_HIDDEN);
          }
        }
      },
      [&]() {
        BMIter iter;
        BMEdge *e;
        int i;
        BM_ITER_MESH_INDEX (e, &iter, bm, BM_EDGES_OF_MESH, i) {
          if (sctx->callbacks.edit_mesh.test_edge_fn) {
            hide_edge.span[i] = !sctx->callbacks.edit_mesh.test_edge_fn(
                e, sctx->callbacks.edit_mesh.user_data);
          }
          else {
            hide_edge.span[i] = BM_elem_flag_test_bool(e, BM_ELEM_HIDDEN);
          }
        }
      },
      [&]() {
        BMIter iter;
        BMFace *f;
        int i;
        BM_ITER_MESH_INDEX (f, &iter, bm, BM_FACES_OF_MESH, i) {
          if (sctx->callbacks.edit_mesh.test_face_fn) {
            hide_poly.span[i] = !sctx->callbacks.edit_mesh.test_face_fn(
                f, sctx->callbacks.edit_mesh.user_data);
          }
          else {
            hide_poly.span[i] = BM_elem_flag_test_bool(f, BM_ELEM_HIDDEN);
          }
        }
      });

  hide_vert.finish();
  hide_edge.finish();
  hide_poly.finish();
  return mesh;
}

static SnapCache_EditMesh *snap_object_data_editmesh_get(SnapObjectContext *sctx,
                                                         const Object *ob_eval,
                                                         bool create)
{
  SnapCache_EditMesh *em_cache = nullptr;

  bool init = false;
  const Mesh *mesh_ref = (G.moving) ? /* WORKAROUND:
                                       * Avoid updating while transforming. Do not check if the
                                       * reference mesh has been updated. */
                             nullptr :
                             get_mesh_ref(ob_eval);

  if (std::unique_ptr<SnapObjectContext::SnapCache> *em_cache_p = sctx->editmesh_caches.lookup_ptr(
          ob_eval->runtime->data_orig))
  {
    em_cache = static_cast<SnapCache_EditMesh *>(em_cache_p->get());

    /* Check if the geometry has changed. */
    if (mesh_ref && em_cache->has_mesh_updated(mesh_ref)) {
      em_cache->clear();
      init = true;
    }
  }
  else if (create) {
    std::unique_ptr<SnapCache_EditMesh> em_cache_ptr = std::make_unique<SnapCache_EditMesh>();
    em_cache = em_cache_ptr.get();
    sctx->editmesh_caches.add_new(ob_eval->runtime->data_orig, std::move(em_cache_ptr));
    init = true;
  }

  if (init) {
    Mesh *new_mesh = create_mesh(sctx, ob_eval, sctx->runtime.params.edit_mode_type);
    auto free_temp_mesh_fn = [](Mesh *m) { BKE_id_free(nullptr, m); };

    em_cache->shared_mesh = std::shared_ptr<Mesh>(new_mesh, free_temp_mesh_fn);
    em_cache->mesh = em_cache->shared_mesh.get();
    if (mesh_ref) {
      em_cache->mesh_ref = mesh_ref;
      em_cache->runtime_ref = mesh_ref->runtime;
      em_cache->edit_data_ref = mesh_ref->runtime->edit_data.get();
    }
  }

  return em_cache;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Object Data
 * \{ */

static eSnapMode editmesh_snap_mode_supported(BMesh *bm)
{
  eSnapMode snap_mode_supported = SCE_SNAP_TO_NONE;
  if (bm->totface) {
    snap_mode_supported |= SCE_SNAP_TO_FACE | SCE_SNAP_TO_FACE_MIDPOINT |
                           SCE_SNAP_INDIVIDUAL_NEAREST | SNAP_TO_EDGE_ELEMENTS | SCE_SNAP_TO_POINT;
  }
  else if (bm->totedge) {
    snap_mode_supported |= SNAP_TO_EDGE_ELEMENTS | SCE_SNAP_TO_POINT;
  }
  else if (bm->totvert) {
    snap_mode_supported |= SCE_SNAP_TO_POINT;
  }
  return snap_mode_supported;
}

static SnapCache_EditMesh *editmesh_snapdata_init(SnapObjectContext *sctx,
                                                  const Object *ob_eval,
                                                  eSnapMode snap_to_flag)
{

  const BMEditMesh *em = BKE_editmesh_from_object(const_cast<Object *>(DEG_get_original(ob_eval)));
  if (em == nullptr) {
    return nullptr;
  }

  SnapCache_EditMesh *em_cache = snap_object_data_editmesh_get(sctx, ob_eval, false);
  if (em_cache != nullptr) {
    return em_cache;
  }

  eSnapMode snap_mode_used = snap_to_flag & editmesh_snap_mode_supported(em->bm);
  if (snap_mode_used == SCE_SNAP_TO_NONE) {
    return nullptr;
  }

  return snap_object_data_editmesh_get(sctx, ob_eval, true);
}

/** \} */

eSnapMode snap_object_editmesh(SnapObjectContext *sctx,
                               const Object *ob_eval,
                               const ID * /*id*/,
                               const float4x4 &obmat,
                               eSnapMode snap_to_flag,
                               bool /*use_hide*/)
{
  SnapCache_EditMesh *em_cache = editmesh_snapdata_init(sctx, ob_eval, snap_to_flag);
  if (em_cache && em_cache->mesh) {
    eSnapMode ret = snap_object_mesh(
        sctx, ob_eval, &em_cache->mesh->id, obmat, snap_to_flag, true, true);
    if (sctx->ret.data == &em_cache->mesh->id) {
      sctx->ret.data_ref = em_cache->shared_mesh;
    }
    return ret;
  }
  return SCE_SNAP_TO_NONE;
}

}  // namespace blender::ed::transform
