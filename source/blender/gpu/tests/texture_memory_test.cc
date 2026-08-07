/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/** \file
 * \ingroup gpu
 *
 * Measures how much GPU memory a mipmapped image texture costs for each of the texture usage
 * flag combinations that image textures could be created with.
 *
 * Image textures need #GPU_TEXTURE_USAGE_SHADER_WRITE for the compute shader mipmap generation
 * path, and #GPU_TEXTURE_USAGE_FORMAT_VIEW on top of that for sRGB textures on backends that
 * can't write sRGB directly. Both flags are suspected to disable driver internal (lossless)
 * texture compression. The alternative is the backend native mipmap generation, which needs
 * neither flag.
 *
 * This is a measurement and not a pass/fail test, it prints a table for the GPU it runs on.
 */

#include "gpu_testing.hh"

#include <algorithm>
#include <cstdio>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_time.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_platform.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"

#include "gpu_texture_private.hh"

namespace blender::gpu::tests {

/* Base level size of the measured textures. Large enough that a driver picking a different
 * internal layout shows up above the noise, small enough to fit on any CI GPU. */
static constexpr int texture_size = 1024;
/* Approximate total amount of memory allocated per measurement. Device memory statistics are
 * noisy and drivers suballocate from big blocks, so allocate many textures at once and divide
 * by their number afterwards. */
static constexpr size_t bytes_per_measurement = 256 * 1024 * 1024;
/* Minimum number of textures per measurement, for formats where the above would allocate
 * only a handful. */
static constexpr int64_t min_textures_per_measurement = 8;
/* Number of texels each invocation of the bandwidth shader reads. */
static constexpr int bandwidth_sample_count = 32;
/* Keep doubling the number of bandwidth passes until they take this long, so the measurement
 * works both on a fast GPU and on a software rasterizer. */
static constexpr double bandwidth_target_seconds = 0.15;
static constexpr int bandwidth_max_passes = 512;
/* Number of timed runs to do, of which the fastest is kept. */
static constexpr int bandwidth_repeats = 2;
/* Number of times the whole list of cases is measured. Cases are measured round-robin rather
 * than one case at a time, so that a machine that gets slower or faster during the run affects
 * every case equally instead of penalizing whichever case ran last. */
static constexpr int measurement_rounds = 3;

enum class MipmapPath {
  /* Single mip level, for reference only. */
  None,
  /* Full mip chain, but never regenerated. */
  NoRegenerate,
  /* Backend native mipmap generation, `glGenerateMipmap` and friends. */
  Native,
  /* Compute shader mipmap generation, needs #GPU_TEXTURE_USAGE_SHADER_WRITE. */
  Compute,
};

struct MemoryCase {
  const char *name;
  eGPUTextureUsage usage;
  MipmapPath mipmap_path;
  /* Create a texture view with a different format, as done for sRGB textures that can't be
   * written to directly. */
  bool create_format_view;
  /* All other cases are reported relative to this one. */
  bool is_baseline;
};

/* Usage flags that every image texture gets today, see #IMB_create_gpu_texture. */
static constexpr eGPUTextureUsage image_usage = GPU_TEXTURE_USAGE_SHADER_READ |
                                                GPU_TEXTURE_USAGE_HOST_READ;
/* Sampled only, so a driver is free to apply its internal color compression. Not reachable
 * through the regular usage flags, see #GPU_TEXTURE_USAGE_DEBUG_NO_STORAGE. */
static constexpr eGPUTextureUsage sampled_only_usage = image_usage |
                                                       GPU_TEXTURE_USAGE_DEBUG_NO_STORAGE;

static const MemoryCase memory_cases[] = {
    {"no mipmaps, sampled only", sampled_only_usage, MipmapPath::None, false, false},
    /* The reference point: a mipmapped texture that nothing but sampling can touch. */
    {"mipmaps, sampled only", sampled_only_usage, MipmapPath::NoRegenerate, false, true},
    /* What an image texture gets today. On Vulkan this already implies storage image usage,
     * which is why the case above needs a flag of its own. */
    {"mipmaps, no regenerate", image_usage, MipmapPath::NoRegenerate, false, false},
    {"mipmaps, native", image_usage, MipmapPath::Native, false, false},
    /* Separates the cost of the flag itself from the cost of actually writing to the texture,
     * which is what the #BKE_image_paint_ensure_gpu_writable deferral is about. */
    {"mipmaps, write flag only",
     image_usage | GPU_TEXTURE_USAGE_SHADER_WRITE,
     MipmapPath::NoRegenerate,
     false,
     false},
    {"mipmaps, compute",
     image_usage | GPU_TEXTURE_USAGE_SHADER_WRITE,
     MipmapPath::Compute,
     false,
     false},
    {"mipmaps, compute + view",
     image_usage | GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_FORMAT_VIEW,
     MipmapPath::Compute,
     true,
     false},
    {"mipmaps, format view only",
     image_usage | GPU_TEXTURE_USAGE_FORMAT_VIEW,
     MipmapPath::NoRegenerate,
     true,
     false},
    {"mipmaps, attachment",
     image_usage | GPU_TEXTURE_USAGE_ATTACHMENT,
     MipmapPath::NoRegenerate,
     false,
     false},
    {"mipmaps, general", GPU_TEXTURE_USAGE_GENERAL, MipmapPath::Compute, true, false},
};

/* Formats used by image textures, see `imb_gpu_get_format`. */
struct MeasuredFormat {
  const char *name;
  TextureFormat format;
  /* Format to create the texture view with, matching what mipmap generation uses. */
  TextureFormat view_format;
};

static const MeasuredFormat measured_formats[] = {
    {"RGBA8", TextureFormat::UNORM_8_8_8_8, TextureFormat::UNORM_8_8_8_8},
    {"sRGBA8", TextureFormat::SRGBA_8_8_8_8, TextureFormat::UNORM_8_8_8_8},
    {"RGBA16F", TextureFormat::SFLOAT_16_16_16_16, TextureFormat::SFLOAT_16_16_16_16},
    {"R8", TextureFormat::UNORM_8, TextureFormat::UNORM_8},
};

struct Measurement {
  /* Size of all mip levels, assuming no padding and no compression. */
  size_t naive_size = 0;
  /* Size the backend reports for a single texture, 0 if unsupported. */
  size_t allocation_size = 0;
  /* Device memory used per texture, derived from the free memory before and after allocating
   * the whole batch. */
  int64_t device_size = 0;
  bool device_size_valid = false;
  /* Number of textures that were allocated for this measurement. */
  int64_t texture_count = 0;
  /* Rate at which the texture can be sampled, in GB/s of uncompressed texel data. Driver
   * internal compression is a bandwidth optimization, so it shows up here even on hardware
   * where it costs no extra memory. */
  double read_gb_per_second = 0.0;
};

static int mip_len_get(const int size)
{
  int mip_len = 1;
  while ((size >> mip_len) > 0) {
    mip_len++;
  }
  return mip_len;
}

static size_t naive_size_get(const TextureFormat format, const int size, const int mip_len)
{
  size_t total = 0;
  for (const int mip : IndexRange(mip_len)) {
    const size_t mip_size = std::max(size >> mip, 1);
    total += mip_size * mip_size * to_bytesize(format);
  }
  return total;
}

/* Free device memory in bytes, or -1 when the backend can't report it. */
static int64_t device_memory_free_get()
{
  if (!GPU_mem_stats_supported()) {
    return -1;
  }
  int total_kb = 0, free_kb = 0;
  GPU_mem_stats_get(&total_kb, &free_kb);
  return int64_t(free_kb) * 1024;
}

/* Textures freed by a previous measurement are not released immediately, they go through a
 * discard pool that only frees once the GPU is done with them. Wait until the reported free
 * memory stops moving, so those releases don't land inside the next measured window and make
 * it look like allocating used less memory than it did. */
static int64_t device_memory_free_settled()
{
  int64_t previous = device_memory_free_get();
  if (previous < 0) {
    return previous;
  }
  for ([[maybe_unused]] const int attempt : IndexRange(16)) {
    GPU_finish();
    const int64_t current = device_memory_free_get();
    if (current == previous) {
      return current;
    }
    previous = current;
  }
  return previous;
}

static void regenerate_mipmaps(gpu::Texture *texture, const MipmapPath mipmap_path)
{
  switch (mipmap_path) {
    case MipmapPath::None:
    case MipmapPath::NoRegenerate:
      break;
    case MipmapPath::Native:
      /* Bypass #GPU_texture_update_mipmap_chain, which would use the compute path when the
       * texture has #GPU_TEXTURE_USAGE_SHADER_WRITE. */
      texture->generate_mipmap();
      break;
    case MipmapPath::Compute:
      GPU_texture_update_mipmap_chain(texture);
      break;
  }
}

/* Smooth gradient, the kind of content driver internal compression is good at. Random noise
 * would be incompressible on any hardware and hide the effect being measured. */
static Array<float4> gradient_data_get(const int size)
{
  Array<float4> data(int64_t(size) * size);
  for (const int64_t y : IndexRange(size)) {
    for (const int64_t x : IndexRange(size)) {
      const float u = float(x) / float(size - 1);
      const float v = float(y) / float(size - 1);
      data[y * size + x] = float4(u, v, u * v, 1.0f);
    }
  }
  return data;
}

/* Sampling throughput in GB/s of uncompressed texel data. Higher than the raw memory bandwidth
 * of the device means the driver is compressing. */
static double measure_bandwidth(gpu::Shader *shader,
                                gpu::Texture *texture,
                                const TextureFormat format,
                                const int size)
{
  gpu::StorageBuf *ssbo = GPU_storagebuf_create_ex(
      sizeof(float) * size_t(size) * size, nullptr, GPU_USAGE_DEVICE_ONLY, __func__);

  GPU_shader_bind(shader);
  GPU_texture_bind(texture, GPU_shader_get_sampler_binding(shader, "source_tx"));
  GPU_storagebuf_bind(ssbo, GPU_shader_get_ssbo_binding(shader, "data_out"));
  GPU_shader_uniform_1i(shader, "sample_count", bandwidth_sample_count);

  const int groups = size / 16;

  /* Warm up, so first use of the shader and texture is not part of the measurement. */
  GPU_compute_dispatch(shader, groups, groups, 1);
  GPU_finish();

  /* Calibrate the pass count so a run is long enough to be meaningful. A software rasterizer
   * needs a single pass, a fast GPU needs many. */
  int passes = 1;
  double seconds = 0.0;
  while (true) {
    const double start = BLI_time_now_seconds();
    for ([[maybe_unused]] const int pass : IndexRange(passes)) {
      GPU_compute_dispatch(shader, groups, groups, 1);
    }
    GPU_finish();
    seconds = BLI_time_now_seconds() - start;
    if (seconds >= bandwidth_target_seconds || passes >= bandwidth_max_passes) {
      break;
    }
    passes *= 2;
  }

  /* Keep the fastest run. Anything slower than the best is contention with the rest of the
   * system, not a property of the texture. */
  for ([[maybe_unused]] const int repeat : IndexRange(bandwidth_repeats - 1)) {
    const double start = BLI_time_now_seconds();
    for ([[maybe_unused]] const int pass : IndexRange(passes)) {
      GPU_compute_dispatch(shader, groups, groups, 1);
    }
    GPU_finish();
    seconds = std::min(seconds, BLI_time_now_seconds() - start);
  }

  GPU_shader_unbind();
  GPU_texture_unbind(texture);
  GPU_storagebuf_free(ssbo);

  if (seconds <= 0.0) {
    return 0.0;
  }
  const double bytes = double(size) * size * bandwidth_sample_count * to_bytesize(format) * passes;
  return bytes / seconds / 1.0e9;
}

static Measurement measure_case(const MeasuredFormat &measured_format,
                                const MemoryCase &memory_case,
                                gpu::Shader *bandwidth_shader)
{
  const int size = texture_size;
  const int mip_len = (memory_case.mipmap_path == MipmapPath::None) ? 1 : mip_len_get(size);
  const size_t naive_size = naive_size_get(measured_format.format, size, mip_len);
  const int64_t texture_count = std::max<int64_t>(min_textures_per_measurement,
                                                  bytes_per_measurement / naive_size);

  Measurement measurement;
  measurement.naive_size = naive_size;

  GPU_render_begin();

  const int64_t free_before = device_memory_free_settled();

  Vector<gpu::Texture *> textures;
  Vector<gpu::Texture *> views;
  for ([[maybe_unused]] const int64_t i : IndexRange(texture_count)) {
    gpu::Texture *texture = GPU_texture_create_2d(
        "texture_memory", size, size, mip_len, measured_format.format, memory_case.usage, nullptr);
    if (texture == nullptr) {
      break;
    }
    textures.append(texture);

    /* Upload a single pixel. Metal creates its textures lazily on first use, so without this
     * the memory would not be allocated yet. */
    const float pixel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    GPU_texture_update_sub(texture, GPU_DATA_FLOAT, pixel, 0, 0, 0, 1, 1, 1);

    if (memory_case.create_format_view) {
      views.append(GPU_texture_create_view("texture_memory_view",
                                           texture,
                                           measured_format.view_format,
                                           0,
                                           mip_len,
                                           0,
                                           1,
                                           false,
                                           false));
    }

    regenerate_mipmaps(texture, memory_case.mipmap_path);
  }

  GPU_finish();
  const int64_t free_after = device_memory_free_get();

  measurement.texture_count = textures.size();
  if (!textures.is_empty()) {
    measurement.allocation_size = GPU_texture_allocation_size(textures.first());
  }
  if (free_before > 0 && free_after > 0 && free_after < free_before) {
    measurement.device_size = (free_before - free_after) / textures.size();
    measurement.device_size_valid = true;
  }

  for (gpu::Texture *view : views) {
    if (view) {
      GPU_texture_free(view);
    }
  }
  for (gpu::Texture *texture : textures) {
    GPU_texture_free(texture);
  }

  /* Measure sampling throughput on a single texture, filled with real content. This is the
   * measurement that detects compression on hardware where it costs no extra memory. */
  if (bandwidth_shader != nullptr) {
    gpu::Texture *texture = GPU_texture_create_2d(
        "texture_bandwidth", size, size, mip_len, measured_format.format, memory_case.usage, nullptr);
    if (texture != nullptr) {
      const Array<float4> data = gradient_data_get(size);
      GPU_texture_update(texture, GPU_DATA_FLOAT, data.data());
      regenerate_mipmaps(texture, memory_case.mipmap_path);
      measurement.read_gb_per_second = measure_bandwidth(
          bandwidth_shader, texture, measured_format.format, size);
      GPU_texture_free(texture);
    }
  }

  /* Make sure the memory is actually released before the next measurement starts. */
  GPU_finish();
  GPU_render_end();

  return measurement;
}

static void print_size(const int64_t bytes)
{
  if (bytes <= 0) {
    printf("%12s", "-");
  }
  else {
    printf("%9.2f MB", double(bytes) / (1024.0 * 1024.0));
  }
}

static void print_ratio(const int64_t bytes, const int64_t base_bytes)
{
  if (bytes <= 0 || base_bytes <= 0) {
    printf("%8s", "-");
  }
  else {
    printf("%8.2f", double(bytes) / double(base_bytes));
  }
}

static void print_rate(const double gb_per_second)
{
  if (gb_per_second <= 0.0) {
    printf("%12s", "-");
  }
  else {
    printf("%9.1f GB/s", gb_per_second);
  }
}

static void print_rate_ratio(const double gb_per_second, const double base_gb_per_second)
{
  if (gb_per_second <= 0.0 || base_gb_per_second <= 0.0) {
    printf("%8s", "-");
  }
  else {
    printf("%8.2f", gb_per_second / base_gb_per_second);
  }
}

static void test_texture_memory_usage()
{
  printf("\n");
  printf("GPU: %s\n", GPU_platform_gpu_name());
  printf("Vendor: %s, Renderer: %s, Version: %s\n",
         GPU_platform_vendor(),
         GPU_platform_renderer(),
         GPU_platform_version());
  printf("Device memory statistics: %s\n",
         GPU_mem_stats_supported() ? "supported" : "unsupported");
  printf("Texture size: %dx%d\n", texture_size, texture_size);
  printf("\n");
  printf(
      "'reported' is what the backend says a single texture allocates, 'device' is the device\n"
      "memory statistics delta divided by the number of textures. Neither sees compression that\n"
      "costs no extra memory, which is the case on NVIDIA, so 'read' is the measurement that\n"
      "matters there: sampling throughput drops when the driver can't compress.\n");
  printf("\n");

  gpu::Shader *bandwidth_shader = GPU_shader_create_from_info_name("gpu_texture_bandwidth_test");
  EXPECT_NE(bandwidth_shader, nullptr) << "bandwidth compute shader failed to compile";

  printf("%-8s %-26s %6s %12s %12s %8s %12s %8s %12s %8s\n",
         "format",
         "case",
         "count",
         "naive",
         "reported",
         "ratio",
         "device",
         "ratio",
         "read",
         "ratio");

  const int64_t cases_num = ARRAY_SIZE(memory_cases);

  for (const MeasuredFormat &measured_format : measured_formats) {
    /* Measure every case once per round rather than repeating a case before moving on, so that
     * a machine that speeds up or slows down over the run biases all cases the same way. */
    Array<Measurement> results(cases_num);
    Array<Vector<int64_t>> device_samples(cases_num);
    for (const int64_t round : IndexRange(measurement_rounds)) {
      for (const int64_t case_index : IndexRange(cases_num)) {
        const Measurement measurement = measure_case(
            measured_format, memory_cases[case_index], bandwidth_shader);

        if (round == 0) {
          results[case_index] = measurement;
        }
        else {
          /* Keep the fastest read, slower rounds are contention rather than a property of the
           * texture. */
          results[case_index].read_gb_per_second = std::max(
              results[case_index].read_gb_per_second, measurement.read_gb_per_second);
        }
        if (measurement.device_size_valid) {
          device_samples[case_index].append(measurement.device_size);
        }
      }
    }

    /* Take the median device memory delta, single rounds are too easily thrown off by memory
     * the driver allocates or releases for its own reasons during the measured window. */
    for (const int64_t case_index : IndexRange(cases_num)) {
      Vector<int64_t> &samples = device_samples[case_index];
      std::sort(samples.begin(), samples.end());
      results[case_index].device_size_valid = !samples.is_empty();
      results[case_index].device_size = samples.is_empty() ? 0 : samples[samples.size() / 2];
    }

    int64_t base_allocation_size = 0;
    int64_t base_device_size = 0;
    double base_gb_per_second = 0.0;
    bool have_base = false;

    for (const int64_t case_index : IndexRange(cases_num)) {
      const MemoryCase &memory_case = memory_cases[case_index];
      const Measurement &measurement = results[case_index];

      if (!have_base && memory_case.is_baseline) {
        base_allocation_size = measurement.allocation_size;
        base_device_size = measurement.device_size_valid ? measurement.device_size : 0;
        base_gb_per_second = measurement.read_gb_per_second;
        have_base = true;
      }

      printf("%-8s %-26s %6d",
             measured_format.name,
             memory_case.name,
             int(measurement.texture_count));
      print_size(measurement.naive_size);
      print_size(measurement.allocation_size);
      print_ratio(measurement.allocation_size, base_allocation_size);
      print_size(measurement.device_size_valid ? measurement.device_size : 0);
      print_ratio(measurement.device_size_valid ? measurement.device_size : 0, base_device_size);
      print_rate(measurement.read_gb_per_second);
      print_rate_ratio(measurement.read_gb_per_second, base_gb_per_second);
      printf("\n");
    }
    printf("\n");
  }

  if (bandwidth_shader != nullptr) {
    GPU_shader_free(bandwidth_shader);
  }

  fflush(stdout);

  /* DEBUG BRANCH: CTest only reports the output of tests that fail, and reading the table above
   * is the entire point of this branch, so fail on purpose. */
  ADD_FAILURE() << "Debug branch: measurement finished, see the table above.";
}
GPU_TEST(texture_memory_usage)

}  // namespace blender::gpu::tests
