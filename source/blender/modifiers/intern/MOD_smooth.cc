/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_screen_types.h"

#include "BKE_deform.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "RNA_types.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  SmoothModifierData *smd = reinterpret_cast<SmoothModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(smd, modifier);
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  SmoothModifierData *smd = reinterpret_cast<SmoothModifierData *>(md);

  const short flag = smd->flag & (MOD_SMOOTH_X | MOD_SMOOTH_Y | MOD_SMOOTH_Z);

  /* disable if modifier is off for X, Y and Z or if factor is 0 */
  if (smd->fac == 0.0f || flag == 0) {
    return true;
  }

  return false;
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  SmoothModifierData *smd = reinterpret_cast<SmoothModifierData *>(md);

  /* Ask for vertex-groups if we need them. */
  if (smd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static float vgroup_weight(const MDeformVert &dv, const int defgrp_index, const bool invert)
{
  const float w = BKE_defvert_find_weight(&dv, defgrp_index);
  return invert ? 1.0f - w : w;
}

static void laplacian_pass(const Span<int2> edges,
                           const Span<float3> src,
                           MutableSpan<float3> dst_avg,
                           MutableSpan<int> dst_count)
{
  dst_avg.fill(float3(0.0f));
  dst_count.fill(0);
  for (const int i : edges.index_range()) {
    const int idx1 = edges[i][0];
    const int idx2 = edges[i][1];
    const float3 mid = (src[idx1] + src[idx2]) * 0.5f;
    dst_count[idx1]++;
    dst_avg[idx1] += mid;
    dst_count[idx2]++;
    dst_avg[idx2] += mid;
  }
  threading::parallel_for(src.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      if (dst_count[i] > 0) {
        dst_avg[i] *= 1.0f / float(dst_count[i]);
      }
    }
  });
}

static void neighbor_avg_pass(const Span<int2> edges,
                              const Span<float3> src,
                              MutableSpan<float3> dst_avg,
                              MutableSpan<int> dst_count)
{
  dst_avg.fill(float3(0.0f));
  dst_count.fill(0);
  for (const int i : edges.index_range()) {
    const int idx1 = edges[i][0];
    const int idx2 = edges[i][1];
    dst_avg[idx1] += src[idx2];
    dst_count[idx1]++;
    dst_avg[idx2] += src[idx1];
    dst_count[idx2]++;
  }
  threading::parallel_for(src.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      if (dst_count[i] > 0) {
        dst_avg[i] *= 1.0f / float(dst_count[i]);
      }
      else {
        dst_avg[i] = src[i];
      }
    }
  });
}

static void apply_blend(MutableSpan<float3> target,
                        const Span<float3> source,
                        const float fac,
                        const short flag,
                        const MDeformVert *dvert,
                        const int defgrp_index,
                        const bool invert_vgroup)
{
  threading::parallel_for(target.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      float f = fac;
      if (dvert) {
        const float w = vgroup_weight(dvert[i], defgrp_index, invert_vgroup);
        if (w <= 0.0f) {
          continue;
        }
        f *= w;
      }
      const float f_orig = 1.0f - f;
      if (flag & MOD_SMOOTH_X) {
        target[i][0] = f_orig * target[i][0] + f * source[i][0];
      }
      if (flag & MOD_SMOOTH_Y) {
        target[i][1] = f_orig * target[i][1] + f * source[i][1];
      }
      if (flag & MOD_SMOOTH_Z) {
        target[i][2] = f_orig * target[i][2] + f * source[i][2];
      }
    }
  });
}

static void hc_correction_pass(MutableSpan<float3> p,
                               const Span<float3> q,
                               const Span<float3> orig,
                               const Span<int2> edges,
                               const float alpha,
                               const float beta,
                               MutableSpan<float3> b,
                               MutableSpan<float3> b_avg,
                               MutableSpan<int> b_count)
{
  threading::parallel_for(p.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      b[i] = p[i] - (alpha * orig[i] + (1.0f - alpha) * q[i]);
    }
  });

  b_avg.fill(float3(0.0f));
  b_count.fill(0);
  for (const int e : edges.index_range()) {
    const int i1 = edges[e][0];
    const int i2 = edges[e][1];
    b_avg[i1] += b[i2];
    b_count[i1]++;
    b_avg[i2] += b[i1];
    b_count[i2]++;
  }

  const float ombeta = 1.0f - beta;
  threading::parallel_for(p.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      const float3 avg = b_count[i] > 0 ? b_avg[i] * (1.0f / float(b_count[i])) : float3(0.0f);
      p[i] -= beta * b[i] + ombeta * avg;
    }
  });
}

static void smoothModifier_do(SmoothModifierData *smd,
                              Object *ob,
                              Mesh *mesh,
                              MutableSpan<float3> vertexCos)
{
  if (mesh == nullptr) {
    return;
  }

  const int verts_num = vertexCos.size();
  Array<float3> accumulated_vecs(verts_num);
  Array<int> accumulated_vecs_count(verts_num);

  const bool invert_vgroup = (smd->flag & MOD_SMOOTH_INVERT_VGROUP) != 0;
  const Span<int2> edges = mesh->edges();

  const MDeformVert *dvert;
  int defgrp_index;
  MOD_get_vgroup(ob, mesh, smd->defgrp_name, &dvert, &defgrp_index);

  switch (smd->method) {
    case MOD_SMOOTH_METHOD_SIMPLE: {
      for (int j = 0; j < smd->repeat; j++) {
        laplacian_pass(edges, vertexCos, accumulated_vecs, accumulated_vecs_count);
        apply_blend(
            vertexCos, accumulated_vecs, smd->fac, smd->flag, dvert, defgrp_index, invert_vgroup);
      }
      break;
    }
    case MOD_SMOOTH_METHOD_TAUBIN: {
      for (int j = 0; j < smd->repeat; j++) {
        neighbor_avg_pass(edges, vertexCos, accumulated_vecs, accumulated_vecs_count);
        apply_blend(
            vertexCos, accumulated_vecs, smd->fac, smd->flag, dvert, defgrp_index, invert_vgroup);
        neighbor_avg_pass(edges, vertexCos, accumulated_vecs, accumulated_vecs_count);
        apply_blend(vertexCos,
                    accumulated_vecs,
                    smd->taubin_mu,
                    smd->flag,
                    dvert,
                    defgrp_index,
                    invert_vgroup);
      }
      break;
    }
    case MOD_SMOOTH_METHOD_HC: {
      Array<float3> hc_p(vertexCos.as_span());
      Array<float3> hc_b(verts_num);
      Array<float3> hc_b_avg(verts_num);
      Array<int> hc_b_count(verts_num);
      for (int j = 0; j < smd->repeat; j++) {
        neighbor_avg_pass(edges, hc_p, accumulated_vecs, accumulated_vecs_count);
        hc_correction_pass(accumulated_vecs,
                           hc_p,
                           vertexCos.as_span(),
                           edges,
                           smd->hc_alpha,
                           smd->hc_beta,
                           hc_b,
                           hc_b_avg,
                           hc_b_count);
        hc_p.as_mutable_span().copy_from(accumulated_vecs);
      }
      apply_blend(vertexCos, hc_p, smd->fac, smd->flag, dvert, defgrp_index, invert_vgroup);
      break;
    }
  }
}

static void deform_verts(ModifierData *md,
                         const ModifierEvalContext *ctx,
                         Mesh *mesh,
                         MutableSpan<float3> positions)
{
  SmoothModifierData *smd = reinterpret_cast<SmoothModifierData *>(md);
  smoothModifier_do(smd, ctx->object, mesh, positions);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  const ui::eUI_Item_Flag toggles_flag = ui::ITEM_R_TOGGLE | ui::ITEM_R_FORCE_BLANK_DECORATE;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);

  layout.prop(ptr, "method", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &row = layout.row(true, IFACE_("Axis"));
  row.prop(ptr, "use_x", toggles_flag, std::nullopt, ICON_NONE);
  row.prop(ptr, "use_y", toggles_flag, std::nullopt, ICON_NONE);
  row.prop(ptr, "use_z", toggles_flag, std::nullopt, ICON_NONE);

  ui::Layout &col = layout.column(false);
  col.prop(ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  const int method = RNA_enum_get(ptr, "method");
  if (method == MOD_SMOOTH_METHOD_TAUBIN) {
    col.prop(ptr, "taubin_mu", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (method == MOD_SMOOTH_METHOD_HC) {
    col.prop(ptr, "hc_alpha", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "hc_beta", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  col.prop(ptr, "iterations", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Smooth, panel_draw);
}

ModifierTypeInfo modifierType_Smooth = {
    /*idname*/ "Smooth",
    /*name*/ N_("Smooth"),
    /*struct_name*/ "SmoothModifierData",
    /*struct_size*/ sizeof(SmoothModifierData),
    /*srna*/ &RNA_SmoothModifier,
    /*type*/ ModifierTypeType::OnlyDeform,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_AcceptsCVs |
        eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_SMOOTH,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ deform_verts,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ nullptr,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
