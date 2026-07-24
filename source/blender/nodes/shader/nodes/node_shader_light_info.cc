/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_light_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.add_input<decl::Int>("LightIndex"_ustr).available(is_gpu_internal);

  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Power"_ustr);
  b.add_output<decl::Vector>("Position"_ustr);
  b.add_output<decl::Vector>("Direction"_ustr);
  b.add_output<decl::Float>("Distance"_ustr);
  b.add_output<decl::Float>("Mask"_ustr);
}

static int node_shader_gpu_light_info(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  // TODO: Use a different flag.
  GPU_material_flag_set(mat, GPU_MATFLAG_SHADER_TO_RGBA);

  return GPU_stack_link(mat, node, "node_light_info", in, out);
}

}  // namespace nodes::node_shader_light_info_cc

/* node type definition */
void register_node_type_sh_light_info()
{
  namespace file_ns = nodes::node_shader_light_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLightInfo"_ustr, SH_NODE_LIGHT_INFO);
  ntype.ui_name = "Light Info";
  ntype.ui_description = "Light Info";
  ntype.enum_name_legacy = "LIGHT INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_info;
  // ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
