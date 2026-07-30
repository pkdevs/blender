/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <algorithm>

#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_string_utf8.hh"

#include "DNA_mesh_types.h"

#include "BKE_editmesh.hh"
#include "BKE_editmesh_bvh.hh"
#include "BKE_report.hh"
#include "BKE_unit.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "BLT_translation.hh"

#include "transform.hh"
#include "transform_constraints.hh"
#include "transform_convert.hh"
#include "transform_mode.hh"
#include "transform_snap.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Transform (Edge Slide)
 * \{ */

struct EdgeSlideData {
  Array<TransDataEdgeSlideVert> sv;

  Vector<BMVert *> original_verts;

  struct CloneChainEdge {
    BMVert *v1, *v2;
  };
  Vector<CloneChainEdge> clone_chain_edges;

  int mval_start[2], mval_end[2];
  int curr_sv_index;

 private:
  float4x4 proj_mat;
  float2 win_half;

 public:
  void update_proj_mat(TransInfo *t, const TransDataContainer *tc)
  {
    ARegion *region = t->region;
    if (region == nullptr) [[unlikely]] {
      this->win_half = {1.0f, 1.0f};
      this->proj_mat = float4x4::identity();
      return;
    }

    this->win_half = {region->winx / 2.0f, region->winy / 2.0f};

    if (t->spacetype == SPACE_VIEW3D) {
      RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
      this->proj_mat = ED_view3d_ob_project_mat_get(rv3d, tc->obedit);

      for (int i = 0; i < 4; i++) {
        this->proj_mat[i][0] *= this->win_half[0];
        this->proj_mat[i][1] *= this->win_half[1];
      }
    }
    else {
      const View2D *v2d = static_cast<View2D *>(t->view);
      ui::view2d_view_to_region_m4(v2d, this->proj_mat.ptr());
      this->proj_mat.location()[0] -= this->win_half[0];
      this->proj_mat.location()[1] -= this->win_half[1];
    }
  }

  void project(const TransDataEdgeSlideVert *svert, float2 &r_sco_a, float2 &r_sco_b) const
  {
    float3 iloc = svert->v_co_orig();
    r_sco_a = math::project_point(this->proj_mat, iloc + svert->dir_side[0]).xy() + this->win_half;
    r_sco_b = math::project_point(this->proj_mat, iloc + svert->dir_side[1]).xy() + this->win_half;
  }
};

struct EdgeSlideParams {
  wmOperator *op;
  float perc;

  /** When un-clamped - use this index: #TransDataEdgeSlideVert.dir_side. */
  int curr_side_unclamp;

  bool use_even;
  bool flipped;
  bool update_status_bar;
  bool use_clone;
};

static bool edge_slide_use_clamp(const TransInfo *t)
{
  const EdgeSlideParams *slp = static_cast<const EdgeSlideParams *>(t->custom.mode.data);
  if (slp && slp->use_clone) {
    return true;
  }
  return !(t->flag & T_ALT_TRANSFORM);
}

/**
 * Get the first valid TransDataContainer *.
 *
 * Note we cannot trust TRANS_DATA_CONTAINER_FIRST_OK because of multi-object that
 * may leave items with invalid custom data in the transform data container.
 */
static TransDataContainer *edge_slide_container_first_ok(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    if (tc->custom.mode.data) {
      return tc;
    }
  }
  BLI_assert_msg(0, "Should never happen, at least one EdgeSlideData should be valid");
  return nullptr;
}

static EdgeSlideData *edgeSlideFirstGet(TransInfo *t)
{
  TransDataContainer *tc = edge_slide_container_first_ok(t);
  return static_cast<EdgeSlideData *>(tc->custom.mode.data);
}

static void calcEdgeSlideCustomPoints(TransInfo *t)
{
  EdgeSlideData *sld = edgeSlideFirstGet(t);

  setCustomPoints(t, &t->mouse, sld->mval_end, sld->mval_start);

  /* #setCustomPoints isn't normally changing as the mouse moves,
   * in this case apply mouse input immediately so we don't refresh
   * with the value from the previous points. */
  applyMouseInput(t, &t->mouse, t->mval, t->values);
}

/* Interpolates along a line made up of 2 segments (used for edge slide). */
static void interp_line_v3_v3v3v3(
    float p[3], const float v1[3], const float v2[3], const float v3[3], float t)
{
  float t_mid, t_delta;

  /* Could be pre-calculated. */
  t_mid = line_point_factor_v3(v2, v1, v3);

  t_delta = t - t_mid;
  if (t_delta < 0.0f) {
    if (fabsf(t_mid) < FLT_EPSILON) [[unlikely]] {
      copy_v3_v3(p, v2);
    }
    else {
      interp_v3_v3v3(p, v1, v2, t / t_mid);
    }
  }
  else {
    t = t - t_mid;
    t_mid = 1.0f - t_mid;

    if (fabsf(t_mid) < FLT_EPSILON) [[unlikely]] {
      copy_v3_v3(p, v3);
    }
    else {
      interp_v3_v3v3(p, v2, v3, t / t_mid);
    }
  }
}

static void edge_slide_data_init_mval(MouseInput *mi, EdgeSlideData *sld, float *mval_dir)
{
  /* Possible all of the edge loops are pointing directly at the view. */
  if (len_squared_v2(mval_dir) < 0.1f) [[unlikely]] {
    mval_dir[0] = 0.0f;
    mval_dir[1] = 100.0f;
  }

  float mval_start[2], mval_end[2];

  /* Zero out Start. */
  zero_v2(mval_start);

  /* `mval_dir` holds a vector along edge loop. */
  copy_v2_v2(mval_end, mval_dir);
  mul_v2_fl(mval_end, 0.5f);

  sld->mval_start[0] = mi->imval[0] + mval_start[0];
  sld->mval_start[1] = mi->imval[1] + mval_start[1];

  sld->mval_end[0] = mi->imval[0] + mval_end[0];
  sld->mval_end[1] = mi->imval[1] + mval_end[1];
}

static bool is_vert_slide_visible_bmesh(TransInfo *t,
                                        TransDataContainer *tc,
                                        const View3D *v3d,
                                        const BMBVHTree *bmbvh,
                                        TransDataEdgeSlideVert *sv)
{
  /* NOTE: */
  BMIter iter_other;
  BMEdge *e;

  BMVert *v = static_cast<BMVert *>(sv->td->extra);
  BM_ITER_ELEM (e, &iter_other, v, BM_EDGES_OF_VERT) {
    if (BM_elem_flag_test(e, BM_ELEM_SELECT | BM_ELEM_HIDDEN)) {
      continue;
    }

    if (BMBVH_EdgeVisible(bmbvh, e, t->depsgraph, t->region, v3d, tc->obedit)) {
      return true;
    }
  }
  return false;
}

/**
 * Calculate screen-space `mval_start` / `mval_end`, optionally slide direction.
 */
static void calcEdgeSlide_mval_range(TransInfo *t,
                                     TransDataContainer *tc,
                                     EdgeSlideData *sld,
                                     const int loop_nr,
                                     const float2 &mval,
                                     const bool use_calc_direction)
{
  View3D *v3d = nullptr;

  /* Use for visibility checks. */
  bool use_occlude_geometry = false;
  if (t->spacetype == SPACE_VIEW3D) {
    v3d = static_cast<View3D *>(t->area ? t->area->spacedata.first : nullptr);
    if (v3d) {
      if (tc->obedit->type == OB_MESH) {
        use_occlude_geometry = (tc->obedit->dt > OB_WIRE && !XRAY_ENABLED(v3d));
      }
    }
  }

  /* NOTE(@ideasman42): At the moment this is only needed for meshes.
   * In principle we could use a generic ray-cast test.
   *
   * Prefer #BMBVHTree over generic snap: #SnapObjectContext
   * or any method that considers all other objects in the scene.
   *
   * While generic snapping is technically "correct" there are multiple reasons not to use this.
   *
   * - Performance, where generic snapping would consider all other objects for every-vertex.
   *   This can cause lockups when #DupliObject have to be created multiple times for each vertex.
   * - In practice it's acceptable (even preferable) to skip back-facing vertices
   *   based on each meshes own faces that doesn't take other scene objects into account,
   *   especially since this includes instances objects from particles or nodes.
   * - The #BMBVH_EdgeVisible check skips faces that the edge is connected to,
   *   unlike generic ray-casts where an edge can (under some conditions) overlap it self.
   *
   * See: #125646 for details.
   */
  BMBVHTree *bmbvh = nullptr;
  Array<float3> bmbvh_coord_storage;
  if (use_occlude_geometry) {
    Scene *scene_eval = DEG_get_evaluated(t->depsgraph, t->scene);
    Object *obedit_eval = DEG_get_evaluated(t->depsgraph, tc->obedit);
    BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);

    const Span<float3> vert_positions = BKE_editmesh_vert_coords_when_deformed(
        t->depsgraph, em, scene_eval, obedit_eval, bmbvh_coord_storage);

    bmbvh = BKE_bmbvh_new_from_editmesh(em,
                                        BMBVH_RESPECT_HIDDEN,
                                        vert_positions.is_empty() ? nullptr :
                                                                    vert_positions.data(),
                                        false);
  }

  /* Find mouse vectors, the global one, and one per loop in case we have
   * multiple loops selected, in case they are oriented different. */
  float2 mval_dir = float2(0);
  float dist_best_sq = FLT_MAX;

  /* Only for use_calc_direction. */
  float2 *loop_dir = nullptr;
  float *loop_maxdist = nullptr;

  if (use_calc_direction) {
    loop_dir = MEM_new_array_zeroed<float2>(loop_nr, "sv loop_dir");
    loop_maxdist = MEM_new_array_uninitialized<float>(loop_nr, "sv loop_maxdist");
    std::fill_n(loop_maxdist, loop_nr, FLT_MAX);
  }

  for (int i : sld->sv.index_range()) {
    TransDataEdgeSlideVert *sv = &sld->sv[i];
    bool is_visible = !use_occlude_geometry || is_vert_slide_visible_bmesh(t, tc, v3d, bmbvh, sv);

    /* This test is only relevant if object is not wire-drawn! See #32068. */
    if (!is_visible && !use_calc_direction) {
      continue;
    }

    /* Search cross edges for visible edge to the mouse cursor,
     * then use the shared vertex to calculate screen vector. */
    /* Screen-space coords. */
    float2 sco_a, sco_b;
    sld->project(sv, sco_a, sco_b);

    /* Global direction. */
    float dist_sq = dist_squared_to_line_segment_v2(mval, sco_b, sco_a);
    if (is_visible) {
      if (dist_sq < dist_best_sq && (len_squared_v2v2(sco_b, sco_a) > 0.1f)) {
        dist_best_sq = dist_sq;
        mval_dir = sco_b - sco_a;
        sld->curr_sv_index = i;
      }
    }

    if (use_calc_direction) {
      /* Per loop direction. */
      int l_nr = sv->loop_nr;
      if (dist_sq < loop_maxdist[l_nr]) {
        loop_maxdist[l_nr] = dist_sq;
        loop_dir[l_nr] = sco_b - sco_a;
      }
    }
  }

  if (use_calc_direction) {
    for (TransDataEdgeSlideVert &sv : sld->sv) {
      /* Switch a/b if loop direction is different from global direction. */
      int l_nr = sv.loop_nr;
      if (math::dot(loop_dir[l_nr], mval_dir) < 0.0f) {
        swap_v3_v3(sv.dir_side[0], sv.dir_side[1]);
      }
    }

    MEM_delete(loop_dir);
    MEM_delete(loop_maxdist);
  }

  edge_slide_data_init_mval(&t->mouse, sld, mval_dir);

  if (bmbvh) {
    BKE_bmbvh_free(bmbvh);
  }
}

/** Orient each loop group so `dir_side[0]` points along `align_dir`. */
static void edge_slide_clone_align_sides(EdgeSlideData *sld,
                                         const int group_len,
                                         const float3 &align_dir)
{
  if (group_len <= 0) {
    return;
  }

  Array<float3> net(group_len, float3(0));
  for (const TransDataEdgeSlideVert &sv : sld->sv) {
    if (sv.loop_nr < 0) {
      continue;
    }
    const float3 side = sv.dir_side[0] - sv.dir_side[1];
    if (!math::is_zero(side)) {
      net[sv.loop_nr] += math::normalize(side);
    }
  }

  for (TransDataEdgeSlideVert &sv : sld->sv) {
    if (sv.loop_nr >= 0 && math::dot(net[sv.loop_nr], align_dir) < 0.0f) {
      swap_v3_v3(sv.dir_side[0], sv.dir_side[1]);
    }
  }
}

static EdgeSlideData *createEdgeSlideVerts(TransInfo *t,
                                           TransDataContainer *tc,
                                           const bool use_double_side,
                                           const float3 *clone_align_dir)
{
  int group_len;
  EdgeSlideData *sld = MEM_new<EdgeSlideData>("sld");
  if (t->data_type == &TransConvertType_MeshUV) {
    sld->sv = transform_mesh_uv_edge_slide_data_create(t, tc, &group_len);
  }
  else {
    sld->sv = transform_mesh_edge_slide_data_create(tc, &group_len);
  }

  if (sld->sv.is_empty()) {
    MEM_delete(sld);
    return nullptr;
  }

  if (!use_double_side) {
    /* Single Side Case.
     * Used by #MESH_OT_offset_edge_loops_slide.
     * It only slides to the side with the longest length. */
    struct TMP {
      float2 accum;
      int count;
    } zero{};

    Array<TMP> array_len(group_len, zero);
    for (TransDataEdgeSlideVert &sv : sld->sv) {
      array_len[sv.loop_nr].accum += float2(math::length(sv.dir_side[0]),
                                            math::length(sv.dir_side[1]));
      array_len[sv.loop_nr].count++;
    }

    for (TMP &accum : array_len) {
      accum.accum /= accum.count;
    }

    for (TransDataEdgeSlideVert &sv : sld->sv) {
      if (array_len[sv.loop_nr].accum[1] > array_len[sv.loop_nr].accum[0]) {
        sv.dir_side[0] = sv.dir_side[1];
      }
      sv.dir_side[1] = float3(0);
      sv.edge_len = math::length(sv.dir_side[0]);
    }
  }

  if (clone_align_dir && use_double_side) {
    edge_slide_clone_align_sides(sld, group_len, *clone_align_dir);
  }

  sld->curr_sv_index = 0;
  sld->update_proj_mat(t, tc);

  calcEdgeSlide_mval_range(t, tc, sld, group_len, t->mval, use_double_side);

  return sld;
}

static void freeEdgeSlideVerts(TransInfo *t, TransDataContainer *tc, TransCustomData *custom_data)
{
  EdgeSlideData *sld = static_cast<EdgeSlideData *>(custom_data->data);

  if (sld == nullptr) {
    return;
  }

  /* `t->custom.mode.data` (the #EdgeSlideParams) is already freed by #postTrans at this point
   * so detect clone mode from the container's own data instead. */
  if (!sld->original_verts.is_empty() && t->state == TRANS_CANCEL) {
    BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);
    BMesh *bm = em->bm;

    /* Remove the clones and restore the original selection that was replaced on init. */
    for (int i = 0; i < int(sld->sv.size()); i++) {
      BMVert *v_clone = static_cast<BMVert *>(sld->sv[i].td->extra);
      if (v_clone) {
        BM_vert_kill(bm, v_clone);
      }
      if (BMVert *v_orig = sld->original_verts[i]) {
        BM_elem_flag_enable(v_orig, BM_ELEM_SELECT);
      }
    }
    EDBM_selectmode_flush(em);

    EDBMUpdate_Params update_params{};
    update_params.calc_looptris = true;
    update_params.calc_normals = true;
    update_params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(tc->obedit->data), &update_params);
  }

  MEM_delete(sld);

  custom_data->data = nullptr;
}

/** The faces a chain vert has on one side of the chain and where that fan ends. */
struct CloneFanSide {
  Vector<BMFace *, 4> faces;
  /** Verts across the non-chain edges the fan stopped at. */
  Vector<BMVert *, 2> bound_verts;
  /** Verts across the edges interior to the fan. */
  Vector<BMVert *, 2> rung_verts;
  BMEdge *end_edge = nullptr;
};

/** Which face of a chain edge lies on either side of the slide. */
struct CloneEdgeSides {
  BMFace *f[2] = {nullptr, nullptr};
};

struct CloneVertInfo {
  BMVert *v;
  CloneFanSide side[2];
};

static BMEdge *edge_slide_clone_face_other_edge(BMFace *f, BMVert *v, BMEdge *e)
{
  BMLoop *l = BM_face_vert_share_loop(f, v);
  if (!l) {
    return nullptr;
  }
  if (l->e == e) {
    return l->prev->e;
  }
  if (l->prev->e == e) {
    return l->e;
  }
  return nullptr;
}

/**
 * The faces at v on one side of the chain step radially from the chain edge's face on that
 * side until another chain edge closes the fan.
 */
static void edge_slide_clone_fan_calc(BMVert *v,
                                      BMEdge *e_chain,
                                      BMFace *f_chain,
                                      const Set<BMEdge *> &chain_edge_set,
                                      const bool is_chain_end,
                                      CloneFanSide *r_side)
{
  if (!f_chain) {
    return;
  }
  r_side->faces.append(f_chain);

  BMFace *f_curr = f_chain;
  BMEdge *e_curr = edge_slide_clone_face_other_edge(f_chain, v, e_chain);
  while (e_curr && !chain_edge_set.contains(e_curr)) {
    BMFace *f_next = nullptr;
    if (!is_chain_end && BM_edge_is_manifold(e_curr)) {
      BMLoop *l = BM_face_edge_share_loop(f_curr, e_curr);
      f_next = l ? l->radial_next->f : nullptr;
    }
    if (!f_next) {
      /* The fan ends at the chain end, at a mesh boundary or at non-manifold geometry. */
      r_side->bound_verts.append(BM_edge_other_vert(e_curr, v));
      return;
    }
    if (r_side->faces.contains(f_next)) {
      /* Wrapped the whole disk, so there is nothing bounding the fan. */
      break;
    }
    r_side->rung_verts.append(BM_edge_other_vert(e_curr, v));
    r_side->faces.append(f_next);
    f_curr = f_next;
    e_curr = edge_slide_clone_face_other_edge(f_next, v, e_curr);
  }
  if (e_curr) {
    r_side->end_edge = e_curr;
  }
}

/**
 * Which of the faces at chain edge #e sits on either side of the slide at v.
 */
static void edge_slide_clone_edge_side_faces(
    BMVert *v, BMEdge *e, const float3 &dir0, const float3 &dir1, BMFace *r_f_side[2])
{
  r_f_side[0] = r_f_side[1] = nullptr;
  float score_side[2] = {-FLT_MAX, FLT_MAX};

  BMLoop *l;
  BMIter l_iter;
  BM_ITER_ELEM (l, &l_iter, e, BM_LOOPS_OF_EDGE) {
    /* The corner of this face at v that the chain edge does not lead to. */
    BMVert *v_other = (l->v == v) ? l->prev->v : l->next->next->v;
    const float3 vec = float3(v_other->co) - float3(v->co);
    if (math::is_zero(vec)) {
      continue;
    }
    const float3 dir = math::normalize(vec);
    const float score = math::dot(dir, dir0) - math::dot(dir, dir1);
    if (score > score_side[0]) {
      score_side[0] = score;
      r_f_side[0] = l->f;
    }
    if (score < score_side[1]) {
      score_side[1] = score;
      r_f_side[1] = l->f;
    }
  }

  if (r_f_side[0] == r_f_side[1]) {
    r_f_side[(score_side[0] >= 0.0f) ? 1 : 0] = nullptr;
  }
}

/**
 * Recompute the faces on either side of the chain from the stored original verts.
 */
static Vector<CloneVertInfo> edge_slide_clone_verts_calc(const EdgeSlideData *sld)
{
  Set<BMEdge *> chain_edge_set;
  chain_edge_set.reserve(sld->clone_chain_edges.size());
  for (const EdgeSlideData::CloneChainEdge &ce : sld->clone_chain_edges) {
    if (BMEdge *e = BM_edge_exists(ce.v1, ce.v2)) {
      chain_edge_set.add(e);
    }
  }

  const int verts_num = int(sld->original_verts.size());

  /* The chain edges at each vert, and the slide directions there. */
  struct VertChain {
    Vector<BMEdge *, 4> edges;
    float3 dir[2];
  };
  Array<VertChain> chain(verts_num);
  Map<BMVert *, int> vert_index;
  vert_index.reserve(verts_num);

  for (int i = 0; i < verts_num; i++) {
    BMVert *v = sld->original_verts[i];
    const TransDataEdgeSlideVert &sv = sld->sv[i];
    vert_index.add_overwrite(v, i);

    BMEdge *ne;
    BMIter ne_iter;
    BM_ITER_ELEM (ne, &ne_iter, v, BM_EDGES_OF_VERT) {
      if (chain_edge_set.contains(ne)) {
        chain[i].edges.append(ne);
      }
    }
    for (int side = 0; side < 2; side++) {
      chain[i].dir[side] = !math::is_zero(sv.dir_side[side]) ? math::normalize(sv.dir_side[side]) :
                                                               float3(0);
    }
  }

  /* Which face of each chain edge lies on which side. Decided from the slide directions once per connected chain. */
  Map<BMEdge *, CloneEdgeSides> edge_sides;
  edge_sides.reserve(sld->clone_chain_edges.size());
  Vector<BMEdge *> queue;

  auto side_assign = [&](BMEdge *e, const int side, BMFace *f) {
    if (!f) {
      return;
    }
    CloneEdgeSides &sides = edge_sides.lookup_or_add_default(e);
    if (sides.f[side]) {
      return;
    }
    sides.f[side] = f;
    queue.append(e);
  };

  for (int i = 0; i < verts_num; i++) {
    for (BMEdge *e_seed : chain[i].edges) {
      if (edge_sides.contains(e_seed)) {
        continue;
      }
      BMFace *f_side[2];
      edge_slide_clone_edge_side_faces(
          sld->original_verts[i], e_seed, chain[i].dir[0], chain[i].dir[1], f_side);
      side_assign(e_seed, 0, f_side[0]);
      side_assign(e_seed, 1, f_side[1]);

      while (!queue.is_empty()) {
        BMEdge *e_curr = queue.pop_last();
        const CloneEdgeSides sides_curr = edge_sides.lookup(e_curr);
        BMVert *e_verts[2] = {e_curr->v1, e_curr->v2};
        for (BMVert *v : e_verts) {
          const int *index = vert_index.lookup_ptr(v);
          if (!index) {
            continue;
          }
          for (int side = 0; side < 2; side++) {
            if (!sides_curr.f[side]) {
              continue;
            }
            CloneFanSide fan;
            edge_slide_clone_fan_calc(v,
                                      e_curr,
                                      sides_curr.f[side],
                                      chain_edge_set,
                                      chain[*index].edges.size() == 1,
                                      &fan);
            if (fan.end_edge && fan.end_edge != e_curr) {
              side_assign(fan.end_edge, side, fan.faces.last());
            }
          }
        }
      }
    }
  }

  Vector<CloneVertInfo> infos;
  infos.reserve(verts_num);

  for (int i = 0; i < verts_num; i++) {
    CloneVertInfo info;
    info.v = sld->original_verts[i];

    const bool is_chain_end = chain[i].edges.size() == 1;
    for (int side = 0; side < 2; side++) {
      for (BMEdge *e_seed : chain[i].edges) {
        const CloneEdgeSides *sides = edge_sides.lookup_ptr(e_seed);
        if (!sides || !sides->f[side]) {
          continue;
        }
        edge_slide_clone_fan_calc(
            info.v, e_seed, sides->f[side], chain_edge_set, is_chain_end, &info.side[side]);
        break;
      }
    }

    infos.append(std::move(info));
  }

  return infos;
}

static eRedrawFlag handleEventEdgeSlide(TransInfo *t, const wmEvent *event)
{
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);

  if (slp) {
    bool is_event_handled = t->redraw && (event->type != MOUSEMOVE);
    slp->update_status_bar |= is_event_handled;
    switch (event->type) {
      case EVT_EKEY:
        if (event->val == KM_PRESS) {
          slp->use_even = !slp->use_even;
          calcEdgeSlideCustomPoints(t);
          slp->update_status_bar = true;
          return TREDRAW_HARD;
        }
        break;
      case EVT_FKEY:
        if (event->val == KM_PRESS) {
          slp->flipped = !slp->flipped;
          calcEdgeSlideCustomPoints(t);
          slp->update_status_bar = true;
          return TREDRAW_HARD;
        }
        break;
      case EVT_CKEY:
        /* Use like a modifier key. Clone is always clamped, see #edge_slide_use_clamp. */
        if (event->val == KM_PRESS && !slp->use_clone) {
          t->flag ^= T_ALT_TRANSFORM;
          calcEdgeSlideCustomPoints(t);
          slp->update_status_bar = true;
          return TREDRAW_HARD;
        }
        break;
      case MOUSEMOVE:
        calcEdgeSlideCustomPoints(t);
        break;
      default:
        break;
    }
  }
  return TREDRAW_NOTHING;
}

static void drawEdgeSlide(TransInfo *t)
{
  EdgeSlideData *sld = edgeSlideFirstGet(t);
  if (sld == nullptr) {
    return;
  }

  const EdgeSlideParams *slp = static_cast<const EdgeSlideParams *>(t->custom.mode.data);
  const bool is_clamp = edge_slide_use_clamp(t);

  const float line_size = ui::theme::get_value_f(TH_OUTLINE_WIDTH) + 0.5f;

  GPU_depth_test(GPU_DEPTH_NONE);

  GPU_blend(GPU_BLEND_ALPHA);

  if (t->spacetype == SPACE_VIEW3D) {
    GPU_matrix_push();
    GPU_matrix_mul(TRANS_DATA_CONTAINER_FIRST_OK(t)->obedit->object_to_world().ptr());
  }

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  TransDataEdgeSlideVert *curr_sv = &sld->sv[sld->curr_sv_index];
  const float3 curr_sv_co_orig = curr_sv->v_co_orig();

  if (slp->use_even == true) {
    /* Even mode. */
    float co_a[3], co_b[3], co_mark[3];
    const float fac = (slp->perc + 1.0f) / 2.0f;
    const float ctrl_size = ui::theme::get_value_f(TH_FACEDOT_SIZE) + 1.5f;
    const float guide_size = ctrl_size - 0.5f;
    const int alpha_shade = -30;

    add_v3_v3v3(co_a, curr_sv_co_orig, curr_sv->dir_side[0]);
    add_v3_v3v3(co_b, curr_sv_co_orig, curr_sv->dir_side[1]);

    GPU_line_width(line_size);
    immUniformThemeColorShadeAlpha(TH_EDGE_SELECT, 80, alpha_shade);
    immBeginAtMost(GPU_PRIM_LINES, 4);
    if (!math::is_zero(curr_sv->dir_side[0])) {
      immVertex3fv(pos, co_a);
      immVertex3fv(pos, curr_sv_co_orig);
    }
    if (!math::is_zero(curr_sv->dir_side[1])) {
      immVertex3fv(pos, co_b);
      immVertex3fv(pos, curr_sv_co_orig);
    }
    immEnd();
    immUnbindProgram();

    immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);
    {
      float *co_test = nullptr;
      if (slp->flipped) {
        if (!math::is_zero(curr_sv->dir_side[1])) {
          co_test = co_b;
        }
      }
      else {
        if (!math::is_zero(curr_sv->dir_side[0])) {
          co_test = co_a;
        }
      }

      if (co_test != nullptr) {
        immUniformThemeColorShadeAlpha(TH_SELECT, -30, alpha_shade);
        GPU_point_size(ctrl_size);
        immBegin(GPU_PRIM_POINTS, 1);
        immVertex3fv(pos, co_test);
        immEnd();
      }
    }

    immUniformThemeColorShadeAlpha(TH_SELECT, 255, alpha_shade);
    GPU_point_size(guide_size);
    immBegin(GPU_PRIM_POINTS, 1);
    interp_line_v3_v3v3v3(co_mark, co_b, curr_sv_co_orig, co_a, fac);
    immVertex3fv(pos, co_mark);
    immEnd();
  }
  else if (is_clamp == false) {
    const int side_index = slp->curr_side_unclamp;
    const int alpha_shade = -160;

    GPU_line_width(line_size);
    immUniformThemeColorShadeAlpha(TH_EDGE_SELECT, 80, alpha_shade);
    immBegin(GPU_PRIM_LINES, sld->sv.size() * 2);

    /* TODO(@ideasman42): Loop over all verts. */
    for (TransDataEdgeSlideVert &sv : sld->sv) {
      float a[3], b[3];

      if (!is_zero_v3(sv.dir_side[side_index])) {
        copy_v3_v3(a, sv.dir_side[side_index]);
      }
      else {
        copy_v3_v3(a, sv.dir_side[!side_index]);
      }

      mul_v3_fl(a, 100.0f);
      negate_v3_v3(b, a);

      const float3 sv_co_orig = sv.v_co_orig();
      add_v3_v3(a, sv_co_orig);
      add_v3_v3(b, sv_co_orig);

      immVertex3fv(pos, a);
      immVertex3fv(pos, b);
    }
    immEnd();
  }
  else if (slp->use_clone) {
    if (!sld->original_verts.is_empty()) {
      Map<BMVert *, BMVert *> orig_to_clone;
      orig_to_clone.reserve(sld->sv.size());
      for (int i = 0; i < int(sld->sv.size()); i++) {
        orig_to_clone.add(sld->original_verts[i], static_cast<BMVert *>(sld->sv[i].td->extra));
      }

      Set<BMVert *> chained_verts;
      if (!sld->clone_chain_edges.is_empty()) {
        GPU_line_width(line_size);
        immUniformThemeColorShadeAlpha(TH_EDGE_SELECT, 80, -100);
        immBeginAtMost(GPU_PRIM_LINES, sld->clone_chain_edges.size() * 2);
        for (const EdgeSlideData::CloneChainEdge &ce : sld->clone_chain_edges) {
          BMVert *v1_clone = orig_to_clone.lookup_default(ce.v1, nullptr);
          BMVert *v2_clone = orig_to_clone.lookup_default(ce.v2, nullptr);
          if (v1_clone && v2_clone) {
            chained_verts.add(ce.v1);
            chained_verts.add(ce.v2);
            immVertex3fv(pos, v1_clone->co);
            immVertex3fv(pos, v2_clone->co);
          }
        }
        immEnd();
      }

      if (chained_verts.size() < sld->original_verts.size()) {
        immUnbindProgram();
        immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);
        immUniformThemeColorShadeAlpha(TH_SELECT, 255, -30);
        GPU_point_size(ui::theme::get_value_f(TH_FACEDOT_SIZE) + 1.5f);
        immBeginAtMost(GPU_PRIM_POINTS, sld->original_verts.size());
        for (int i = 0; i < int(sld->original_verts.size()); i++) {
          if (!chained_verts.contains(sld->original_verts[i])) {
            immVertex3fv(pos, static_cast<BMVert *>(sld->sv[i].td->extra)->co);
          }
        }
        immEnd();
      }
    }
  }
  else {
    /* Common case. */
    const int alpha_shade = -160;

    float co_dir[3];
    add_v3_v3v3(co_dir, curr_sv_co_orig, curr_sv->dir_side[slp->curr_side_unclamp]);

    GPU_line_width(line_size);
    immUniformThemeColorShadeAlpha(TH_EDGE_SELECT, 80, alpha_shade);
    immBeginAtMost(GPU_PRIM_LINES, 2);
    immVertex3fv(pos, curr_sv_co_orig);
    immVertex3fv(pos, co_dir);
    immEnd();
  }

  immUnbindProgram();

  if (t->spacetype == SPACE_VIEW3D) {
    GPU_matrix_pop();
    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  }

  GPU_blend(GPU_BLEND_NONE);
}

static void edge_slide_snap_apply(TransInfo *t, float *value)
{
  TransDataContainer *tc = edge_slide_container_first_ok(t);
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  EdgeSlideData *sld_active = static_cast<EdgeSlideData *>(tc->custom.mode.data);
  TransDataEdgeSlideVert *sv = &sld_active->sv[sld_active->curr_sv_index];
  float3 co_orig, co_dest[2], dvec, snap_point;
  co_orig = sv->v_co_orig();
  co_dest[0] = co_orig + sv->dir_side[0];
  co_dest[1] = co_orig + sv->dir_side[1];

  if (tc->use_local_mat) {
    mul_m4_v3(tc->mat, co_orig);
    mul_m4_v3(tc->mat, co_dest[0]);
    mul_m4_v3(tc->mat, co_dest[1]);
  }

  getSnapPoint(t, dvec);
  sub_v3_v3(dvec, t->tsnap.snap_source);
  add_v3_v3v3(snap_point, co_orig, dvec);

  float perc = *value;
  int side_index;
  float t_mid;
  if (slp->use_even == false) {
    const bool is_clamp = edge_slide_use_clamp(t);
    if (is_clamp) {
      side_index = perc < 0.0f;
    }
    else {
      /* Use the side indicated in `EdgeSlideParams::curr_side_unclamp` as long as that side is not
       * zero length. */
      side_index = int(slp->curr_side_unclamp ==
                       !math::is_zero(sv->dir_side[slp->curr_side_unclamp]));
    }
  }
  else {
    /* Could be pre-calculated. */
    t_mid = line_point_factor_v3(float3{0.0f, 0.0f, 0.0f}, sv->dir_side[0], sv->dir_side[1]);

    float t_snap = line_point_factor_v3(snap_point, co_dest[0], co_dest[1]);
    side_index = t_snap >= t_mid;
  }

  if (t->tsnap.target_type & (SCE_SNAP_TO_EDGE | SCE_SNAP_TO_FACE)) {
    float co_dir[3];
    sub_v3_v3v3(co_dir, co_dest[side_index], co_orig);
    normalize_v3(co_dir);
    if (t->tsnap.target_type & SCE_SNAP_TO_EDGE) {
      transform_constraint_snap_axis_to_edge(t, co_dir, dvec);
    }
    else {
      transform_constraint_snap_axis_to_face(t, co_dir, dvec);
    }
    add_v3_v3v3(snap_point, co_orig, dvec);
  }

  perc = line_point_factor_v3(snap_point, co_orig, co_dest[side_index]);
  if (slp->use_even == false) {
    if (side_index) {
      perc *= -1;
    }
  }
  else {
    if (!side_index) {
      perc = (1.0f - perc) * t_mid;
    }
    else {
      perc = perc * (1.0f - t_mid) + t_mid;
    }

    if (slp->flipped) {
      perc = 1.0f - perc;
    }

    perc = (2 * perc) - 1.0f;

    if (!slp->flipped) {
      perc *= -1;
    }
  }

  *value = perc;
}

static void edge_slide_apply_elem(const TransDataEdgeSlideVert &sv,
                                  const float fac,
                                  const float curr_length_fac,
                                  const int curr_side_unclamp,
                                  const bool use_clamp,
                                  const bool use_even,
                                  const bool use_flip,
                                  float r_co[3])
{
  copy_v3_v3(r_co, sv.v_co_orig());

  if (use_even == false) {
    if (use_clamp) {
      const int side_index = (fac < 0.0f);
      const float fac_final = fabsf(fac);
      madd_v3_v3fl(r_co, sv.dir_side[side_index], fac_final);
    }
    else {
      int side_index = curr_side_unclamp;
      if (is_zero_v3(sv.dir_side[side_index])) {
        side_index = int(!side_index);
      }
      const float fac_final = (side_index == (fac < 0.0f) ? fabsf(fac) : -fabsf(fac));
      madd_v3_v3fl(r_co, sv.dir_side[side_index], fac_final);
    }
  }
  else {
    /**
     * NOTE(@ideasman42): Implementation note, even mode ignores the starting positions and uses
     * only the a/b verts, this could be changed/improved so the distance is
     * still met but the verts are moved along their original path (which may not be straight),
     * however how it works now is OK and matches 2.4x.
     *
     * \note `len_v3v3(curr_sv->dir_side[0], curr_sv->dir_side[1])`
     * is the same as the distance between the original vert locations,
     * same goes for the lines below.
     */
    if (sv.edge_len > FLT_EPSILON) {
      float co_a[3], co_b[3];
      const float fac_final = min_ff(sv.edge_len, curr_length_fac) / sv.edge_len;

      add_v3_v3v3(co_a, r_co, sv.dir_side[0]);
      add_v3_v3v3(co_b, r_co, sv.dir_side[1]);

      if (use_flip) {
        interp_line_v3_v3v3v3(r_co, co_b, r_co, co_a, fac_final);
      }
      else {
        interp_line_v3_v3v3v3(r_co, co_a, r_co, co_b, fac_final);
      }
    }
  }
}

static void doEdgeSlide(TransInfo *t, float perc)
{
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  EdgeSlideData *sld_active = edgeSlideFirstGet(t);

  slp->perc = perc;

  const bool use_clamp = edge_slide_use_clamp(t);
  const bool use_even = slp->use_even;
  const bool use_flip = slp->flipped;

  const int curr_side_unclamp = slp->curr_side_unclamp;
  float curr_length_fac = 0.0f;
  if (use_even) {
    TransDataEdgeSlideVert *sv_active = &sld_active->sv[sld_active->curr_sv_index];
    curr_length_fac = sv_active->edge_len * (((use_flip ? perc : -perc) + 1.0f) / 2.0f);
  }
  else if (use_clamp) {
    slp->curr_side_unclamp = (perc < 0.0f);
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    EdgeSlideData *sld = static_cast<EdgeSlideData *>(tc->custom.mode.data);

    if (sld == nullptr) {
      continue;
    }

    for (TransDataEdgeSlideVert &sv : sld->sv) {
      edge_slide_apply_elem(
          sv, perc, curr_length_fac, curr_side_unclamp, use_clamp, use_even, use_flip, sv.td->loc);
    }
  }
}

static void applyEdgeSlide(TransInfo *t)
{
  char str[UI_MAX_DRAW_STR];
  size_t ofs = 0;
  float final;
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  bool flipped = slp->flipped;
  bool use_even = slp->use_even;
  const bool is_clamp = edge_slide_use_clamp(t);
  const bool is_constrained = !(is_clamp == false || hasNumInput(&t->num));
  const bool is_precision = t->modifiers & MOD_PRECISION;
  const bool is_snap = t->modifiers & MOD_SNAP;
  const bool is_snap_invert = t->modifiers & MOD_SNAP_INVERT;

  final = t->values[0] + t->values_modal_offset[0];

  transform_snap_mixed_apply(t, &final);
  if (!validSnap(t)) {
    transform_snap_increment(t, &final);
  }

  /* Only do this so out of range values are not displayed. */
  if (is_constrained) {
    CLAMP(final, -1.0f, 1.0f);
  }

  applyNumInput(&t->num, &final);

  t->values_final[0] = final;

  /* Header string. */
  ofs += BLI_strncpy_utf8_rlen(str + ofs, RPT_("Edge Slide: "), sizeof(str) - ofs);
  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN];
    outputNumInput(&(t->num), c, t->scene->unit);
    ofs += BLI_strncpy_utf8_rlen(str + ofs, &c[0], sizeof(str) - ofs);
  }
  else {
    ofs += BLI_snprintf_utf8_rlen(str + ofs, sizeof(str) - ofs, "%.4f ", final);
  }
  /* Done with header string. */

  /* Do stuff here. */
  doEdgeSlide(t, final);

  recalc_data(t);

  ED_area_status_text(t->area, str);

  wmOperator *op = slp->op;
  if (!op) {
    return;
  }

  if (slp->update_status_bar) {
    slp->update_status_bar = false;

    WorkspaceStatus status(t->context);
    status.opmodal(IFACE_("Confirm"), op->type, TFM_MODAL_CONFIRM);
    status.opmodal(IFACE_("Cancel"), op->type, TFM_MODAL_CANCEL);
    status.opmodal(IFACE_("Snap"), op->type, TFM_MODAL_SNAP_TOGGLE, is_snap);
    status.opmodal(IFACE_("Snap Invert"), op->type, TFM_MODAL_SNAP_INV_ON, is_snap_invert);
    status.opmodal(IFACE_("Set Snap Base"), op->type, TFM_MODAL_EDIT_SNAP_SOURCE_ON);
    status.opmodal(IFACE_("Move"), op->type, TFM_MODAL_TRANSLATE);
    status.opmodal(IFACE_("Rotate"), op->type, TFM_MODAL_ROTATE);
    status.opmodal(IFACE_("Resize"), op->type, TFM_MODAL_RESIZE);
    status.opmodal(IFACE_("Precision Mode"), op->type, TFM_MODAL_PRECISION, is_precision);
    if (!slp->use_clone) {
      status.item_bool(IFACE_("Clamp"), is_clamp, ICON_EVENT_C, ICON_EVENT_ALT);
    }
    status.item_bool(IFACE_("Even"), use_even, ICON_EVENT_E);
    if (use_even) {
      status.item_bool(IFACE_("Flipped"), flipped, ICON_EVENT_F);
    }
  }
}

static void edge_slide_transform_matrix_fn(TransInfo *t, float mat_xform[4][4])
{
  float delta[3], orig_co[3], final_co[3];

  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  TransDataContainer *tc = edge_slide_container_first_ok(t);
  EdgeSlideData *sld_active = static_cast<EdgeSlideData *>(tc->custom.mode.data);
  TransDataEdgeSlideVert &sv_active = sld_active->sv[sld_active->curr_sv_index];

  copy_v3_v3(orig_co, sv_active.v_co_orig());

  const float fac = t->values_final[0];
  float curr_length_fac = 0.0f;
  if (slp->use_even) {
    curr_length_fac = sv_active.edge_len * (((slp->flipped ? fac : -fac) + 1.0f) / 2.0f);
  }

  edge_slide_apply_elem(sv_active,
                        fac,
                        curr_length_fac,
                        slp->curr_side_unclamp,
                        edge_slide_use_clamp(t),
                        slp->use_even,
                        slp->flipped,
                        final_co);

  if (tc->use_local_mat) {
    mul_m4_v3(tc->mat, orig_co);
    mul_m4_v3(tc->mat, final_co);
  }

  sub_v3_v3v3(delta, final_co, orig_co);
  add_v3_v3(mat_xform[3], delta);
}

/**
 * Copy loop data into f from the face across e_shared, matching corners by vertex.
 * Follows #bm_extrude_copy_face_loop_attributes: created side geometry inherits from the surface
 * it was built out of, so the band ends up a zero width UV strip.
 */
static void edge_slide_clone_loops_copy_across(BMesh *bm, BMFace *f, BMEdge *e_shared)
{
  BMLoop *l_dst = e_shared ? BM_face_edge_share_loop(f, e_shared) : nullptr;
  if (!l_dst) {
    return;
  }
  BMLoop *l_src = l_dst->radial_next;
  if (l_src == l_dst || l_src->f == f) {
    return;
  }
  const bool aligned = (l_src->v == l_dst->v);
  BM_elem_attrs_copy(bm, aligned ? l_src : l_src->next, l_dst);
  BM_elem_attrs_copy(bm, aligned ? l_src->next : l_src, l_dst->next);
}

/**
 * Give a link edge the data its two rungs agree on: a boolean flag continues across the band only
 * when both rungs carry it, scalar layers take the average of the two.
 */
static void edge_slide_clone_link_edge_data(BMesh *bm,
                                            BMEdge *e_link,
                                            BMEdge *e_rung_a,
                                            BMEdge *e_rung_b)
{
  if (!e_link || !e_rung_a || !e_rung_b) {
    return;
  }
  BM_data_interp_from_edges(bm, e_rung_a, e_rung_b, e_link, 0.5f);

  const char hflag_and = e_rung_a->head.hflag & e_rung_b->head.hflag;
  const char hflag_or = e_rung_a->head.hflag | e_rung_b->head.hflag;
  BM_elem_flag_set(e_link, BM_ELEM_SEAM, (hflag_and & BM_ELEM_SEAM) != 0);
  BM_elem_flag_set(e_link, BM_ELEM_SMOOTH, (hflag_or & BM_ELEM_SMOOTH) != 0);
}

void transform_mode_edge_slide_clone_confirm(TransInfo *t)
{
  if (t->mode != TFM_EDGE_SLIDE) {
    return;
  }

  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  if (!slp || !slp->use_clone) {
    return;
  }

  bool clone_topology_skipped = false;
  bool clone_dir_written = false;

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    EdgeSlideData *sld = static_cast<EdgeSlideData *>(tc->custom.mode.data);
    if (!sld || sld->original_verts.is_empty()) {
      continue;
    }

    BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);
    BMesh *bm = em->bm;
    const int keep_side = (t->values_final[0] >= 0.0f) ? 0 : 1;

    const Vector<CloneVertInfo> vert_infos = edge_slide_clone_verts_calc(sld);

    Map<BMVert *, BMVert *> orig_to_clone;
    orig_to_clone.reserve(sld->sv.size());
    for (int i = 0; i < int(sld->sv.size()); i++) {
      orig_to_clone.add(sld->original_verts[i], static_cast<BMVert *>(sld->sv[i].td->extra));
    }

    /* Separate keep-side face loops from each original vert. */
    Map<BMVert *, BMVert *> orig_to_sep;
    orig_to_sep.reserve(vert_infos.size());

    for (const CloneVertInfo &nd : vert_infos) {
      BMVert *v_orig = nd.v;

      Vector<BMLoop *> keep_loops;
      for (BMFace *f : nd.side[keep_side].faces) {
        if (BMLoop *l = BM_face_vert_share_loop(f, v_orig)) {
          keep_loops.append(l);
        }
      }

      if (keep_loops.is_empty()) {
        continue;
      }

      /* Separates keep side faces from the original loop. Creates separated vertices. */
      BMVert *v_sep = BM_face_loop_separate_multi(bm, keep_loops.data(), keep_loops.size());
      if (v_sep == v_orig) {
        v_sep = BM_face_loop_separate_multi_isolated(bm, keep_loops[0]);
      }
      orig_to_sep.add(v_orig, v_sep);
    }

    if (orig_to_sep.is_empty()) {
      clone_topology_skipped = true;
    }

    /* Record the direction a positive factor produced, for the next run to align to. */
    if (slp->op && !clone_dir_written && !orig_to_sep.is_empty()) {
      float3 dir(0);
      for (const CloneVertInfo &nd : vert_infos) {
        BMVert *v_clone = orig_to_clone.lookup(nd.v);
        dir += float3(v_clone->co) - float3(nd.v->co);
      }
      if (!math::is_zero(dir)) {
        dir = math::normalize(dir);
        if (t->values_final[0] < 0.0f) {
          dir = -dir;
        }
        const float dir_arr[3] = {dir.x, dir.y, dir.z};
        RNA_float_set_array(slp->op->ptr, "clone_dir", dir_arr);
        clone_dir_written = true;
      }
    }

    /* The band subdivides the keep side faces rather than adding surface, so both it and their own
     * moved corners take their loop data from the pre-slide face. */
    Set<BMVert *> sep_verts;
    Map<BMFace *, BMFace *> keep_face_orig;
    BMeshCreateParams bm_orig_params{};
    bm_orig_params.use_toolflags = false;
    BMesh *bm_orig = BM_mesh_create(&bm_mesh_allocsize_default, &bm_orig_params);
    BM_mesh_copy_init_customdata_all_layers(bm_orig, bm, BM_LOOP, nullptr);
    {
      const BMCustomDataCopyMap cd_face_map = CustomData_bmesh_copy_map_calc(bm->pdata,
                                                                             bm_orig->pdata);
      const BMCustomDataCopyMap cd_loop_map = CustomData_bmesh_copy_map_calc(bm->ldata,
                                                                             bm_orig->ldata);
      for (BMVert *v_sep : orig_to_sep.values()) {
        sep_verts.add(v_sep);
        BMFace *f;
        BMIter f_iter;
        BM_ITER_ELEM (f, &f_iter, v_sep, BM_FACES_OF_VERT) {
          keep_face_orig.lookup_or_add_cb(
              f, [&]() { return BM_face_copy(bm_orig, cd_face_map, cd_loop_map, f, true, true); });
        }
      }
    }

    /* Reposition separated verts to clone positions, then kill clone verts. */
    for (const CloneVertInfo &nd : vert_infos) {
      BMVert *v_orig = nd.v;
      BMVert *v_clone = orig_to_clone.lookup(v_orig);
      BMVert *v_sep = orig_to_sep.lookup_default(v_orig, nullptr);

      if (v_sep) {
        copy_v3_v3(v_sep->co, v_clone->co);
      }
      BM_vert_kill(bm, v_clone);
    }

    /* The keep side faces kept the loop data of the corners they were separated from, which now
     * sit a slide away. Reinterpolate them so the texture stays where it was. */
    for (const auto item : keep_face_orig.items()) {
      BMLoop *l_iter = BM_FACE_FIRST_LOOP(item.key);
      for (int i = 0; i < item.key->len; i++, l_iter = l_iter->next) {
        if (sep_verts.contains(l_iter->v)) {
          BM_loop_interp_from_face(bm, l_iter, item.value, false, false);
        }
      }
    }

    /* Create band faces from original vertices to the separated vertices. */
    for (const EdgeSlideData::CloneChainEdge &ce : sld->clone_chain_edges) {
      BMVert *v_sep1 = orig_to_sep.lookup_default(ce.v1, nullptr);
      BMVert *v_sep2 = orig_to_sep.lookup_default(ce.v2, nullptr);

      if (!v_sep1 || !v_sep2) {
        continue;
      }

      BMEdge *e_sep = BM_edge_exists(v_sep1, v_sep2);
      BMEdge *e_orig = BM_edge_exists(ce.v1, ce.v2);

      /* The band fills the area the keep side face vacated, so it inherits that face's attributes */
      BMFace *f_example = (e_sep && e_sep->l)   ? e_sep->l->f :
                          (e_orig && e_orig->l) ? e_orig->l->f :
                                                  nullptr;

      BMVert *quad[4] = {ce.v1, v_sep1, v_sep2, ce.v2};
      BMFace *f_band = BM_face_create_verts(bm, quad, 4, f_example, BM_CREATE_NOP, true);
      if (f_band) {
        BM_elem_flag_disable(f_band, BM_ELEM_HIDDEN);
        BMEdge *ref_edges[2] = {e_sep, e_orig};
        for (int r = 0; r < 2; r++) {
          BMLoop *l_band = ref_edges[r] ? BM_face_edge_share_loop(f_band, ref_edges[r]) : nullptr;
          BMLoop *l_ref = l_band ? l_band->radial_next : nullptr;
          if (l_band && l_ref && l_ref != l_band) {
            if (l_ref->v == l_band->v) {
              BM_face_normal_flip(bm, f_band);
            }
            break;
          }
        }
        BMFace *f_orig = f_example ? keep_face_orig.lookup_default(f_example, nullptr) : nullptr;
        if (f_orig) {
          BMLoop *l_iter = BM_FACE_FIRST_LOOP(f_band);
          for (int i = 0; i < f_band->len; i++, l_iter = l_iter->next) {
            BM_loop_interp_from_face(bm, l_iter, f_orig, false, false);
          }
        }
        else {
          edge_slide_clone_loops_copy_across(bm, f_band, e_orig);
          edge_slide_clone_loops_copy_across(bm, f_band, e_sep);
        }
      }
    }

    /* Where the keep-side fan ended on a rung rather than on a chain edge
     * the face beyond that rung keeps the original vert, so it takes the transition polygon. */
    for (const CloneVertInfo &nd : vert_infos) {
      BMVert *v_orig = nd.v;
      BMVert *v_sep = orig_to_sep.lookup_default(v_orig, nullptr);
      if (!v_sep) {
        continue;
      }

      for (BMVert *nb_bound : nd.side[keep_side].bound_verts) {
        /* Where the chain doubles back the vert across the rung is a chain vert that may have
         * taken the rung along, so insert against whichever one the face beyond it still holds. */
        BMVert *nb_cands[2] = {orig_to_sep.lookup_default(nb_bound, nullptr), nb_bound};
        BMVert *nb_keep = nullptr;
        Vector<BMFace *> faces_to_fix;
        for (BMVert *nb_cand : nb_cands) {
          if (!nb_cand) {
            continue;
          }
          BMFace *f;
          BMIter f_iter;
          BM_ITER_ELEM (f, &f_iter, v_orig, BM_FACES_OF_VERT) {
            if (BM_vert_in_face(nb_cand, f) && !BM_vert_in_face(v_sep, f)) {
              faces_to_fix.append(f);
            }
          }
          if (!faces_to_fix.is_empty()) {
            nb_keep = nb_cand;
            break;
          }
        }
        if (!nb_keep) {
          continue;
        }

        for (BMFace *face : faces_to_fix) {
          /* Rebuild face with v_sep inserted between nb_keep and v_orig. */
          Vector<BMVert *> new_verts;
          new_verts.reserve(face->len + 1);
          BMLoop *start = BM_FACE_FIRST_LOOP(face);
          for (int i = 0; i < face->len; i++, start = start->next) {
            new_verts.append(start->v);
            /* Insert v_sep on whichever side of v_orig nb_keep is adjacent. */
            if ((start->v == nb_keep && start->next->v == v_orig) ||
                (start->v == v_orig && start->next->v == nb_keep))
            {
              new_verts.append(v_sep);
            }
          }

          if (int(new_verts.size()) == face->len + 1) {
            BMFace *new_face = BM_face_create_verts(
                bm, new_verts.data(), new_verts.size(), face, BM_CREATE_NOP, true);
            if (new_face) {
              /* Face creation copies face data only. */
              BMLoop *l_new = BM_FACE_FIRST_LOOP(new_face);
              for (int i = 0; i < new_face->len; i++, l_new = l_new->next) {
                if (l_new->v == v_sep) {
                  BM_loop_interp_from_face(bm, l_new, face, false, true);
                  continue;
                }
                if (BMLoop *l_old = BM_face_vert_share_loop(face, l_new->v)) {
                  BM_elem_attrs_copy(bm, l_old, l_new);
                }
              }
              BM_face_kill(bm, face);
              BMEdge *stale_edge = BM_edge_exists(v_orig, nb_keep);
              if (stale_edge && BM_edge_is_wire(stale_edge)) {
                BM_edge_kill(bm, stale_edge);
              }
            }
          }
        }
      }
    }

    for (const CloneVertInfo &nd : vert_infos) {
      BMVert *v_orig = nd.v;
      BMVert *v_sep = orig_to_sep.lookup_default(v_orig, nullptr);
      if (!v_sep) {
        continue;
      }
      BMEdge *e_link = BM_edge_exists(v_orig, v_sep);
      if (!e_link) {
        continue;
      }

      BMEdge *e_rung_keep = nullptr;
      for (BMVert *nb : nd.side[keep_side].rung_verts) {
        if (orig_to_clone.contains(nb)) {
          continue;
        }
        e_rung_keep = BM_edge_exists(v_sep, nb);
        if (e_rung_keep) {
          break;
        }
      }
      BMEdge *e_rung_other = nullptr;
      for (BMVert *nb : nd.side[1 - keep_side].rung_verts) {
        if (orig_to_clone.contains(nb)) {
          continue;
        }
        e_rung_other = BM_edge_exists(v_orig, nb);
        if (e_rung_other) {
          break;
        }
      }
      edge_slide_clone_link_edge_data(bm, e_link, e_rung_keep, e_rung_other);
    }

    /* Leave the slid loop selected so the result is chainable. */
    if (orig_to_sep.is_empty()) {
      for (const CloneVertInfo &nd : vert_infos) {
        BM_vert_select_set(bm, nd.v, true);
      }
    }
    else {
      for (const CloneVertInfo &nd : vert_infos) {
        BM_vert_select_set(bm, nd.v, false);
        if (BMVert *v_sep = orig_to_sep.lookup_default(nd.v, nullptr)) {
          BM_vert_select_set(bm, v_sep, true);
        }
      }
      for (const EdgeSlideData::CloneChainEdge &ce : sld->clone_chain_edges) {
        if (BMEdge *e_orig = BM_edge_exists(ce.v1, ce.v2)) {
          BM_edge_select_set(bm, e_orig, false);
        }
        BMVert *v_sep1 = orig_to_sep.lookup_default(ce.v1, nullptr);
        BMVert *v_sep2 = orig_to_sep.lookup_default(ce.v2, nullptr);
        if (v_sep1 && v_sep2) {
          if (BMEdge *e_new = BM_edge_exists(v_sep1, v_sep2)) {
            BM_edge_select_set(bm, e_new, true);
          }
        }
      }
    }

    for (const CloneVertInfo &nd : vert_infos) {
      if (orig_to_sep.contains(nd.v) && nd.v->e == nullptr) {
        BM_vert_kill(bm, nd.v);
      }
    }

    BM_mesh_free(bm_orig);

    EDBM_selectmode_flush(em);

    EDBMUpdate_Params update_params{};
    update_params.calc_looptris = true;
    update_params.calc_normals = true;
    update_params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(tc->obedit->data), &update_params);
  }

  if (clone_topology_skipped) {
    BKE_report(
        t->reports, RPT_WARNING, "Clone slide skipped: no adjacent edge loop on the slide side");
  }
}

static void initEdgeSlide_ex(TransInfo *t,
                             wmOperator *op,
                             bool use_double_side,
                             bool use_even,
                             bool flipped,
                             bool use_clamp,
                             bool use_clone)
{
  EdgeSlideData *sld;
  bool ok = false;

  t->mode = TFM_EDGE_SLIDE;

  {
    EdgeSlideParams *slp = MEM_new_zeroed<EdgeSlideParams>(__func__);
    slp->op = op;
    slp->use_even = use_even;
    slp->flipped = flipped;
    /* Happens to be best for single-sided. */
    if (use_double_side == false) {
      slp->flipped = !flipped;
    }
    slp->perc = 0.0f;
    slp->update_status_bar = true;
    slp->use_clone = use_clone;

    if (!use_clamp) {
      t->flag |= T_ALT_TRANSFORM;
    }

    t->custom.mode.data = slp;
    t->custom.mode.use_free = true;
  }

  float3 clone_align_dir(0);
  bool has_clone_align = false;
  if (use_clone && op) {
    PropertyRNA *prop = RNA_struct_find_property(op->ptr, "clone_dir");
    if (prop && RNA_property_is_set(op->ptr, prop)) {
      float dir[3];
      RNA_property_float_get_array(op->ptr, prop, dir);
      clone_align_dir = float3(dir);
      has_clone_align = !math::is_zero(clone_align_dir);
    }
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    sld = createEdgeSlideVerts(
        t, tc, use_double_side, has_clone_align ? &clone_align_dir : nullptr);
    if (sld) {
      tc->custom.mode.data = sld;
      tc->custom.mode.free_cb = freeEdgeSlideVerts;
      ok = true;
    }
  }

  /* Cloning flag for edge slide. */
  if (use_clone && ok && t->data_type != &TransConvertType_MeshUV) {
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      EdgeSlideData *sld = static_cast<EdgeSlideData *>(tc->custom.mode.data);
      if (sld) {
        BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);
        BMesh *bm = em->bm;

        Map<BMVert *, BMVert *> vert_map;
        vert_map.reserve(sld->sv.size());

        for (TransDataEdgeSlideVert &sv : sld->sv) {
          /* duplicate bmesh verts */
          BMVert *v = static_cast<BMVert *>(sv.td->extra);
          BMVert *v_clone = BM_vert_create(bm, v->co, v, BM_CREATE_NOP);

          sld->original_verts.append(v);
          vert_map.add(v, v_clone);

          /* transform clones, not originals */
          BM_elem_flag_disable(v, BM_ELEM_SELECT);

          sv.td->loc = v_clone->co;
          sv.td->extra = v_clone;
        }

        /* Recreate the selected chain edges between the clones. */
        for (BMVert *v_orig : sld->original_verts) {
          BMEdge *e;
          BMIter e_iter;
          BM_ITER_ELEM (e, &e_iter, v_orig, BM_EDGES_OF_VERT) {
            if (e->v1 != v_orig || !BM_elem_flag_test(e, BM_ELEM_SELECT)) {
              continue;
            }
            BMVert *v1 = vert_map.lookup_default(e->v1, nullptr);
            BMVert *v2 = vert_map.lookup_default(e->v2, nullptr);

            if (v1 && v2) {
              BM_edge_create(bm, v1, v2, e, BM_CREATE_NOP);
              sld->clone_chain_edges.append({e->v1, e->v2});
            }
          }
        }

        /* Keep the clones out of the evaluated mesh, the overlay draws them.
         * Runs after the chain edges exist so they are hidden along with the verts. */
        for (BMVert *v_orig : sld->original_verts) {
          BM_vert_hide_set(vert_map.lookup(v_orig), true);
        }

        EDBM_selectmode_flush(em);
      }
    }
  }

  if (!ok) {
    t->state = TRANS_CANCEL;
    return;
  }

  /* Set custom point first if you want value to be initialized by init. */
  calcEdgeSlideCustomPoints(t);
  initMouseInputMode(t, &t->mouse, INPUT_CUSTOM_RATIO_FLIP);

  t->idx_max = 0;
  t->num.idx_max = 0;
  t->increment[0] = 0.1f;
  t->increment_precision = 0.1f;

  copy_v3_fl(t->num.val_inc, t->increment[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;
}

static void initEdgeSlide(TransInfo *t, wmOperator *op)
{
  bool use_double_side = true;
  bool use_even = false;
  bool flipped = false;
  bool use_clamp = true;
  bool use_clone = false;
  if (op) {
    PropertyRNA *prop;
    /* The following properties could be unset when transitioning from this
     * operator to another and back. For example pressing "G" to move, and
     * then "G" again to go back to edge slide. */
    prop = RNA_struct_find_property(op->ptr, "single_side");
    use_double_side = (prop) ? !RNA_property_boolean_get(op->ptr, prop) : true;
    prop = RNA_struct_find_property(op->ptr, "use_even");
    use_even = (prop) ? RNA_property_boolean_get(op->ptr, prop) : false;
    prop = RNA_struct_find_property(op->ptr, "flipped");
    flipped = (prop) ? RNA_property_boolean_get(op->ptr, prop) : false;
    prop = RNA_struct_find_property(op->ptr, "use_clamp");
    use_clamp = (prop) ? RNA_property_boolean_get(op->ptr, prop) : true;
    prop = RNA_struct_find_property(op->ptr, "use_clone");
    use_clone = (prop) ? RNA_property_boolean_get(op->ptr, prop) : false;
  }
  initEdgeSlide_ex(t, op, use_double_side, use_even, flipped, use_clamp, use_clone);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mouse Input Utilities
 * \{ */

void transform_mode_edge_slide_reproject_input(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    EdgeSlideData *sld = static_cast<EdgeSlideData *>(tc->custom.mode.data);
    if (sld) {
      sld->update_proj_mat(t, tc);
      TransDataEdgeSlideVert *curr_sv = &sld->sv[sld->curr_sv_index];

      float2 sco_a, sco_b;
      sld->project(curr_sv, sco_a, sco_b);
      float2 mval_dir = sco_b - sco_a;
      edge_slide_data_init_mval(&t->mouse, sld, mval_dir);
    }
  }

  EdgeSlideData *sld = edgeSlideFirstGet(t);
  setCustomPoints(t, &t->mouse, sld->mval_end, sld->mval_start);
}

/** \} */

TransModeInfo TransMode_edgeslide = {
    /*flags*/ T_NO_CONSTRAINT,
    /*init_fn*/ initEdgeSlide,
    /*transform_fn*/ applyEdgeSlide,
    /*transform_matrix_fn*/ edge_slide_transform_matrix_fn,
    /*handle_event_fn*/ handleEventEdgeSlide,
    /*snap_distance_fn*/ transform_snap_distance_len_squared_fn,
    /*snap_apply_fn*/ edge_slide_snap_apply,
    /*draw_fn*/ drawEdgeSlide,
};

}  // namespace blender::ed::transform
