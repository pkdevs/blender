/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_light_accumulation_aov_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.add_input<decl::Int>("LightIndex"_ustr).available(is_gpu_internal);

  b.add_input<decl::Color>("Light"_ustr).default_value({0.0f, 0.0f, 0.0f, 1.0f}).hide_value();

  b.add_output<decl::Shader>("Shader"_ustr).available(is_gpu_internal);
}

static int node_shader_gpu_light_accumulation_aov(GPUMaterial *mat,
                                                  bNode *node,
                                                  bNodeExecData * /*execdata*/,
                                                  GPUNodeStack *in,
                                                  GPUNodeStack *out)
{
  if (!in[0].link) {
    /* Error: not linked to a light accumulation node */
    return false;
  }

  GPU_material_flag_set(mat, GPU_MATFLAG_LIGHTING);

  return GPU_stack_link(
      mat, node, "node_light_accumulation_aov", in, out, GPU_kernel_globals(), GPU_shading_data());
}

}  // namespace nodes::node_shader_light_accumulation_aov_cc

/* node type definition */
void register_node_type_sh_light_accumulation_aov()
{
  namespace file_ns = nodes::node_shader_light_accumulation_aov_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLightAccumulationAOV"_ustr, SH_NODE_LIGHT_ACCUMULATION_AOV);
  ntype.ui_name = "Light Accumulation AOV";
  ntype.ui_description =
      "Adds the result of all the evaluated lights and stores the result into an AOV.\n";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  // TODO: Should filter the Shader Output
  ntype.gather_link_search_ops = search_link_ops_for_shader_material_lighting_node;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_accumulation_aov;
  // ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
