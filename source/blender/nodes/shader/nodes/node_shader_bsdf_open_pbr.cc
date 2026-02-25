/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_bsdf_open_pbr_cc {

// TODO (OpenPBR): Add correct in and outputs

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Base Weight")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define SOCK_BASE_WEIGTH_ID 0
  b.add_input<decl::Color>("Base Color")
      .default_value({0.8f, 0.8f, 0.8f, 1.0f});
#define SOCK_BASE_COLOR_ID 1
  b.add_input<decl::Float>("Base Metalness")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define SOCK_BASE_METALNESS_ID 2
  b.add_input<decl::Float>("Diffuse Roughness")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define SOCK_DIFFUSE_ROUGHNESS_ID 3
  
  b.add_input<decl::Vector>("Geometry Normal").hide_value();
#define SOCK_NORMAL_ID 4
  // TODO (Sebastian): Understand the usage of the Weight input better
  b.add_input<decl::Float>("Weight").available(false);
  b.add_output<decl::Shader>("BSDF");
}

static int node_shader_gpu_bsdf_open_pbr(GPUMaterial *mat,
                                        bNode *node,
                                        bNodeExecData * /*execdata*/,
                                        GPUNodeStack *in,
                                        GPUNodeStack *out)
{
  /* Normals */
  if (!in[SOCK_NORMAL_ID].link) {
    GPU_link(mat, "world_normals_get", &in[SOCK_NORMAL_ID].link);
  }


  eGPUMaterialFlag flag = GPU_MATFLAG_DIFFUSE;
  /* TODO: setup flags based on the settings: see principled*/
  GPU_material_flag_set(mat, flag);

  return GPU_stack_link(mat, node, "node_bsdf_open_pbr", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  if (to_type_ != NodeItem::Type::BSDF) {
    return empty();
  }

  NodeItem base_weight = get_input_value("Base Weight", NodeItem::Type::Float);
  NodeItem base_color = get_input_value("Base Color", NodeItem::Type::Color3);
  NodeItem base_metalness = get_input_value("Base Metalness", NodeItem::Type::Float);
  NodeItem base_roughness = get_input_value("Diffuse Roughness", NodeItem::Type::Float);

  NodeItem geometry_normal = get_input_link("Geometry Normal", NodeItem::Type::Vector3);
  /* set up the open pbr MaterialX shader node: "open_pbr_surface"*/
  return create_node("oren_nayar_diffuse_bsdf",
                     NodeItem::Type::BSDF,
                     {{"base_color", base_color}, {"base_roughness", base_roughness}, {"geometry_normal", geometry_normal}});
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_bsdf_open_pbr_cc

/* node type definition */
void register_node_type_sh_bsdf_open_pbr()
{
  namespace file_ns = nodes::node_shader_bsdf_open_pbr_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBsdfOpenPBR", SH_NODE_BSDF_OPEN_PBR);
  ntype.ui_name = "Open PBR";
  ntype.ui_description = "Open PBR Ueber-Shader material model (based on rev. 1.1.1).";
  ntype.enum_name_legacy = "BSDF_OPEN_PBR";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.gather_link_search_ops = search_link_ops_for_shader_bsdf_node;
  ntype.add_ui_poll = object_shader_nodes_poll;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Middle);
  ntype.gpu_fn = file_ns::node_shader_gpu_bsdf_open_pbr;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
