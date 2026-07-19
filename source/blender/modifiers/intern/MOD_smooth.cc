/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_deform.hh"
#include "BKE_mesh_mapping.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "RNA_types.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

#include "eigen_capi.h"

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

  if (flag == 0) {
    return true;
  }
  if (smd->fac == 0.0f && (smd->method != MOD_SMOOTH_METHOD_TAUBIN || smd->taubin_mu == 0.0f)) {
    return true;
  }

  return false;
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  SmoothModifierData *smd = reinterpret_cast<SmoothModifierData *>(md);

  if (smd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static float vgroup_weight(const MDeformVert &dv, const int defgrp_index, const bool invert)
{
  const float w = BKE_defvert_find_weight(&dv, defgrp_index);
  return invert ? 1.0f - w : w;
}

template<typename Fn>
static void pin_edge_endpoints(const Span<int2> edges, MutableSpan<bool> is_pinned, const Fn &edge_selected)
{
  for (const int e : edges.index_range()) {
    if (edge_selected(e)) {
      is_pinned[edges[e][0]] = true;
      is_pinned[edges[e][1]] = true;
    }
  }
}

static Array<bool> compute_pinned_vertex_mask(const Mesh &mesh, const short pin_flags)
{
  const Span<int2> edges = mesh.edges();
  Array<bool> is_pinned(mesh.verts_num, false);

  if (pin_flags & MOD_SMOOTH_PIN_BOUNDARY) {
    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corner_edges = mesh.corner_edges();

    Array<int> edge_face_count(edges.size(), 0);
    for (const int f : faces.index_range()) {
      for (const int edge : corner_edges.slice(faces[f])) {
        edge_face_count[edge]++;
      }
    }
    pin_edge_endpoints(edges, is_pinned, [&](const int e) { return edge_face_count[e] == 1; });
  }

  const bke::AttributeAccessor attributes = mesh.attributes();

  if (pin_flags & MOD_SMOOTH_PIN_SEAM) {
    if (const VArray<bool> seams = *attributes.lookup<bool>("uv_seam", bke::AttrDomain::Edge)) {
      const VArraySpan<bool> seam_span(seams);
      pin_edge_endpoints(edges, is_pinned, [&](const int e) { return seam_span[e]; });
    }
  }

  if (pin_flags & MOD_SMOOTH_PIN_SHARP) {
    if (const VArray<bool> sharp = *attributes.lookup<bool>("sharp_edge", bke::AttrDomain::Edge)) {
      const VArraySpan<bool> sharp_span(sharp);
      pin_edge_endpoints(edges, is_pinned, [&](const int e) { return sharp_span[e]; });
    }
  }

  return is_pinned;
}

static Array<float> compute_edge_cotangent_weights(const Mesh &mesh, const Span<float3> positions)
{
  const Span<int2> edges = mesh.edges();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();

  Array<int> e2c_offsets, e2c_indices;
  const GroupedSpan<int> edge_to_corner_map = bke::mesh::build_edge_to_corner_map(
      corner_edges, edges.size(), e2c_offsets, e2c_indices);
  const Array<int> corner_to_face_map = bke::mesh::build_corner_to_face_map(faces);

  Array<float> weights(edges.size(), 0.0f);

  threading::parallel_for(edges.index_range(), 2048, [&](const IndexRange range) {
    for (const int e : range) {
      float w = 0.0f;
      for (const int corner_curr : edge_to_corner_map[e]) {
        const IndexRange face = faces[corner_to_face_map[corner_curr]];
        const int n = face.size();
        if (n < 3) {
          continue;
        }
        const int k = corner_curr - int(face.start());
        const int corner_next = int(face[(k + 1) % n]);
        const int corner_prev = int(face[(k + n - 1) % n]);
        const float3 &p_prev = positions[corner_verts[corner_prev]];
        const float3 &p_curr = positions[corner_verts[corner_curr]];
        const float3 &p_next = positions[corner_verts[corner_next]];
        const float cot = cotangent_tri_weight_v3(p_prev, p_curr, p_next) * 0.5f;
        if (cot <= 0.0f) {
          continue;
        }
        w += cot;
      }
      weights[e] = w;
    }
  });

  return weights;
}

static void gather_avg_pass(const Span<int2> edges,
                            const GroupedSpan<int> vert_to_edge_map,
                            const Span<float3> src,
                            const Span<float> edge_weights,
                            const Span<bool> pinned,
                            const bool use_midpoint,
                            MutableSpan<float3> dst_avg)
{
  const bool weighted = !edge_weights.is_empty();
  const bool has_pin = !pinned.is_empty();
  threading::parallel_for(src.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      if (has_pin && pinned[i]) {
        dst_avg[i] = src[i];
        continue;
      }
      const Span<int> incident = vert_to_edge_map[i];
      float3 sum(0.0f);
      float wsum = 0.0f;
      for (const int e : incident) {
        const int2 edge = edges[e];
        const int other = (edge[0] == i) ? edge[1] : edge[0];
        const float w = weighted ? edge_weights[e] : 1.0f;
        if (w == 0.0f) {
          continue;
        }
        const float3 contribution = use_midpoint ? (src[i] + src[other]) * 0.5f : src[other];
        sum += w * contribution;
        wsum += w;
      }
      dst_avg[i] = (wsum > 0.0f) ? sum * (1.0f / wsum) : src[i];
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
                               const GroupedSpan<int> vert_to_edge_map,
                               const Span<float> edge_weights,
                               const Span<bool> pinned,
                               const float alpha,
                               const float beta,
                               MutableSpan<float3> b,
                               MutableSpan<float3> b_avg)
{
  threading::parallel_for(p.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      b[i] = p[i] - (alpha * orig[i] + (1.0f - alpha) * q[i]);
    }
  });

  gather_avg_pass(edges, vert_to_edge_map, b, edge_weights, pinned, false, b_avg);

  const float ombeta = 1.0f - beta;
  threading::parallel_for(p.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      p[i] -= beta * b[i] + ombeta * b_avg[i];
    }
  });
}

static bool frequency_filter(const Span<int2> edges,
                             const GroupedSpan<int> vert_to_edge_map,
                             const Span<float> edge_weights,
                             const Span<bool> pinned,
                             const float cutoff,
                             const int iterations,
                             MutableSpan<float3> positions)
{
  const int verts_num = positions.size();
  if (verts_num == 0 || iterations == 0) {
    return true;
  }

  LinearSolver *solver = EIG_linear_least_squares_solver_new(2 * verts_num, verts_num, 3);
  const bool has_pin = !pinned.is_empty();
  for (const int i : positions.index_range()) {
    EIG_linear_solver_variable_set(solver, 0, i, positions[i].x);
    EIG_linear_solver_variable_set(solver, 1, i, positions[i].y);
    EIG_linear_solver_variable_set(solver, 2, i, positions[i].z);
    if (has_pin && pinned[i]) {
      EIG_linear_solver_variable_lock(solver, i);
    }
  }

  const bool weighted = !edge_weights.is_empty();
  const float laplacian_scale = 1.0f / ((cutoff > 1e-6f) ? cutoff : 1e-6f);
  for (const int i : positions.index_range()) {
    EIG_linear_solver_matrix_add(solver, i, i, 1.0);

    const Span<int> incident = vert_to_edge_map[i];
    float weight_sum = 0.0f;
    for (const int e : incident) {
      weight_sum += weighted ? edge_weights[e] : 1.0f;
    }
    if (weight_sum <= 0.0f) {
      continue;
    }

    const int row = verts_num + i;
    EIG_linear_solver_matrix_add(solver, row, i, laplacian_scale);
    for (const int e : incident) {
      const int2 edge = edges[e];
      const int other = (edge[0] == i) ? edge[1] : edge[0];
      const float weight = weighted ? edge_weights[e] : 1.0f;
      EIG_linear_solver_matrix_add(
          solver, row, other, -laplacian_scale * weight / weight_sum);
    }
  }

  bool success = true;
  for (int iteration = 0; iteration < iterations; iteration++) {
    for (const int i : positions.index_range()) {
      EIG_linear_solver_right_hand_side_add(solver, 0, i, positions[i].x);
      EIG_linear_solver_right_hand_side_add(solver, 1, i, positions[i].y);
      EIG_linear_solver_right_hand_side_add(solver, 2, i, positions[i].z);
    }
    if (!EIG_linear_solver_solve(solver)) {
      success = false;
      break;
    }
    for (const int i : positions.index_range()) {
      positions[i].x = EIG_linear_solver_variable_get(solver, 0, i);
      positions[i].y = EIG_linear_solver_variable_get(solver, 1, i);
      positions[i].z = EIG_linear_solver_variable_get(solver, 2, i);
    }
  }

  EIG_linear_solver_delete(solver);
  return success;
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

  const bool invert_vgroup = (smd->flag & MOD_SMOOTH_INVERT_VGROUP) != 0;
  const Span<int2> edges = mesh->edges();

  Array<int> v2e_offsets, v2e_indices;
  const GroupedSpan<int> vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
      edges, verts_num, v2e_offsets, v2e_indices);

  const bool use_cotan = (smd->flag & MOD_SMOOTH_USE_COTAN) &&
                         smd->method != MOD_SMOOTH_METHOD_SIMPLE;
  Array<float> edge_weights;
  if (use_cotan) {
    edge_weights = compute_edge_cotangent_weights(*mesh, vertexCos.as_span());
  }
  const Span<float> weights_span = use_cotan ? edge_weights.as_span() : Span<float>{};

  const short pin_flags = smd->flag &
                          (MOD_SMOOTH_PIN_BOUNDARY | MOD_SMOOTH_PIN_SEAM | MOD_SMOOTH_PIN_SHARP);
  Array<bool> pinned_mask;
  if (pin_flags) {
    pinned_mask = compute_pinned_vertex_mask(*mesh, pin_flags);
  }
  const Span<bool> pinned_span = pinned_mask.is_empty() ? Span<bool>{} : pinned_mask.as_span();

  const MDeformVert *dvert;
  int defgrp_index;
  MOD_get_vgroup(ob, mesh, smd->defgrp_name, &dvert, &defgrp_index);

  switch (smd->method) {
    case MOD_SMOOTH_METHOD_SIMPLE: {
      for (int j = 0; j < smd->repeat; j++) {
        gather_avg_pass(
            edges, vert_to_edge_map, vertexCos, {}, pinned_span, true, accumulated_vecs);
        apply_blend(
            vertexCos, accumulated_vecs, smd->fac, smd->flag, dvert, defgrp_index, invert_vgroup);
      }
      break;
    }
    case MOD_SMOOTH_METHOD_TAUBIN: {
      for (int j = 0; j < smd->repeat; j++) {
        gather_avg_pass(edges,
                        vert_to_edge_map,
                        vertexCos,
                        weights_span,
                        pinned_span,
                        false,
                        accumulated_vecs);
        apply_blend(
            vertexCos, accumulated_vecs, smd->fac, smd->flag, dvert, defgrp_index, invert_vgroup);
        gather_avg_pass(edges,
                        vert_to_edge_map,
                        vertexCos,
                        weights_span,
                        pinned_span,
                        false,
                        accumulated_vecs);
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
      for (int j = 0; j < smd->repeat; j++) {
        gather_avg_pass(
            edges, vert_to_edge_map, hc_p, weights_span, pinned_span, false, accumulated_vecs);
        hc_correction_pass(accumulated_vecs,
                           hc_p,
                           vertexCos.as_span(),
                           edges,
                           vert_to_edge_map,
                           weights_span,
                           pinned_span,
                           smd->hc_alpha,
                           smd->hc_beta,
                           hc_b,
                           hc_b_avg);
        hc_p.as_mutable_span().copy_from(accumulated_vecs);
      }
      apply_blend(vertexCos, hc_p, smd->fac, smd->flag, dvert, defgrp_index, invert_vgroup);
      break;
    }
    case MOD_SMOOTH_METHOD_FREQUENCY: {
      Array<float3> filtered_positions(vertexCos.as_span());
      if (frequency_filter(edges,
                           vert_to_edge_map,
                           weights_span,
                           pinned_span,
                           smd->frequency_cutoff,
                           smd->repeat,
                           filtered_positions))
      {
        apply_blend(vertexCos,
                    filtered_positions,
                    smd->fac,
                    smd->flag,
                    dvert,
                    defgrp_index,
                    invert_vgroup);
      }
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
  else if (method == MOD_SMOOTH_METHOD_FREQUENCY) {
    col.prop(ptr, "frequency_cutoff", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (ELEM(method,
           MOD_SMOOTH_METHOD_TAUBIN,
           MOD_SMOOTH_METHOD_HC,
           MOD_SMOOTH_METHOD_FREQUENCY))
  {
    col.prop(ptr, "use_cotangent_weights", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  col.prop(ptr, "iterations", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &pin_col = layout.column(true, IFACE_("Pin"));
  pin_col.prop(ptr, "use_pin_boundary", UI_ITEM_NONE, IFACE_("Boundaries"), ICON_NONE);
  pin_col.prop(ptr, "use_pin_seam", UI_ITEM_NONE, IFACE_("Seams"), ICON_NONE);
  pin_col.prop(ptr, "use_pin_sharp", UI_ITEM_NONE, IFACE_("Sharp"), ICON_NONE);

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
