/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <cstdint>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"

#include "GPU_shader.hh"
#include "GPU_state.hh"

#include "COM_algorithm_smol_gaussian_blur.hh"
#include "COM_context.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

/* Smol Gaussian, adapted from Blur Playground: reduce each axis independently with area
 * integration, apply a small separable Gaussian, then reconstruct with bilinear filtering up to 2x
 * enlargement, or a positive cubic B-spline for coarser grids. Radius corresponds to three
 * standard deviations. The working Gaussian compensates for the approximate variance introduced by
 * reduction and reconstruction.
 *
 * Before changing resolution, smoothstep blends approximations of the same target blur on
 * neighboring grids. Triangular interpolation needs at most three endpoints. A canonical tree
 * reduces both axes together first, then the remaining axis, sharing reduction prefixes and
 * keeping odd-sized resampling consistent across transitions. Each endpoint is reconstructed
 * directly on the output grid, and all four channels use the same weights.
 *
 * Gaussian taps have smoothly tapered tails. GPU taps are bilinear-paired (at most 25 reads
 * per axis); CPU convolution reads pixels directly. CPU reconstruction indices and weights are
 * precomputed per axis. Reconstruction is skipped when no resizing or blending is needed.
 * Unreduced axes bypass cubic reconstruction; a zero-radius axis is neither reduced nor blurred.
 * CPU and GPU share the resolution plan, variance compensation, and kernel coefficients.
 *
 * Each reduced axis has a one-pixel border preserving the original edges, rather than
 * extending averaged interior pixels. Later reductions preserve these borders too, integrating
 * along the other axis. Filter the borders with the image, but exclude them from logical sizes,
 * variance estimates, and reconstruction coordinates. This follows Skia's boundary preservation
 * approach and matters particularly when the radius exceeds the image dimensions.
 *
 * Related work:
 * - Intel, "An Investigation of Fast Real-Time GPU-Based Image Blur Algorithms" (2014),
 *   "Working in Lower Resolution": downsample, apply a smaller Gaussian, then upsample.
 *   https://www.intel.com/content/www/us/en/developer/articles/technical/an-investigation-of-fast-real-time-gpu-based-image-blur-algorithms.html
 * - Marius Bjorge, "Bandwidth-Efficient Rendering" (SIGGRAPH 2015), slides 17-18:
 *   filtering at multiple resolutions. "Mixing resolutions" there refers to the filtering
 *   pipeline, rather than crossfading between alternative working resolutions.
 *   https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/siggraph2015_2D00_mmg_2D00_marius_2D00_notes.pdf
 * - Skia's GPU Gaussian blur independently chooses X/Y reduction scales from their sigmas:
 *   Its reduced images also preserve a border for clamped sampling.
 *   https://skia.googlesource.com/skia/+/f970746dd2c98003b16c23d518f2b667c4c1fcfa%5E%21/
 * - GPU Gems 2, Chapter 20, "Fast Third-Order Texture Filtering": cubic B-spline
 *   reconstruction using hardware linear samples, as used by our GPU reconstruction.
 *   https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-20-fast-third-order-texture-filtering
 *
 * These are precedents for individual parts, not a specification of this exact combination.
 * The approximate variance accounting, shared reduction paths, and triangular transition scheme
 * are choices of this implementation. In particular, each blended endpoint approximates the same
 * requested blur width on a different grid; the crossfade hides a resolution change rather than
 * interpolating between images computed for different requested blur widths. */

static void bind_input(gpu::Shader *shader, const Result &input, const char *name)
{
  GPU_texture_filter_mode(input, true);
  GPU_texture_extend_mode(input, GPU_SAMPLER_EXTEND_MODE_EXTEND);
  input.bind_as_texture(shader, name);
}

struct ReducedImage {
  Result image;
  int2 size;
  int2 padding;

  int4 bounds() const
  {
    return int4(size.x, size.y, padding.x, padding.y);
  }
};

static ReducedImage downsample(Context &context,
                               const ReducedImage &source,
                               int2 size,
                               int2 padding)
{
  Result output = context.create_result(ResultType::Color);
  const Result &input = source.image;
  const int2 physical_size = size + 2 * padding;
  output.allocate_texture(physical_size);
  if (context.use_gpu()) {
    gpu::Shader *shader = context.get_shader("compositor_smol_gaussian_downsample");
    GPU_shader_bind(shader);
    GPU_shader_uniform_int_ex(
        shader, GPU_shader_get_uniform(shader, "source_bounds"), 4, 1, source.bounds());
    GPU_shader_uniform_int_ex(shader,
                              GPU_shader_get_uniform(shader, "destination_bounds"),
                              4,
                              1,
                              int4(size.x, size.y, padding.x, padding.y));
    bind_input(shader, input, "input_tx");
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, physical_size);
    input.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }
  else {
    const float2 scale = float2(source.size) / float2(size);
    parallel_for(physical_size, [&](const int2 texel) {
      const int2 pixel = texel - padding;
      float2 lo = float2(pixel) * scale + float2(source.padding);
      float2 hi = math::min(float2(pixel + int2(1)) * scale, float2(source.size)) +
                  float2(source.padding);
      for (int axis = 0; axis < 2; axis++) {
        if (pixel[axis] < 0) {
          lo[axis] = 0.0f;
          hi[axis] = 1.0f;
        }
        else if (pixel[axis] >= size[axis]) {
          lo[axis] = input.domain().data_size[axis] - 1;
          hi[axis] = input.domain().data_size[axis];
        }
      }
      float4 color(0.0f);
      for (int y = int(std::floor(lo.y)); y < int(std::ceil(hi.y)); y++) {
        for (int x = int(std::floor(lo.x)); x < int(std::ceil(hi.x)); x++) {
          const float2 overlap = math::max(
              float2(0.0f), math::min(hi, float2(x + 1, y + 1)) - math::max(lo, float2(x, y)));
          color += float4(input.load_pixel_extended<Color>(int2(x, y))) * overlap.x * overlap.y;
        }
      }
      output.store_pixel(texel, Color(color / ((hi.x - lo.x) * (hi.y - lo.y))));
    });
  }
  return {output, size, padding};
}

static Result gaussian_axis(Context &context, const Result &input, float sigma, int axis)
{
  Result output = context.create_result(ResultType::Color);
  if (sigma < 0.01f || input.domain().data_size[axis] == 1) {
    output.share_data(input);
    return output;
  }

  /* Twelve paired taps cover four sigma while the working sigma stays below six.
   * Once reduction reaches one logical pixel, wider kernels remain truncated to this cap. */
  float4 taps[12] = {};
  float cpu_weights[24] = {};
  /* Clamp before converting to int: radii from connected sockets can exceed the UI range. */
  const int count = int(std::ceil(math::min(12.0f, 2.0f * sigma)));
  const auto weight = [sigma](int i) {
    const float x = i / sigma;
    const float t = math::clamp(4.0f - x, 0.0f, 1.0f);
    return std::exp(-0.5f * x * x) * t * t * (3.0f - 2.0f * t);
  };
  float sum = 1.0f;
  for (int j = 0; j < count; j++) {
    const int i = 2 * j + 1;
    const float a = weight(i);
    const float b = weight(i + 1);
    const float w = a + b;
    cpu_weights[2 * j] = a;
    cpu_weights[2 * j + 1] = b;
    taps[j] = float4(w > 0.0f ? i + b / w : i, w, 0.0f, 0.0f);
    sum += 2.0f * w;
  }
  for (int j = 0; j < count; j++) {
    taps[j].y /= sum;
    cpu_weights[2 * j] /= sum;
    cpu_weights[2 * j + 1] /= sum;
  }
  const float center_weight = 1.0f / sum;
  const int2 size = input.domain().data_size;
  output.allocate_texture(input.domain());
  if (context.use_gpu()) {
    gpu::Shader *shader = context.get_shader("compositor_smol_gaussian_filter");
    GPU_shader_bind(shader);
    float2 step(0.0f);
    step[axis] = 1.0f / size[axis];
    GPU_shader_uniform_2fv(shader, "uv_step", step);
    GPU_shader_uniform_1f(shader, "center_weight", center_weight);
    GPU_shader_uniform_1i(shader, "tap_count", count);
    GPU_shader_uniform_4fv_array(shader, "taps", 12, (const float (*)[4])taps);
    bind_input(shader, input, "input_tx");
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, size);
    input.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }
  else {
    int cpu_count = 2 * count;
    while (cpu_count > 0 && cpu_weights[cpu_count - 1] == 0.0f) {
      cpu_count--;
    }
    parallel_for(size, [&](const int2 texel) {
      float4 color = float4(input.load_pixel<Color>(texel)) * center_weight;
      for (int i = 1; i <= cpu_count; i++) {
        int2 negative = texel;
        int2 positive = texel;
        negative[axis] = math::max(0, texel[axis] - i);
        positive[axis] = math::min(size[axis] - 1, texel[axis] + i);
        color += (float4(input.load_pixel<Color>(negative)) +
                  float4(input.load_pixel<Color>(positive))) *
                 cpu_weights[i - 1];
      }
      output.store_pixel(texel, Color(color));
    });
  }
  return output;
}

struct ReconstructionSample {
  int4 indices;
  float4 weights;
};

/* Resolve reconstruction weights and extended-boundary indices once per output coordinate.
 * Unreduced axes use a single exact pixel read, avoiding both cubic filtering and bilinear
 * round-off. */
static Array<ReconstructionSample> reconstruction_axis(int size, int output_size, int padding)
{
  Array<ReconstructionSample> samples(output_size);
  for (int i = 0; i < output_size; i++) {
    if (size == output_size) {
      samples[i] = {int4(i), float4(1.0f, 0.0f, 0.0f, 0.0f)};
      continue;
    }
    const float p = (float(i) + 0.5f) / output_size * size - 0.5f;
    const int base = int(std::floor(p));
    const float f = p - base;
    if (2 * size >= output_size) {
      samples[i].indices = math::clamp(
          int4(base, base + 1, 0, 0) + int4(padding), int4(0), int4(size + 2 * padding - 1));
      samples[i].weights = float4(1.0f - f, f, 0.0f, 0.0f);
      continue;
    }
    samples[i].indices = math::clamp(int4(base - 1, base, base + 1, base + 2) + int4(padding),
                                     int4(0),
                                     int4(size + 2 * padding - 1));
    samples[i].weights = float4((1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f,
                                (3.0f * f * f * f - 6.0f * f * f + 4.0f) / 6.0f,
                                (-3.0f * f * f * f + 3.0f * f * f + 3.0f * f + 1.0f) / 6.0f,
                                f * f * f / 6.0f);
  }
  return samples;
}

static float4 reconstruct(const Result &input,
                          const ReconstructionSample &x,
                          const ReconstructionSample &y,
                          int2 count)
{
  float4 color(0.0f);
  for (int j = 0; j < count.y; j++) {
    float4 row(0.0f);
    for (int i = 0; i < count.x; i++) {
      row += float4(input.load_pixel<Color>(int2(x.indices[i], y.indices[j]))) * x.weights[i];
    }
    color += row * y.weights[j];
  }
  return color;
}

struct AxisPlan {
  float sigma;
  int level;
  float blend;
};

static int reduced_size(int size, int level)
{
  /* Use a wide divisor to also support the last level of very large images. */
  const int64_t divisor = int64_t(1) << level;
  return int((size + divisor - 1) / divisor);
}

static AxisPlan axis_plan(float radius, int size)
{
  const float sigma = math::max(0.0f, radius) / 3.0f;
  int level = 0;
  while (reduced_size(size, level) > 1 &&
         sigma >= 6.0f * (float(size) / reduced_size(size, level)))
  {
    level++;
  }
  const float t = math::clamp(
      (sigma / (float(size) / reduced_size(size, level)) - 4.5f) / 1.5f, 0.0f, 1.0f);
  return {sigma, level, reduced_size(size, level) > 1 ? t * t * (3.0f - 2.0f * t) : 0.0f};
}

/* Cache entries own their results. Return borrowed copies, including the original input at
 * level zero. Reduce both axes together first, then the remaining axis, to share prefixes. */
static ReducedImage reduced_level(Context &context,
                                  const Result &input,
                                  Map<int2, ReducedImage> &levels,
                                  int2 level)
{
  if (level == int2(0)) {
    return {input, input.domain().data_size, int2(0)};
  }
  if (const ReducedImage *cached = levels.lookup_ptr(level)) {
    return *cached;
  }
  const int2 previous = level.x > level.y ? level - int2(1, 0) :
                        level.y > level.x ? level - int2(0, 1) :
                                            level - int2(1);
  const ReducedImage source = reduced_level(context, input, levels, previous);
  const int2 size = input.domain().data_size;
  ReducedImage result = downsample(
      context,
      source,
      int2(reduced_size(size.x, level.x), reduced_size(size.y, level.y)),
      int2(level.x > 0, level.y > 0));
  levels.add(level, result);
  return result;
}

void smol_gaussian_blur(Context &context, const Result &input, Result &output, float2 radius)
{
  const int2 size = input.domain().data_size;
  const AxisPlan x = axis_plan(radius.x, size.x);
  const AxisPlan y = axis_plan(radius.y, size.y);
  Map<int2, ReducedImage> levels;
  const auto endpoint = [&](int2 level) {
    const ReducedImage source = reduced_level(context, input, levels, level);
    const float2 scale = float2(size) / float2(source.size);
    const float2 variance = (scale * scale - float2(1.0f)) / 12.0f;
    const auto sigma = [&](float target, int axis) {
      const float s = scale[axis];
      /* Bilinear variance is phase-dependent: exact 2x uses phases 1/4 and 3/4;
       * odd dimensions use the phase average. Cubic adds 1/3 reduced pixel squared. */
      const float reconstruction_variance = s == 1.0f ? 0.0f :
                                                        s * s *
                                                            (s > 2.0f  ? 1.0f / 3.0f :
                                                             s == 2.0f ? 3.0f / 16.0f :
                                                                         1.0f / 6.0f);
      return std::sqrt(
                 math::max(0.0f, target * target - variance[axis] - reconstruction_variance)) /
             s;
    };
    Result horizontal = gaussian_axis(context, source.image, sigma(x.sigma, 0), 0);
    Result result = gaussian_axis(context, horizontal, sigma(y.sigma, 1), 1);
    horizontal.release();
    return ReducedImage{result, source.size, source.padding};
  };

  /* Triangulate the level cell: a is the lower corner, b advances the more blended axis,
   * and c advances both axes. Their weights sum to one. */
  const float2 weights(std::abs(x.blend - y.blend), math::min(x.blend, y.blend));
  ReducedImage a = endpoint(int2(x.level, y.level));
  ReducedImage b = {context.create_result(ResultType::Color), a.size, a.padding};
  ReducedImage c = {context.create_result(ResultType::Color), a.size, a.padding};
  if (weights.x > 0.0f) {
    b = endpoint(int2(x.level + (x.blend > y.blend), y.level + (y.blend > x.blend)));
  }
  else {
    b.image.share_data(a.image);
  }
  if (weights.y > 0.0f) {
    c = endpoint(int2(x.level + 1, y.level + 1));
  }
  else {
    c.image.share_data(a.image);
  }
  if (weights == float2(0.0f) && a.size == size) {
    output.share_data(a.image);
  }
  else if (context.use_gpu()) {
    output.allocate_texture(input.domain());
    gpu::Shader *shader = context.get_shader("compositor_smol_gaussian_reconstruct");
    GPU_shader_bind(shader);
    GPU_shader_uniform_2fv(shader, "weights", weights);
    GPU_shader_uniform_int_ex(
        shader, GPU_shader_get_uniform(shader, "base_bounds"), 4, 1, a.bounds());
    GPU_shader_uniform_int_ex(
        shader, GPU_shader_get_uniform(shader, "neighbor_bounds"), 4, 1, b.bounds());
    GPU_shader_uniform_int_ex(
        shader, GPU_shader_get_uniform(shader, "diagonal_bounds"), 4, 1, c.bounds());
    bind_input(shader, a.image, "input_tx");
    bind_input(shader, b.image, "neighbor_tx");
    bind_input(shader, c.image, "diagonal_tx");
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, size);
    a.image.unbind_as_texture();
    b.image.unbind_as_texture();
    c.image.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }
  else {
    output.allocate_texture(input.domain());
    /* Neighboring endpoints often share an axis size, so share its reconstruction table too. */
    Map<int, Array<ReconstructionSample>> x_tables, y_tables;
    const auto prepare = [&](const ReducedImage &result) {
      const int2 endpoint_size = result.size;
      if (!x_tables.contains(endpoint_size.x)) {
        x_tables.add(endpoint_size.x,
                     reconstruction_axis(endpoint_size.x, size.x, result.padding.x));
      }
      if (!y_tables.contains(endpoint_size.y)) {
        y_tables.add(endpoint_size.y,
                     reconstruction_axis(endpoint_size.y, size.y, result.padding.y));
      }
    };
    prepare(a);
    if (weights.x > 0.0f) {
      prepare(b);
    }
    if (weights.y > 0.0f) {
      prepare(c);
    }
    const auto sampler = [&](const ReducedImage &result) {
      const int2 endpoint_size = result.size;
      const int2 count(endpoint_size.x == size.x     ? 1 :
                       2 * endpoint_size.x >= size.x ? 2 :
                                                       4,
                       endpoint_size.y == size.y     ? 1 :
                       2 * endpoint_size.y >= size.y ? 2 :
                                                       4);
      const Span<ReconstructionSample> xs = x_tables.lookup(endpoint_size.x);
      const Span<ReconstructionSample> ys = y_tables.lookup(endpoint_size.y);
      return [&result, xs, ys, count](int2 texel) {
        return reconstruct(result.image, xs[texel.x], ys[texel.y], count);
      };
    };
    const auto sample_a = sampler(a);
    const auto sample_b = sampler(weights.x > 0.0f ? b : a);
    const auto sample_c = sampler(weights.y > 0.0f ? c : a);
    parallel_for(size, [&](const int2 texel) {
      float4 color = sample_a(texel) * (1.0f - weights.x - weights.y);
      if (weights.x > 0.0f) {
        color += sample_b(texel) * weights.x;
      }
      if (weights.y > 0.0f) {
        color += sample_c(texel) * weights.y;
      }
      output.store_pixel(texel, Color(color));
    });
  }
  a.image.release();
  b.image.release();
  c.image.release();
  for (ReducedImage &cached : levels.values()) {
    cached.image.release();
  }
}

}  // namespace blender::compositor
