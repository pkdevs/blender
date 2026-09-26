/* SPDX-FileCopyrightText: 2017-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Pipelines to generate Look Up Tables for BxDFs. Not used in the default configuration as the
 * tables are stored in the blender executable. This is used for reference or to update them.
 */

#pragma once

#include "eevee_bxdf.bsl.hh"
#include "eevee_bxdf_lut_lib.bsl.hh"
#include "eevee_precompute_shared.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_subsurface_shared.hh"
#include "gpu_shader_compat.hh"
#include "gpu_shader_math_base.bsl.hh"

namespace eevee::lut {

/* -------------------------------------------------------------------- */
/** \name LUT models
 * \{ */

/**
 * Generate 2D GGX BRDF LUT.
 *
 * Follows the split sum approximation used in [Real shading in unreal engine 4]
 * (https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf).
 *
 * The 2D LUT is parameterized on:
 * - `x = roughness`,
 * - `y = sqrt(1 - cos(theta))`,
 *
 * and the result is interpreted as:
 * - `integral = F0 * scale + F90 * bias - F82_tint * metal_bias`,
 *   where `F82_tint = mix(F0, float3(1), pow5f(6/7) * 7 / pow6f(6/7)) * (1 - F82)`.
 */
struct GGXBrdfClosure {
  GGXBrdf closure;
  float3 V;

  static GGXBrdfClosure from_coords(const float3 coords)
  {
    const float roughness = coords.x;

    const float NV = clamp(1.0f - square(coords.y), 1e-4f, 0.9999f);
    const float3 V = float3(sqrt(1.0f - square(NV)), 0.0f, NV);

    return {.closure = GGXBrdf::init(roughness), .V = V};
  }

  GGXBrdfData eval(const float3 rand) const
  {
    /* Geometric normal. */
    constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

    /* Return values. */
    GGXBrdfData brdf = {};

    /* Sample light vector, recover half vector as microfacet normal. */
    const float3 L = closure.bsdf_sample(rand, V).direction;
    const float3 H = normalize(V + L);

    /* Restrict light vector to positive hemisphere. */
    const float NL = L.z;
    if (NL > 0.0f) {
      const float weight = closure.bsdf_eval(N, L, V).weight;
      const float VH = saturate(dot(V, H));

      /* Schlick's Fresnel. */
      const float s = saturate(pow5f(1.0f - VH));

      /* F82 tint effect. */
      const float b = VH * saturate(pow6f(1.0f - VH));

      brdf.scale = (1.0f - s) * weight;
      brdf.bias = s * weight;
      brdf.metal_bias = b * weight;
    }

    return brdf;
  }

  static GGXBrdfClosure from_ltc_coords(const float3 coords)
  {
    const ltc::CoordParam coord_param = ltc::from_isotropic_coords(coords);
    const float roughness = square(coord_param.roughness);
    const float cos_theta = coord_param.cos_theta;

    /* Avoid numerical issues at grazing angles. */
    const float NV = max(cos_theta, 1e-4f);
    const float3 V = float3(sin_from_cos(NV), 0.0f, NV);

    return {.closure = GGXBrdf::init(roughness), .V = V};
  }
};

/**
 * Generate 3D GGX BSDF LUT, for IOR < 1.
 *
 * Follows the split sum approximation used in [Real shading in unreal engine 4]
 * (https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf).
 * and uses Schlick's approximation to weight R, T components.
 *
 * The 3D LUT is parameterized with:
 * - `x = sqrt((ior - 1) / (ior + 1))`,
 * - `y = sqrt(1 - cos(theta))`,
 * - `z = roughness`,
 */
struct GGXBsdfLUT {
  GGXBrdf brdf;
  GGXBtdf btdf;
  float3 V;

  static GGXBsdfLUT from_coords(float3 coords)
  {
    /* IOR is the sine of the critical angle. */
    const float ior = clamp(sqrt(coords.x), 1e-4f, 0.9999f);
    const float critical_cos = sqrt(1.0f - saturate(square(ior)));

    /* Maximize texture usage on both sides of the critical angle. */
    coords.y = coords.y * 2.0f - 1.0f;
    coords.y *= (coords.y > 0.0f) ? (1.0f - critical_cos) : critical_cos;
    /* Center LUT around critical angle to avoid strange interpolation
     * issues when the critical angle is changing. */
    coords.y += critical_cos;

    const float roughness = coords.z;

    const float NV = clamp(coords.y, 1e-4f, 0.9999f);
    const float3 V = float3(sqrt(1.0f - square(NV)), 0.0f, NV);

    return {.brdf = GGXBrdf::init(roughness),
            .btdf = GGXBtdf::init(roughness, ior, Thickness{}),
            .V = V};
  }

  GGXBsdfData eval(const float3 rand) const
  {
    /* Geometric normal. */
    constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

    GGXBsdfData bsdf = {};

    /* Reflection, restricted to positive hemisphere. */
    const float3 R = brdf.bsdf_sample(rand, V).direction;
    float NR = R.z;
    if (NR > 0.0f) {
      /* Recover half vector as GGX normal. */
      const float3 H = normalize(V + R);
      const float3 L = refract(-V, H, 1.0f / btdf.ior);
      const float HL = abs(dot(H, L));

      /* Schlick's Fresnel. */
      const float s = saturate(pow5f(1.0f - saturate(HL)));

      const float weight = brdf.bsdf_eval(N, R, V).weight;
      bsdf.scale = (1.0f - s) * weight;
      bsdf.bias = s * weight;
    }

    /* Refraction, restricted to negative hemisphere. */
    const float3 T = btdf.bsdf_sample(rand, V).direction;
    const float NT = T.z;
    /* In the case of TIR, `T == float3(0)`. */
    if (NT < 0.0f) {
      /* Recover half vector as GGX normal. */
      const float3 H = normalize(btdf.ior * T + V);
      const float HL = abs(dot(H, T));

      /* Schlick's Fresnel. */
      const float s = saturate(pow5f(1.0f - saturate(HL)));

      bsdf.transmission_factor = (1.0f - s) * btdf.bsdf_eval(N, T, V).weight;
    }

    return bsdf;
  }
};

/**
 * Generate Oren Nayar LUT.
 *
 * Reference:
 *
 * [portsmouth2024] Jamie Portsmouth, Peter Kutz and Stephen Hill.
 *                  EON: A practical energy-preserving rough diffuse BRDF (2024)
 *                  https://arxiv.org/pdf/2410.18026
 *
 * The 2D LUT is parameterized as:
 * - `x = roughness`,
 * - `y = cos(theta)`,
 *
 * and output is interpreted as:
 * - `reflectance = color * Ess + Fms * Ems`,
 * where `Fms = color^2 * Eavg / (1 - Eavg) / (1 - color * (1 - Eavg))` and is computed during
 * runtime.
 *
 */

struct OrenNayarClosure {
  OrenNayar closure;
  float3 V;

  static OrenNayarClosure from_coords(const float3 coords)
  {
    const float roughness = coords.x;
    const float NV = coords.y;
    const float3 V = float3(sin_from_cos(NV), 0.0f, NV);

    return {.closure = OrenNayar::init(roughness), .V = V};
  }

  static OrenNayarClosure from_ltc_coords(const float3 coords)
  {
    const ltc::CoordParam coord_param = ltc::from_isotropic_coords(coords);
    const float roughness = coord_param.roughness;
    const float cos_theta = coord_param.cos_theta;
    /* Avoid numerical issues at grazing angles. */
    const float NV = max(cos_theta, 1e-4f);
    const float3 V = float3(sin_from_cos(NV), 0.0f, NV);

    return {.closure = OrenNayar::init(roughness), .V = V};
  }

  OrenNayarData eval(const float3 rand) const
  {
    const float3 L = sample_cos_hemisphere(rand);
    const float nl = max(L.z, 0.0f);
    const float nv = max(V.z, 0.0f);

    return {closure.single_scatter(L, V, nl, nv),
            saturate(1.0f - closure.E(nv)) * saturate(1.0f - closure.E(nl))};
  }
};

/**
 * Generate 3D GGX BTDF LUT, for IOR > 1.
 *
 * Using Schlick's approximation; only the transmittance is needed because scale and
 * bias do not depend on the IOR, and can be obtained independently from the BRDF LUT.
 *
 * The 3D LUT is parameterized with:
 * - `x = sqrt((ior - 1) / (ior + 1))` for higher precision in the range `1 < IOR < 2`
 * - `y = sqrt(1.0f - cos(theta))`
 * - `z = roughness`
 *
 * and output is interpreted as:
 * - `transmittance = (1 - F0) * transmission_factor`.
 */
struct GGXBtdfGt1Closure {
  GGXBtdf closure;
  float3 V;

  static GGXBtdfGt1Closure from_coords(float3 coords)
  {
    const float roughness = coords.z;

    const float F0 = clamp(square(coords.x), 1e-4f, 0.9999f);
    const float ior = (1.0f + F0) / (1.0f - F0);

    const float NV = clamp(1.0f - square(coords.y), 1e-4f, 0.9999f);
    const float3 V = float3(sqrt(1.0f - square(NV)), 0.0f, NV);

    return {.closure = GGXBtdf::init(roughness, ior, Thickness{}), .V = V};
  }

  GGXBtdfGt1Data eval(const float3 rand) const
  {
    /* Geometric normal. */
    constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

    GGXBtdfGt1Data btdf = {};

    /* Refraction, restricted to negative hemisphere. */
    const float3 L = closure.bsdf_sample(rand, V).direction;
    const float NL = L.z;
    if (NL < 0.0f) {
      /* Recover half vector as GGX normal. */
      const float3 H = normalize(closure.ior * L + V);
      const float HV = abs(dot(H, V));

      /* Schlick's Fresnel. */
      const float s = saturate(pow5f(1.0f - saturate(HV)));

      btdf.transmission_factor = (1.0f - s) * closure.bsdf_eval(N, L, V).weight;
    }

    return btdf;
  }
};

/**
 * Generate Christensen-Burley translucency profile.
 *
 * Generally follows:
 * [Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering]
 * (https://blog.selfshadow.com/publications/s2015-shading-course/burley/s2015_pbs_disney_bsdf_notes.pdf)
 * with empirical fitting to match cycles. Precomputes exit radiance for a slab of homogeneous
 * material, back-lit by a directional light. We only care about a single color primary, as the
 * profile is applied to each primary independently. The other components are for debugging.
 *
 * Parameters: `x = distance`, while output is interpreted as exit radiance.
 */
float4 burley_sss_translucency(float3 coords)
{
  /* Note that we only store the 1st (radius == 1) component.
   * The others are here for debugging overall appearance. */
  float3 radii = float3(1.0f, 0.2f, 0.1f);
  float thickness = coords.x * SSS_TRANSMIT_LUT_RADIUS;
  float3 r = thickness / radii;

  /* Manual fit based on cycles render of a backlit slab of varying thickness.
   * Mean Error: 0.003
   * Max Error: 0.015 */
  float3 exponential = exp(-3.6f * pow(r, float3(1.11f)));
  float3 gaussian = exp(-pow(3.4f * r, float3(1.6f)));
  float3 fac = square(saturate(0.5f + r / 0.6f));
  float3 profile = saturate(mix(gaussian, exponential, fac));

  /* Mask off the end progressively to 0. */
  profile *= saturate(1.0f - pow5f(coords.x));

  return float4(profile, 0.0f);
}

/* Note: unused. */
float4 random_walk_sss_translucency(float3 coords)
{
  /* Note that we only store the 1st (radius == 1) component.
   * The others are here for debugging overall appearance. */
  float3 radii = float3(1.0f, 0.2f, 0.1f);
  float thickness = coords.x * SSS_TRANSMIT_LUT_RADIUS;
  float3 r = thickness / radii;

  /* Manual fit based on cycles render of a backlit slab of varying thickness.
   * Mean Error: 0.003
   * Max Error: 0.016 */
  float3 scale = float3(0.31f, 0.47f, 0.32f);
  float3 exponent = float3(-22.0f, -5.8f, -0.5f);
  float3 profile = float3(dot(scale, exp(exponent * r.r)),
                          dot(scale, exp(exponent * r.g)),
                          dot(scale, exp(exponent * r.b)));
  profile = saturate(profile - 0.1f);

  /* Mask off the end progressively to 0. */
  profile *= saturate(1.0f - pow5f(coords.x));

  return float4(profile, 0.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name LUT computation, integration
 * \{ */

template<typename F> float4 integrate(const F &func)
{
  constexpr uint sample_count = 512u * 512u;

  /* TODO(not_mark): Remove workaround for BSL-spec #4. */
  // F func = F::from_coords(coords);

  /* Measure func using N samples. */
  float4 measure = float4(0.0f);
  for (uint i = 1u; i <= sample_count; i++) {
    /* Warp sequence to a point on the unit cylinder. */
    float2 rand = random::hammersley_2d(i, sample_count);
    float3 Xi = sample_cylinder(rand);

    /* Add sample to measure. */
    measure += func.eval(Xi).pack();
  }
  return measure / float(sample_count);
}
template float4 integrate<GGXBrdfClosure>(const GGXBrdfClosure &);
template float4 integrate<GGXBsdfLUT>(const GGXBsdfLUT &);
template float4 integrate<GGXBtdfGt1Closure>(const GGXBtdfGt1Closure &);
template float4 integrate<OrenNayarClosure>(const OrenNayarClosure &);

struct LUT {
  [[image(0, read_write, SFLOAT_32_32_32_32)]] image3D image;
  [[push_constant]] const int type;
  [[push_constant]] const int3 extent;
};

template<typename T> float3 average_light_dir(const T &lut)
{
  /* Geometric normal. */
  constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

  float3 average_dir = float3(0.0f);
  constexpr uint sample_count = 50u * 50u;

  for (uint i = 1u; i <= sample_count; i++) {
    /* Warp sequence to a point on the unit cylinder. */
    const float3 rand = sample_cylinder(random::hammersley_2d(i, sample_count));
    const float3 L = lut.closure.bsdf_sample(rand, lut.V).direction;
    if (L.z > 0.0f) {
      average_dir += lut.closure.bsdf_eval(N, L, lut.V).weight * L;
    }
  }

  /* Clear y component, which should be zero with isotropic BRDFs. */
  average_dir.y = 0.0f;

  return normalize(average_dir);
}

template float3 average_light_dir<GGXBrdfClosure>(const GGXBrdfClosure &);
template float3 average_light_dir<OrenNayarClosure>(const OrenNayarClosure &);

struct FitLTC {
  LtcData ltc;
  bool isotropic;

  static FitLTC init(LtcData &ltc_, bool isotropic_)
  {
    return {.ltc = ltc_, .isotropic = isotropic_};
  }

  void update(const float4 params)
  {
    constexpr float MIN_ALPHA = 0.0001f;
    float m11 = max(params.x, MIN_ALPHA);
    float m22 = max(params.y, MIN_ALPHA);
    float m13 = params.z;
    float m23 = params.w;

    if (isotropic) {
      ltc.m11 = m11;
      ltc.m22 = m11;
      ltc.m13 = 0.0f;
      ltc.m23 = 0.0f;
    }
    else {
      ltc.m11 = m11;
      ltc.m22 = m22;
      ltc.m13 = m13;
      ltc.m23 = m23;
    }
    ltc.update();
  }
};

template<typename LUT> float error_(const FitLTC &fitter, const LUT &lut, const float3 L)
{
  /* Geometric normal. */
  constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

  const BsdfEval eval_closure = lut.closure.bsdf_eval(N, L, lut.V);
  const BsdfEval eval_ltc = fitter.ltc.eval(L);
  const float error = abs(eval_closure.throughput - eval_ltc.throughput);

  /* L3 error with MIS weight. */
  return (error * error * error) / (eval_ltc.pdf + eval_closure.pdf);
}

template float error_<GGXBrdfClosure>(const FitLTC &, const GGXBrdfClosure &, const float3);
template float error_<OrenNayarClosure>(const FitLTC &, const OrenNayarClosure &, const float3);

template<typename LUT> float compute_error(FitLTC &fitter, const float4 params, const LUT &lut)
{
  fitter.update(params);

  float error = 0.0f;
  constexpr int Nsample = 50;

  /* Importance sample LTC. */
  for (int j = 0; j < Nsample; ++j) {
    for (int i = 0; i < Nsample; ++i) {
      const float U1 = (i + 0.5f) / float(Nsample);
      const float U2 = (j + 0.5f) / float(Nsample);

      const float3 L = fitter.ltc.sample_light(U1, U2);
      error += error_(fitter, lut, L);
    }
  }

  constexpr uint sample_count = uint(Nsample * Nsample);
  /* Importance sample BRDF. */
  for (uint i = 1u; i <= sample_count; i++) {
    const float3 rand = sample_cylinder(random::hammersley_2d(i, sample_count));
    const float3 L = lut.closure.bsdf_sample(rand, lut.V).direction;
    error += error_(fitter, lut, L);
  }

  return error / float(Nsample * Nsample);
}

template float compute_error<GGXBrdfClosure>(FitLTC &, const float4, const GGXBrdfClosure &);
template float compute_error<OrenNayarClosure>(FitLTC &, const float4, const OrenNayarClosure &);

// Downhill simplex solver:
// http://en.wikipedia.org/wiki/Nelder%E2%80%93Mead_method#One_possible_variation_of_the_NM_algorithm
// using the termination criterion from Numerical Recipes in C++ (3rd Ed.)
template<typename Closure>
float NelderMead(float4 &pmin,
                 const float4 start,
                 float delta,
                 float tolerance,
                 int maxIters,
                 FitLTC objectiveFn,
                 const Closure &closure)
{
  constexpr int DIM = 4;
  // standard coefficients from Nelder-Mead
  const float reflect = 1.0f;
  const float expand = 2.0f;
  const float contract = 0.5f;
  const float shrink = 0.5f;

  constexpr int NB_POINTS = DIM + 1;

  float4 s[NB_POINTS];
  float f[NB_POINTS];

  // initialise simplex
  s[0] = start;
  for (int i = 1; i < NB_POINTS; i++) {
    s[i] = start;
    s[i][i - 1] += delta;
  }

  // evaluate function at each point on simplex
  for (int i = 0; i < NB_POINTS; i++) {
    f[i] = compute_error(objectiveFn, s[i], closure);
  }

  int lo = 0, hi, nh;

  for (int j = 0; j < maxIters; j++) {
    // find lowest, highest and next highest
    lo = hi = nh = 0;
    for (int i = 1; i < NB_POINTS; i++) {
      if (f[i] < f[lo]) {
        lo = i;
      }
      if (f[i] > f[hi]) {
        nh = hi;
        hi = i;
      }
      else if (f[i] > f[nh]) {
        nh = i;
      }
    }

    // stop if we've reached the required tolerance level
    float a = abs(f[lo]);
    float b = abs(f[hi]);
    if (2.0f * abs(a - b) < (a + b) * tolerance) {
      break;
    }

    // compute centroid (excluding the worst point)
    float4 o = float4(0.0f);
    for (int i = 0; i < NB_POINTS; i++) {
      if (i == hi) {
        continue;
      }
      o += s[i];
    }

    for (int i = 0; i < DIM; i++) {
      o[i] /= DIM;
    }

    // reflection
    float4 r;
    for (int i = 0; i < DIM; i++) {
      r[i] = o[i] + reflect * (o[i] - s[hi][i]);
    }

    float fr = compute_error(objectiveFn, r, closure);
    if (fr < f[nh]) {
      if (fr < f[lo]) {
        // expansion
        float4 e;
        for (int i = 0; i < DIM; i++) {
          e[i] = o[i] + expand * (o[i] - s[hi][i]);
        }

        float fe = compute_error(objectiveFn, e, closure);
        if (fe < fr) {
          s[hi] = e;
          f[hi] = fe;
          continue;
        }
      }

      s[hi] = r;
      f[hi] = fr;
      continue;
    }

    // contraction
    float4 c;
    for (int i = 0; i < DIM; i++) {
      c[i] = o[i] - contract * (o[i] - s[hi][i]);
    }

    float fc = compute_error(objectiveFn, c, closure);
    if (fc < f[hi]) {
      s[hi] = c;
      f[hi] = fc;
      continue;
    }

    // reduction
    for (int k = 0; k < NB_POINTS; k++) {
      if (k == lo) {
        continue;
      }
      for (int i = 0; i < DIM; i++) {
        s[k][i] = s[lo][i] + shrink * (s[k][i] - s[lo][i]);
      }
      f[k] = compute_error(objectiveFn, s[k], closure);
    }
  }

  // return best point and its value
  pmin = s[lo];
  return f[lo];
}
template float NelderMead<GGXBrdfClosure>(
    float4 &, const float4, float, float, int, FitLTC, const GGXBrdfClosure &);
template float NelderMead<OrenNayarClosure>(
    float4 &, const float4, float, float, int, FitLTC, const OrenNayarClosure &);

template<typename C>
void fit(LtcData &ltc, const C &closure, const bool isotropic = false, const float epsilon = 0.05f)
{
  float4 resultFit;

  FitLTC fitter = FitLTC::init(ltc, isotropic);

  /* Find best-fit LTC lobe (scale, alphax, alphay) */
  /* float error = */ NelderMead(resultFit, ltc.pack(), epsilon, 1e-5f, 100, fitter, closure);

  /* Update LTC with best fitting values */
  fitter.update(resultFit);
  ltc = fitter.ltc;
}
template void fit<GGXBrdfClosure>(LtcData &, const GGXBrdfClosure &, const bool, const float);
template void fit<OrenNayarClosure>(LtcData &, const OrenNayarClosure &, const bool, const float);

template<typename LUT> float4 fitLTC(const LUT &lut, LtcData &ltc)
{
  const bool isotropic = (lut.V.z == 1.0f);

  const float3 L = average_light_dir(lut);
  ltc.X = float3(L.z, 0, -L.x);
  ltc.Y = float3(0.0f, 1.0f, 0.0f);
  ltc.Z = L;
  ltc.update();

  fit(ltc, lut, isotropic);

  return ltc.pack_inverse();
}
template float4 fitLTC<GGXBrdfClosure>(const GGXBrdfClosure &, LtcData &);
template float4 fitLTC<OrenNayarClosure>(const OrenNayarClosure &, LtcData &);

[[compute]] [[local_size(LUT_WORKGROUP_SIZE, LUT_WORKGROUP_SIZE)]]
void comp_main([[global_invocation_id]] const uint3 global_id, [[resource_table]] LUT &lut)
{
  /* Make sure coordinates are covering the whole [0..1] range at texel center. */
  float3 lut_normalized_coordinate = float3(global_id) / float3(lut.extent - 1);

  /* Make sure missing cases are noticeable. */
  float4 result = float4(-1);
  switch (PrecomputeType(lut.type)) {
    case LUT_GGX_BRDF_SPLIT_SUM:
      result = integrate(GGXBrdfClosure::from_coords(lut_normalized_coordinate));
      break;
    case LUT_GGX_BSDF_SPLIT_SUM:
      result = integrate(GGXBsdfLUT::from_coords(lut_normalized_coordinate));
      break;
    case LUT_GGX_BTDF_IOR_GT_ONE:
      result = integrate(GGXBtdfGt1Closure::from_coords(lut_normalized_coordinate));
      break;
    case LUT_BURLEY_SSS_PROFILE:
      result = burley_sss_translucency(lut_normalized_coordinate);
      break;
    case LUT_RANDOM_WALK_SSS_PROFILE:
      result = random_walk_sss_translucency(lut_normalized_coordinate);
      break;
    case LUT_OREN_NAYAR:
      result = integrate(OrenNayarClosure::from_coords(lut_normalized_coordinate));
      break;
    case LUT_OREN_NAYAR_LTC: {
      /* Initialize with diffuse. Note this works because Oren Nayar is diffuse. For GGX we need
       * better initial guess. */
      LtcData init;
      init.m13 = init.m23 = 0.0f;
      init.m11 = init.m22 = 1.0f;
      result = fitLTC(OrenNayarClosure::from_ltc_coords(lut_normalized_coordinate), init);
      break;
    }
    case LUT_GGX_BRDF_LTC: {
      /* NOTE: this part is not tested due to bad initialization. */
      LtcData init;
      init.m13 = init.m23 = 0.0f;
      init.m11 = init.m22 = 1.0f;
      result = fitLTC(GGXBrdfClosure::from_ltc_coords(lut_normalized_coordinate), init);
      break;
    }
  }

  imageStore(lut.image, int3(global_id), result);
}

/** \} */

PipelineCompute comp(comp_main);

}  // namespace eevee::lut
