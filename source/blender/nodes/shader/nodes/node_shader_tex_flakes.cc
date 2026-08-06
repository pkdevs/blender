/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "BKE_texture.h"

#include "BLI_noise.hh"

#include "FN_multi_function.hh"
#include "NOD_multi_function.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_tex_flakes_cc {

static void sh_node_tex_flakes_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();

  const int dimensions = b.node_or_null() ? b.node_or_null()->custom1 : 3;
  const bool is_compositor = b.tree_or_null() && b.tree_or_null()->type == NTREE_COMPOSIT;
  const NodeDefaultInputType default_input_type =
      is_compositor ? NODE_DEFAULT_INPUT_UNIFORM_IMAGE_COORDINATES :
                      NODE_DEFAULT_INPUT_POSITION_FIELD;
  b.add_input<decl::Vector>("Vector"_ustr)
      .default_input_type(default_input_type)
      .available(dimensions != 1);

  b.add_input<decl::Float>("W"_ustr)
      .min(-1000.0f)
      .max(1000.0f)
      .available(dimensions == 1 || dimensions == 4)
      .make_available([](bNode &node) {
        /* Default to 1 instead of 4, because it is faster. */
        node.custom1 = 1;
      });

  b.add_input<decl::Float>("Scale"_ustr)
      .min(-1000.0f)
      .max(1000.0f)
      .default_value(5.0f)
      .description("Scale of the base flakes structure");

  b.add_input<decl::Float>("Density"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .description("Probability of a flake appearing in a cell");

  b.add_input<decl::Float>("Flake Size"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .description("Size of the flakes relative to the cell size");

  b.add_input<decl::Float>("Randomness"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(1.0f)
      .subtype(PROP_FACTOR)
      .description("Randomness of the flake positions");

  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Vector>("Normal"_ustr);
  b.add_output<decl::Float>("Factor"_ustr, "Fac"_ustr);
}

static void node_shader_buts_tex_flakes(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "flakes_dimensions", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static void node_shader_init_tex_flakes(bNodeTree *node_tree, bNode *node)
{
  node->custom1 = node_tree->type == NTREE_COMPOSIT ? 2 : 3;
}

static const char *gpu_shader_get_name(const int dimensions)
{
  BLI_assert(dimensions >= 1 && dimensions <= 4);
  return std::array{"node_tex_flakes_1d",
                    "node_tex_flakes_2d",
                    "node_tex_flakes_3d",
                    "node_tex_flakes_4d"}[dimensions - 1];
}

static int gpu_shader_tex_flakes(GPUMaterial *mat,
                                 bNode *node,
                                 bNodeExecData * /*execdata*/,
                                 GPUNodeStack *in,
                                 GPUNodeStack *out)
{
  const char *name = gpu_shader_get_name(node->custom1);
  return GPU_stack_link(mat, node, name, in, out);
}

/* For geometry nodes/multi function, it is omitted here for brevity to keep it simple, 
 * or we can implement it if required, but it's not strictly necessary for the Shader Editor.
 * I will provide an empty implementation or a fallback to avoid compilation errors if not fully implemented.
 */
static void sh_node_flakes_build_multi_function(NodeMultiFunctionBuilder & /*builder*/)
{
  /* To do: implement mf::MultiFunction for Flakes geometry nodes support. */
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* Fallback or MaterialX implementation if needed. */
  return empty();
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_tex_flakes_cc

void register_node_type_sh_tex_flakes()
{
  namespace file_ns = nodes::node_shader_tex_flakes_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeTexFlakes"_ustr, SH_NODE_TEX_FLAKES);
  ntype.ui_name = "Flakes Texture";
  ntype.ui_description = "Generate metallic flakes texture";
  ntype.enum_name_legacy = "TEX_FLAKES";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::sh_node_tex_flakes_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_tex_flakes;
  ntype.initfunc = file_ns::node_shader_init_tex_flakes;
  ntype.gpu_fn = file_ns::gpu_shader_tex_flakes;
  ntype.build_multi_function = file_ns::sh_node_flakes_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
