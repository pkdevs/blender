/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include "kernel/closure/bsdf_microfacet.h"
#include "kernel/closure/bsdf_util.h"
#include "kernel/sample/mapping.h"
#include "kernel/svm/types.h"
#include "kernel/util/colorspace.h"

CCL_NAMESPACE_BEGIN

/* Diffraction grating BSDF.
 *
 * Models a periodic reflective surface relief grating (a CD/DVD data track or an
 * embossed holographic/OVD foil): a regular groove pattern with period `d`, not a
 * random microrough surface. Each diffraction order m obeys the grating equation
 *
 *   sin(theta_o) = sin(theta_i) + m * lambda / d
 *
 * and carries a fraction eta_m(lambda) of the reflected energy, set by the shape of
 * the groove cross-section. Two groove profiles are supported:
 *
 * - Sinusoidal: continuous cosine profile, thin phase grating (Raman-Nath) model.
 *   eta_m = J_m(A)^2, A = (2*pi/lambda) * h * cos(theta_i). Sum over all m is exactly
 *   1 (Bessel identity), so the profile never loses energy by itself.
 *
 * - Rectangular: two-level pit/land profile, the actual cross-section of a stamped
 *   CD/DVD track or OVD foil. eta_m is the squared Fourier coefficient of a binary
 *   phase function; sum over all m is exactly 1 by Parseval's theorem (the surface
 *   is a pure phase reflector, so it cannot absorb energy by itself either).
 *
 * In both cases only orders that can physically leave the surface (propagating
 * orders, |sin(theta_o)| <= 1) contribute; the rest are evanescent and carry no
 * energy, so the real reflectance is always <= the Fresnel reflectance already
 * baked into the closure's weight.
 *
 * Because the order an incoming ray takes depends on wavelength, this closure needs
 * one concrete wavelength per shading point. It reuses the same hero-wavelength
 * sampling that dispersion already uses in `bsdf_glass_ior()`: one wavelength is
 * drawn once per path (from `sd->rand_wavelength`) and baked into the closure at
 * setup time, and the same "this path is now spectral" bookkeeping is reused
 * (`SR_BSDF_HAS_DISPERSION`) so the path throughput gets the correct spectral
 * weighting.
 *
 * Each order's angular spread around its ideal direction is modeled as a GGX lobe
 * using the closure's roughness, reusing `bsdf_D`/`bsdf_G` from bsdf_microfacet.h.
 * The lobe for order m is centered using a generalized half-vector (Stam, 1999,
 * "Diffraction Shaders"): the ordinary half-vector `wi + wo`, offset by the
 * grating's momentum shift along the grating vector before normalizing.
 *
 * The groove's own tangent direction (input as `T`, exactly like the anisotropic
 * GGX case) sets the grating vector via the same tangent-frame helper the
 * anisotropic Glossy BSDF uses; on a disc with radial UVs, the generated tangent
 * already runs circumferentially, so a CD's concentric groove pattern falls out for
 * free without any texture input. */

#define DIFFRACTION_MAX_ORDER 8

struct DiffractionBsdf {
  SHADER_CLOSURE_BASE;

  float3 T; /* Groove tangent; the grating vector is derived from this, like Glossy's anisotropy. */

  float roughness;       /* GGX roughness of each order's angular lobe. */
  float period_um;       /* Grating period d, in micrometers (CD track pitch is about 1.6um). */
  float groove_depth_nm; /* Groove depth h, in nanometers; controls the per-order energy split. */
  float duty_cycle;      /* Pit/land width fraction; only used by the rectangular profile. */
  float wavelength_um;   /* Hero wavelength for this path, baked in at setup time. */
  float energy_scale;    /* Single-scatter energy compensation; 1.0 until wired up, see setup. */

  int profile; /* DiffractionProfile. */
};

static_assert(sizeof(ShaderClosure) >= sizeof(DiffractionBsdf), "DiffractionBsdf is too large!");

/* How far above the highest order actually needed to seed Miller's algorithm (see
 * diffraction_fill_sinusoidal_efficiencies below). Forward recurrence for J_n(A) is
 * numerically unstable whenever an order exceeds A -- routine here, since A shrinks
 * to 0 at grazing incidence regardless of order count -- so this closure always
 * recurses downward from a seed order safely above every order it needs, then
 * normalizes with a closed-form identity. The margin has to grow with A itself (a
 * deep groove can push A into the tens), capped so the loop stays bounded. */
ccl_device_inline int diffraction_bessel_miller_start(const int max_order, const float A)
{
  const int margin = max(10, (int)(1.6f * A) + 8);
  return min(max_order + margin, 64);
}

/* Fills eta_out[0..2*max_order] with J_m(A)^2 for m = -max_order..max_order
 * (eta_out[max_order + m]), i.e. the sinusoidal profile's per-order efficiency
 * (see file header) for every order at once, sharing a single downward recurrence
 * instead of an independent series per order.
 *
 * Miller's algorithm: seed J_{n+1}=0, J_n=1 (arbitrary) at some n = n_start well
 * above every order needed, then apply the Bessel recurrence
 *   J_{n-1}(A) = (2n/A) J_n(A) - J_{n+1}(A)
 * downward to n=0. Downward recurrence is the numerically stable direction (unlike
 * going upward from J_0/J_1, which blows up once n exceeds A); this is the standard
 * technique for computing Bessel functions of a given order range for exactly that
 * reason. The raw values this produces aren't yet the true J_n -- they're only
 * correct up to an overall scale -- so they're normalized using the closed-form
 * identity J_0(A) + 2*sum_{k=1,2,3,...} J_2k(A) = 1 (from the Bessel generating
 * function at t=1), accumulated during the same downward pass. */
ccl_device_inline void diffraction_fill_sinusoidal_efficiencies(const float A,
                                                                 const int max_order,
                                                                 ccl_private float *eta_out)
{
  /* Below this, J_0(A) is already ~1 and every higher order is already negligible;
   * skip the recurrence entirely rather than push it into its own small-argument
   * instability (the ratio 2n/A used below grows without bound as A -> 0). */
  if (A < 0.15f) {
    for (int m = -max_order; m <= max_order; m++) {
      eta_out[m + max_order] = (m == 0) ? 1.0f : 0.0f;
    }
    return;
  }

  const int n_start = diffraction_bessel_miller_start(max_order, A);
  float raw[DIFFRACTION_MAX_ORDER + 1];
  for (int i = 0; i <= max_order; i++) {
    raw[i] = 0.0f;
  }

  float J_next = 0.0f; /* J_{n_start + 1}. */
  float J_curr = 1.0f; /* J_{n_start}, arbitrary seed. */
  float sum = 0.0f;
  for (int n = n_start; n >= 1; n--) {
    const float J_prev = (2.0f * (float)n / A) * J_curr - J_next;
    if (n - 1 <= max_order) {
      raw[n - 1] = J_prev;
    }
    if (((n - 1) & 1) == 0 && (n - 1) > 0) {
      sum += 2.0f * J_prev;
    }
    J_next = J_curr;
    J_curr = J_prev;
  }
  sum += raw[0];
  const float inv_sum = (fabsf(sum) > 1e-8f) ? 1.0f / sum : 0.0f;

  for (int m = -max_order; m <= max_order; m++) {
    const float J = raw[abs(m)] * inv_sum;
    eta_out[m + max_order] = sqr(J);
  }
}

ccl_device_inline float diffraction_efficiency_rectangular(const int m,
                                                            const float wavelength_nm,
                                                            const float cos_theta_i,
                                                            const float groove_depth_nm,
                                                            const float duty_cycle)
{
  /* Round-trip phase delay of the pit floor relative to the land. */
  const float phi = (M_2PI_F / wavelength_nm) * 2.0f * groove_depth_nm * cos_theta_i;
  const float g = duty_cycle;

  if (m == 0) {
    const float re = (1.0f - g) + g * cosf(phi);
    const float im = g * sinf(phi);
    return sqr(re) + sqr(im);
  }

  const float mg = (float)m * g;
  const float sinc_mg = (fabsf(mg) < 1e-6f) ? 1.0f : sinf(M_PI_F * mg) / (M_PI_F * mg);
  const float amp = 2.0f * sinf(0.5f * phi) * g * sinc_mg;
  return sqr(amp);
}

/* Fills eta_out[0..2*max_order] with the efficiency of every order at once (indexed
 * as eta_out[max_order + m], m = -max_order..max_order), so eval() and sample() can
 * look orders up instead of recomputing each one independently. The rectangular
 * profile is already O(1) per order (a couple of trig calls), so it is simply
 * evaluated order by order here; the sinusoidal profile shares one Bessel
 * recurrence across all orders instead (see diffraction_fill_sinusoidal_efficiencies). */
ccl_device_inline void diffraction_fill_efficiencies(const float wavelength_nm,
                                                      const float cos_theta_i,
                                                      const float groove_depth_nm,
                                                      const float duty_cycle,
                                                      const int profile,
                                                      const int max_order,
                                                      ccl_private float *eta_out)
{
  if (profile == DIFFRACTION_PROFILE_RECTANGULAR) {
    for (int m = -max_order; m <= max_order; m++) {
      eta_out[m + max_order] = diffraction_efficiency_rectangular(
          m, wavelength_nm, cos_theta_i, groove_depth_nm, duty_cycle);
    }
    return;
  }

  const float A = (M_2PI_F / wavelength_nm) * groove_depth_nm * cos_theta_i;
  diffraction_fill_sinusoidal_efficiencies(A, max_order, eta_out);
}

/* Highest order that can still leave the surface for this period/wavelength pair;
 * anything beyond it is evanescent (see file header) and skipped entirely. */
ccl_device_inline int diffraction_max_propagating_order(const float period_nm,
                                                         const float wavelength_nm)
{
  const int bound = (int)ceilf(period_nm / wavelength_nm) + 1;
  return min(bound, DIFFRACTION_MAX_ORDER);
}

ccl_device int bsdf_diffraction_setup(ccl_private ShaderData *sd, ccl_private DiffractionBsdf *bsdf)
{
  bsdf->roughness = saturatef(bsdf->roughness);
  bsdf->period_um = fmaxf(bsdf->period_um, 1e-4f);
  bsdf->groove_depth_nm = fmaxf(bsdf->groove_depth_nm, 0.0f);
  bsdf->duty_cycle = saturatef(bsdf->duty_cycle);
  /* TODO: reuse the GGX single-scatter energy compensation tables (energy_scale in
   * bsdf_microfacet.h) for the loss inside each order's own GGX lobe at high
   * roughness. The budget *between* orders is already handled by construction
   * (Bessel/Parseval identities above), which is a separate issue from this one. */
  bsdf->energy_scale = 1.0f;

#ifdef __SPECTRAL__
  /* A grating needs one concrete wavelength per path, exactly like glass dispersion.
   * Reuse the same per-path hero wavelength and spectral-path bookkeeping. */
  bsdf->wavelength_um = sample_wavelength(sd->rand_wavelength);
  sd->runtime_flag |= SR_BSDF_HAS_DISPERSION;
#else
  /* No wavelength sampling available in this build: fall back to a fixed mid-visible
   * wavelength. The lobe shape stays correct, but the rainbow color split is lost. */
  bsdf->wavelength_um = 0.55f;
#endif

  bsdf->type = CLOSURE_BSDF_DIFFRACTION_ID;
  return SR_BSDF | SR_BSDF_HAS_EVAL;
}

ccl_device void bsdf_diffraction_blur(ccl_private ShaderClosure *sc, const float roughness)
{
  ccl_private DiffractionBsdf *bsdf = (ccl_private DiffractionBsdf *)sc;
  bsdf->roughness = fmaxf(roughness, bsdf->roughness);
}

ccl_device Spectrum bsdf_diffraction_eval(KernelGlobals kg,
                                          const ccl_private ShaderClosure *sc,
                                          const float3 wi,
                                          const float3 wo,
                                          ccl_private float *pdf)
{
  const ccl_private DiffractionBsdf *bsdf = (const ccl_private DiffractionBsdf *)sc;
  const float3 N = bsdf->N;
  const float cos_NI = dot(N, wi);
  const float cos_NO = dot(N, wo);
  if (cos_NI <= 0.0f || cos_NO <= 0.0f) {
    *pdf = 0.0f;
    return zero_spectrum();
  }

  float3 X, Y;
  if (is_zero(bsdf->T)) {
    /* No groove tangent available (e.g. mesh without UVs): fall back to an arbitrary
     * but consistent basis, exactly like isotropic Glossy does without a tangent. */
    make_orthonormals(N, &X, &Y);
  }
  else {
    make_orthonormals_tangent(N, bsdf->T, &X, &Y);
  }

  const float period_nm = bsdf->period_um * 1000.0f;
  const float wavelength_nm = bsdf->wavelength_um * 1000.0f;
  const float alpha2 = sqr(fmaxf(bsdf->roughness, 1e-4f));
  const int max_order = diffraction_max_propagating_order(period_nm, wavelength_nm);

  float eta_arr[2 * DIFFRACTION_MAX_ORDER + 1];
  diffraction_fill_efficiencies(wavelength_nm,
                                cos_NI,
                                bsdf->groove_depth_nm,
                                bsdf->duty_cycle,
                                bsdf->profile,
                                max_order,
                                eta_arr);

  Spectrum sum = zero_spectrum();
  float pdf_sum = 0.0f;
  float weight_sum = 0.0f;

  const float Ix = dot(X, wi);
  const float Iy = dot(Y, wi);

  for (int m = -max_order; m <= max_order; m++) {
    const float eta_m = eta_arr[m + max_order];
    if (eta_m < 1e-6f) {
      continue;
    }

    /* Virtual incident direction carrying this order's grating momentum shift
     * (same construction bsdf_diffraction_sample() uses to pick a direction).
     * Unlike shifting wi+wo directly, this stays a real unit vector, so the
     * ordinary half-vector identity dot(wi_v,H) == dot(wo,H) holds and the
     * classic Torrance-Sparrow formula below stays energy-conserving -- just
     * evaluated at an effective incidence angle instead of the real one. */
    const float shift = (float)m * wavelength_nm / period_nm;
    const float Ix_v = Ix + shift;
    if (sqr(Ix_v) + sqr(Iy) > 1.0f) {
      continue; /* Evanescent for this order at this incidence direction. */
    }
    const float cos_NI_v = safe_sqrtf(1.0f - sqr(Ix_v) - sqr(Iy));
    const float3 wi_v = Ix_v * X + Iy * Y + cos_NI_v * N;

    const float3 H_un = wi_v + wo;
    const float H_len = len(H_un);
    if (H_len < 1e-6f) {
      continue;
    }
    const float3 H = H_un / H_len;
    const float cos_NH = dot(N, H);
    if (cos_NH <= 0.0f) {
      continue;
    }

    const float D = bsdf_D<MicrofacetType::GGX>(alpha2, cos_NH);
    const float G = bsdf_G<MicrofacetType::GGX>(alpha2, cos_NI_v, cos_NO);
    /* Matches bsdf_microfacet_eval()'s reflection formula exactly: divide by
     * cos_NI_v only, not by cos_NO too. An extra 1/cos_NO here would blow up
     * for grazing wo and silently cancel out in any test that integrates with
     * the usual cos_theta_o weighting -- which is exactly why this stayed
     * hidden through every prior furnace-test check. */
    const float lobe = eta_m * bsdf->energy_scale * D * G / (4.0f * cos_NI_v);
    sum += make_spectrum(lobe);

    const float cos_HI = dot(H, wi_v);
    if (cos_HI > 0.0f) {
      /* Standard VNDF-sampling pdf for this order's lobe (matches
       * bsdf_microfacet_eval()'s pdf exactly: dot(wi_v,H) == dot(wo,H) for this
       * well-formed half-vector, so the usual cos_HI/cos_HO ratio cancels to 1),
       * weighted by how often bsdf_diffraction_sample() would pick this order. */
      const float G1 = bsdf_G<MicrofacetType::GGX>(alpha2, cos_NI_v);
      pdf_sum += eta_m * D * G1 / (4.0f * cos_NI_v);
    }
    weight_sum += eta_m;
  }

  *pdf = (weight_sum > 0.0f) ? pdf_sum / weight_sum : 0.0f;
  return sum;
}

ccl_device int bsdf_diffraction_sample(KernelGlobals kg,
                                       const ccl_private ShaderClosure *sc,
                                       const float3 Ng,
                                       const float3 wi,
                                       const float3 rand,
                                       ccl_private Spectrum *eval,
                                       ccl_private float3 *wo,
                                       ccl_private float *pdf,
                                       ccl_private float2 *sampled_roughness,
                                       ccl_private float *eta)
{
  *eta = 1.0f; /* Reflective closure only, no index-of-refraction change. */

  const ccl_private DiffractionBsdf *bsdf = (const ccl_private DiffractionBsdf *)sc;
  *sampled_roughness = make_float2(bsdf->roughness, bsdf->roughness);

  const float3 N = bsdf->N;
  const float cos_NI = dot(N, wi);
  if (cos_NI <= 0.0f) {
    return LABEL_NONE;
  }

  float3 X, Y;
  if (is_zero(bsdf->T)) {
    /* No groove tangent available (e.g. mesh without UVs): fall back to an arbitrary
     * but consistent basis, exactly like isotropic Glossy does without a tangent. */
    make_orthonormals(N, &X, &Y);
  }
  else {
    make_orthonormals_tangent(N, bsdf->T, &X, &Y);
  }
  const float Ix = dot(X, wi);
  const float Iy = dot(Y, wi);

  const float period_nm = bsdf->period_um * 1000.0f;
  const float wavelength_nm = bsdf->wavelength_um * 1000.0f;
  const int max_order = diffraction_max_propagating_order(period_nm, wavelength_nm);
  const int order_count = 2 * max_order + 1;

  float eta_arr[2 * DIFFRACTION_MAX_ORDER + 1];
  diffraction_fill_efficiencies(
      wavelength_nm, cos_NI, bsdf->groove_depth_nm, bsdf->duty_cycle, bsdf->profile, max_order, eta_arr);

  /* Pick which order to sample, weighted by its efficiency at this incidence angle
   * (the same kind of discrete lobe choice as picking reflection vs. transmission
   * in a glass BSDF, just with more than two options). */
  float weights[2 * DIFFRACTION_MAX_ORDER + 1];
  float weight_sum = 0.0f;
  for (int i = 0; i < order_count; i++) {
    const int m = i - max_order;
    /* Must match eval()'s evanescent check: an order can have nonzero efficiency
     * from the depth/wavelength formula alone while still being geometrically
     * evanescent at this incidence direction. Skipping it only by its pdf later
     * is not enough -- selecting it here would feed a non-unit "shifted incident
     * direction" into VNDF sampling below (its z-component gets clamped to 0
     * without renormalizing the rest), producing an undefined H and wo. */
    const float shift_i = (float)m * wavelength_nm / period_nm;
    const float Ix_shifted_i = Ix + shift_i;
    if (sqr(Ix_shifted_i) + sqr(Iy) > 1.0f) {
      weights[i] = 0.0f;
      continue;
    }
    const float eta_m = eta_arr[i];
    weights[i] = eta_m;
    weight_sum += eta_m;
  }
  if (weight_sum <= 0.0f) {
    return LABEL_NONE;
  }

  float u = rand.z * weight_sum;
  int m = -max_order;
  for (int i = 0; i < order_count; i++) {
    if (u < weights[i] || i == order_count - 1) {
      m = i - max_order;
      break;
    }
    u -= weights[i];
  }

  /* Sample a microfacet normal around the virtual (shifted) incident direction
   * for this order, then reflect that same virtual direction about it below.
   * The VNDF sampling itself is still an approximation of the true per-order
   * lobe shape (see bsdf_diffraction_eval() for the exact formula, which is
   * the actual source of truth for the returned value and pdf), but the
   * reflected direction it produces must at least be geometrically consistent
   * with what that pdf assumes. */
  const float shift = (float)m * wavelength_nm / period_nm;
  const float Ix_shifted = Ix + shift;
  const float Iz_shifted = safe_sqrtf(fmaxf(0.0f, 1.0f - sqr(Ix_shifted) - sqr(Iy)));
  const float3 local_I_shifted = make_float3(Ix_shifted, Iy, Iz_shifted);

  const float alpha = fmaxf(bsdf->roughness, 1e-4f);
  const float3 local_H = microfacet_ggx_sample_vndf(local_I_shifted, alpha, alpha, make_float2(rand));
  const float3 H = to_global(local_H, X, Y, N);

  /* Reflect the virtual (shifted) incident direction, not the real wi: H was
   * sampled around wi_v, so only reflecting wi_v about it reproduces the
   * dot(wi_v,H) == dot(wo,H) identity that bsdf_diffraction_eval()'s pdf above
   * assumes. Reflecting the real wi here would silently generate directions
   * close to plain order-0 specular regardless of which order m was picked,
   * mismatching eval()'s pdf for the direction actually produced. */
  const float3 wi_v = local_I_shifted.x * X + local_I_shifted.y * Y + local_I_shifted.z * N;
  const float cos_HI = dot(H, wi_v);
  if (cos_HI <= 0.0f) {
    return LABEL_NONE;
  }
  *wo = 2.0f * cos_HI * H - wi_v;

  const float cos_NO = dot(N, *wo);
  const float cos_NgO = dot(Ng, *wo);
  if (cos_NO <= 0.0f || cos_NgO <= 0.0f) {
    return LABEL_NONE;
  }

  *eval = bsdf_diffraction_eval(kg, sc, wi, *wo, pdf);
  if (*pdf <= 0.0f) {
    return LABEL_NONE;
  }

  return LABEL_REFLECT | LABEL_GLOSSY;
}

CCL_NAMESPACE_END
