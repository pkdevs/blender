/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief GSplat API for render engines
 *
 * Draw cache creating buffers and batches for the gsplat implementation. This handles
 * quantized packing of gsplat attribute data. For matching unpacking, refer to the
 * `draw_gsplat_lib.bsl.hh` shader-side.
 *
 * References:
 *
 *  [quat2017]  Mark Reynolds
 *              Quaternion quantization
 *              https://marc-b-reynolds.github.io/quaternions/2017/05/02/QuatQuantPart1.html
 *
 *  [gspt2026]  Joris Rijsdijk et al.
 *              Gaussian Point Splatting
 *              ACM Transactions on Graphics (SIGGRAPH 2026)
 *              https://jorisar.nl/gaussian_point_splatting/
 */

#include <cstring>

#include "BLI_array_utils.hh"
#include "BLI_bounds.hh"
#include "BLI_color_types.hh"
#include "BLI_listbase.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.hh"

#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_userdef_types.h"

#include "BKE_attribute.hh"
#include "BKE_material.hh"
#include "BKE_pointcloud.hh"

#include "GPU_batch.hh"
#include "GPU_material.hh"
#include "GPU_uniform_buffer.hh"

#include "DRW_render.hh"

#include "draw_attributes.hh"
#include "draw_cache_impl.hh"
#include "draw_cache_inline.hh"
#include "draw_gsplat_private.hh" /* own include */
#include "draw_shader_shared.hh"

namespace blender::draw {

/* Draw module and real-time engines (EEVEE, Overlay, Workbench) restrict max
 * 3 spherical harmonics degrees for performance and memory reasons. */
constexpr uint32_t radiance_degs_max = 3;

/* Nr. of coefficients matching 3 degree spherical harmonics, minus degree 0. */
constexpr uint32_t radiance_coeffs_max = (radiance_degs_max + 1u) * (radiance_degs_max + 1u) - 1u;

namespace detail {

/**
 * Given an IndexMask, produce a gpu::IndexBuffer holding mask indices.
 */
static void index_mask_to_ibo(const IndexMask &mask, gpu::IndexBuf &ibo)
{
  const int max_index = mask.min_array_size();
  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_POINTS, mask.size(), max_index);
  MutableSpan<uint> data = GPU_indexbuf_get_data(&builder);
  mask.to_indices<int>(data.cast<int>());
  GPU_indexbuf_build_in_place_ex(&builder, 0, max_index, false, &ibo);
}

/**
 * Given a boundary, normalize and clamp a value to lie within a [0,1] range.
 */
template<typename T> T normalize_to_bounds(const Bounds<T> bounds, const T &value_to_pack)
{
  return math::clamp((value_to_pack - bounds.min) / bounds.size(), T(0), T(1));
}

/**
 * Expmap packing [quat2017] of quaternion data.
 *
 * \note Keep in sync with `unpack_quaternion_3_to_4` in `draw_gsplat_lib.bsl.hh`.
 */
float3 pack_quaternion_4_to_3(float4 q)
{
  float w = q.w;
  float a = 1.0f - w * w;
  float b = 1.0f / sqrt(a + 1.0e-8f);
  float k = a * b;
  float s = (2.0f / M_PI) * atan(k / w) * b;
  return s * float3(q);
}

/**
 * Tightly pack gaussian shape data to a uint4.
 *
 * This currently follows the quantization in [gspt2026], which relies on separately stored
 * boundaries in a UBO. The quantization reduces gaussian mean to 23+22+23b, scale to 3x10b,
 * and rotation to 3x10b. Across 4 words, data is distributed as follows:
 *
 * - word 0: [ meanx, 23 ] [ meany,   9 ]
 * - word 1: [ meany, 13 ] [ meanz,  19 ]
 * - word 2: [ meanz,  4 ] [ quatx,  10 ] [ quaty,  10 ] [ quatz,   8 ]
 * - word 3: [ quatz,  2 ] [ scalex, 10 ] [ scalew, 10 ] [ scalez, 10 ]
 *
 * \note Keep in sync with `unpack_gaussian_*` in `draw_gsplat_lib.bsl.hh`.
 */
uint4 pack_gaussian(float3 mean,
                    float3 log_scale,
                    math::Quaternion rotation,
                    const Bounds<float3> mean_bounds,
                    const Bounds<float3> log_scale_bounds)
{
  /* Quantize mean to `pu`: 23+22+23b. */
  float3 p = normalize_to_bounds(mean_bounds, mean);
  uint3 pu = uint3(static_cast<uint32_t>(p.x * ((1u << 23) - 1u)),
                   static_cast<uint32_t>(p.y * ((1u << 22) - 1u)),
                   static_cast<uint32_t>(p.z * ((1u << 23) - 1u)));

  /* Quantize rotation to `ru`: 3x10b. Note that we shuffle quaternion wxyz to xyzw for consistency
   * inside the draw module shaders. */
  float4 q = math::normalize(float4(rotation.x, rotation.y, rotation.z, rotation.w));
  if (q.w < 0.0f) {
    q = -q;
  }
  float3 vr = math::clamp((pack_quaternion_4_to_3(q) + 1.0f) * 0.5f, 0.0f, 1.0f);
  uint3 ru = static_cast<uint3>(vr * 1023.0f);

  /* Quantize scaling to `su`: 3x10b. */
  float3 s = normalize_to_bounds(log_scale_bounds, log_scale);
  uint3 su = static_cast<uint3>(s * 1023.0f);

  /* Now for the fun part: pack quantized data to 4 words. */
  uint4 w;
  w.x = /* 23 bits */ (pu.x & 0x7FFFFFu) |
        /* 9  bits */ ((pu.y & 0x1FFu) << 23);
  w.y = /* 13 bits */ ((pu.y >> 9) & 0x1FFFu) |
        /* 19 bits */ (pu.z << 13);
  w.z = /* 4  bits */ ((pu.z >> 19) & 0xFu) |
        /* 10 bits */ ((ru.x & 0x3FFu) << 4u) |
        /* 10 bits */ ((ru.y & 0x3FFu) << 14u) |
        /* 8  bits */ ((ru.z & 0xFFu) << 24u);
  w.w = /* 2  bits */ ((ru.z >> 8u) & 0x3u) |
        /* 10 bits */ ((su.x & 0x3FFu) << 2) |
        /* 10 bits */ ((su.y & 0x3FFu) << 12) |
        /* 10 bits */ ((su.z & 0x3FFu) << 22);
  return w;
}

struct RadiancePack {
  float4 radiance_base;
  uint8_t radiance_sh[48];
};
static_assert(sizeof(RadiancePack) == 64ul);

/**
 * Tightly pack radiance and opacity data to 4x uint4.
 *
 * This quantizes 3 channels of spherical harmonics degrees 1-3 using 8b per coefficient, so
 * 3x15 coefficients. Note that the draw module restricts spherical harmonics to 3 degrees; packing
 * a 4th would greatly increase memory and packing complexity for little gain. Further note that
 * currently degree 0 and opacity are stored at full precision.
 *
 * The 4x1 (deg 0 + alpha) and 3x15 (deg 1-3) values are distributed over 16 words as follows:
 *
 *  [ 0,  1,  2,  3] : [rrrr][gggg][bbbb][aaaa]
 *  [ 4,  5,  6,  7] : [rgbr][gbrg][brgb][rgb_]
 *  [ 8,  9, 10, 11] : [rgbr][gbrg][brgb][rgb_]
 *  [12, 13, 14, 15] : [rgbr][gbrg][brgb][rgb_]
 *
 * \note Keep in sync with `eval_radiance_*` in `draw_gsplat.bsl.hh`.
 */
RadiancePack pack_radiance(float4 radiance_base,
                           const Array<float3, radiance_coeffs_max> radiance_sh,
                           const Bounds<float3> &radiance_sh_bounds)
{
  RadiancePack pack = {.radiance_base = radiance_base};

  for (uint32_t i = 0u; i < radiance_coeffs_max; ++i) {
    /* Quantize float3 coefficients to 3x8b. */
    float3 shf = normalize_to_bounds(radiance_sh_bounds, radiance_sh[i]);
    uchar3 shu = static_cast<uchar3>(math::clamp(shf * 255.0f, 0.0f, 255.0f));

    /* Coefficients are stored in packs with a padding byte every 15 bytes.
     * This simplifies unpacking shader-side, until we find more data to pack in. */
    uchar3 *shu_data = reinterpret_cast<uchar3 *>(pack.radiance_sh + (3 * i) + (i / 5));
    *shu_data = shu;
  }

  return pack;
}

}  // namespace detail

/* -------------------------------------------------------------------- */
/** \name GSplatEvalCache management
 * \{ */

void GSplatEvalCache::ensure_builtin_buffers(PointCloud &pointcloud)
{
  /* Accessors over built-ins. */
  const Span mean = pointcloud.positions();
  const Span exp_scale = pointcloud.gsplat_scale();
  const Span quaternion = pointcloud.gsplat_rotation();
  const Span radiance_base = pointcloud.radiance_base();
  const Vector<VArraySpan<float3>> radiance_sh = pointcloud.radiance_sh();

  /* An activation function `exp(x)` is typically applied to scaling coefficients before rendering,
   * as the scale data comes directly from the fitting process. We strip this function for packing
   * purposes, and re-apply it when unpacking shader-side. */
  Vector<float3> scale(exp_scale.size());
  threading::parallel_for(exp_scale.index_range(), 4096, [&](IndexRange range) {
    for (const int i : range) {
      scale[i] = math::log(exp_scale[i]);
    }
  });

  /* For packing purposes, we compute data boundaries. The packing functions below quantize the
   * data such that it within [0, 1], as defined over the data range. */
  Bounds<float3> bounds_mean = Bounds(float3(0.0f));
  Bounds<float3> bounds_scale = Bounds(float3(0.0f));
  Bounds<float3> bounds_radiance_sh = Bounds(float3(0.0f));
  if (auto bounds_opt = bounds::min_max(mean); bounds_opt.has_value()) {
    bounds_mean = bounds_opt.value();
  }
  if (auto bounds_opt = bounds::min_max(scale.as_span()); bounds_opt.has_value()) {
    bounds_scale = bounds_opt.value();
  }
  for (const VArraySpan<float3> &span : radiance_sh) {
    if (auto bounds_opt = bounds::min_max(span); bounds_opt.has_value()) {
      bounds_radiance_sh = bounds::merge(bounds_radiance_sh, bounds_opt.value());
    }
  }

  /* Push data boundaries to a UBO for unpacking shader-side. */
  GSplatBoundsInfos bounds_buf_data = {
      .mean = {float4(bounds_mean.min, 0.0f), float4(bounds_mean.max, 0.0f)},
      .log_scale = {float4(bounds_scale.min, 0.0f), float4(bounds_scale.max, 0.0f)},
      .radiance_sh = {float4(bounds_radiance_sh.min, 0.0f), float4(bounds_radiance_sh.max, 0.0f)},
  };
  GPU_uniformbuf_update(bounds_buf, &bounds_buf_data);

  /* Gaussian shape and radiance data is packed into uint4 and uint4x4 buffers respectively. */
  static constexpr GPUUsageType usage_flag = GPU_USAGE_STATIC | GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY;
  shape_data_buf->init(gpu::GenericVertexFormat<uint4>::format(), usage_flag);
  shape_data_buf->allocate(mean.size());
  radiance_data_buf->init(gpu::GenericVertexFormat<uint4>::format(), usage_flag);
  radiance_data_buf->allocate(mean.size() * 4); /* Each splat uses uint4x4. */

  /* Pack gaussian shape data to uint4 span. */
  MutableSpan shape_span = shape_data_buf->data<uint4>();
  threading::parallel_for(shape_span.index_range(), 4096, [&](IndexRange range) {
    for (const int i : range) {
      shape_span[i] = detail::pack_gaussian(
          mean[i], scale[i], quaternion[i], bounds_mean, bounds_scale);
    }
  });

  /* Pack radiance and opacity data to 4x uint4 span. */
  MutableSpan radiance_span = radiance_data_buf->data<detail::RadiancePack>();
  threading::parallel_for(radiance_span.index_range(), 4096, [&](IndexRange range) {
    for (const int i : range) {
      float4 radiance_base_i = radiance_base[i];
      Array<float3, radiance_coeffs_max> radiance_sh_i(radiance_coeffs_max, float3(0.0f));
      for (uint32_t j = 0; j < radiance_coeffs_max; ++j) {
        radiance_sh_i[j] = radiance_sh[j][i];
      }
      radiance_span[i] = detail::pack_radiance(radiance_base_i, radiance_sh_i, bounds_radiance_sh);
    }
  });
}

void GSplatEvalCache::ensure_edit_dots_indices(PointCloud &pointcloud)
{
  const bke::AttributeAccessor attributes = pointcloud.attributes();
  const VArray selection = *attributes.lookup_or_default<bool>(
      ".selection", bke::AttrDomain::Point, true);

  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_bools(selection, memory);
  if (mask.is_empty()) {
    return;
  }
  detail::index_mask_to_ibo(mask, *edit_dots_indices);
}

void GSplatEvalCache::ensure_attribute(PointCloud &pointcloud, int attrib_i)
{
  gpu::VertBuf &attr_buf = *attributes_buf[attrib_i];
  const StringRef attr_name = attr_used[attrib_i];

  /* TODO(@kevindietrich): float4 is used for scalar attributes as the conversion done
   * by OpenGL to float4 for a scalar `s` will produce a `float4(s, 0, 0, 1)`. However,
   * following the Blender convention, it should be `float4(s, s, s, 1)`. This could be
   * resolved using a similar texture state swizzle to map the attribute correctly as
   * for volume attributes, so we can control the conversion ourselves. */
  const bke::AttributeAccessor attributes = pointcloud.attributes();
  bke::AttributeReader<ColorGeometry4f> attribute = attributes.lookup_or_default<ColorGeometry4f>(
      attr_name, bke::AttrDomain::Point, {0.0f, 0.0f, 0.0f, 1.0f});

  static const GPUVertFormat format = [&]() {
    GPUVertFormat format{};
    GPU_vertformat_attr_add(&format, "attr", gpu::VertAttrType::SFLOAT_32_32_32_32);
    return format;
  }();
  static const GPUUsageType usage_flag = GPU_USAGE_STATIC | GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY;
  GPU_vertbuf_init_with_format_ex(attr_buf, format, usage_flag);
  GPU_vertbuf_data_alloc(attr_buf, pointcloud.totpoint);

  array_utils::copy(attribute.varray, attr_buf.data<ColorGeometry4f>());
}

void GSplatEvalCache::ensure_compute_buffers(PointCloud &pointcloud)
{
  static const GPUVertFormat ellipse_format = gpu::GenericVertexFormat<uint2>::format();
  static const GPUVertFormat radiance_format = gpu::GenericVertexFormat<float2>::format();
  static const GPUUsageType buffer_usage_flag = GPU_USAGE_STATIC | GPU_USAGE_DEVICE_ONLY |
                                                GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY;

  ellipses_comp_buf->init(ellipse_format, buffer_usage_flag);
  ellipses_comp_buf->allocate(pointcloud.totpoint);
  radiance_comp_buf->init(radiance_format, buffer_usage_flag);
  radiance_comp_buf->allocate(pointcloud.totpoint);
}

void GSplatEvalCache::ensure_requested(PointCloud &pointcloud)
{
  if (DRW_batch_requested(dots, GPU_PRIM_POINTS)) {
    DRW_vbo_request(dots, &shape_data_buf);
  }
  if (DRW_batch_requested(edit_dots, GPU_PRIM_POINTS)) {
    DRW_ibo_request(edit_dots, &edit_dots_indices);
    DRW_vbo_request(edit_dots, &shape_data_buf);
  }
  if (DRW_ibo_requested(edit_dots_indices)) {
    ensure_edit_dots_indices(pointcloud);
  }
  if (DRW_vbo_requested(shape_data_buf) || DRW_vbo_requested(radiance_data_buf)) {
    ensure_builtin_buffers(pointcloud);
  }
  if (DRW_vbo_requested(ellipses_comp_buf) || DRW_vbo_requested(radiance_comp_buf)) {
    ensure_compute_buffers(pointcloud);
  }
  for (int request_i : attr_used.index_range()) {
    DRW_vbo_request(nullptr, &attributes_buf[request_i]);
    if (DRW_vbo_requested(attributes_buf[request_i])) {
      ensure_attribute(pointcloud, request_i);
    }
  }
}

void GSplatEvalCache::discard_buffers()
{
  GPU_BATCH_DISCARD_SAFE(surface);
  GPU_BATCH_DISCARD_SAFE(dots);
  GPU_BATCH_DISCARD_SAFE(edit_dots);

  GPU_VERTBUF_DISCARD_SAFE(shape_data_buf);
  GPU_VERTBUF_DISCARD_SAFE(radiance_data_buf);
  GPU_VERTBUF_DISCARD_SAFE(ellipses_comp_buf);
  GPU_VERTBUF_DISCARD_SAFE(radiance_comp_buf);
  GPU_VERTBUF_DISCARD_SAFE(attr_viewer);

  GPU_UBO_FREE_SAFE(bounds_buf);
  GPU_INDEXBUF_DISCARD_SAFE(edit_dots_indices);

  if (surface_per_mat) {
    for (int i = 0; i < mat_len; ++i) {
      GPU_BATCH_DISCARD_SAFE(surface_per_mat[i]);
    }
  }
  MEM_SAFE_DELETE(surface_per_mat);

  discard_attributes();
}

void GSplatEvalCache::discard_attributes()
{
  for (const int j : IndexRange(GPU_MAX_ATTR)) {
    GPU_VERTBUF_DISCARD_SAFE(attributes_buf[j]);
  }
  attr_used.clear();
}

gpu::VertBuf **GSplatEvalCache::attributes_get(uint request_i)
{
  return &attributes_buf[request_i];
}

gpu::VertBuf *GSplatEvalCache::shape_data_buf_get()
{
  DRW_vbo_request(nullptr, &shape_data_buf);
  return shape_data_buf;
}

gpu::VertBuf *GSplatEvalCache::radiance_data_buf_get()
{
  DRW_vbo_request(nullptr, &radiance_data_buf);
  return radiance_data_buf;
}

gpu::VertBuf *GSplatEvalCache::ellipses_comp_buf_get()
{
  DRW_vbo_request(nullptr, &ellipses_comp_buf);
  return ellipses_comp_buf;
}

gpu::VertBuf *GSplatEvalCache::radiance_comp_buf_get()
{
  DRW_vbo_request(nullptr, &radiance_comp_buf);
  return radiance_comp_buf;
}

gpu::UniformBuf *GSplatEvalCache::bounds_buf_get()
{
  if (bounds_buf == nullptr) {
    bounds_buf = GPU_uniformbuf_create_ex(sizeof(GSplatBoundsInfos), nullptr, "GSplatBoundsInfos");
  }
  return bounds_buf;
}

gpu::Batch *GSplatEvalCache::dots_get()
{
  return DRW_batch_request(&dots);
}

gpu::Batch *GSplatEvalCache::edit_dots_get()
{
  return DRW_batch_request(&edit_dots);
}

gpu::Batch *GSplatEvalCache::surface_get(const PointCloud &pointcloud)
{
  if (surface == nullptr) {
    const int32_t strip_tile_size = DRW_GSPLAT_STRIP_TILE_SIZE;
    surface = GPU_batch_create_procedural(GPU_PRIM_TRIS /*  GPU_PRIM_TRI_STRIP */,
                                          strip_tile_size * pointcloud.totpoint);

    DRW_vbo_request(nullptr, &shape_data_buf);
    DRW_vbo_request(nullptr, &radiance_data_buf);

    if (!bounds_buf) {
      bounds_buf = GPU_uniformbuf_create_ex(
          sizeof(GSplatBoundsInfos), nullptr, "GSplatBoundsInfos");
    }
  }
  return surface;
}

gpu::Batch **GSplatEvalCache::surface_shaded_get(const PointCloud &pointcloud,
                                                 GPUMaterial **gpu_materials,
                                                 int mat_len)
{
  const bke::AttributeAccessor attributes = pointcloud.attributes();
  VectorSet<std::string> attrs_needed;

  /* Add material requested attributes. */
  for (GPUMaterial *gpu_material : Span<GPUMaterial *>(gpu_materials, mat_len)) {
    ListBaseT<GPUMaterialAttribute> gpu_attrs = GPU_material_attributes(gpu_material);
    for (GPUMaterialAttribute &gpu_attr : gpu_attrs) {
      const StringRef name = gpu_attr.name;
      if (!attributes.contains(name)) {
        continue;
      }
      drw_attributes_add_request(&attrs_needed, name);
    }
  }

  if (!drw_attributes_overlap(&attr_used, &attrs_needed)) {
    /* Some new attributes have been added, free all and start over. */
    for (const int i : IndexRange(GPU_MAX_ATTR)) {
      GPU_VERTBUF_DISCARD_SAFE(attributes_buf[i]);
    }
    drw_attributes_merge(&attr_used, &attrs_needed);
  }
  drw_attributes_merge(&attr_used_over_time, &attrs_needed);

  DRW_batch_request(&surface_per_mat[0]);
  return surface_per_mat;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GSplatBatchCache management
 * \{ */

/**
 * Wrapper struct which is attached to the pointcloud.
 */
struct GSplatBatchCache {
  /* Underlying cache of batches, buffers, etc. */
  GSplatEvalCache eval_cache;
  /* Determines if cache is invalid */
  bool is_dirty;

 public:
  static GSplatBatchCache *get_ptr(PointCloud &pointcloud)
  {
    return pointcloud.gsplat_batch_cache;
  }

  static bool is_valid(PointCloud &pointcloud)
  {
    GSplatBatchCache *ptr = GSplatBatchCache::get_ptr(pointcloud);
    if (!ptr) {
      return false;
    }
    if (ptr->eval_cache.mat_len != BKE_id_material_used_with_fallback_eval(pointcloud.id)) {
      return false;
    }
    return !ptr->is_dirty;
  }

  static void init(PointCloud &pointcloud)
  {
    GSplatBatchCache *ptr = GSplatBatchCache::get_ptr(pointcloud);
    if (!ptr) {
      ptr = MEM_new<GSplatBatchCache>(__func__);
      pointcloud.gsplat_batch_cache = ptr;
    }
    else {
      ptr->eval_cache = {};
      ptr->eval_cache.edit_dots = nullptr;
      ptr->eval_cache.edit_dots_indices = nullptr;
    }

    ptr->eval_cache.mat_len = BKE_id_material_used_with_fallback_eval(pointcloud.id);
    ptr->eval_cache.surface_per_mat = MEM_new_array_zeroed<gpu::Batch *>(ptr->eval_cache.mat_len,
                                                                         __func__);
    ptr->is_dirty = false;
  }

  static void clear(PointCloud &pointcloud)
  {
    GSplatBatchCache *ptr = GSplatBatchCache::get_ptr(pointcloud);
    if (!ptr) {
      return;
    }
    ptr->eval_cache.discard_buffers();
  }
};

GSplatEvalCache &GSplatEvalCache::get(PointCloud &pointcloud)
{
  return GSplatBatchCache::get_ptr(pointcloud)->eval_cache;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Batch cache API
 * \{ */

gpu::VertBuf **DRW_gsplat_evaluated_attribute(PointCloud *pointcloud, const StringRef name)
{
  if (pointcloud->render_as != PT_RENDER_AS_SPLATS) {
    return nullptr;
  }

  const bke::AttributeAccessor attributes = pointcloud->attributes();
  GSplatEvalCache &cache = GSplatEvalCache::get(*pointcloud);

  if (!attributes.contains(name)) {
    return nullptr;
  }
  {
    VectorSet<std::string> requests{};
    drw_attributes_add_request(&requests, name);
    drw_attributes_merge(&cache.attr_used, &requests);
  }

  for (const int i : IndexRange(cache.attr_used.index_range())) {
    if (cache.attr_used[i] == name) {
      return cache.attributes_get(i);
    }
  }
  return nullptr;
}

gpu::Batch *DRW_gsplat_batch_cache_get_dots(Object *ob)
{
  PointCloud &pointcloud = DRW_object_get_data_for_drawing<PointCloud>(*ob);
  return pointcloud.render_as == PT_RENDER_AS_SPLATS ?
             GSplatEvalCache::get(pointcloud).dots_get() :
             nullptr;
}

gpu::Batch *DRW_gsplat_batch_cache_get_edit_dots(Object *ob)
{
  PointCloud &pointcloud = DRW_object_get_data_for_drawing<PointCloud>(*ob);
  return pointcloud.render_as == PT_RENDER_AS_SPLATS ?
             GSplatEvalCache::get(pointcloud).edit_dots_get() :
             nullptr;
}

void DRW_gsplat_batch_cache_create_requested(Object *ob)
{
  PointCloud &pointcloud = DRW_object_get_data_for_drawing<PointCloud>(*ob);
  if (pointcloud.render_as != PT_RENDER_AS_SPLATS) {
    return;
  }
  GSplatEvalCache::get(pointcloud).ensure_requested(pointcloud);
}

void DRW_gsplat_batch_cache_dirty_tag(PointCloud *pointcloud, int mode)
{
  GSplatBatchCache *ptr = GSplatBatchCache::get_ptr(*pointcloud);
  if (ptr == nullptr) {
    return;
  }

  switch (mode) {
    case BKE_POINTCLOUD_BATCH_DIRTY_ALL:
      ptr->is_dirty = true;
      break;
    default:
      BLI_assert(0);
  }
}

void DRW_gsplat_batch_cache_validate(PointCloud *pointcloud)
{
  if (pointcloud->render_as != PT_RENDER_AS_SPLATS) {
    return;
  }

  if (!GSplatBatchCache::is_valid(*pointcloud)) {
    GSplatBatchCache::clear(*pointcloud);
    GSplatBatchCache::init(*pointcloud);
  }
}

void DRW_gsplat_batch_cache_free(PointCloud *pointcloud)
{
  GSplatBatchCache::clear(*pointcloud);
  MEM_delete(pointcloud->gsplat_batch_cache);
  pointcloud->gsplat_batch_cache = nullptr;
}

void DRW_gsplat_batch_cache_free_old(PointCloud *pointcloud, int ctime)
{
  if (!GSplatBatchCache::get_ptr(*pointcloud)) {
    return;
  }
  GSplatEvalCache &cache = GSplatEvalCache::get(*pointcloud);

  bool do_discard = false;

  if (drw_attributes_overlap(&cache.attr_used_over_time, &cache.attr_used)) {
    cache.last_attr_matching_time = ctime;
  }

  if (ctime - cache.last_attr_matching_time > U.vbotimeout) {
    do_discard = true;
  }

  cache.attr_used_over_time.clear();
  if (do_discard) {
    cache.discard_attributes();
  }
}

/** \} */

}  // namespace blender::draw
