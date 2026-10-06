/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BLI_hash_c.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BKE_context.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_light_accumulation_aov_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.add_input<decl::Color>("Internal_LightAccumulation"_ustr).available(is_gpu_internal);

  b.add_input<decl::Color>("Light"_ustr).default_value({0.0f, 0.0f, 0.0f, 1.0f}).hide_value();

  b.add_output<decl::Color>("Internal_Result"_ustr).available(is_gpu_internal);
}

static BIFIconID aov_icon(const ViewLayer *view_layer, PointerRNA *ptr)
{
  std::string aov_name = RNA_string_get(ptr, "aov_name");
  if (aov_name.empty()) {
    return ICON_RECORD_OFF;
  }

  const ViewLayerAOV *aov = static_cast<const ViewLayerAOV *>(
      BLI_findstring(&view_layer->aovs, aov_name.c_str(), offsetof(ViewLayerAOV, name)));

  return aov ? ICON_NODE_SOCKET_RGBA : ICON_RECORD_OFF;
}

static void node_shader_buts(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  if (scene && view_layer) {
    PointerRNA view_layer_rna_ptr = RNA_pointer_create_id_subdata(
        scene->id, RNA_ViewLayer, view_layer);
    // TODO: Filter value AOVs out.
    layout.prop_search(
        ptr, "aov_name", &view_layer_rna_ptr, "aovs", "", aov_icon(view_layer, ptr));
  }
  else {
    layout.prop(ptr, "aov_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
}

static void node_shader_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderLightAccumulationAOV *aov = MEM_new<NodeShaderLightAccumulationAOV>(
      "NodeShaderLightAccumulationAOV");
  node->storage = aov;
}

static int node_shader_gpu_light_accumulation_aov(GPUMaterial *mat,
                                                  bNode *node,
                                                  bNodeExecData * /*execdata*/,
                                                  GPUNodeStack *in,
                                                  GPUNodeStack *out)
{
  if (!in[0].link) {
    /* Error: not linked to a light accumulation zone. */
    BLI_assert_unreachable();
    return false;
  }

  GPU_material_flag_set(mat, GPU_MATFLAG_LIGHTING);
  return GPU_stack_link(mat, node, "node_light_accumulation_aov", in, out);
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
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts;
  ntype.initfunc = file_ns::node_shader_init;
  bke::node_type_storage(ntype,
                         "NodeShaderLightAccumulationAOV",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  ntype.gather_link_search_ops = search_link_ops_for_shader_material_lighting_node;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_accumulation_aov;
  // ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
