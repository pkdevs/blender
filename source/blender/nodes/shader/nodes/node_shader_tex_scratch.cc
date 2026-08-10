/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "BKE_texture.h"

#include "BLI_noise.hh"

#include "NOD_multi_function.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_tex_scratch_cc {

NODE_STORAGE_FUNCS(NodeTexScratch)

static void sh_node_tex_scratch_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();

  const bool is_compositor = b.tree_or_null() && b.tree_or_null()->type == NTREE_COMPOSIT;
  const NodeDefaultInputType default_input_type =
      is_compositor ? NODE_DEFAULT_INPUT_UNIFORM_IMAGE_COORDINATES :
                      NODE_DEFAULT_INPUT_POSITION_FIELD;

  b.add_input<decl::Vector>("Vector"_ustr).default_input_type(default_input_type);

  b.add_input<decl::Float>("Scale"_ustr)
      .min(-1000.0f)
      .max(1000.0f)
      .default_value(5.0f)
      .description("Overall scale of the scratch texture");
  b.add_input<decl::Float>("Density"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .description("Amount of visible scratches");
  b.add_input<decl::Float>("Scratch Scale"_ustr)
      .min(0.0f)
      .max(100.0f)
      .default_value(5.0f)
      .description("Scale of the scratch pattern");
  b.add_input<decl::Float>("Roughness"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .description("Roughness of the scratch contour");
  b.add_input<decl::Float>("Randomness"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(1.0f)
      .subtype(PROP_FACTOR)
      .description("Randomness of the scratch distribution");
  b.add_input<decl::Float>("Distortion"_ustr)
      .min(-1000.0f)
      .max(1000.0f)
      .default_value(0.0f)
      .description("Amount of distortion applied to the scratch pattern");

  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
  b.add_output<decl::Float>("Fac"_ustr, "Fac"_ustr).no_muted_links();
}

static void node_shader_buts_tex_scratch(ui::Layout & /*layout*/, bContext * /*C*/, PointerRNA * /*ptr*/)
{
}

static void node_shader_init_tex_scratch(bNodeTree * /*ntree*/, bNode *node)
{
  NodeTexScratch *tex = MEM_new<NodeTexScratch>(__func__);
  BKE_texture_mapping_default(&tex->base.tex_mapping, TEXMAP_TYPE_POINT);
  BKE_texture_colormapping_default(&tex->base.color_mapping);
  node->storage = tex;
}

class ScratchFunction : public mf::MultiFunction {
 public:
  ScratchFunction()
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"ScratchFunction", signature};
      builder.single_input<float3>("Vector");
      builder.single_input<float>("Scale");
      builder.single_input<float>("Density");
      builder.single_input<float>("Scratch Scale");
      builder.single_input<float>("Roughness");
      builder.single_input<float>("Randomness");
      builder.single_input<float>("Distortion");
      builder.single_output<ColorGeometry4f>("Color");
      builder.single_output<float>("Fac", mf::ParamFlag::SupportsUnusedOutput);
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float3> &vector = params.readonly_single_input<float3>(0, "Vector");
    const VArray<float> &scale = params.readonly_single_input<float>(1, "Scale");
    const VArray<float> &density = params.readonly_single_input<float>(2, "Density");
    const VArray<float> &scratch_scale = params.readonly_single_input<float>(3, "Scratch Scale");
    const VArray<float> &roughness = params.readonly_single_input<float>(4, "Roughness");
    const VArray<float> &randomness = params.readonly_single_input<float>(5, "Randomness");
    const VArray<float> &distortion = params.readonly_single_input<float>(6, "Distortion");

    MutableSpan<ColorGeometry4f> r_color = params.uninitialized_single_output<ColorGeometry4f>(7,
                                                                                             "Color");
    MutableSpan<float> r_fac = params.uninitialized_single_output_if_required<float>(8, "Fac");

    const bool compute_factor = !r_fac.is_empty();

    mask.foreach_index([&](const int64_t i) {
      float3 p = vector[i] * scale[i];
      const float3 s = p * scratch_scale[i];
      const float3 cell = math::floor(s);
      const float3 local = math::fract(s);

      float3 cell_noise = noise::hash_float_to_float3(cell + float3(1.37f, 2.73f, 4.19f));
      const float wave = sinf((local.x * (2.0f + randomness[i] * 10.0f) +
                              local.y * (1.5f + roughness[i] * 7.0f) +
                              cell_noise.x * 6.2831853f) *
                             (1.0f + roughness[i] * 5.0f));
      const float scratch_band = 1.0f - math::abs(wave);
      const float band = 1.0f - math::abs(local.y - 0.5f + (cell_noise.y - 0.5f) * randomness[i]);
      const float stripe = smoothstep(0.0f, 0.8f + density[i] * 0.4f, band * scratch_band);
      const float distort = noise::perlin_fbm<float3>(p * (1.0f + scratch_scale[i]),
                                                     3.0f,
                                                     roughness[i],
                                                     2.0f,
                                                     true) *
                            (0.5f + distortion[i] * 0.5f);
      float fac = clamp_f(stripe + distort * (0.5f + density[i]), 0.0f, 1.0f);
      fac = fac * (0.7f + density[i] * 0.6f);
      fac = clamp_f(fac, 0.0f, 1.0f);

      float3 base = noise::hash_float_to_float3(cell + float3(9.81f, 5.23f, 1.17f));
      base = base * (0.4f + fac) + float3(0.35f, 0.35f, 0.35f);
      r_color[i] = ColorGeometry4f(base[0], base[1], base[2], 1.0f);
      if (compute_factor) {
        r_fac[i] = fac;
      }
    });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
  }
};

static void sh_node_scratch_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  builder.construct_and_set_matching_fn<ScratchFunction>();
}

}  // namespace nodes::node_shader_tex_scratch_cc

void register_node_type_sh_tex_scratch()
{
  namespace file_ns = nodes::node_shader_tex_scratch_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeTexScratch"_ustr, SH_NODE_TEX_SCRATCH);
  ntype.ui_name = "Scratch Texture";
  ntype.ui_description = "Generate a procedural scratch texture";
  ntype.enum_name_legacy = "TEX_SCRATCH";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::sh_node_tex_scratch_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_tex_scratch;
  ntype.initfunc = file_ns::node_shader_init_tex_scratch;
  bke::node_type_storage(
      ntype, "NodeTexScratch", node_free_standard_storage, node_copy_standard_storage);
  ntype.build_multi_function = file_ns::sh_node_scratch_build_multi_function;

  bke::node_register_type(ntype);
}

}  // namespace blender
