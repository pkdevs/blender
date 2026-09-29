/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"

#include "GPU_shader.hh"
#include "GPU_state.hh"

#include "COM_algorithm_downsampled_gaussian_blur.hh"
#include "COM_algorithm_pad.hh"
#include "COM_algorithm_symmetric_separable_blur.hh"
#include "COM_context.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

/* "Downsampled" Gaussian blur: downsample to a working resolution (which could be different
 * per axis), apply a small separable Gaussian there, reconstruct to full resolution image.
 *
 * The algorithm is similar to Skia's GPU Gaussian blur as of 2026 Sep (FilterResult::Builder::blur
 * and FilterResult::rescale),
 * https://skia.googlesource.com/skia/+/15a9437eec87/src/core/SkImageFilterTypes.cpp
 * independent X/Y scaling, texture samples placed to use bilinear filtering, one pixel border on
 * downsampled images that preserve the original image edges (this way bright interiors do not
 * overbright the result at large radius). Additions compared to Skia blur are:
 *
 * - When downsampling odd-sized image, we do more correct pixel area integration, so that isolated
 *   bright pixels do not flicker with sampling phase shift.
 * - Downsampled levels are ceil-halved sizes, and downsampling kicks in at sigma=6 (so in
 *   practice, blur sizes 18, 36, 72, 144, ... switch to new level). Skia instead scales
 *   continuously, to keep working sigma under 4.
 * - When doing the final Gaussian blur, we take into account the blur introduced by downsampling
 *   and later reconstruction, i.e. subtract their variance from the blur kernel.
 * - The final Gaussian kernel extends to sigma=4 (i.e. not truncated at sigma=3), and weights in
 *   sigma 3..4 region are tapered to reach zero. This helps to reduce the "blur is cut off" with
 *   very bright highlights, and looks better when radius is animated and number of taps changes.
 * - Final reconstruction to full image size uses a cubic B-spline (fully positive kernel, so no
 *   ringing) instead of bilinear, beyond 2x enlargement. This helps to avoid slope discontinuities
 *   of bilinear.
 * - Pairs of exact 2x reductions are done as single 4x reduction as an optimization.
 *
 * Other related work:
 * - Fabian Giesen, "Gaussian blur kernels" on gdalgorithms-list (2009): low-pass filter before
 *   decimating (otherwise thin objects flicker), blur at lower resolution, then upsample. He
 *   suggests bilinear upsampling is good enough; we use cubic beyond 2x enlargement:
 *   https://sourceforge.net/p/gdalgorithms/mailman/message/23077758/
 * - Intel, "An Investigation of Fast Real-Time GPU-Based Image Blur Algorithms" (2014) has
 *   "Working in Lower Resolution" section:
 *   https://www.intel.com/content/www/us/en/developer/articles/technical/an-investigation-of-fast-real-time-gpu-based-image-blur-algorithms.html
 * - GPU Gems 2, Chapter 20, "Fast Third-Order Texture Filtering": cubic B-spline filtering using
 *   bilinear samples:
 *   https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-20-fast-third-order-texture-filtering
 */

static constexpr float direct_blur_radius_sum = 40.0f;
static constexpr float max_working_sigma = 6.0f;
/* Bilinear-paired taps on each side of the center; their 24 pixels cover four sigma at
 * max_working_sigma. Must match the size of the taps array in the filter shader. */
static constexpr int max_tap_pairs = 12;

static void bind_as_bilinear_texture(gpu::Shader *shader, const Result &input, const char *name)
{
  GPU_texture_filter_mode(input, true);
  GPU_texture_extend_mode(input, GPU_SAMPLER_EXTEND_MODE_EXTEND);
  input.bind_as_texture(shader, name);
}

struct DownsampledImage {
  Result image; /* Image size itself: `size + 2 * padding`. */
  int2 size;    /* Logical size. */
  int2 padding; /* Potential one pixel border. */
};

static DownsampledImage downsample(Context &context,
                                   const DownsampledImage &input,
                                   const int2 size,
                                   const int2 padding,
                                   const bool use_4x)
{
  DownsampledImage output = {context.create_result(ResultType::Color), size, padding};
  const int2 physical_size = size + 2 * padding;
  output.image.allocate_texture(physical_size);
  /* GPU downsample. */
  if (context.use_gpu()) {
    gpu::Shader *shader = context.get_shader(use_4x ?
                                                 "compositor_downsampled_gaussian_downsample_4x" :
                                                 "compositor_downsampled_gaussian_downsample");
    GPU_shader_bind(shader);
    GPU_shader_uniform_int_ex(shader,
                              GPU_shader_get_uniform(shader, "source_bounds"),
                              4,
                              1,
                              int4(input.size, input.padding));
    GPU_shader_uniform_int_ex(
        shader, GPU_shader_get_uniform(shader, "destination_bounds"), 4, 1, int4(size, padding));
    bind_as_bilinear_texture(shader, input.image, "input_tx");
    output.image.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, physical_size);
    input.image.unbind_as_texture();
    output.image.unbind_as_image();
    GPU_shader_unbind();
    return output;
  }

  /* CPU downsample: same logic as GPU shader, but don't use paired bilinear reads since they
   * are not "free" on the CPU. No need for special 4x downsample code here either. */
  const int2 input_physical_size = input.image.domain().data_size;
  const float2 scale = float2(input.size) / float2(size);
  parallel_for(physical_size, [&](const int2 texel) {
    const int2 pixel = texel - padding;
    float2 lo = float2(pixel) * scale + float2(input.padding);
    float2 hi = math::min(float2(pixel + int2(1)) * scale, float2(input.size)) +
                float2(input.padding);
    /* Preserve the outermost input edge in the border. */
    for (int axis = 0; axis < 2; axis++) {
      if (pixel[axis] < 0) {
        lo[axis] = 0.0f;
        hi[axis] = 1.0f;
      }
      else if (pixel[axis] >= size[axis]) {
        lo[axis] = input_physical_size[axis] - 1;
        hi[axis] = input_physical_size[axis];
      }
    }
    float4 color(0.0f);
    for (int y = int(std::floor(lo.y)); y < int(std::ceil(hi.y)); y++) {
      for (int x = int(std::floor(lo.x)); x < int(std::ceil(hi.x)); x++) {
        const float2 overlap = math::max(
            float2(0.0f), math::min(hi, float2(x + 1, y + 1)) - math::max(lo, float2(x, y)));
        color += float4(input.image.load_pixel<Color>(int2(x, y))) * overlap.x * overlap.y;
      }
    }
    output.image.store_pixel(texel, Color(color / ((hi.x - lo.x) * (hi.y - lo.y))));
  });
  return output;
}

static Result gaussian_axis(Context &context,
                            const Result &input,
                            const float sigma,
                            const int axis)
{
  Result output = context.create_result(ResultType::Color);
  if (sigma < 0.01f || input.domain().data_size[axis] == 1) {
    output.share_data(input);
    return output;
  }

  /* Downsampling stops at one logical pixel, so at radius above 18x the image size the
   * working sigma can exceed max_working_sigma, and the kernel is truncated to max_tap_pairs. */
  float4 taps[max_tap_pairs] = {};
  float weights[2 * max_tap_pairs] = {};
  /* Note: clamp before integer conversion; a radius coming from a socket can exceed the UI range.
   */
  const int tap_count = int(std::ceil(math::min(float(max_tap_pairs), 2.0f * sigma)));
  /* Gaussian kernel, with a smoothstep taper towards zero over the last sigma. */
  const auto weight = [sigma](const int i) {
    const float x = i / sigma;
    const float t = math::clamp(4.0f - x, 0.0f, 1.0f);
    return std::exp(-0.5f * x * x) * t * t * (3.0f - 2.0f * t);
  };
  float sum = 1.0f;
  for (int j = 0; j < tap_count; j++) {
    const int i = 2 * j + 1;
    const float a = weight(i);
    const float b = weight(i + 1);
    weights[2 * j] = a;
    weights[2 * j + 1] = b;
    /* Calculate bilinear sample placement: one sample at `i + b / (a + b)`, weighted by `a + b`.
     * However a and b both can be zero near the tapered kernel tail. */
    const float w = a + b;
    taps[j] = float4(w > 0.0f ? i + b / w : i, w, 0.0f, 0.0f);
    sum += 2.0f * w;
  }
  for (float4 &tap : taps) {
    tap.y /= sum;
  }
  for (float &w : weights) {
    w /= sum;
  }
  const float center_weight = 1.0f / sum;
  const int2 size = input.domain().data_size;
  output.allocate_texture(input.domain());

  /* GPU blur. */
  if (context.use_gpu()) {
    gpu::Shader *shader = context.get_shader("compositor_downsampled_gaussian_filter");
    GPU_shader_bind(shader);
    float2 uv_step(0.0f);
    uv_step[axis] = 1.0f / size[axis];
    GPU_shader_uniform_2fv(shader, "uv_step", uv_step);
    GPU_shader_uniform_1f(shader, "center_weight", center_weight);
    GPU_shader_uniform_1i(shader, "tap_count", tap_count);
    GPU_shader_uniform_4fv_array(
        shader, "taps", max_tap_pairs, reinterpret_cast<const float (*)[4]>(taps));
    bind_as_bilinear_texture(shader, input, "input_tx");
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, size);
    input.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
    return output;
  }

  /* CPU blur: same logic as GPU, except we don't use bilinear paired taps. */
  int weight_count = 2 * tap_count;
  while (weight_count > 0 && weights[weight_count - 1] == 0.0f) {
    weight_count--;
  }
  parallel_for(size, [&](const int2 texel) {
    float4 color = float4(input.load_pixel<Color>(texel)) * center_weight;
    for (int i = 1; i <= weight_count; i++) {
      int2 negative = texel;
      int2 positive = texel;
      negative[axis] = math::max(0, texel[axis] - i);
      positive[axis] = math::min(size[axis] - 1, texel[axis] + i);
      color += (float4(input.load_pixel<Color>(negative)) +
                float4(input.load_pixel<Color>(positive))) *
               weights[i - 1];
    }
    output.store_pixel(texel, Color(color));
  });
  return output;
}

/* Samples per output pixel to reconstruct an axis: one exact read for axes that were not
 * downsampled, two for bilinear up to 2x enlargement, and four for a cubic B-spline beyond that.
 * Must match the filter selection in the reconstruct shader. */
static int reconstruction_sample_count(const int size, const int output_size)
{
  if (size == output_size) {
    return 1;
  }
  return 2 * size >= output_size ? 2 : 4;
}

/* Reconstruction variance in output pixels squared:
 *
 * Bilinear variance at phase f is `f * (1 - f)` input pixels squared: exact 2x only uses phases
 * 1/4 and 3/4 (3/16), while other scales up to 2x use the phase average (1/6).
 *
 * Cubic B-spline variance is 1/3. */
static float reconstruction_variance(const int size, const int output_size)
{
  const float scale = float(output_size) / size;
  switch (reconstruction_sample_count(size, output_size)) {
    case 1:
      return 0.0f;
    case 2:
      return scale * scale * (output_size == 2 * size ? 3.0f / 16.0f : 1.0f / 6.0f);
    default:
      return scale * scale / 3.0f;
  }
}

/* Sigma in working pixels that approximates the target sigma after downsampling from output_size
 * to size and reconstructing back. Variance adds under convolution, so in output pixels:
 *
 *   sigma^2 ~= downsampling_variance + (scale * working_sigma)^2 + reconstruction_variance
 *
 * Downsampling by scale acts like a box of scale pixels, with variance (scale^2 - 1) / 12. This
 * is an approximation (odd sizes are not exact boxes). */
static float working_sigma(const float sigma, const int size, const int output_size)
{
  const float scale = float(output_size) / size;
  const float downsampling_variance = (scale * scale - 1.0f) / 12.0f;
  const float variance = sigma * sigma - downsampling_variance -
                         reconstruction_variance(size, output_size);
  return std::sqrt(math::max(0.0f, variance)) / scale;
}

struct ReconstructionSample {
  int4 indices;
  float4 weights;
};

/* Calculates reconstruction weights and extended-boundary indices once per output coordinate. */
static Array<ReconstructionSample> reconstruction_axis(const int size,
                                                       const int output_size,
                                                       const int padding)
{
  const int sample_count = reconstruction_sample_count(size, output_size);
  const int4 max_index(size + 2 * padding - 1);
  Array<ReconstructionSample> samples(output_size);
  for (int i = 0; i < output_size; i++) {
    if (sample_count == 1) {
      samples[i] = {int4(i), float4(1.0f, 0.0f, 0.0f, 0.0f)};
      continue;
    }
    const float p = (float(i) + 0.5f) / output_size * size - 0.5f;
    const int base = int(std::floor(p));
    const float f = p - base;
    if (sample_count == 2) {
      samples[i].indices = math::clamp(int4(base, base + 1, 0, 0) + padding, int4(0), max_index);
      samples[i].weights = float4(1.0f - f, f, 0.0f, 0.0f);
      continue;
    }
    samples[i].indices = math::clamp(
        int4(base - 1, base, base + 1, base + 2) + padding, int4(0), max_index);
    samples[i].weights = float4((1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f,
                                (3.0f * f * f * f - 6.0f * f * f + 4.0f) / 6.0f,
                                (-3.0f * f * f * f + 3.0f * f * f + 3.0f * f + 1.0f) / 6.0f,
                                f * f * f / 6.0f);
  }
  return samples;
}

static int downsampled_size(const int size, const int level)
{
  const int divisor = 1 << level;
  return (size + divisor - 1) / divisor;
}

/* Find the finest level where the working sigma is below max_working_sigma. Note that levels use
 * ceil sizes for odd dimensions, and e.g. 5 -> 3 is a 3/5 scale, not 1/2. */
static int working_level(const float sigma, const int size)
{
  int level = 0;
  while (downsampled_size(size, level) > 1 &&
         sigma >= max_working_sigma * (float(size) / downsampled_size(size, level)))
  {
    level++;
  }
  return level;
}

/* Downsample along both axes until one reaches its target level, then the other axis alone.
 * When two reduction steps are exact halves on each downsampled axis, do a fused 4x reduction as
 * an optimization. */
static DownsampledImage downsample_to_level(Context &context,
                                            const Result &input,
                                            const int2 target_level)
{
  const int2 original_size = input.domain().data_size;
  const auto size_at_level = [&](const int2 level) {
    return int2(downsampled_size(original_size.x, level.x),
                downsampled_size(original_size.y, level.y));
  };
  DownsampledImage current = {context.create_result(ResultType::Color), original_size, int2(0)};
  current.image.share_data(input);
  int2 level(0);
  while (level != target_level) {
    int2 next = math::min(level + int2(1), target_level);
    const int2 candidate = math::min(next + int2(1), target_level);
    const int2 delta = candidate - level;
    const bool use_4x = candidate != next && (delta.x == 0 || delta.x == 2) &&
                        (delta.y == 0 || delta.y == 2) &&
                        current.size ==
                            size_at_level(candidate) * int2(1 << delta.x, 1 << delta.y);
    if (use_4x) {
      next = candidate;
    }
    /* Axes that have been downsampled get one pixel of padding. */
    DownsampledImage downsampled = downsample(
        context, current, size_at_level(next), math::min(next, int2(1)), use_4x);
    current.image.release();
    current = downsampled;
    level = next;
  }
  return current;
}

static void do_blur(Context &context, const Result &input, Result &output, const float2 &radius)
{
  const int2 size = input.domain().data_size;
  const float2 sigma = math::max(radius, float2(0.0f)) / 3.0f;
  DownsampledImage working = downsample_to_level(
      context, input, int2(working_level(sigma.x, size.x), working_level(sigma.y, size.y)));
  Result horizontal = gaussian_axis(
      context, working.image, working_sigma(sigma.x, working.size.x, size.x), 0);
  Result filtered = gaussian_axis(
      context, horizontal, working_sigma(sigma.y, working.size.y, size.y), 1);
  horizontal.release();
  working.image.release();
  if (working.size == size) {
    output.share_data(filtered);
  }
  else if (context.use_gpu()) {
    /* GPU upsample reconstruction. */
    output.allocate_texture(input.domain());
    gpu::Shader *shader = context.get_shader("compositor_downsampled_gaussian_reconstruct");
    GPU_shader_bind(shader);
    GPU_shader_uniform_int_ex(shader,
                              GPU_shader_get_uniform(shader, "input_bounds"),
                              4,
                              1,
                              int4(working.size, working.padding));
    bind_as_bilinear_texture(shader, filtered, "input_tx");
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, size);
    filtered.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }
  else {
    /* CPU upsample reconstruction. */
    output.allocate_texture(input.domain());
    const Array<ReconstructionSample> samples_x = reconstruction_axis(
        working.size.x, size.x, working.padding.x);
    const Array<ReconstructionSample> samples_y = reconstruction_axis(
        working.size.y, size.y, working.padding.y);
    const int2 samples_per_axis(reconstruction_sample_count(working.size.x, size.x),
                                reconstruction_sample_count(working.size.y, size.y));
    parallel_for(size, [&](const int2 texel) {
      const ReconstructionSample &sample_x = samples_x[texel.x];
      const ReconstructionSample &sample_y = samples_y[texel.y];
      float4 color(0.0f);
      for (int j = 0; j < samples_per_axis.y; j++) {
        float4 row(0.0f);
        for (int i = 0; i < samples_per_axis.x; i++) {
          row += float4(
                     filtered.load_pixel<Color>(int2(sample_x.indices[i], sample_y.indices[j]))) *
                 sample_x.weights[i];
        }
        color += row * sample_y.weights[j];
      }
      output.store_pixel(texel, Color(color));
    });
  }
  filtered.release();
}

void downsampled_gaussian_blur(Context &context,
                               const Result &input,
                               Result &output,
                               const float2 &radius,
                               const bool extend_bounds)
{
  /* Direct blur is faster for small blur radii. */
  if (radius.x + radius.y < direct_blur_radius_sum) {
    symmetric_separable_blur(
        context, input, output, radius, math::FilterKernel::Gauss, extend_bounds);
    return;
  }

  if (!extend_bounds) {
    do_blur(context, input, output, radius);
    return;
  }
  Result padded_input = context.create_result(input.type());
  pad(context, input, padded_input, int2(math::ceil(radius)), PaddingMethod::Zero);
  do_blur(context, padded_input, output, radius);
  padded_input.release();
}

}  // namespace blender::compositor
