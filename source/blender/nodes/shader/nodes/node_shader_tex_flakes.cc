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


class FlakesFunction : public mf::MultiFunction {
 private:
  int dimensions_;

 public:
  FlakesFunction(int dimensions) : dimensions_(dimensions)
  {
    static const mf::Signature signatures[4] = {
        create_signature(1),
        create_signature(2),
        create_signature(3),
        create_signature(4),
    };
    this->set_signature(&signatures[dimensions - 1]);
  }

  static mf::Signature create_signature(int dimensions)
  {
    mf::Signature signature;
    mf::SignatureBuilder builder{"Flakes", signature};

    if (dimensions != 1) {
      builder.single_input<float3>("Vector");
    }
    if (dimensions == 1 || dimensions == 4) {
      builder.single_input<float>("W");
    }
    builder.single_input<float>("Scale");
    builder.single_input<float>("Density");
    builder.single_input<float>("Flake Size");
    builder.single_input<float>("Randomness");

    builder.single_output<ColorGeometry4f>("Color", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float3>("Normal", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float>("Factor", mf::ParamFlag::SupportsUnusedOutput);

    return signature;
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    int param = 0;
    if (dimensions_ != 1) param++;
    if (dimensions_ == 1 || dimensions_ == 4) param++;
    param += 4; // Scale, Density, Flake Size, Randomness

    MutableSpan<ColorGeometry4f> r_color =
        params.uninitialized_single_output_if_required<ColorGeometry4f>(param++, "Color");
    MutableSpan<float3> r_normal =
        params.uninitialized_single_output_if_required<float3>(param++, "Normal");
    MutableSpan<float> r_factor =
        params.uninitialized_single_output_if_required<float>(param++, "Factor");

    const bool compute_color = !r_color.is_empty();
    const bool compute_normal = !r_normal.is_empty();
    const bool compute_factor = !r_factor.is_empty();

    switch (dimensions_) {
      case 1: {
        const VArray<float> &w = params.readonly_single_input<float>(0, "W");
        const VArray<float> &scale = params.readonly_single_input<float>(1, "Scale");
        const VArray<float> &density = params.readonly_single_input<float>(2, "Density");
        const VArray<float> &flake_size = params.readonly_single_input<float>(3, "Flake Size");
        const VArray<float> &randomness = params.readonly_single_input<float>(4, "Randomness");
        
        mask.foreach_index([&](const int64_t i) {
          float p = w[i] * scale[i];
          float cell = math::floor(p);

          float best_fac = 0.0f;
          float3 best_normal = float3(0.0f, 0.0f, 1.0f);
          float3 best_color = float3(0.0f, 0.0f, 0.0f);
          float highest_z = -1000.0f;

          for (int xi = -1; xi <= 1; xi++) {
            float neighbor = cell + (float)xi;
            float hash_id = noise::hash_float_to_float(neighbor);

            if (hash_id < density[i]) {
              float h = noise::hash_float_to_float(neighbor + 0.5f);
              float pos = neighbor + h * randomness[i];
              float dist = math::abs(p - pos);

              if (dist < flake_size[i] / 2.0f) {
                float flake_z = noise::hash_float_to_float(neighbor + 12.34f);
                if (flake_z > highest_z) {
                  highest_z = flake_z;
                  best_fac = 1.0f;
                  best_normal = math::normalize(noise::hash_float_to_float3(neighbor + 56.78f) * 2.0f - float3(1.0f));
                  best_color = noise::hash_float_to_float3(neighbor + 90.12f);
                }
              }
            }
          }

          if (compute_color) r_color[i] = ColorGeometry4f(best_color[0], best_color[1], best_color[2], 1.0f);
          if (compute_normal) r_normal[i] = best_normal;
          if (compute_factor) r_factor[i] = best_fac;
        });
        break;
      }
      case 2: {
        const VArray<float3> &vector = params.readonly_single_input<float3>(0, "Vector");
        const VArray<float> &scale = params.readonly_single_input<float>(1, "Scale");
        const VArray<float> &density = params.readonly_single_input<float>(2, "Density");
        const VArray<float> &flake_size = params.readonly_single_input<float>(3, "Flake Size");
        const VArray<float> &randomness = params.readonly_single_input<float>(4, "Randomness");
        
        mask.foreach_index([&](const int64_t i) {
          float2 p = float2(vector[i].x, vector[i].y) * scale[i];
          float2 cell = math::floor(p);

          float best_fac = 0.0f;
          float3 best_normal = float3(0.0f, 0.0f, 1.0f);
          float3 best_color = float3(0.0f, 0.0f, 0.0f);
          float highest_z = -1000.0f;

          for (int xi = -1; xi <= 1; xi++) {
            for (int yi = -1; yi <= 1; yi++) {
              float2 neighbor = cell + float2((float)xi, (float)yi);
              float hash_id = noise::hash_float_to_float(neighbor);

              if (hash_id < density[i]) {
                float2 h = noise::hash_float_to_float2(neighbor + float2(0.5f));
                float2 pos = neighbor + h * randomness[i];
                float dist = math::length(p - pos);

                if (dist < flake_size[i] / 2.0f) {
                  float flake_z = noise::hash_float_to_float(neighbor + float2(12.34f));
                  if (flake_z > highest_z) {
                    highest_z = flake_z;
                    best_fac = 1.0f;
                    best_normal = math::normalize(noise::hash_float_to_float3(neighbor + float2(56.78f)) * 2.0f - float3(1.0f));
                    best_color = noise::hash_float_to_float3(neighbor + float2(90.12f));
                  }
                }
              }
            }
          }

          if (compute_color) r_color[i] = ColorGeometry4f(best_color[0], best_color[1], best_color[2], 1.0f);
          if (compute_normal) r_normal[i] = best_normal;
          if (compute_factor) r_factor[i] = best_fac;
        });
        break;
      }
      case 3: {
        const VArray<float3> &vector = params.readonly_single_input<float3>(0, "Vector");
        const VArray<float> &scale = params.readonly_single_input<float>(1, "Scale");
        const VArray<float> &density = params.readonly_single_input<float>(2, "Density");
        const VArray<float> &flake_size = params.readonly_single_input<float>(3, "Flake Size");
        const VArray<float> &randomness = params.readonly_single_input<float>(4, "Randomness");
        
        mask.foreach_index([&](const int64_t i) {
          float3 p = vector[i] * scale[i];
          float3 cell = math::floor(p);

          float best_fac = 0.0f;
          float3 best_normal = float3(0.0f, 0.0f, 1.0f);
          float3 best_color = float3(0.0f, 0.0f, 0.0f);
          float highest_z = -1000.0f;

          for (int xi = -1; xi <= 1; xi++) {
            for (int yi = -1; yi <= 1; yi++) {
              for (int zi = -1; zi <= 1; zi++) {
                float3 neighbor = cell + float3((float)xi, (float)yi, (float)zi);
                float hash_id = noise::hash_float_to_float(neighbor);

                if (hash_id < density[i]) {
                  float3 h = noise::hash_float_to_float3(neighbor + float3(0.5f));
                  float3 pos = neighbor + h * randomness[i];
                  float dist = math::length(p - pos);

                  if (dist < flake_size[i] / 2.0f) {
                    float flake_z = noise::hash_float_to_float(neighbor + float3(12.34f));
                    if (flake_z > highest_z) {
                      highest_z = flake_z;
                      best_fac = 1.0f;
                      best_normal = math::normalize(noise::hash_float_to_float3(neighbor + float3(56.78f)) * 2.0f - float3(1.0f));
                      best_color = noise::hash_float_to_float3(neighbor + float3(90.12f));
                    }
                  }
                }
              }
            }
          }

          if (compute_color) r_color[i] = ColorGeometry4f(best_color[0], best_color[1], best_color[2], 1.0f);
          if (compute_normal) r_normal[i] = best_normal;
          if (compute_factor) r_factor[i] = best_fac;
        });
        break;
      }
      case 4: {
        const VArray<float3> &vector = params.readonly_single_input<float3>(0, "Vector");
        const VArray<float> &w = params.readonly_single_input<float>(1, "W");
        const VArray<float> &scale = params.readonly_single_input<float>(2, "Scale");
        const VArray<float> &density = params.readonly_single_input<float>(3, "Density");
        const VArray<float> &flake_size = params.readonly_single_input<float>(4, "Flake Size");
        const VArray<float> &randomness = params.readonly_single_input<float>(5, "Randomness");
        
        mask.foreach_index([&](const int64_t i) {
          float4 p = float4(vector[i].x, vector[i].y, vector[i].z, w[i]) * scale[i];
          float4 cell = math::floor(p);

          float best_fac = 0.0f;
          float3 best_normal = float3(0.0f, 0.0f, 1.0f);
          float3 best_color = float3(0.0f, 0.0f, 0.0f);
          float highest_z = -1000.0f;

          for (int xi = -1; xi <= 1; xi++) {
            for (int yi = -1; yi <= 1; yi++) {
              for (int zi = -1; zi <= 1; zi++) {
                for (int wi = -1; wi <= 1; wi++) {
                  float4 neighbor = cell + float4((float)xi, (float)yi, (float)zi, (float)wi);
                  float hash_id = noise::hash_float_to_float(neighbor);

                  if (hash_id < density[i]) {
                    float4 h = noise::hash_float_to_float4(neighbor + float4(0.5f));
                    float4 pos = neighbor + h * randomness[i];
                    float dist = math::length(p - pos);

                    if (dist < flake_size[i] / 2.0f) {
                      float flake_z = noise::hash_float_to_float(neighbor + float4(12.34f));
                      if (flake_z > highest_z) {
                        highest_z = flake_z;
                        best_fac = 1.0f;
                        best_normal = math::normalize(noise::hash_float_to_float3(neighbor + float4(56.78f)) * 2.0f - float3(1.0f));
                        best_color = noise::hash_float_to_float3(neighbor + float4(90.12f));
                      }
                    }
                  }
                }
              }
            }
          }

          if (compute_color) r_color[i] = ColorGeometry4f(best_color[0], best_color[1], best_color[2], 1.0f);
          if (compute_normal) r_normal[i] = best_normal;
          if (compute_factor) r_factor[i] = best_fac;
        });
        break;
      }
    }
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(dimensions_);
  }
};

static void sh_node_flakes_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNode &node = builder.node();
  builder.construct_and_set_matching_fn<FlakesFunction>(int(node.custom1));
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
