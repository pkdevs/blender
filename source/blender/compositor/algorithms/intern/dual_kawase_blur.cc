/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "COM_context.hh"
#include "COM_utilities.hh"

#include "COM_algorithm_dual_kawase_blur.hh"

#include "GPU_shader.hh"
#include "GPU_state.hh"

namespace blender::compositor {

/* Dual Kawase blur (Marius Bjørge, "Bandwidth-Efficient Rendering", SIGGRAPH 2015) builds a
 * pyramid with a five-tap downsample filter, then reconstructs it with an eight-tap upsample
 * filter. Each level roughly doubles the blur, while working on fewer pixels.
 *
 * The basic algorithm only gives discrete blur sizes. We take the requested blur radius,
 * and map it to two neighboring levels (approximately 0, 6, 12, 24, ... pixels), and blend between
 * these results. The blend fraction is remapped with t * (2 + t) / 3 for a more even change in
 * apparent blur size.
 *
 * For independent X/Y radii, each axis chooses its own neighboring levels. This cell of
 * of level pairs and blends is triangulated to at most three corner results. An axis that has
 * reached its level keeps its resolution and uses zero sampling offset while the other axis
 * continues. The corners share downsample prefixes, and blending happens before their common
 * upsample suffix. This effectively allows to achieve independent X/Y blur radii, while
 * still being very similar performance profile to basic Dual Kawase algorithm. */

static Result kawase_downsample(
    Context &context, const Result &input, const int2 full_size, int2 divisor, float2 ratio)
{
  Result output = context.create_result(ResultType::Color);
  const int2 size = math::max(full_size / math::max(divisor, int2(1)), int2(1));
  output.allocate_texture(size);
  const float2 step = ratio / float2(size);
  if (context.use_gpu()) {
    gpu::Shader *shader = context.get_shader("compositor_dual_kawase_downsample");
    GPU_shader_bind(shader);
    GPU_texture_filter_mode(input, true);
    GPU_texture_extend_mode(input, GPU_SAMPLER_EXTEND_MODE_EXTEND);
    GPU_shader_uniform_2fv(shader, "uv_step", step);
    input.bind_as_texture(shader, "input_tx");
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, output.domain().data_size);
    input.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }
  else {
    const float2 texel_mul = float2(1.0f) / float2(size);
    const float2 texel_add = float2(0.5f) / float2(size);
    parallel_for(size, [&](const int2 texel) {
      /* Each invocation corresponds to output pixel. */
      float2 uv = float2(texel) * texel_mul + texel_add;
      float4 col0 = float4(input.sample_bilinear_extended<Color>(uv));
      float4 col1 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, -0.5f)));
      float4 col2 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, +0.5f)));
      float4 col3 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, -0.5f)));
      float4 col4 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, +0.5f)));
      float4 col = (4.0f * col0 + col1 + col2 + col3 + col4) * 0.125f;
      output.store_pixel(texel, Color(col));
    });
  }
  return output;
}

static Result kawase_upsample(
    Context &context, const Result &input, const int2 full_size, int2 divisor, float2 ratio)
{
  Result output = context.create_result(ResultType::Color);
  const int2 size = math::max(full_size / math::max(divisor, int2(1)), int2(1));
  output.allocate_texture(size);
  const float2 step = ratio / float2(input.domain().data_size);
  if (context.use_gpu()) {
    gpu::Shader *shader = context.get_shader("compositor_dual_kawase_upsample");
    GPU_shader_bind(shader);
    GPU_texture_filter_mode(input, true);
    GPU_texture_extend_mode(input, GPU_SAMPLER_EXTEND_MODE_EXTEND);
    GPU_shader_uniform_2fv(shader, "uv_step", step);
    input.bind_as_texture(shader, "input_tx");
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, output.domain().data_size);
    input.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }
  else {
    const float2 texel_mul = float2(1.0f) / float2(size);
    const float2 texel_add = float2(0.5f) / float2(size);
    parallel_for(size, [&](const int2 texel) {
      /* Each invocation corresponds to output pixel. */
      float2 uv = float2(texel) * texel_mul + texel_add;

      float4 col0 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(0, -1)));
      float4 col1 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(0, +1)));
      float4 col2 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(-1, 0)));
      float4 col3 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(+1, 0)));

      float4 col4 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, -0.5f)));
      float4 col5 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, +0.5f)));
      float4 col6 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, -0.5f)));
      float4 col7 = float4(
          input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, +0.5f)));

      float4 col = (col0 + col1 + col2 + col3 + (col4 + col5 + col6 + col7) * 2.0f) *
                   (1.0f / 12.0f);
      output.store_pixel(texel, Color(col));
    });
  }
  return output;
}

static void kawase_mix(Context &context, const Result &input, Result &output, float input_weight)
{
  if (context.use_gpu()) {
    gpu::Shader *shader = context.get_shader("compositor_dual_kawase_mix");
    GPU_shader_bind(shader);
    GPU_shader_uniform_1f(shader, "input_weight", input_weight);
    input.bind_as_texture(shader, "input_tx");
    output.bind_as_image(shader, "output_img", true);
    compute_dispatch_threads_at_least(shader, output.domain().data_size);
    input.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }
  else {
    parallel_for(output.domain().data_size, [&](const int2 texel) {
      float4 pix_curr = float4(input.load_pixel<Color>(texel));
      float4 pix_next = float4(output.load_pixel<Color>(texel));
      float4 pix = math::interpolate(pix_next, pix_curr, input_weight);
      output.store_pixel(texel, Color(pix));
    });
  }
}

struct RadiusInterval {
  int lower;
  int upper;
  float fraction;
};

static RadiusInterval kawase_radius_interval(float radius)
{
  /* Eyeballed formula how many Kawase iterations are needed
   * to approximately match gaussian blur radius. */
  const float steps = math::max(radius, 0.0f) / 3.0f;
  int lower = 0;
  int upper = 2;
  while (upper <= steps) {
    lower = upper;
    upper *= 2;
  }
  const float t = (steps - lower) / (upper - lower);
  /* Just interpolating between several discrete levels of blur does not feel very smooth;
   * remap the fraction to make it feel nicer. */
  return {lower, upper, t * (2.0f + t) / 3.0f};
}

struct KawaseCorner {
  int2 level;
  float weight;
};

void dual_kawase_blur(Context &context, const Result &input, Result &output, float2 radius)
{
  const RadiusInterval x = kawase_radius_interval(radius.x);
  const RadiusInterval y = kawase_radius_interval(radius.y);
  const int2 full_size = input.domain().data_size;

  /* For X=Y blur, things are fairly simple, but for anisotropic (X!=Y) blur we conceptually
   * have a cell of X&Y lower/upper blur levels, and fraction of X/Y in there. Triangulate
   * this cell; X!=Y cases will need at most three discrete blur kernels to interpolate between. */
  const KawaseCorner candidates[3] = {
      {{x.lower, y.lower}, 1.0f - math::max(x.fraction, y.fraction)},
      x.fraction >= y.fraction ? KawaseCorner{{x.upper, y.lower}, x.fraction - y.fraction} :
                                 KawaseCorner{{x.lower, y.upper}, y.fraction - x.fraction},
      {{x.upper, y.upper}, math::min(x.fraction, y.fraction)},
  };
  Vector<KawaseCorner> corners;
  for (const KawaseCorner &corner : candidates) {
    if (corner.weight > 0.0f) {
      corners.append(corner);
    }
  }

  /* All corners share the isotropic downsample prefix and upsample suffix; do that first. */
  const int common_level = math::min(x.lower, y.lower);
  Result common = input;
  bool common_is_input = true;
  for (int i = 2; i <= common_level; i *= 2) {
    Result next = kawase_downsample(context, common, full_size, int2(i), float2(1.0f));
    if (!common_is_input) {
      common.release();
    }
    common = next;
    common_is_input = false;
  }

  Vector<Result> corner_results;
  Vector<bool> corner_is_common;
  /* We downsample both axes first, then the remaining axis. A divisor pair is enough to identify
   * the same prefix result in every corner that uses it. */
  Map<int2, Result> downsample_cache;
  for (const KawaseCorner &corner : corners) {
    Result curr = common;
    bool is_common = true;
    const int min_level = math::min(corner.level.x, corner.level.y);
    const int max_level = math::max(corner.level.x, corner.level.y);

    for (int i = math::max(common_level, 1) * 2; i <= max_level; i *= 2) {
      const bool do_x = i <= corner.level.x;
      const bool do_y = i <= corner.level.y;
      const int2 divisor(do_x ? i : min_level, do_y ? i : min_level);
      Result *cached = downsample_cache.lookup_ptr(divisor);
      Result next = cached ? *cached :
                             kawase_downsample(context,
                                               curr,
                                               full_size,
                                               divisor,
                                               float2(do_x ? 1.0f : 0.0f, do_y ? 1.0f : 0.0f));
      if (!cached) {
        downsample_cache.add(divisor, next);
      }
      if (!is_common) {
        curr.release();
      }
      curr = next;
      is_common = false;
    }

    for (int i = max_level / 2; i >= math::max(common_level, 1); i /= 2) {
      const bool do_x = i <= corner.level.x;
      const bool do_y = i <= corner.level.y;
      Result next = kawase_upsample(context,
                                    curr,
                                    full_size,
                                    int2(do_x ? i : min_level, do_y ? i : min_level),
                                    float2(do_x ? 1.0f : 0.0f, do_y ? 1.0f : 0.0f));
      if (!is_common) {
        curr.release();
      }
      curr = next;
      is_common = false;
    }
    corner_results.append(curr);
    corner_is_common.append(is_common);
  }

  /* The last corner owns a writable result whenever a mix is necessary. */
  const int last = corners.size() - 1;
  Result curr = corner_results[last];
  bool curr_is_common = corner_is_common[last];
  if (!curr_is_common) {
    corner_results[last].release();
  }
  float total_weight = corners[last].weight;
  for (int i = last - 1; i >= 0; i--) {
    total_weight += corners[i].weight;
    kawase_mix(context, corner_results[i], curr, corners[i].weight / total_weight);
    if (!corner_is_common[i]) {
      corner_results[i].release();
    }
  }

  for (int i = common_level / 2; i >= 1; i /= 2) {
    Result next = kawase_upsample(context, curr, full_size, int2(i), float2(1.0f));
    if (!curr_is_common) {
      curr.release();
    }
    curr = next;
    curr_is_common = false;
  }

  output.share_data(curr);
  if (!curr_is_common) {
    curr.release();
  }
  if (!common_is_input) {
    common.release();
  }
  for (Result &cached : downsample_cache.values()) {
    cached.release();
  }
}

}  // namespace blender::compositor
