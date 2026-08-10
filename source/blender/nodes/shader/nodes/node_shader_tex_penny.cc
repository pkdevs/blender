

/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"
#include <cfloat>

#include "MEM_guardedalloc.h"

#include "BKE_texture.h"

#include "NOD_multi_function.hh"

#include "BLI_noise_penny.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_tex_penny_cc {

NODE_STORAGE_FUNCS(NodeTexPenny)

static void sh_node_tex_penny_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Coordinate"_ustr)
      .default_input_type(NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Float>("W"_ustr).min(-1000.0f).max(1000.0f).default_value(0.0f);
  /* Octaves is intentionally declared before Scale. */
  b.add_input<decl::Float>("Octaves"_ustr).min(1.0f).max(15.0f).default_value(7.0f);
  b.add_input<decl::Float>("Scale"_ustr)
      .min(0.0f)
      .max(10000.0f)
      .default_value(100.0f)
      .subtype(PROP_PERCENTAGE);
  b.add_input<decl::Float>("Scale X"_ustr)
      .min(0.0f)
      .max(10000.0f)
      .default_value(100.0f)
      .subtype(PROP_PERCENTAGE);
  b.add_input<decl::Float>("Scale Y"_ustr)
      .min(0.0f)
      .max(10000.0f)
      .default_value(100.0f)
      .subtype(PROP_PERCENTAGE);
  b.add_input<decl::Float>("Scale Z"_ustr)
      .min(0.0f)
      .max(10000.0f)
      .default_value(100.0f)
      .subtype(PROP_PERCENTAGE);
  b.add_input<decl::Float>("Scale W"_ustr)
      .min(0.0f)
      .max(10000.0f)
      .default_value(100.0f)
      .subtype(PROP_PERCENTAGE);
  b.add_input<decl::Float>("Roughness"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>("Level Min"_ustr).min(-FLT_MAX).max(FLT_MAX).default_value(0.0f);
  b.add_input<decl::Float>("Level Mid"_ustr)
      .min(0.001f)
      .max(0.999f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>("Level Max"_ustr).min(-FLT_MAX).max(FLT_MAX).default_value(1.0f);
  b.add_input<decl::Float>("Period X"_ustr).min(1.0f).max(256.0f).default_value(256.0f);
  b.add_input<decl::Float>("Period Y"_ustr).min(1.0f).max(256.0f).default_value(256.0f);
  b.add_input<decl::Float>("Period Z"_ustr).min(1.0f).max(256.0f).default_value(256.0f);
  b.add_input<decl::Float>("Period W"_ustr).min(1.0f).max(256.0f).default_value(256.0f);
  b.add_output<decl::Float>("Fac"_ustr).no_muted_links();
  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
  b.add_output<decl::Vector>("Gradient"_ustr).no_muted_links();
}

static void node_shader_buts_tex_penny(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  /* Full-width dropdowns (empty label) so long enum names aren't truncated. */
  layout.prop(ptr, "penny_dimensions", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
  layout.prop(ptr, "noise_type", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
  layout.prop(ptr, "blend_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
  layout.prop(ptr, "tileable", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_shader_init_tex_penny(bNodeTree * /*ntree*/, bNode *node)
{
  NodeTexPenny *tex = MEM_new<NodeTexPenny>(__func__);
  BKE_texture_mapping_default(&tex->base.tex_mapping, TEXMAP_TYPE_POINT);
  BKE_texture_colormapping_default(&tex->base.color_mapping);
  tex->period_x = 256;
  tex->period_y = 256;
  tex->period_z = 256;
  tex->period_w = 256;
  tex->blend_mode = 0;
  tex->noise_type = 0;
  tex->dimensions = 4;
  tex->tileable = 1;
  node->storage = tex;
}

/* Per-noise-type GLSL entry point. Penny Noise is compiled as one shader variant
 * per noise type (see gpu_shader_material_tex_penny.glsl): linking only the used
 * variant lets the shader compiler process a single noise type instead of all 19,
 * which is what makes the Vulkan compile time tractable. This mirrors the Noise
 * Texture node's gpu_shader_get_name(). Dimensions stays a runtime constant inside
 * each variant (start with 19 variants; split by dimension later if needed). */
static const char *gpu_shader_get_name(const int noise_type)
{
  switch (noise_type) {
    case 0:
      return "node_tex_penny_fbm";
    case 1:
      return "node_tex_penny_turbulence";
    case 2:
      return "node_tex_penny_stupl";
    case 3:
      return "node_tex_penny_luka";
    case 4:
      return "node_tex_penny_blistered";
    case 5:
      return "node_tex_penny_box";
    case 6:
      return "node_tex_penny_mod";
    case 7:
      return "node_tex_penny_nutous";
    case 8:
      return "node_tex_penny_cranal";
    case 9:
      return "node_tex_penny_buya";
    case 10:
      return "node_tex_penny_cell";
    case 11:
      return "node_tex_penny_disturb";
    case 12:
      return "node_tex_penny_hama";
    case 13:
      return "node_tex_penny_naki";
    case 14:
      return "node_tex_penny_poxo";
    case 15:
      return "node_tex_penny_pezo";
    case 16:
      return "node_tex_penny_dents";
    case 17:
      return "node_tex_penny_sema";
    case 18:
      return "node_tex_penny_vl";
  }
  /* Default to fBm (noise_type 0) for any out-of-range value. */
  return "node_tex_penny_fbm";
}

static int node_shader_gpu_tex_penny(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{
  node_shader_gpu_default_tex_coord(mat, node, &in[0].link);
  node_shader_gpu_tex_mapping(mat, node, in, out);

  const NodeTexPenny &storage = node_storage(*node);
  float blend_mode = (float)storage.blend_mode;
  float dimensions = (float)storage.dimensions;
  float tileable = (float)storage.tileable;
  /* Compile-time flag: skip the gradient taps unless its output is connected. */
  float gradient_connected = out[2].hasoutput ? 1.0f : 0.0f;

  /* Noise type is now implicit in the linked variant name, so it is no longer
   * passed as a runtime constant. */
  const char *name = gpu_shader_get_name(storage.noise_type);
  return GPU_stack_link(mat,
                        node,
                        name,
                        in,
                        out,
                        GPU_constant(&blend_mode),
                        GPU_constant(&dimensions),
                        GPU_constant(&tileable),
                        GPU_constant(&gradient_connected));
}

class PennyFunction : public mf::MultiFunction {
 private:
  int dimensions_;
  int noise_type_;
  int blend_mode_;
  int tileable_;
  mf::Signature signature_;

 public:
  PennyFunction(int dimensions, int noise_type, int blend_mode, int tileable)
      : dimensions_(dimensions), noise_type_(noise_type), blend_mode_(blend_mode), tileable_(tileable)
  {
    BLI_assert(dimensions_ >= 1 && dimensions_ <= 4);
    mf::SignatureBuilder builder{"Penny", signature_};
    if (ELEM(dimensions_, 2, 3, 4)) {
      builder.single_input<float3>("Coordinate");
    }
    if (ELEM(dimensions_, 1, 4)) {
      builder.single_input<float>("W");
    }
    builder.single_input<float>("Octaves");
    builder.single_input<float>("Scale");
    builder.single_input<float>("Scale X");
    builder.single_input<float>("Scale Y");
    builder.single_input<float>("Scale Z");
    builder.single_input<float>("Scale W");
    builder.single_input<float>("Roughness");
    builder.single_input<float>("Level Min");
    builder.single_input<float>("Level Mid");
    builder.single_input<float>("Level Max");
    if (tileable_) {
      builder.single_input<float>("Period X");
      builder.single_input<float>("Period Y");
      builder.single_input<float>("Period Z");
      builder.single_input<float>("Period W");
    }
    builder.single_output<float>("Fac", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<ColorGeometry4f>("Color", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float3>("Gradient", mf::ParamFlag::SupportsUnusedOutput);
    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    /* Inputs shift index depending on which of Coordinate/W are present. */
    int p = 0;
    const bool has_coord = ELEM(dimensions_, 2, 3, 4);
    const bool has_w = ELEM(dimensions_, 1, 4);
    const int coord_index = has_coord ? p++ : -1;
    const int w_index = has_w ? p++ : -1;
    const int octaves_index = p++;
    const int scale_index = p++;
    const int scale_x_index = p++;
    const int scale_y_index = p++;
    const int scale_z_index = p++;
    const int scale_w_index = p++;
    const int roughness_index = p++;
    const int lvl_min_index = p++;
    const int lvl_mid_index = p++;
    const int lvl_max_index = p++;
    const int per_x_index = tileable_ ? p++ : -1;
    const int per_y_index = tileable_ ? p++ : -1;
    const int per_z_index = tileable_ ? p++ : -1;
    const int per_w_index = tileable_ ? p++ : -1;
    const int fac_index = p++;
    const int color_index = p++;
    const int gradient_index = p++;

    const VArray<float> &scale = params.readonly_single_input<float>(scale_index, "Scale");
    const VArray<float> &scale_x = params.readonly_single_input<float>(scale_x_index, "Scale X");
    const VArray<float> &scale_y = params.readonly_single_input<float>(scale_y_index, "Scale Y");
    const VArray<float> &scale_z = params.readonly_single_input<float>(scale_z_index, "Scale Z");
    const VArray<float> &scale_w = params.readonly_single_input<float>(scale_w_index, "Scale W");
    const VArray<float> &octaves = params.readonly_single_input<float>(octaves_index, "Octaves");
    const VArray<float> &roughness = params.readonly_single_input<float>(roughness_index,
                                                                         "Roughness");
    const VArray<float> &lvl_min = params.readonly_single_input<float>(lvl_min_index, "Level Min");
    const VArray<float> &lvl_mid = params.readonly_single_input<float>(lvl_mid_index, "Level Mid");
    const VArray<float> &lvl_max = params.readonly_single_input<float>(lvl_max_index, "Level Max");
    VArray<float> per_x, per_y, per_z, per_w;
    if (tileable_) {
      per_x = params.readonly_single_input<float>(per_x_index, "Period X");
      per_y = params.readonly_single_input<float>(per_y_index, "Period Y");
      per_z = params.readonly_single_input<float>(per_z_index, "Period Z");
      per_w = params.readonly_single_input<float>(per_w_index, "Period W");
    }

    VArray<float3> coord;
    if (has_coord) {
      coord = params.readonly_single_input<float3>(coord_index, "Coordinate");
    }
    VArray<float> w_in;
    if (has_w) {
      w_in = params.readonly_single_input<float>(w_index, "W");
    }

    MutableSpan<float> r_fac = params.uninitialized_single_output_if_required<float>(fac_index,
                                                                                     "Fac");
    MutableSpan<ColorGeometry4f> r_color =
        params.uninitialized_single_output_if_required<ColorGeometry4f>(color_index, "Color");
    MutableSpan<float3> r_gradient = params.uninitialized_single_output_if_required<float3>(
        gradient_index, "Gradient");

    const bool compute_fac = !r_fac.is_empty();
    const bool compute_color = !r_color.is_empty();
    const bool compute_gradient = !r_gradient.is_empty();

    mask.foreach_index([&](const int64_t i) {
      /* actual_scale = (Scale/100) * (Scale_axis/100) * 0.25; input divided by it. */
      const float global_factor = scale[i] * 0.01f;
      const float fx_scale = math::max(global_factor * scale_x[i] * 0.01f, 1e-6f) * 0.25f;
      const float fy_scale = math::max(global_factor * scale_y[i] * 0.01f, 1e-6f) * 0.25f;
      const float fz_scale = math::max(global_factor * scale_z[i] * 0.01f, 1e-6f) * 0.25f;
      const float fw_scale = math::max(global_factor * scale_w[i] * 0.01f, 1e-6f) * 0.25f;

      /* Assemble the sampled coordinate per dimension (matches Blender Noise):
       * 1D -> (0,0,0,W); 2D -> (X,Y,0,0); 3D -> (X,Y,Z,0); 4D -> (X,Y,Z,W). */
      const float3 p = has_coord ? coord[i] : float3(0.0f, 0.0f, 0.0f);
      const float wv = has_w ? w_in[i] : 0.0f;
      float px = 0.0f, py = 0.0f, pz = 0.0f, pw = 0.0f;
      int4 per_final;
      if (tileable_) {
        /* Tileable: snap the lattice to round(Period / scale) and sample at
         * coord * (lattice / Period) so it repeats exactly every Period. */
        const int Px = math::clamp(int(per_x[i]), 1, 256);
        const int Py = math::clamp(int(per_y[i]), 1, 256);
        const int Pz = math::clamp(int(per_z[i]), 1, 256);
        const int Pw = math::clamp(int(per_w[i]), 1, 256);
        const int Lx = math::clamp(int(float(Px) / fx_scale + 0.5f), 1, 4096);
        const int Ly = math::clamp(int(float(Py) / fy_scale + 0.5f), 1, 4096);
        const int Lz = math::clamp(int(float(Pz) / fz_scale + 0.5f), 1, 4096);
        const int Lw = math::clamp(int(float(Pw) / fw_scale + 0.5f), 1, 4096);
        const float kx = float(Lx) / float(Px);
        const float ky = float(Ly) / float(Py);
        const float kz = float(Lz) / float(Pz);
        const float kw = float(Lw) / float(Pw);
        switch (dimensions_) {
          case 1: pw = wv * kw; break;
          case 2: px = p.x * kx; py = p.y * ky; break;
          case 3: px = p.x * kx; py = p.y * ky; pz = p.z * kz; break;
          default: px = p.x * kx; py = p.y * ky; pz = p.z * kz; pw = wv * kw; break;
        }
        per_final = int4(Lx, Ly, Lz, Lw);
      }
      else {
        /* Non-tiling: continuous Scale; large fixed lattice (effectively never repeats). */
        switch (dimensions_) {
          case 1: pw = wv / fw_scale; break;
          case 2: px = p.x / fx_scale; py = p.y / fy_scale; break;
          case 3: px = p.x / fx_scale; py = p.y / fy_scale; pz = p.z / fz_scale; break;
          default: px = p.x / fx_scale; py = p.y / fy_scale; pz = p.z / fz_scale; pw = wv / fw_scale; break;
        }
        per_final = int4(256, 256, 256, 256);
      }
      const float4 pos(px, py, pz, pw);

      noise::penny::PennyParams pp;
      pp.type = noise_type_;
      pp.blend_mode = blend_mode_;
      pp.detail = math::clamp(octaves[i], 1.0f, 15.0f);
      pp.roughness = math::max(roughness[i], 0.0f);
      pp.levels = float3(lvl_min[i], lvl_mid[i], lvl_max[i]);
      pp.period = per_final;

      const float3 rgb = noise::penny::penny(pos, pp);
      if (compute_fac) {
        r_fac[i] = rgb.x;
      }
      if (compute_color) {
        r_color[i] = ColorGeometry4f(rgb.x, rgb.y, rgb.z, 1.0f);
      }
      if (compute_gradient) {
        /* Raw pre-Levels gradient: central differences with identity Levels, in
         * co-space to match the other backends. See noise_penny.cc for rationale. */
        noise::penny::PennyParams pg = pp;
        pg.levels = float3(0.0f, 0.5f, 1.0f);
        constexpr float delta = 0.01f;
        constexpr float inv_2delta = 1.0f / (2.0f * delta);
        const float gx = noise::penny::penny(float4(px + delta, py, pz, pw), pg).x -
                         noise::penny::penny(float4(px - delta, py, pz, pw), pg).x;
        const float gy = noise::penny::penny(float4(px, py + delta, pz, pw), pg).x -
                         noise::penny::penny(float4(px, py - delta, pz, pw), pg).x;
        const float gz = noise::penny::penny(float4(px, py, pz + delta, pw), pg).x -
                         noise::penny::penny(float4(px, py, pz - delta, pw), pg).x;
        r_gradient[i] = float3(gx / fx_scale, gy / fy_scale, gz / fz_scale) * inv_2delta;
      }
    });
  }
};

static void sh_node_penny_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNode &node = builder.node();
  const NodeTexPenny &storage = node_storage(node);
  builder.construct_and_set_matching_fn<PennyFunction>(
      storage.dimensions, storage.noise_type, storage.blend_mode, storage.tileable);
}

static void node_shader_update_tex_penny(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *sockCoordinate = bke::node_find_socket(*node, SOCK_IN, "Coordinate"_ustr);
  bNodeSocket *sockW = bke::node_find_socket(*node, SOCK_IN, "W"_ustr);
  bNodeSocket *sockPX = bke::node_find_socket(*node, SOCK_IN, "Period X"_ustr);
  bNodeSocket *sockPY = bke::node_find_socket(*node, SOCK_IN, "Period Y"_ustr);
  bNodeSocket *sockPZ = bke::node_find_socket(*node, SOCK_IN, "Period Z"_ustr);
  bNodeSocket *sockPW = bke::node_find_socket(*node, SOCK_IN, "Period W"_ustr);

  const NodeTexPenny &storage = node_storage(*node);
  const bool tileable = storage.tileable != 0;

  bke::node_set_socket_availability(*ntree, *sockCoordinate, storage.dimensions != 1);
  bke::node_set_socket_availability(
      *ntree, *sockW, storage.dimensions == 1 || storage.dimensions == 4);
  /* Period only applies to seamless tiling; hide it when Tileable is off, and
   * respect the active Dimensions so unused axes stay hidden. */
  bke::node_set_socket_availability(*ntree, *sockPX, tileable && storage.dimensions != 1);
  bke::node_set_socket_availability(*ntree, *sockPY, tileable && storage.dimensions != 1);
  bke::node_set_socket_availability(*ntree, *sockPZ, tileable && storage.dimensions >= 3);
  bke::node_set_socket_availability(
      *ntree, *sockPW, tileable && (storage.dimensions == 1 || storage.dimensions == 4));
}

}  // namespace nodes::node_shader_tex_penny_cc

void register_node_type_sh_tex_penny()
{
  namespace file_ns = nodes::node_shader_tex_penny_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeTexPenny"_ustr, SH_NODE_TEX_PENNY);
  ntype.ui_name = "Penny Noise";
  ntype.ui_description =
      "Tileable 4D Improved Perlin noise. "
      "X/Y/Z input sockets override Vector components when connected";
  ntype.enum_name_legacy = "TEX_PENNY";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::sh_node_tex_penny_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_tex_penny;
  ntype.initfunc = file_ns::node_shader_init_tex_penny;
  ntype.updatefunc = file_ns::node_shader_update_tex_penny;
  bke::node_type_storage(
      ntype, "NodeTexPenny", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_tex_penny;
  ntype.build_multi_function = file_ns::sh_node_penny_build_multi_function;
  ntype.default_width = bke::NodeWidth::_160;

  bke::node_register_type(ntype);
}

}  // namespace blender
