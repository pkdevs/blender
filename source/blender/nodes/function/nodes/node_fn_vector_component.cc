/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GPU_material.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_vector_component_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).optional_label();
  b.add_input<decl::Int>("Index"_ustr).min(0).max(2);
  b.add_output<decl::Float>("Value"_ustr);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI2_SO<float3, int, float>("vector_component",
                                                         [](const float3 &vec, const int index) {
                                                           if (index >= 0 && index <= 2) {
                                                             return vec[index];
                                                           }
                                                           return 0.0f;
                                                         });
  builder.set_matching_fn(&fn);
}

static int node_gpu_material(GPUMaterial *mat,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack *in,
                             GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_vector_component", in, out);
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_cmp_node_type_base(&ntype, "FunctionNodeVectorComponent"_ustr);
  ntype.ui_name = "Vector Component";
  ntype.ui_description = "Get the X, Y or Z component of a vector by index";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.gpu_fn = node_gpu_material;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_vector_component_cc
