/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_bsdf_diffraction_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.add_input<decl::Color>("Color"_ustr).default_value({0.9f, 0.9f, 0.9f, 1.0f});
  b.add_input<decl::Float>("Roughness"_ustr)
      .default_value(0.05f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Angular spread of each diffraction order, like Glossy roughness");
  b.add_input<decl::Float>("Period"_ustr)
      .default_value(1.6f)
      .min(0.05f)
      .max(50.0f)
      .subtype(PROP_DISTANCE)
      .description("Grating period (groove spacing), in micrometers. A CD track pitch is 1.6");
  b.add_input<decl::Float>("Groove Depth"_ustr)
      .default_value(110.0f)
      .min(0.0f)
      .max(2000.0f)
      .description("Groove depth, in nanometers. Sets how energy splits between orders");
  b.add_input<decl::Float>("Duty Cycle"_ustr)
      .default_value(0.45f)
      .min(0.01f)
      .max(0.99f)
      .subtype(PROP_FACTOR)
      .description("Pit-to-period width fraction. Only used by the Rectangular profile");
  b.add_input<decl::Float>("Rotation"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Rotates the groove direction, with 1.0 going full circle");
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
  b.add_input<decl::Vector>("Tangent"_ustr).hide_value();
  b.add_input<decl::Float>("Weight"_ustr).available(is_gpu_internal);
  b.add_output<decl::Shader>("BSDF"_ustr);
}

static void node_shader_buts_diffraction(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "profile", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static void node_shader_init_diffraction(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_DIFFRACTION_RECTANGULAR;
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* No equivalent BSDF exists in the MaterialX specification for a diffraction
   * grating, so this closure does not participate in MaterialX export. */
  return empty();
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_bsdf_diffraction_cc

/* node type definition */
void register_node_type_sh_bsdf_diffraction()
{
  namespace file_ns = nodes::node_shader_bsdf_diffraction_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBsdfDiffraction"_ustr, SH_NODE_BSDF_DIFFRACTION);
  ntype.ui_name = "Diffraction BSDF";
  ntype.ui_description =
      "Reflection off a periodic grooved surface (a CD/DVD track or a holographic foil), "
      "splitting light into its diffraction orders";
  ntype.enum_name_legacy = "BSDF_DIFFRACTION";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.gather_link_search_ops = search_link_ops_for_shader_bsdf_node;
  ntype.add_ui_poll = object_shader_nodes_poll;
  ntype.draw_buttons = file_ns::node_shader_buts_diffraction;
  ntype.default_width = bke::NodeWidth::_160;
  ntype.initfunc = file_ns::node_shader_init_diffraction;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
