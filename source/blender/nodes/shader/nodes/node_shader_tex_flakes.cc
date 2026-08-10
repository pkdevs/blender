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

  b.add_input<decl::Float>("Roughness"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.2f)
      .subtype(PROP_FACTOR)
      .description("Roughness of the flake normal distribution");

  b.add_input<decl::Float>("Coverage"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .description("Density of flake coverage (0 = no flakes, 1 = full coverage)");

  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Vector>("Normal"_ustr);
  b.add_output<decl::Float>("Factor"_ustr, "Fac"_ustr);
  b.add_output<decl::Float>("Presence"_ustr);
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

static float3 mx_rotate_flake(const float3 &p, const float3 &rot_noise)
{
  float theta = M_PI * 2.0f * rot_noise.x;
  float phi = M_PI * 2.0f * rot_noise.y;
  float z = rot_noise.z * 2.0f;

  float r = math::sqrt(z);
  float vx = math::sin(phi) * r;
  float vy = math::cos(phi) * r;
  float vz = math::sqrt(2.0f - z);

  float s_theta = math::sin(theta);
  float c_theta = math::cos(theta);
  float sx = vx * c_theta - vy * s_theta;
  float sy = vx * s_theta + vy * c_theta;

  float3 result;
  result.x = (vx * sx - c_theta) * p.x + (vy * sx + s_theta) * p.y + (vz * sx) * p.z;
  result.y = (vx * sy - s_theta) * p.x + (vy * sy - c_theta) * p.y + (vz * sy) * p.z;
  result.z = (vx * vz) * p.x + (vy * vz) * p.y + (1.0f - z) * p.z;

  return result;
}

static float mx_flake_density_to_probability(float x)
{
  float a = -26.19771808f;
  float b = 26.39663835f;
  float c = 85.53857017f;
  float d = -102.35069432f;
  float e = -101.42634862f;
  float f = 118.45082288f;
  float xx = x * x;
  return (a * xx + b * x) / (c * xx * x + d * xx + e * x + f);
}

static void compute_flake_3d(const float3 &p,
                      float roughness,
                      float coverage,
                      ColorGeometry4f &r_color,
                      float3 &r_normal,
                      float &r_factor,
                      float &r_presence)
{
  float probability = mx_flake_density_to_probability(coverage);
  float flake_diameter = 1.5f / math::sqrt(3.0f);
  float flake_radius2 = (flake_diameter * flake_diameter) * 3.0f;

  float3 cell_center = math::floor(p) + float3(0.5f);

  float3 closest_flake_pos = float3(0.0f);
  float3 closest_flake_normal = float3(0.0f, 0.0f, 1.0f);
  float closest_flake_dist2 = 1e5f;
  float closest_flake_priority = -1.0f;
  bool flake_found = false;

  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      for (int z = -1; z <= 1; z++) {
        float3 current_cell = cell_center + float3(x, y, z);
        
        float cell_presence = noise::hash_float_to_float(current_cell);
        if (cell_presence > probability) {
          continue;
        }

        float priority = noise::hash_float_to_float(current_cell + float3(3.0f));
        float3 flake_pos = current_cell + noise::hash_float_to_float3(current_cell + float3(1.0f)) - float3(0.5f);
        
        float3 delta = p - flake_pos;
        float dist2 = math::length_squared(delta);

        if (dist2 < flake_radius2) {
          if (priority > closest_flake_priority) {
            closest_flake_priority = priority;
            closest_flake_dist2 = dist2;
            closest_flake_pos = current_cell;
            
            float3 rot_noise = noise::hash_float_to_float3(current_cell);
            closest_flake_normal = mx_rotate_flake(float3(0.0f, 0.0f, 1.0f), rot_noise);
            flake_found = true;
          }
        }
      }
    }
  }

  if (flake_found) {
    float3 rot_noise = noise::hash_float_to_float3(closest_flake_pos + float3(2.0f));
    
    float alpha2 = roughness * roughness;
    float theta_noise = rot_noise.x;
    float phi_noise = rot_noise.y;
    float tan_theta = math::sqrt(theta_noise * alpha2 / (1.0f - theta_noise));
    float sin_theta = tan_theta / math::sqrt(1.0f + tan_theta * tan_theta);
    float cos_theta = 1.0f / math::sqrt(1.0f + tan_theta * tan_theta);
    float phi = 2.0f * M_PI * phi_noise;
    
    float3 local_dir(math::cos(phi) * sin_theta, math::sin(phi) * sin_theta, cos_theta);
    
    float3 n = float3(0.0f, 0.0f, 1.0f);
    float3 t = float3(1.0f, 0.0f, 0.0f);
    float3 b = float3(0.0f, 1.0f, 0.0f);
    
    float3 perturbed_normal = local_dir.x * t + local_dir.y * b + local_dir.z * closest_flake_normal;
    r_normal = math::normalize(perturbed_normal);
    
    r_factor = noise::hash_float_to_float(closest_flake_pos + float3(4.0f));
    float3 color_hash = noise::hash_float_to_float3(closest_flake_pos + float3(5.0f));
    r_color = ColorGeometry4f(color_hash.x, color_hash.y, color_hash.z, 1.0f);
    r_presence = 1.0f;
  }
  else {
    r_normal = float3(0.0f, 0.0f, 1.0f);
    r_factor = 0.0f;
    r_color = ColorGeometry4f(0.0f, 0.0f, 0.0f, 1.0f);
    r_presence = 0.0f;
  }
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
    builder.single_input<float>("Roughness");
    builder.single_input<float>("Coverage");

    builder.single_output<ColorGeometry4f>("Color", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float3>("Normal", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float>("Factor", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float>("Presence", mf::ParamFlag::SupportsUnusedOutput);

    return signature;
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    int param = 0;
    if (dimensions_ != 1) param++;
    if (dimensions_ == 1 || dimensions_ == 4) param++;
    param += 3; // Scale, Roughness, Coverage

    MutableSpan<ColorGeometry4f> r_color =
        params.uninitialized_single_output_if_required<ColorGeometry4f>(param++, "Color");
    MutableSpan<float3> r_normal =
        params.uninitialized_single_output_if_required<float3>(param++, "Normal");
    MutableSpan<float> r_factor =
        params.uninitialized_single_output_if_required<float>(param++, "Factor");
    MutableSpan<float> r_presence =
        params.uninitialized_single_output_if_required<float>(param++, "Presence");

    const bool compute_color = !r_color.is_empty();
    const bool compute_normal = !r_normal.is_empty();
    const bool compute_factor = !r_factor.is_empty();
    const bool compute_presence = !r_presence.is_empty();

    switch (dimensions_) {
      case 1: {
        const VArray<float> &w = params.readonly_single_input<float>(0, "W");
        const VArray<float> &scale = params.readonly_single_input<float>(1, "Scale");
        const VArray<float> &roughness = params.readonly_single_input<float>(2, "Roughness");
        const VArray<float> &coverage = params.readonly_single_input<float>(3, "Coverage");
        
        mask.foreach_index([&](const int64_t i) {
          float3 p = float3(w[i] * scale[i], 0.0f, 0.0f);
          
          ColorGeometry4f color; float3 normal; float factor, presence;
          compute_flake_3d(p, roughness[i], coverage[i], color, normal, factor, presence);
          
          if (compute_color) r_color[i] = color;
          if (compute_normal) r_normal[i] = normal;
          if (compute_factor) r_factor[i] = factor;
          if (compute_presence) r_presence[i] = presence;
        });
        break;
      }
      case 2: {
        const VArray<float3> &vector = params.readonly_single_input<float3>(0, "Vector");
        const VArray<float> &scale = params.readonly_single_input<float>(1, "Scale");
        const VArray<float> &roughness = params.readonly_single_input<float>(2, "Roughness");
        const VArray<float> &coverage = params.readonly_single_input<float>(3, "Coverage");
        
        mask.foreach_index([&](const int64_t i) {
          float3 p = float3(vector[i].x, vector[i].y, 0.0f) * scale[i];
          
          ColorGeometry4f color; float3 normal; float factor, presence;
          compute_flake_3d(p, roughness[i], coverage[i], color, normal, factor, presence);
          
          if (compute_color) r_color[i] = color;
          if (compute_normal) r_normal[i] = normal;
          if (compute_factor) r_factor[i] = factor;
          if (compute_presence) r_presence[i] = presence;
        });
        break;
      }
      case 3: {
        const VArray<float3> &vector = params.readonly_single_input<float3>(0, "Vector");
        const VArray<float> &scale = params.readonly_single_input<float>(1, "Scale");
        const VArray<float> &roughness = params.readonly_single_input<float>(2, "Roughness");
        const VArray<float> &coverage = params.readonly_single_input<float>(3, "Coverage");
        
        mask.foreach_index([&](const int64_t i) {
          float3 p = vector[i] * scale[i];
          
          ColorGeometry4f color; float3 normal; float factor, presence;
          compute_flake_3d(p, roughness[i], coverage[i], color, normal, factor, presence);
          
          if (compute_color) r_color[i] = color;
          if (compute_normal) r_normal[i] = normal;
          if (compute_factor) r_factor[i] = factor;
          if (compute_presence) r_presence[i] = presence;
        });
        break;
      }
      case 4: {
        const VArray<float3> &vector = params.readonly_single_input<float3>(0, "Vector");
        const VArray<float> &w = params.readonly_single_input<float>(1, "W");
        const VArray<float> &scale = params.readonly_single_input<float>(2, "Scale");
        const VArray<float> &roughness = params.readonly_single_input<float>(3, "Roughness");
        const VArray<float> &coverage = params.readonly_single_input<float>(4, "Coverage");
        
        mask.foreach_index([&](const int64_t i) {
          // Flatten 4D to 3D for flake logic as flakes are fundamentally 3D microfacets
          float3 p = float3(vector[i].x, vector[i].y, vector[i].z + w[i]) * scale[i];
          
          ColorGeometry4f color; float3 normal; float factor, presence;
          compute_flake_3d(p, roughness[i], coverage[i], color, normal, factor, presence);
          
          if (compute_color) r_color[i] = color;
          if (compute_normal) r_normal[i] = normal;
          if (compute_factor) r_factor[i] = factor;
          if (compute_presence) r_presence[i] = presence;
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
