/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_object.hh"
#include "BLI_math_matrix.h"
#include "BLI_time.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"
#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"
#include "ED_view3d_pencil_hover.hh"
#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "bmesh.hh"

namespace blender {
namespace {
struct HoverState {
  ARegion *region = nullptr;
  View3DPencilHover hit;
  int2 position = int2(-1);
  float4x4 view = float4x4::identity();
  uint64_t evaluation = 0;
  int mode = -1, selectmode = -1, xray = -1, shading = -1;
  bool objects_only = false;
  uint32_t active_uid = 0;
  double last_pick = 0;
  /* Own coordinates, never BMesh pointers: topology edits and undo can free them. */
  Vector<float3> points, lines, triangles;
};
HoverState hover;
}  // namespace

View3DPencilHover ED_view3d_pencil_hover_get(const ARegion *region)
{
  return region && hover.region == region ? hover.hit : View3DPencilHover{};
}

void ED_view3d_pencil_hover_clear(ARegion *region)
{
  if (region && hover.region != region) {
    return;
  }
  if (hover.region && hover.hit.kind) {
    if (hover.hit.kind == 1) {
      ED_region_tag_redraw(hover.region);
    }
    else {
      ED_region_tag_redraw_editor_overlays(hover.region);
    }
  }
  hover = {};
}

void ED_view3d_pencil_hover_update(bContext *C,
                                   bool active,
                                   const int mval[2],
                                   bool objects_only)
{
  ARegion *region = CTX_wm_region(C);
  if (hover.region != region) {
    ED_view3d_pencil_hover_clear(nullptr);
  }
  View3D *v3d = CTX_wm_view3d(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Object *object = CTX_data_active_object(C);
  const int mode = object ? object->mode : OB_MODE_OBJECT;
  if (!active || !region || !v3d || !rv3d ||
      !(objects_only || ELEM(mode, OB_MODE_OBJECT, OB_MODE_EDIT)) ||
      G.moving || (rv3d->rflag & RV3D_NAVIGATING) || (v3d->flag2 & V3D_HIDE_OVERLAYS) ||
      mval[0] < 0 || mval[1] < 0 || mval[0] >= region->winx || mval[1] >= region->winy)
  {
    ED_view3d_pencil_hover_clear(region);
    return;
  }
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const int selectmode = CTX_data_scene(C)->toolsettings->selectmode;
  const int xray = XRAY_FLAG_ENABLED(v3d);
  const uint64_t evaluation = DEG_get_update_count(depsgraph);
  const uint32_t active_uid = object ? object->id.session_uid : 0;
  const bool same_context = hover.region == region && hover.mode == mode &&
                            hover.objects_only == objects_only &&
                            hover.selectmode == selectmode && hover.xray == xray &&
                            hover.evaluation == evaluation && hover.shading == v3d->shading.type &&
                            hover.active_uid == active_uid &&
                            equals_m4m4(hover.view.ptr(), rv3d->persmat);
  if (same_context && hover.position == int2(mval)) {
    return;
  }
  const double now = BLI_time_now_seconds();
  if (same_context && now - hover.last_pick < 1.0 / 60.0) {
    return;
  }
  hover.region = region;
  hover.position = int2(mval);
  hover.mode = mode;
  hover.objects_only = objects_only;
  hover.selectmode = selectmode;
  hover.xray = xray;
  hover.shading = v3d->shading.type;
  hover.active_uid = active_uid;
  hover.evaluation = evaluation;
  hover.view = float4x4(rv3d->persmat);
  hover.last_pick = now;
  const View3DPencilHover previous = hover.hit;
  hover.hit = {};
  hover.points.clear();
  hover.lines.clear();
  hover.triangles.clear();

  if (mode == OB_MODE_OBJECT || objects_only) {
    if (Base *base = ED_view3d_give_base_under_cursor(C, mval)) {
      if (base->flag & BASE_SELECTABLE) {
        hover.hit = {base->object->id.session_uid, 1, -1};
      }
    }
  }
  else if (object && object->type == OB_MESH) {
    view3d_operator_needs_gpu(C);
    ViewContext vc = em_setup_viewcontext(C);
    vc.mval[0] = mval[0];
    vc.mval[1] = mval[1];
    Vector<Base *> bases = BKE_view_layer_array_from_bases_in_edit_mode(
        vc.scene, vc.view_layer, v3d);
    BMVert *vert = nullptr;
    BMEdge *edge = nullptr;
    BMFace *face = nullptr;
    int base_index = -1;
    if (EDBM_unified_findnearest(&vc, bases, &base_index, &vert, &edge, &face, true)) {
      Object *ob = bases[base_index]->object;
      BMesh *bm = BKE_editmesh_from_object(ob)->bm;
      BM_mesh_elem_index_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
      const Object *evaluated = DEG_get_evaluated(depsgraph, ob);
      const Mesh *cage = BKE_object_get_editmesh_eval_cage(evaluated);
      Span<float3> positions;
      if (cage && BKE_mesh_wrapper_vert_len(cage) == bm->totvert) {
        positions = BKE_mesh_wrapper_vert_coords(cage);
      }
      auto coordinate = [&](BMVert *vertex) {
        float3 point = positions.is_empty() ? float3(vertex->co) :
                                              positions[BM_elem_index_get(vertex)];
        mul_m4_v3(ob->object_to_world().ptr(), point);
        return point;
      };
      hover.hit.object_uid = ob->id.session_uid;
      if (vert) {
        hover.hit.kind = 2;
        hover.hit.index = BM_elem_index_get(vert);
        hover.points.append(coordinate(vert));
      }
      else if (edge) {
        hover.hit.kind = 3;
        hover.hit.index = BM_elem_index_get(edge);
        hover.lines.append(coordinate(edge->v1));
        hover.lines.append(coordinate(edge->v2));
      }
      else if (face) {
        hover.hit.kind = 4;
        hover.hit.index = BM_elem_index_get(face);
        Array<BMLoop *> loops(face->len);
        Array<uint3> triangles(face->len - 2);
        BM_face_calc_tessellation(
            face, false, loops.data(), reinterpret_cast<uint(*)[3]>(triangles.data()));
        for (BMLoop *loop : loops) {
          hover.lines.append(coordinate(loop->v));
          hover.lines.append(coordinate(loop->next->v));
        }
        for (uint3 triangle : triangles) {
          for (int i = 0; i < 3; i++) {
            hover.triangles.append(coordinate(loops[triangle[i]]->v));
          }
        }
      }
    }
  }
  if (previous.kind || hover.hit.kind) {
    /* Object silhouettes live in the draw engine, not the editor's post-view callback. */
    if (previous.kind == 1 || hover.hit.kind == 1) {
      ED_region_tag_redraw(region);
    }
    else {
      ED_region_tag_redraw_editor_overlays(region);
    }
  }
}

void ED_view3d_pencil_hover_draw(const bContext *C, ARegion *region, void * /*customdata*/)
{
  if (hover.region != region || hover.hit.kind < 2 || G.moving || !CTX_data_edit_object(C) ||
      (CTX_wm_view3d(C)->flag2 & V3D_HIDE_OVERLAYS))
  {
    return;
  }
  /* Picking respects depth and X-Ray. Only the picked component is drawn on top. */
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);
  auto draw = [](Span<float3> coordinates, GPUPrimType primitive, float alpha) {
    if (coordinates.is_empty()) {
      return;
    }
    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    immBindBuiltinProgram(primitive == GPU_PRIM_POINTS ?
                              GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA :
                          primitive == GPU_PRIM_LINES ? GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR :
                                                        GPU_SHADER_3D_UNIFORM_COLOR);
    if (primitive == GPU_PRIM_POINTS) {
      immUniform1f("size", 6.0f * U.pixelsize);
    }
    else if (primitive == GPU_PRIM_LINES) {
      float viewport[4];
      GPU_viewport_size_get_f(viewport);
      immUniform2fv("viewportSize", &viewport[2]);
      immUniform1f("lineWidth", 2.0f * U.pixelsize);
    }
    immUniformColor4f(0.012f, 0.63f, 0.99f, alpha);
    immBegin(primitive, coordinates.size());
    for (const float3 &coordinate : coordinates) {
      immVertex3fv(pos, coordinate);
    }
    immEnd();
    immUnbindProgram();
  };
  draw(hover.triangles, GPU_PRIM_TRIS, 0.22f);
  draw(hover.lines, GPU_PRIM_LINES, 0.95f);
  GPU_program_point_size(true);
  draw(hover.points, GPU_PRIM_POINTS, 1.0f);
  GPU_program_point_size(false);
  GPU_blend(GPU_BLEND_NONE);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
}
}  // namespace blender
