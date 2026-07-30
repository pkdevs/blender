/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_light_accumulation_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.add_input<decl::Color>("Diffuse Light"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value();
  b.add_input<decl::Color>("Diffuse Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value();
  b.add_input<decl::Color>("Glossy Light"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value();
  b.add_input<decl::Color>("Glossy Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value();
  b.add_input<decl::Color>("Transmission Light"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value();
  b.add_input<decl::Color>("Transmission Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value();
  b.add_input<decl::Float>("Weight"_ustr).available(is_gpu_internal);
  b.add_output<decl::Shader>("Shader"_ustr);
}

static int node_shader_gpu_light_accumulation(GPUMaterial *mat,
                                              bNode *node,
                                              bNodeExecData * /*execdata*/,
                                              GPUNodeStack *in,
                                              GPUNodeStack *out)
{
  // TODO: Use a different flag.
  GPU_material_flag_set(mat, GPU_MATFLAG_SHADER_TO_RGBA);

  return GPU_stack_link(mat, node, "node_light_accumulation", in, out);
}

}  // namespace nodes::node_shader_light_accumulation_cc

/* node type definition */
void register_node_type_sh_light_accumulation()
{
  namespace file_ns = nodes::node_shader_light_accumulation_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLightAccumulation"_ustr, SH_NODE_LIGHT_ACCUMULATION);
  ntype.ui_name = "Light Accumulation";
  ntype.ui_description = "Light Accumulation";
  ntype.enum_name_legacy = "LIGHT ACCUMULATION";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_accumulation;
  // ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
