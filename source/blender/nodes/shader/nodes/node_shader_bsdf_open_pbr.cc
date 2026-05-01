/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_base.h"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "node_shader_util.hh"

#include "BKE_node_runtime.hh"

namespace blender {

namespace nodes::node_shader_bsdf_open_pbr_cc {

// TODO (OpenPBR): Add correct in and outputs

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.use_custom_socket_order();

  b.add_output<decl::Shader>("BSDF"_ustr);

  // TODO (Sebastian): Understand the usage of the Weight input better
  b.add_input<decl::Float>("Weight"_ustr).available(is_gpu_internal);
#define OPENPBR_SOCK_WEIGHT_ID 0
  /********************************************************************
   * Base Component
   * *****************************************************************/
  PanelDeclarationBuilder &base = b.add_panel("Base"_ustr).default_closed(false);
  base.add_input<decl::Float>("Base Weight"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_BASE_WEIGHT_ID 1
  base.add_input<decl::Color>("Base Color"_ustr).default_value({0.8f, 0.8f, 0.8f, 1.0f});
#define OPENPBR_SOCK_BASE_COLOR_ID 2
  base.add_input<decl::Float>("Base Metalness"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_BASE_METALNESS_ID 3
  base.add_input<decl::Float>("Base Diffuse Roughness"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_DIFFUSE_ROUGHNESS_ID 4
  /********************************************************************
   * Specular Component
   * *****************************************************************/
  PanelDeclarationBuilder &specular = b.add_panel("Specular"_ustr).default_closed(false);
  specular.add_input<decl::Float>("Specular Weight"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_SPECULAR_WEIGHT_ID 5
  specular.add_input<decl::Color>("Specular Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
#define OPENPBR_SOCK_SPECULAR_COLOR_ID 6
  specular.add_input<decl::Float>("Specular Roughness"_ustr)
      .default_value(0.3f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_SPECULAR_ROUGHNESS_ID 7
  specular.add_input<decl::Float>("Specular Roughness Anisotropy"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_SPECULAR_ROUGHNESS_ANISOTROPY_ID 8
  specular.add_input<decl::Float>("Specular IOR"_ustr)
      .default_value(1.5f)
      .min(0.0f)
      .max(3.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_SPECULAR_IOR_ID 9
  /********************************************************************
   * Transmission Component
   * *****************************************************************/
  PanelDeclarationBuilder &transmission = b.add_panel("Transmission"_ustr).default_closed(false);
  transmission.add_input<decl::Float>("Transmission Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_TRANSMISSION_WEIGHT_ID 10
  transmission.add_input<decl::Color>("Transmission Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f});
#define OPENPBR_SOCK_TRANSMISSION_COLOR_ID 11
  transmission.add_input<decl::Float>("Transmission Depth"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_TRANSMISSION_DEPTH_ID 12
  transmission.add_input<decl::Color>("Transmission Scatter"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f});
#define OPENPBR_SOCK_TRANSMISSION_SCATTER_ID 13
  transmission.add_input<decl::Float>("Transmission Scatter Anisotropy"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_TRANSMISSION_SCATTER_ANISOTROPY_ID 14
  transmission.add_input<decl::Float>("Transmission Dispersion Scale"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_TRANSMISSION_DISPERSION_SCALE_ID 15
  transmission.add_input<decl::Float>("Transmission Dispersion Abbe Number"_ustr)
      .default_value(20.0f)
      .min(0.0f)
      .max(91.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_TRANSMISSION_DISPERSION_ABBE_NUMBER_ID 16
  /********************************************************************
   * Subsurface Component
   * *****************************************************************/
  PanelDeclarationBuilder &subsurface = b.add_panel("Subsurface"_ustr).default_closed(false);
  subsurface.add_input<decl::Float>("Subsurface Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_SUBSURFACE_WEIGHT_ID 17
  subsurface.add_input<decl::Color>("Subsurface Color"_ustr)
      .default_value({0.8f, 0.8f, 0.8f, 1.0f});
#define OPENPBR_SOCK_SUBSURFACE_COLOR_ID 18
  subsurface.add_input<decl::Float>("Subsurface Radius"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_SUBSURFACE_RADIUS_ID 19
  subsurface.add_input<decl::Color>("Subsurface Radius Scale"_ustr)
      .default_value({1.0f, 0.5f, 0.25f, 1.0f});
#define OPENPBR_SOCK_SUBSURFACE_RADIUS_SCALE_ID 20
  subsurface.add_input<decl::Float>("Subsurface Scatter Anisotropy"_ustr)
      .default_value(0.0f)
      .min(-1.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_SUBSURFACE_SCATTER_ANISOTROPY_ID 21
  /********************************************************************
   * Coat Component
   * *****************************************************************/
  PanelDeclarationBuilder &coat = b.add_panel("Coat"_ustr).default_closed(false);
  coat.add_input<decl::Float>("Coat Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_COAT_WEIGHT_ID 22
  coat.add_input<decl::Color>("Coat Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
#define OPENPBR_SOCK_COAT_COLOR_ID 23
  coat.add_input<decl::Float>("Coat Roughness"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_COAT_ROUGHNESS_ID 24
  coat.add_input<decl::Float>("Coat Roughness Anisotropy"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_COAT_ROUGHNESS_ANISOTROPY_ID 25
  coat.add_input<decl::Float>("Coat IOR"_ustr)
      .default_value(1.6f)
      .min(0.0f)
      .max(3.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_COAT_IOR_ID 26
  coat.add_input<decl::Float>("Coat Darkening"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_COAT_DARKENING_ID 27
  /********************************************************************
   * Fuzz Component
   * *****************************************************************/
  PanelDeclarationBuilder &fuzz = b.add_panel("Fuzz"_ustr).default_closed(false);
  fuzz.add_input<decl::Float>("Fuzz Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_FUZZ_WEIGHT_ID 28
  fuzz.add_input<decl::Color>("Fuzz Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
#define OPENPBR_SOCK_FUZZ_COLOR_ID 29
  fuzz.add_input<decl::Float>("Fuzz Roughness"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_FUZZ_ROUGHNESS_ID 30
  /********************************************************************
   * Emission Component
   * *****************************************************************/
  PanelDeclarationBuilder &emission = b.add_panel("Emission"_ustr).default_closed(false);
  emission.add_input<decl::Float>("Emission Luminance"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1000.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_EMISSION_LUMINANCE_ID 31
  emission.add_input<decl::Color>("Emission Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
#define OPENPBR_SOCK_EMISSION_COLOR_ID 32
  /********************************************************************
   * Thin-film Component
   * *****************************************************************/
  PanelDeclarationBuilder &thinfilm = b.add_panel("Thin Film"_ustr).default_closed(false);
  thinfilm.add_input<decl::Float>("Thin Film Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_THIN_FILM_WEIGHT_ID 33
  thinfilm.add_input<decl::Float>("Thin Film Thickness"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(100000.0f)
      .description("Thickness of the film in micrometers");
  /* TODO(weizhen): OpenPBR unit for thin film is micrometer, but our default is nanometer. Need to
   * address this discrepancy. */
#define OPENPBR_SOCK_THIN_FILM_THICKNESS_ID 34
  thinfilm.add_input<decl::Float>("Thin Film IOR"_ustr)
      .default_value(1.4f)
      .min(0.0f)
      .max(3.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_THIN_FILM_IOR_ID 35
  /********************************************************************
   * Geometry Component
   * *****************************************************************/
  PanelDeclarationBuilder &geometry = b.add_panel("Geometry"_ustr).default_closed(false);
  geometry.add_input<decl::Float>("Geometry Opacity"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
#define OPENPBR_SOCK_GEOMETRY_OPACITY_ID 36
  geometry.add_input<decl::Bool>("Geometry Thin Walled"_ustr).default_value(false);
#define OPENPBR_SOCK_GEOMETRY_THIN_WALLED_ID 37
  geometry.add_input<decl::Vector>("Geometry Normal"_ustr).hide_value();
#define OPENPBR_SOCK_GEOMETRY_NORMAL_ID 38
  geometry.add_input<decl::Vector>("Geometry Tangent"_ustr).hide_value();
#define OPENPBR_SOCK_GEOMETRY_TANGENT_ID 39
  geometry.add_input<decl::Vector>("Geometry Coat Normal"_ustr).hide_value();
#define OPENPBR_SOCK_GEOMETRY_COAT_NORMAL_ID 40
  geometry.add_input<decl::Vector>("Geometry Coat Tangent"_ustr).hide_value();
#define OPENPBR_SOCK_GEOMETRY_COAT_TANGENT_ID 41
}

static int node_shader_gpu_bsdf_open_pbr(GPUMaterial *mat,
                                         bNode *node,
                                         bNodeExecData * /*execdata*/,
                                         GPUNodeStack *in,
                                         GPUNodeStack *out)
{
  /* Normals */
  if (!in[OPENPBR_SOCK_GEOMETRY_NORMAL_ID].link) {
    GPU_link(mat, "world_normals_get", &in[OPENPBR_SOCK_GEOMETRY_NORMAL_ID].link);
  }

  eGPUMaterialFlag flag = GPU_MATFLAG_GLOSSY;
  flag |= GPU_MATFLAG_DIFFUSE;
  /* TODO: setup flags based on the settings: see principled*/
  GPU_material_flag_set(mat, flag);
  // Needed to be set explicitly when glossy reflection can have a color
  GPU_material_flag_set(mat, GPU_MATFLAG_REFLECTION_MAYBE_COLORED);
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
  NodeItem base_roughness = get_input_value("Base Diffuse Roughness", NodeItem::Type::Float);

  NodeItem geometry_normal = get_input_link("Geometry Normal", NodeItem::Type::Vector3);
  /* set up the open pbr MaterialX shader node: "open_pbr_surface"*/
  return create_node("oren_nayar_diffuse_bsdf",
                     NodeItem::Type::BSDF,
                     {{"base_color", base_color},
                      {"base_roughness", base_roughness},
                      {"geometry_normal", geometry_normal}});
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_bsdf_open_pbr_cc

/* node type definition */
void register_node_type_sh_bsdf_open_pbr()
{
  namespace file_ns = nodes::node_shader_bsdf_open_pbr_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBsdfOpenPBR"_ustr, SH_NODE_BSDF_OPEN_PBR);
  ntype.ui_name = "OpenPBR";
  ntype.ui_description = "OpenPBR Ueber-Shader material model (based on rev. 1.1.1).";
  ntype.enum_name_legacy = "BSDF_OPEN_PBR";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.gather_link_search_ops = search_link_ops_for_shader_bsdf_node;
  ntype.add_ui_poll = object_shader_nodes_poll;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  ntype.gpu_fn = file_ns::node_shader_gpu_bsdf_open_pbr;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
