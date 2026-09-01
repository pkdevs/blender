/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Compute shader code for gsplat implementation. For draw interfaces, packing and
 * unpacking code, refer to `draw_gsplat_lib.bsl.hh`.
 *
 * References:
 *
 *  [3dgs2023]  Bernhard Kebl, Georgios Kopanas et al.
 *              3D Gaussian Splatting for Real-Time Radiance Field Rendering
 *              ACM Transactions on Graphics (SIGGRAPH 2023)
 *              https://repo-sam.inria.fr/fungraph/3d-gaussian-splatting/
 */

#pragma once

#include "draw_gsplat_lib.bsl.hh"
#include "draw_model.bsl.hh"
#include "draw_shader_shared.hh"
#include "draw_view.bsl.hh"

#include "gpu_shader_math_matrix_transform_lib.glsl"
#include "gpu_shader_math_spherical_harmonics_lib.bsl.hh"
#include "gpu_shader_math_vector_reduce_lib.glsl"

namespace draw::gsplat {

namespace detail {

/* TODO(not_mark): probably duplicate of some header in the gpu shaders */
float3 inverse_srgb_transfer(float3 srgb)
{
  float3 c = max(srgb, float3(0.0f));
  float3 c1 = c * (1.0f / 12.92f);
  float3 c2 = pow((c + 0.055f) * (1.0f / 1.055f), float3(2.4f));
  return mix(c1, c2, step(float3(0.04045f), c));
}

/**
 * Test if a splat's projection is partially or entirely inside, or partially or
 * entirely enclosing NDC space.
 */
bool is_inside_viewport(AxisTuple axes, float3 hP, float2 viewport_size)
{
  float2 corner = 2.0f * max(abs(axes.axis_0), abs(axes.axis_1)) * 2.0f / viewport_size;
  float2 bbox_bl = hP.xy + corner;
  float2 bbox_ur = hP.xy - corner;

  constexpr float ndc_bounds = 1.0f;
  bool clip_bl = any(equal(max(bbox_bl, float2(-ndc_bounds)), float2(-ndc_bounds)));
  bool clip_ur = any(equal(min(bbox_ur, float2(ndc_bounds)), float2(ndc_bounds)));

  return !(clip_bl || clip_ur);
}

/**
 * Quickly test if a splat's projection is entirely contained in a 3x3 pixel patch.
 */
bool is_3x3_footprint(AxisTuple axes)
{
  float2 corner = 2.0f * max(abs(axes.axis_0), abs(axes.axis_1)) * 2.0f;
  return reduce_max(corner) >= 3.0f;
}

}  // namespace detail

/* Shader can skip ellipses (e.g. for point primitives) or radiance parts (e.g. for overlay). */
struct Pipeline {
  [[compilation_constant]] bool do_ellipses;
  [[compilation_constant]] bool do_radiance_base;
  [[compilation_constant]] bool do_radiance_full;
};

struct Resources {
  [[resource_table]] srt_t<BoundsResources<0>> bounds_data;

  [[storage(0, read)]] const uint4 (&shape_data)[];
  [[storage(1, read)]] const uint4 (&radiance_data)[];
  [[storage(2, write)]] uint2 (&ellipses_comp)[];
  [[storage(3, write)]] float2 (&radiance_comp)[];

  [[push_constant]] const float2 viewport_size;
  [[push_constant]] const int resource_id;
  [[push_constant]] const int num_points;
  /* [[push_constant]] const uint inst_id; */
  /* [[push_constant]] const uint base_inst; */

  Gaussian get(uint gs_id) const
  {
    [[resource_table]] const BoundsResources<0> &bounds_srt = bounds_data;
    const uint4 data = shape_data[gs_id];

    Gaussian gs;
    gs.id = gs_id;
    gs.mean = detail::unpack_gaussian_mean(data, bounds_srt.mean());
    gs.scale = detail::unpack_gaussian_scale(data, bounds_srt.log_scale());
    gs.rotation = detail::unpack_gaussian_rotation(data);
    return gs;
  }
};

/**
 * Evaluate projected gsplat data, perform early culling of simple features, and return
 * a packed format for geometry draws.
 */
uint2 eval_ellipse_packed(Gaussian gs,
                          ObjectMatrices obj,
                          ViewMatrices view,
                          [[resource_table]] const Resources &srt)
{
  /* Is the splat behind the viewer? */
  float3 wP = obj.point_object_to_world(gs.mean);
  float3 hP = view.point_world_to_ndc(wP);
  if (view.is_perspective() && hP.z < 0.0f) {
    return uint2(0);
  }

  float4x4 viewmat = view.viewmat * obj.model;
  float3x3 covmat = get_covmat(gs.rotation, gs.scale);

  /* Project gaussian to screen-space, recovering 2D axes for a splat billboard. */
  float3 covmat_proj = project_covmat(
      gs.mean, viewmat, view.winmat, covmat, srt.viewport_size, view.is_perspective());
  AxisTuple axes = covmat_to_axes(covmat_proj);

  if (
      /* Is the splat partially/entirely inside/overlapping the viewport? */
      !detail::is_inside_viewport(axes, hP, srt.viewport_size) ||
      /* Is the splat footprint enclosed in a 3x3 patch? */
      !detail::is_3x3_footprint(axes))
  {
    return uint2(0);
  }

  /* Compute and pack normal. */
  float3 N = get_planar_normal(gs.rotation, gs.scale);
  float2 oct = detail::pack_normal_octahedral(N);

  /* Pack output to a single uint. */
  return uint2(axes.pack(), packSnorm2x16(oct));
}

/**
 * Evaluate radiance base (spherical harmonics degree 0), plus gsplat alpha.
 *
 * \note Keep in sync with `pack_radiance` in `draw_cache_impl_gsplat.cc`.
 */
float4 eval_radiance_base(Gaussian gs, [[resource_table]] const Resources &srt)
{
  float4 sh_base = uintBitsToFloat(srt.radiance_data[4 * gs.id]);

  spherical_harmonics::BandL0<float3> L0 = {.M0 = sh_base.rgb};
  float3 radiance = L0.evaluate(float3(0) /* L0 is view indirectional */);

  /* Resolve as (sh + 0.5) [3dgs2023]. */
  return float4(radiance, sh_base.a);
}

/**
 * Evaluate radiance rest (spherical harmonics degree 1-3).
 *
 * \note Keep in sync with `pack_radiance` in `draw_cache_impl_gsplat.cc`.
 */
float3 eval_radiance_rest(Gaussian gs,
                          ObjectMatrices obj,
                          ViewMatrices view,
                          [[resource_table]] const Resources &srt)
{
  /* Object-space incident view vector. */
  float3 wP = obj.point_object_to_world(gs.mean);
  float3 wV = view.world_incident_vector(wP);
  float3 lV = transform_direction(obj.model_inverse, wV);
  lV = -lV;

  /* Unpack spherical harmonic degrees 1, 2, 3.
   *
   * The fun part! These require 3*15 coefficients, which are packed in three
   * uint4s (i.e. 48by) using 8b per coefficient. Layout is as follows:
   *
   *  pack_0 : [rgbr][gbrg][brgb][rgb_]
   *  pack_1 : [rgbr][gbrg][brgb][rgb_]
   *  pack_2 : [rgbr][gbrg][brgb][rgb_]
   *
   * Each uint4 packs 3x5 values. `slice_tuple<>()` reads disjointed colors
   * across the uint4, unpacking the 5 sets of 3 bytes left to right. */
  [[resource_table]] const BoundsResources<0> &srt_bounds = srt.bounds_data;
  const detail::Bounds<float3> bounds = srt_bounds.sh_rest();
  const uint4 sh_rest_0 = srt.radiance_data[4 * gs.id + 1];
  const uint4 sh_rest_1 = srt.radiance_data[4 * gs.id + 2];
  const uint4 sh_rest_2 = srt.radiance_data[4 * gs.id + 3];

  float3 radiance = float3(0.0f);

  /* Degree 1. */
  spherical_harmonics::BandL1<float3> L1 = {
      .Mn1 = bounds.unpack(float3(detail::slice_tuple<0>(sh_rest_0)) / 255.0f),
      .M0 = bounds.unpack(float3(detail::slice_tuple<1>(sh_rest_0)) / 255.0f),
      .Mp1 = bounds.unpack(float3(detail::slice_tuple<2>(sh_rest_0)) / 255.0f)};
  radiance.rgb += L1.evaluate(lV);

  /* Degree 2. */
  spherical_harmonics::BandL2<float3> L2 = {
      .Mn2 = bounds.unpack(float3(detail::slice_tuple<3>(sh_rest_0)) / 255.0f),
      .Mn1 = bounds.unpack(float3(detail::slice_tuple<4>(sh_rest_0)) / 255.0f),
      .M0 = bounds.unpack(float3(detail::slice_tuple<0>(sh_rest_1)) / 255.0f),
      .Mp1 = bounds.unpack(float3(detail::slice_tuple<1>(sh_rest_1)) / 255.0f),
      .Mp2 = bounds.unpack(float3(detail::slice_tuple<2>(sh_rest_1)) / 255.0f)};
  radiance.rgb += L2.evaluate(lV);

  /* Degree 3. */
  spherical_harmonics::BandL3<float3> L3 = {
      .Mn3 = bounds.unpack(float3(detail::slice_tuple<3>(sh_rest_1)) / 255.0f),
      .Mn2 = bounds.unpack(float3(detail::slice_tuple<4>(sh_rest_1)) / 255.0f),
      .Mn1 = bounds.unpack(float3(detail::slice_tuple<0>(sh_rest_2)) / 255.0f),
      .M0 = bounds.unpack(float3(detail::slice_tuple<1>(sh_rest_2)) / 255.0f),
      .Mp1 = bounds.unpack(float3(detail::slice_tuple<2>(sh_rest_2)) / 255.0f),
      .Mp2 = bounds.unpack(float3(detail::slice_tuple<3>(sh_rest_2)) / 255.0f),
      .Mp3 = bounds.unpack(float3(detail::slice_tuple<4>(sh_rest_2)) / 255.0f)};
  radiance.rgb += L3.evaluate(lV);

  return radiance;
}

[[compute]] [[local_size(DRW_GSPLAT_GROUP_SIZE)]]
void compute_entry([[resource_table]] Resources &srt,
                   [[resource_table]] const Pipeline &pipeline,
                   [[resource_table]] const draw::View &views,
                   [[resource_table]] const draw::Model &models,
                   [[global_invocation_id]] const uint3 global_id)
{
  if (global_id.x >= srt.num_points) {
    return;
  }

  /* TODO(not_mark): instances :( */
  /* TODO(not_mark): multiview :( */
  /* TODO(not_mark): coffee :D */
  const ObjectMatrices obj = models.get(srt.resource_id);
  const ViewMatrices view = views.get(0);
  const Gaussian gs = srt.get(global_id.x);

  /* Ellipse computation and output. */
  uint2 ellipse_pack = uint2(0u);
  bool is_ellipse_culled = false;
  if (pipeline.do_ellipses) [[static_branch]] {
    ellipse_pack = eval_ellipse_packed(gs, obj, view, srt);
    is_ellipse_culled = (ellipse_pack.x == 0u);
    srt.ellipses_comp[global_id.x] = ellipse_pack;
  }

  /* Base/full radiance computation and output. */
  if (pipeline.do_radiance_base) [[static_branch]] {
    float4 radiance = float4(0.0f);
    if (!is_ellipse_culled) {
      radiance += eval_radiance_base(gs, srt);

      /* Splat data is generally encoded as (sRGB - 0.5) [3dgs2023]. */
      radiance.rgb = detail::inverse_srgb_transfer(radiance.rgb + 0.5f);
    }

    /* Radiance is packed to fp16. */
    srt.radiance_comp[global_id.x] = uintBitsToFloat(
        uint2(packHalf2x16(radiance.xy), packHalf2x16(radiance.zw)));
  }

  if (pipeline.do_radiance_full) [[static_branch]] {
    float4 radiance = float4(0.0f);
    if (!is_ellipse_culled) {
      radiance += eval_radiance_base(gs, srt);
      radiance.rgb += eval_radiance_rest(gs, obj, view, srt);

      /* Splat data is generally encoded as (sRGB - 0.5) [3dgs2023]. */
      radiance.rgb = detail::inverse_srgb_transfer(radiance.rgb + 0.5f);
    }

    /* Radiance is packed to fp16. */
    srt.radiance_comp[global_id.x] = uintBitsToFloat(
        uint2(packHalf2x16(radiance.xy), packHalf2x16(radiance.zw)));
  }
}

/* clang-format off */

PipelineCompute compute_ellipses(compute_entry, Pipeline{.do_ellipses = true, .do_radiance_base = false, .do_radiance_full = false});
PipelineCompute compute_ellipses_radiance_base(compute_entry, Pipeline{.do_ellipses = true, .do_radiance_base = true, .do_radiance_full = false});
PipelineCompute compute_ellipses_radiance_full(compute_entry, Pipeline{.do_ellipses = true, .do_radiance_base = false, .do_radiance_full = true});

/* Radiance only is unused. */
/* PipelineCompute compute_radiance_base(compute_entry, Pipeline{.do_ellipses = false, .do_radiance_base = true, .do_radiance_full = false}); */
/* PipelineCompute compute_radiance_full(compute_entry, Pipeline{.do_ellipses = false, .do_radiance_base = false, .do_radiance_full = true}); */

/* clang-format on */

}  // namespace draw::gsplat
