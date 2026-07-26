/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/** \file
 * \ingroup svm
 *
 * Penny Noise Cycles SVM kernel: self-contained noise math and dispatcher,
 * mirroring the CPU implementation in
 * source/blender/blenlib/intern/noise_penny.cc. Lacunarity is locked to 2 for
 * tiling continuity, Scale inputs are percentages, and Levels are applied
 * per-noise rather than as a global post-process. */

#pragma once

CCL_NAMESPACE_BEGIN

/* -------------------------------------------------------------------- */
/** \name Foundation: fade, gradient, tileable 4D Perlin
 *
 * Core Perlin noise primitives. The tileable 4D Perlin function is the
 * single noise source that every Penny noise type builds upon.
 * \{ */

/** Quintic fade curve (Perlin's improved interpolant): 6t^5 - 15t^4 + 10t^3.
 *  Zero first and second derivatives at t=0 and t=1, eliminating lattice artifacts. */
ccl_device_inline float penny_fade(float t)
{
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/** 32-direction gradient for 4D Perlin.
 *  Selects 3 of 4 axes via bits 3-4 of hash, applies sign flips from bits 0-2.
 *  Inlined to avoid SSE-branch helpers in noise.h. */
ccl_device_inline float penny_grad(uint hash, float x, float y, float z, float w)
{
  uint h = hash & 31;
  uint sel = h >> 3;
  float a, b, c;
  if (sel == 0) {
    a = y;
    b = z;
    c = w;
  }
  else if (sel == 1) {
    a = x;
    b = z;
    c = w;
  }
  else if (sel == 2) {
    a = x;
    b = y;
    c = w;
  }
  else {
    a = x;
    b = y;
    c = z;
  }
  float sa = (h & 4u) ? a : -a;
  float sb = (h & 2u) ? b : -b;
  float sc = (h & 1u) ? c : -c;
  return sa + sb + sc;
}

/** Tileable 4D Perlin noise (single octave, 3-channel output).
 *  Lattice coordinates wrap modulo \a period for seamless tiling. Three
 *  decorrelated channels are produced by right-shifting each corner hash
 *  by `c * 5` bits before gradient selection, avoiding 3× hashing cost.
 *  Uses `hash_uint4` (bit-identical to blenlib's `blender::noise::hash`). */
ccl_device_noinline float3
penny_perlin_4d_tile(const float x, const float y, const float z, const float w, const int4 period)
{
  /* Floor + fractional. */
  const int X_floor = floor_to_int(x);
  const int Y_floor = floor_to_int(y);
  const int Z_floor = floor_to_int(z);
  const int W_floor = floor_to_int(w);
  const float fx = x - (float)X_floor;
  const float fy = y - (float)Y_floor;
  const float fz = z - (float)Z_floor;
  const float fw = w - (float)W_floor;

  /* Branchless positive modulo. */
  const int px = max(period.x, 1);
  const int py = max(period.y, 1);
  const int pz = max(period.z, 1);
  const int pw = max(period.w, 1);
  int X = X_floor % px;
  X += (X >> 31) & px;
  int Y = Y_floor % py;
  Y += (Y >> 31) & py;
  int Z = Z_floor % pz;
  Z += (Z >> 31) & pz;
  int W = W_floor % pw;
  W += (W >> 31) & pw;
  const int X1 = (X + 1 == px) ? 0 : X + 1;
  const int Y1 = (Y + 1 == py) ? 0 : Y + 1;
  const int Z1 = (Z + 1 == pz) ? 0 : Z + 1;
  const int W1 = (W + 1 == pw) ? 0 : W + 1;

  const float u = penny_fade(fx);
  const float v = penny_fade(fy);
  const float t = penny_fade(fz);
  const float s = penny_fade(fw);

  const float fxm = fx - 1.0f, fym = fy - 1.0f, fzm = fz - 1.0f, fwm = fw - 1.0f;

  /* Compute 16 hashes once, derive 3 gradients per corner. */
  const uint h0 = hash_uint4(X, Y, Z, W);
  const uint h1 = hash_uint4(X1, Y, Z, W);
  const uint h2 = hash_uint4(X, Y1, Z, W);
  const uint h3 = hash_uint4(X1, Y1, Z, W);
  const uint h4 = hash_uint4(X, Y, Z1, W);
  const uint h5 = hash_uint4(X1, Y, Z1, W);
  const uint h6 = hash_uint4(X, Y1, Z1, W);
  const uint h7 = hash_uint4(X1, Y1, Z1, W);
  const uint h8 = hash_uint4(X, Y, Z, W1);
  const uint h9 = hash_uint4(X1, Y, Z, W1);
  const uint h10 = hash_uint4(X, Y1, Z, W1);
  const uint h11 = hash_uint4(X1, Y1, Z, W1);
  const uint h12 = hash_uint4(X, Y, Z1, W1);
  const uint h13 = hash_uint4(X1, Y, Z1, W1);
  const uint h14 = hash_uint4(X, Y1, Z1, W1);
  const uint h15 = hash_uint4(X1, Y1, Z1, W1);

  float channels[3];
  for (int c = 0; c < 3; c++) {
    const uint sh = (uint)(c * 5);
    const float n0 = penny_grad(h0 >> sh, fx, fy, fz, fw);
    const float n1 = penny_grad(h1 >> sh, fxm, fy, fz, fw);
    const float n2 = penny_grad(h2 >> sh, fx, fym, fz, fw);
    const float n3 = penny_grad(h3 >> sh, fxm, fym, fz, fw);
    const float n4 = penny_grad(h4 >> sh, fx, fy, fzm, fw);
    const float n5 = penny_grad(h5 >> sh, fxm, fy, fzm, fw);
    const float n6 = penny_grad(h6 >> sh, fx, fym, fzm, fw);
    const float n7 = penny_grad(h7 >> sh, fxm, fym, fzm, fw);
    const float n8 = penny_grad(h8 >> sh, fx, fy, fz, fwm);
    const float n9 = penny_grad(h9 >> sh, fxm, fy, fz, fwm);
    const float n10 = penny_grad(h10 >> sh, fx, fym, fz, fwm);
    const float n11 = penny_grad(h11 >> sh, fxm, fym, fz, fwm);
    const float n12 = penny_grad(h12 >> sh, fx, fy, fzm, fwm);
    const float n13 = penny_grad(h13 >> sh, fxm, fy, fzm, fwm);
    const float n14 = penny_grad(h14 >> sh, fx, fym, fzm, fwm);
    const float n15 = penny_grad(h15 >> sh, fxm, fym, fzm, fwm);

    const float x00 = mix(n0, n1, u);
    const float x01 = mix(n2, n3, u);
    const float x10 = mix(n4, n5, u);
    const float x11 = mix(n6, n7, u);
    const float x20 = mix(n8, n9, u);
    const float x21 = mix(n10, n11, u);
    const float x30 = mix(n12, n13, u);
    const float x31 = mix(n14, n15, u);
    const float y0 = mix(x00, x01, v);
    const float y1 = mix(x10, x11, v);
    const float y2 = mix(x20, x21, v);
    const float y3 = mix(x30, x31, v);
    const float z0 = mix(y0, y1, t);
    const float z1 = mix(y2, y3, t);
    channels[c] = mix(z0, z1, s);
  }
  return make_float3(channels[0], channels[1], channels[2]);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend modes (Photoshop-style, 24 modes)
 *
 * Operate on [0, 1] scalars per-channel. Mode 0 = Normal (passthrough).
 * Must match the GLSL, OSL, and blenlib (`noise_penny.cc`) implementations
 * exactly. The integer mode IDs match the RNA enum in `rna_nodetree.cc`.
 * \{ */
ccl_device_inline float penny_blendReflect(float base, float blend)
{
  return (blend == 1.0f) ? blend : min(base * base / (1.0f - blend), 1.0f);
}
ccl_device_inline float penny_blendAdd(float base, float blend)
{
  return base + blend;
}
ccl_device_inline float penny_blendMultiply(float base, float blend)
{
  return base * blend;
}
ccl_device_inline float penny_blendDifference(float base, float blend)
{
  return fabsf(base - blend);
}
ccl_device_inline float penny_blendAverage(float base, float blend)
{
  return (base + blend) * 0.5f;
}
ccl_device_inline float penny_blendColorBurn(float base, float blend)
{
  return (blend == 0.0f) ? blend : (1.0f - ((1.0f - base) / blend));
}
ccl_device_inline float penny_blendColorDodge(float base, float blend)
{
  return (blend == 1.0f) ? blend : base / (1.0f - blend);
}
ccl_device_inline float penny_blendDarken(float base, float blend)
{
  return min(blend, base);
}
ccl_device_inline float penny_blendLighten(float base, float blend)
{
  return max(blend, base);
}
ccl_device_inline float penny_blendExclusion(float base, float blend)
{
  return base + blend - 2.0f * base * blend;
}
ccl_device_inline float penny_blendGlow(float base, float blend)
{
  return penny_blendReflect(blend, base);
}
ccl_device_inline float penny_blendOverlay(float base, float blend)
{
  return (base < 0.5f) ? (2.0f * base * blend) : (1.0f - 2.0f * (1.0f - base) * (1.0f - blend));
}
ccl_device_inline float penny_blendHardLight(float base, float blend)
{
  return penny_blendOverlay(blend, base);
}
ccl_device_inline float penny_blendVividLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendColorBurn(base, 2.0f * blend) :
                          penny_blendColorDodge(base, 2.0f * (blend - 0.5f));
}
ccl_device_inline float penny_blendHardMix(float base, float blend)
{
  return (penny_blendVividLight(base, blend) < 0.5f) ? 0.0f : 1.0f;
}
ccl_device_inline float penny_blendLinearBurn(float base, float blend)
{
  return base + blend - 1.0f;
}
ccl_device_inline float penny_blendLinearDodge(float base, float blend)
{
  return base + blend;
}
ccl_device_inline float penny_blendLinearLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendLinearBurn(base, 2.0f * blend) :
                          penny_blendLinearDodge(base, 2.0f * (blend - 0.5f));
}
ccl_device_inline float penny_blendNegation(float base, float blend)
{
  return 1.0f - fabsf(1.0f - base - blend);
}
ccl_device_inline float penny_blendNormal(float /*base*/, float blend)
{
  return blend;
}
ccl_device_inline float penny_blendPhoenix(float base, float blend)
{
  return min(base, blend) - max(base, blend) + 1.0f;
}
ccl_device_inline float penny_blendPinLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendDarken(base, 2.0f * blend) :
                          penny_blendLighten(base, 2.0f * (blend - 0.5f));
}
ccl_device_inline float penny_blendScreen(float base, float blend)
{
  return base + blend - base * blend;
}
ccl_device_inline float penny_blendSoftLight(float base, float blend)
{
  return (blend < 0.5f) ? (2.0f * base * blend + base * base * (1.0f - 2.0f * blend)) :
                          (sqrtf(base) * (2.0f * blend - 1.0f) + 2.0f * base * (1.0f - blend));
}
ccl_device_inline float penny_blendSubtract(float base, float blend)
{
  return base + blend - 1.0f;
}

ccl_device_inline float penny_blend(int mode, float base, float blend)
{
  switch (mode) {
    case 1:
      return penny_blendAdd(base, blend);
    case 2:
      return penny_blendMultiply(base, blend);
    case 3:
      return penny_blendDifference(base, blend);
    case 4:
      return penny_blendDarken(base, blend);
    case 5:
      return penny_blendLighten(base, blend);
    case 6:
      return penny_blendColorDodge(base, blend);
    case 7:
      return penny_blendColorBurn(base, blend);
    case 8:
      return penny_blendOverlay(base, blend);
    case 9:
      return penny_blendAverage(base, blend);
    case 10:
      return penny_blendExclusion(base, blend);
    case 11:
      return penny_blendGlow(base, blend);
    case 12:
      return penny_blendHardLight(base, blend);
    case 13:
      return penny_blendHardMix(base, blend);
    case 14:
      return penny_blendLinearBurn(base, blend);
    case 15:
      return penny_blendLinearDodge(base, blend);
    case 16:
      return penny_blendLinearLight(base, blend);
    case 17:
      return penny_blendNegation(base, blend);
    case 18:
      return penny_blendPhoenix(base, blend);
    case 19:
      return penny_blendPinLight(base, blend);
    case 20:
      return penny_blendReflect(base, blend);
    case 21:
      return penny_blendScreen(base, blend);
    case 22:
      return penny_blendSoftLight(base, blend);
    case 23:
      return penny_blendSubtract(base, blend);
    case 24:
      return penny_blendVividLight(base, blend);
    default:
      return penny_blendNormal(base, blend); /* 0 = Normal */
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name darf_* helpers — shaping primitives
 *
 * Named after the DARF Noise Displace addon these originated from. Core
 * tonal shaping: unclamped remap (noxstep), clamped remap (boxstep),
 * Hermite smoothstep, power-curve bias, linear map, and clip.
 * Must match GLSL and OSL implementations exactly.
 * \{ */
ccl_device_inline float darf_mix(float a, float b, float l)
{
  return ((1.0f - l) * a) + (b * l);
}
ccl_device_inline float darf_clip(float n, float lower, float upper)
{
  return max(lower, min(n, upper));
}
ccl_device_inline float darf_boxstep(float a, float b, float x)
{
  if (b == a) {
    return (x < a) ? 0.0f : 1.0f;
  }
  x = (x - a) / (b - a);
  if (x < 0.0f) {
    return 0.0f;
  }
  if (x > 1.0f) {
    return 1.0f;
  }
  return x;
}
ccl_device_inline float darf_smoothstep(float a, float b, float x)
{
  if (x < a) {
    return 0.0f;
  }
  if (x >= b) {
    return 1.0f;
  }
  x = (x - a) / (b - a);
  return x * x * (3.0f - 2.0f * x);
}
/** Unclamped linear remap: maps x from [a,b] → [0,1] WITHOUT clamping.
 *  Extrapolation beyond [0,1] is intentional (Buya HDR recovery depends on it). */
ccl_device_inline float darf_noxstep(float a, float b, float x)
{
  if (b == a) {
    return 0.0f;
  }
  return (x - a) / (b - a);
}
ccl_device_inline float darf_bias(float b, float x)
{
  return powf(x, logf(b) / logf(0.5f));
}
ccl_device_inline float darf_map(
    float value, float old_lo, float old_hi, float new_lo, float new_hi)
{
  float old_range = old_hi - old_lo;
  if (old_range == 0.0f) {
    return new_lo;
  }
  float new_range = new_hi - new_lo;
  return (((value - old_lo) * new_range) / old_range) + new_lo;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Levels — three-point output remap
 *
 * Two variants: `penny_levels` (clamped, used by Buya's baked table) and
 * `penny_oct_levels` (unclamped noxstep, used by all 19 noise types for
 * the user's Level Min/Mid/Max controls). Identity at (0, 0.5, 1).
 * \{ */

/** Clamped three-point Levels. lo/hi = black/white points, mid = gamma midpoint. */
ccl_device_inline float penny_levels(float x, float lo, float mid, float hi)
{
  float n = clamp((x - lo) / fmaxf(hi - lo, 1e-6f), 0.0f, 1.0f);
  float g = logf(clamp(mid, 0.001f, 0.999f)) / logf(0.5f);
  return powf(n, g);
}

/* Penny Levels control (David Farmer's model): a noxstep(low,high) black/white-point
 * remap followed by a gamma(mid) curve. Used two ways across the noises:
 *   - PER-OCTAVE (fBm, Turbulence): abandoned - leveling each [0,1] octave then
 *     re-centering to signed and summing SELF-CANCELS on a signed-sum fractal, so
 *     these now use output Levels like everyone else (see penny_fbm / turbulence).
 *   - OUTPUT (all 19): level the final per-channel value (v / v2 / base / val), or
 *     after Buya's baked remap. This is the surviving approach.
 * Identity at default 0/0.5/1 so every noise is byte-identical unless the sliders move.
 * noxstep guarantees its result is in [0,1], so the powf below is NaN-safe even when
 * Level Min/Max are driven outside [0,1] (HDR black/white points). */
ccl_device_inline float penny_oct_levels(float x, float low, float mid, float high)
{
  if (low == 0.0f && mid == 0.5f && high == 1.0f) {
    return x;
  }
  float n = darf_noxstep(low, high, x);
  float g = logf(clamp(mid, 0.001f, 0.999f)) / logf(0.5f);
  return powf(n, g);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Noise building blocks and type implementations
 *
 * Each `penny_*_tile()` function implements one of the 19 noise types.
 * They share a common structure:
 *   1. Evaluate noise algorithm per-channel (3 decorrelated channels for RGB).
 *   2. Apply `penny_oct_levels()` for the user's Level Min/Mid/Max remap.
 *   3. If blend_mode != 0, generate a second decorrelated sample (offset
 *      +0.5 in XYZ) and combine via `penny_blend()`.
 *
 * Turbulence building blocks (penny_turb_additive, penny_abs_turb2,
 * penny_reg_turb2, penny_reg_turb2_signed, penny_blist_turb, penny_luka_turb)
 * are shared helpers that multiple noise types compose.
 * \{ */

/** Tileable 4D fBm (fractal Brownian motion). Lacunarity locked to 2. */
ccl_device_inline float3 penny_fbm_norm(const float x,
                                        const float y,
                                        const float z,
                                        const float w,
                                        int4 period,
                                        int octaves,
                                        float roughness,
                                        float lvl_lo,
                                        float lvl_mid,
                                        float lvl_hi)
{
  /* Clean normalized fBm (signed sum). Levels applied at tile output, not per-octave. */
  float amp = 1.0f, freq = 1.0f, norm = 0.0f;
  float3 sum = make_float3(0.0f, 0.0f, 0.0f);
  int4 op = period;
  for (int i = 0; i < octaves; ++i) {
    sum = sum + amp * penny_perlin_4d_tile(x * freq, y * freq, z * freq, w * freq, op);
    norm += amp;
    amp *= roughness;
    freq *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
  }
  float3 _r = (norm > 0.0f) ? (sum * (1.0f / norm)) * 0.5f + make_float3(0.5f, 0.5f, 0.5f) :
                              make_float3(0.5f, 0.5f, 0.5f);
  /* fBm uses OUTPUT Levels (not per-octave): per-octave self-cancels on a signed
   * sum, so we level the final normalized [0,1] value instead. */
  return make_float3(penny_oct_levels(_r.x, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(_r.y, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(_r.z, lvl_lo, lvl_mid, lvl_hi));
}

ccl_device_inline float3 penny_fbm_tile(const float x,
                                        const float y,
                                        const float z,
                                        const float w,
                                        int4 period,
                                        int octaves,
                                        float roughness,
                                        int blend_mode,
                                        float lvl_lo,
                                        float lvl_mid,
                                        float lvl_hi)
{
  octaves = max(octaves, 1);

  if (blend_mode == 0) {
    return penny_fbm_norm(x, y, z, w, period, octaves, roughness, lvl_lo, lvl_mid, lvl_hi);
  }

  /* Blend path: single blend between two decorrelated normalized fBm samples
   * (no per-octave clamp compounding). Second layer 30% at 1 octave, 100% at 2+. */
  float3 base = penny_fbm_norm(x, y, z, w, period, octaves, roughness, lvl_lo, lvl_mid, lvl_hi);
  float3 base2 = penny_fbm_norm(
      x + 0.5f, y + 0.5f, z + 0.5f, w, period, octaves, roughness, lvl_lo, lvl_mid, lvl_hi);
  float3 blended = make_float3(penny_blend(blend_mode, base.x, base2.x),
                               penny_blend(blend_mode, base.y, base2.y),
                               penny_blend(blend_mode, base.z, base2.z));
  float bs = (octaves == 1) ? 0.3f : 1.0f;
  /* Blend path: single blend of two decorrelated normalized fBm samples. Second
   * layer contributes 30% at 1 octave, 100% at 2+ (bs). */
  return mix(base, blended, bs);
}

/* Additive Turbulence building block — RGB, tileable. Matches GLSL penny_turb_additive.
 * Takes lvl params for signature uniformity with other building blocks but does not use
 * them; Levels are applied by the caller (penny_turbulence_tile) at output. */
ccl_device_inline float3 penny_turb_additive(const float x,
                                             const float y,
                                             const float z,
                                             const float w,
                                             int4 period,
                                             int octaves,
                                             float roughness,
                                             float /*lvl_lo*/,
                                             float /*lvl_mid*/,
                                             float /*lvl_hi*/)
{
  float3 value = make_float3(0.0f, 0.0f, 0.0f);
  float f = 1.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    float off = (f - 1.0f) * 1000.0f;
    int4 op = make_int4(
        (int)(period.x * f), (int)(period.y * f), (int)(period.z * f), (int)(period.w * f));
    float3 n = penny_perlin_4d_tile(x * f + off, y * f + off, z * f + off, w * f + off, op);
    float3 t = 0.7f * (n * 0.5f + make_float3(0.5f, 0.5f, 0.5f)) + make_float3(0.3f, 0.3f, 0.3f);
    value = value + (2.0f * t - make_float3(1.0f, 1.0f, 1.0f)) * amp;
    norm += amp;
    f *= 2.0f;
    amp *= roughness;
  }
  value = value * (0.5f / norm);
  return make_float3(darf_noxstep(-0.12f, 0.34f, value.x),
                     darf_noxstep(-0.12f, 0.34f, value.y),
                     darf_noxstep(-0.12f, 0.34f, value.z));
}

/* Penny Turbulence — RGB, tileable. Self-blend for rich blend modes. Matches GLSL. */
ccl_device_inline float3 penny_turbulence_tile(const float x,
                                               const float y,
                                               const float z,
                                               const float w,
                                               int4 period,
                                               int octaves,
                                               float roughness,
                                               int blend_mode,
                                               float lvl_lo,
                                               float lvl_mid,
                                               float lvl_hi)
{
  octaves = max(octaves, 1);

  float3 base = penny_turb_additive(
      x, y, z, w, period, octaves, roughness, lvl_lo, lvl_mid, lvl_hi);
  if (blend_mode == 0) {
    /* Turbulence uses OUTPUT Levels (same self-cancel reason as fBm). */
    return make_float3(penny_oct_levels(base.x, lvl_lo, lvl_mid, lvl_hi),
                       penny_oct_levels(base.y, lvl_lo, lvl_mid, lvl_hi),
                       penny_oct_levels(base.z, lvl_lo, lvl_mid, lvl_hi));
  }
  /* Self-blend: same additive Turbulence at +0.5 shifted coord. */
  float3 base2 = penny_turb_additive(
      x + 0.5f, y + 0.5f, z + 0.5f, w, period, octaves, roughness, lvl_lo, lvl_mid, lvl_hi);
  float3 _tb = make_float3(
      clamp(penny_blend(blend_mode, clamp(base.x, 0.0f, 1.0f), clamp(base2.x, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.y, 0.0f, 1.0f), clamp(base2.y, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.z, 0.0f, 1.0f), clamp(base2.z, 0.0f, 1.0f)),
            0.0f,
            1.0f));
  return make_float3(penny_oct_levels(_tb.x, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(_tb.y, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(_tb.z, lvl_lo, lvl_mid, lvl_hi));
}

/* Maxon Stupl helper: abs-turbulence. Per channel sum += abs(2*p01-1)*amp
 * (== abs(perlin)*amp). Roughness-driven, normalized, tileable. */
ccl_device_inline float3 penny_abs_turb2(const float x,
                                         const float y,
                                         const float z,
                                         const float w,
                                         int4 period,
                                         int octaves,
                                         float roughness)
{
  float3 sum = make_float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 p01 = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w * scale + off, op) *
                     0.5f +
                 make_float3(0.5f, 0.5f, 0.5f);
    sum = sum + make_float3(fabsf(2.0f * p01.x - 1.0f),
                            fabsf(2.0f * p01.y - 1.0f),
                            fabsf(2.0f * p01.z - 1.0f)) *
                    amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : make_float3(0.0f, 0.0f, 0.0f);
}

/* Maxon Stupl helper: regular (signed) turbulence, then darf_noxstep(-1,1).
 * Roughness-driven, normalized, tileable. */
ccl_device_inline float3 penny_reg_turb2(const float x,
                                         const float y,
                                         const float z,
                                         const float w,
                                         int4 period,
                                         int octaves,
                                         float roughness)
{
  float3 sum = make_float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 p01 = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w * scale + off, op) *
                     0.5f +
                 make_float3(0.5f, 0.5f, 0.5f);
    sum = sum + make_float3(2.0f * p01.x - 1.0f, 2.0f * p01.y - 1.0f, 2.0f * p01.z - 1.0f) * amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  sum = (norm > 0.0f) ? sum * (1.0f / norm) : make_float3(0.0f, 0.0f, 0.0f);
  return make_float3(darf_noxstep(-1.0f, 1.0f, sum.x),
                     darf_noxstep(-1.0f, 1.0f, sum.y),
                     darf_noxstep(-1.0f, 1.0f, sum.z));
}

/* Signed (pre-noxstep) reg_turb2: normalized fractal sum (~[-1,1]). Matches Penny
 * noise_Turbulence raw output, which Hama's remap chain expects. */
ccl_device_inline float3 penny_reg_turb2_signed(const float x,
                                                const float y,
                                                const float z,
                                                const float w,
                                                int4 period,
                                                int octaves,
                                                float roughness)
{
  float3 sum = make_float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 p01 = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w * scale + off, op) *
                     0.5f +
                 make_float3(0.5f, 0.5f, 0.5f);
    sum = sum + make_float3(2.0f * p01.x - 1.0f, 2.0f * p01.y - 1.0f, 2.0f * p01.z - 1.0f) * amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : make_float3(0.0f, 0.0f, 0.0f);
}

/* Maxon Stupl: per-channel domain-warped turbulence. Matches GLSL. */
ccl_device_inline float3 penny_stupl_tile(const float x,
                                          const float y,
                                          const float z,
                                          const float w,
                                          int4 period,
                                          int octaves,
                                          float roughness,
                                          int blend_mode,
                                          float lvl_lo,
                                          float lvl_mid,
                                          float lvl_hi)
{
  octaves = max(octaves, 1);

  float3 turb = penny_abs_turb2(x, y, z, w, period, octaves, roughness);
  float3 one = make_float3(1.0f, 1.0f, 1.0f);
  float3 disp3 = one - 2.0f * penny_reg_turb2(x, y, z, w, period, 1, roughness);

  float tch[3];
  tch[0] = turb.x;
  tch[1] = turb.y;
  tch[2] = turb.z;
  float dch[3];
  dch[0] = disp3.x;
  dch[1] = disp3.y;
  dch[2] = disp3.z;
  float res[3];
  for (int c = 0; c < 3; ++c) {
    float d = dch[c];
    float3 inner3 = one -
                    2.0f * penny_reg_turb2(x - d, y + d, z + d, w, period, octaves, roughness);
    float ich[3];
    ich[0] = inner3.x;
    ich[1] = inner3.y;
    ich[2] = inner3.z;
    float inner = ich[c];
    float v = darf_noxstep(-2.45f, 2.5f, inner);
    v = darf_noxstep(0.5f - tch[c], 0.5f + tch[c], v);
    v = darf_smoothstep(0.0f, 1.0f, v);
    v = darf_bias(0.45f, v);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[c] = v;
    }
    else {
      float3 inner3b =
          one -
          2.0f * penny_reg_turb2(
                     x - d + 0.5f, y + d + 0.5f, z + d + 0.5f, w, period, octaves, roughness);
      float ichb[3];
      ichb[0] = inner3b.x;
      ichb[1] = inner3b.y;
      ichb[2] = inner3b.z;
      float v2 = darf_noxstep(-2.45f, 2.5f, ichb[c]);
      v2 = darf_noxstep(0.5f - tch[c], 0.5f + tch[c], v2);
      v2 = darf_smoothstep(0.0f, 1.0f, v2);
      v2 = darf_bias(0.45f, v2);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      res[c] = clamp(
          penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

/* Luka abs-turbulence: abs(snoise * 0.73) per octave. Roughness-driven,
 * normalized, tileable. (Distinct from Stupl's turb2 helpers.) */
ccl_device_inline float3 penny_luka_turb(const float x,
                                         const float y,
                                         const float z,
                                         const float w,
                                         int4 period,
                                         int octaves,
                                         float roughness)
{
  float3 sum = make_float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 n = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w * scale + off, op);
    sum = sum + make_float3(fabsf(n.x * 0.73f), fabsf(n.y * 0.73f), fabsf(n.z * 0.73f)) * amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : make_float3(0.0f, 0.0f, 0.0f);
}

/* Penny "noise_Turbulence" base — signed fractal sum. Dampening = roughness
 * (0.5 reproduces the original). Normalized by scale-sum, tileable, RGB. */
ccl_device_inline float3 penny_blist_turb(const float x,
                                          const float y,
                                          const float z,
                                          const float w,
                                          int4 period,
                                          int octaves,
                                          float roughness)
{
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float scale_sum = 0.0f;
  float f = 1.0f;

  /* First octave: signed perlin at scale 1. scale_sum += scale BEFORE damping. */
  float3 n0 = penny_perlin_4d_tile(x, y, z, w, period);
  result = n0 * scale;
  scale_sum += scale;
  scale *= roughness;
  f *= 2.0f;

  for (int i = 1; i < octaves; ++i) {
    float off = (f - 1.0f) * 1000.0f;
    int4 op = make_int4(
        (int)(period.x * f), (int)(period.y * f), (int)(period.z * f), (int)(period.w * f));
    float3 n = penny_perlin_4d_tile(x * f + off, y * f + off, z * f + off, w * f + off, op);
    result = result + n * scale;
    scale *= roughness;
    scale_sum += scale;
    f *= 2.0f;
  }
  return result * (1.0f / scale_sum);
}

/* Penny Blistered Turbulence — adaptive contrast window. Self-blend for modes. */
ccl_device_inline float3 penny_blistered_tile(const float x,
                                              const float y,
                                              const float z,
                                              const float w,
                                              int4 period,
                                              int octaves,
                                              float roughness,
                                              int blend_mode,
                                              float lvl_lo,
                                              float lvl_mid,
                                              float lvl_hi)
{
  octaves = max(octaves, 1);

  /* temp = boxstep(-1.5,1.5, 1 - 2*boxstep(-1,1, turb(co))) — per-channel width. */
  float3 t0 = penny_blist_turb(x, y, z, w, period, octaves, roughness);
  float t0c[3];
  t0c[0] = t0.x;
  t0c[1] = t0.y;
  t0c[2] = t0.z;
  float tch[3];
  for (int ch = 0; ch < 3; ++ch) {
    tch[ch] = darf_boxstep(-1.5f, 1.5f, 1.0f - 2.0f * darf_boxstep(-1.0f, 1.0f, t0c[ch]));
  }

  /* Inner sample at 2x coordinate (higher frequency). */
  float3 inner = penny_blist_turb(x * 2.0f, y * 2.0f, z * 2.0f, w, period, octaves, roughness);
  float ich[3];
  ich[0] = inner.x;
  ich[1] = inner.y;
  ich[2] = inner.z;

  float3 innerb = penny_blist_turb(
      x * 2.0f + 0.5f, y * 2.0f + 0.5f, z * 2.0f + 0.5f, w, period, octaves, roughness);
  float ichb[3];
  ichb[0] = innerb.x;
  ichb[1] = innerb.y;
  ichb[2] = innerb.z;

  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    float s = darf_boxstep(-1.0f, 1.0f, ich[ch]);
    float v = darf_boxstep(0.5f - tch[ch], 0.5f + tch[ch], s);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float sb = darf_boxstep(-1.0f, 1.0f, ichb[ch]);
      float v2 = darf_boxstep(0.5f - tch[ch], 0.5f + tch[ch], sb);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      res[ch] = clamp(
          penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

/* Penny Mod — snap coord to integer cell center (C-fmod truncation), sample Perlin.
 * snap(p) = trunc(p) + 0.5 + (p<0 ? -1.001 : 1.001).  Returns [0,1] RGB. */
ccl_device_inline float3
penny_mod_sample(const float x, const float y, const float z, const float w, int4 period)
{
  float sx = trunc(x) + 0.5f + (x < 0.0f ? -1.001f : 1.001f);
  float sy = trunc(y) + 0.5f + (y < 0.0f ? -1.001f : 1.001f);
  float sz = trunc(z) + 0.5f + (z < 0.0f ? -1.001f : 1.001f);
  return penny_perlin_4d_tile(sx, sy, sz, w, period) * 0.5f + make_float3(0.5f, 0.5f, 0.5f);
}

/* Penny Mod noise — summed snapped-Perlin octaves (blocky). Centered roughness
 * (0.5 = original). Final: 2*sum - octaves + 0.5. Self-blend, RGB. */
ccl_device_noinline float3 penny_mod_tile(const float x,
                                          const float y,
                                          const float z,
                                          const float w,
                                          int4 period,
                                          int octaves,
                                          float roughness,
                                          int blend_mode,
                                          float lvl_lo,
                                          float lvl_mid,
                                          float lvl_hi)
{
  octaves = max(octaves, 1);
  float cx = x + 1.11f, cy = y + 1.54f, cz = z + 1.82f;
  float ramp = roughness * 2.0f;

  float3 value = make_float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float amp = 1.0f;
  float ampsum = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    int4 op = make_int4((int)(period.x * scale),
                        (int)(period.y * scale),
                        (int)(period.z * scale),
                        (int)(period.w * scale));
    value = value +
            penny_mod_sample(cx * scale + off, cy * scale + off, cz * scale + off, w * scale + off, op) * amp;
    ampsum += amp;
    scale *= 2.0f;
    amp *= ramp;
  }
  /* Normalize by ampsum first, THEN fixed 2x contrast around 0.5. Keeps the deviation
   * from scaling with octave count (old 2*value-ampsum blew past [0,1] at 2+ octaves,
   * crushing to black/white). G=2 matches the 1-octave look exactly. */
  /* BUGFIX: normalize summed octaves by ampsum BEFORE the 2x contrast. The old
   * 2*value - ampsum + 0.5 let the deviation scale with octave count, crushing to
   * black/white at 2+ octaves. Dividing by ampsum first keeps midtones; the fixed
   * 2x contrast around 0.5 matches the 1-octave look. */
  float3 base = make_float3(clamp(2.0f * (value.x / fmaxf(ampsum, 1e-6f)) - 0.5f, 0.0f, 1.0f),
                            clamp(2.0f * (value.y / fmaxf(ampsum, 1e-6f)) - 0.5f, 0.0f, 1.0f),
                            clamp(2.0f * (value.z / fmaxf(ampsum, 1e-6f)) - 0.5f, 0.0f, 1.0f));
  base = make_float3(penny_oct_levels(base.x, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(base.y, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(base.z, lvl_lo, lvl_mid, lvl_hi));
  if (blend_mode == 0) {
    return base;
  }

  float c2x = cx + 0.5f, c2y = cy + 0.5f, c2z = cz + 0.5f;
  float3 value2 = make_float3(0.0f, 0.0f, 0.0f);
  scale = 1.0f;
  amp = 1.0f;
  float ampsum2 = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    int4 op = make_int4((int)(period.x * scale),
                        (int)(period.y * scale),
                        (int)(period.z * scale),
                        (int)(period.w * scale));
    value2 = value2 +
             penny_mod_sample(c2x * scale + off, c2y * scale + off, c2z * scale + off, w * scale + off, op) *
                 amp;
    ampsum2 += amp;
    scale *= 2.0f;
    amp *= ramp;
  }
  float3 base2 = make_float3(clamp(2.0f * (value2.x / fmaxf(ampsum2, 1e-6f)) - 0.5f, 0.0f, 1.0f),
                             clamp(2.0f * (value2.y / fmaxf(ampsum2, 1e-6f)) - 0.5f, 0.0f, 1.0f),
                             clamp(2.0f * (value2.z / fmaxf(ampsum2, 1e-6f)) - 0.5f, 0.0f, 1.0f));
  base2 = make_float3(penny_oct_levels(base2.x, lvl_lo, lvl_mid, lvl_hi),
                      penny_oct_levels(base2.y, lvl_lo, lvl_mid, lvl_hi),
                      penny_oct_levels(base2.z, lvl_lo, lvl_mid, lvl_hi));
  return make_float3(
      clamp(penny_blend(blend_mode, clamp(base.x, 0.0f, 1.0f), clamp(base2.x, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.y, 0.0f, 1.0f), clamp(base2.y, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.z, 0.0f, 1.0f), clamp(base2.z, 0.0f, 1.0f)),
            0.0f,
            1.0f));
}

/* Penny box_Noise — domain distortion via axis-sliced Perlin offsets.
 * Each RGB channel computed independently. Returns [0,1]. Tileable.
 * distortion hardcoded 4.0. w passes through. */
ccl_device_inline float3 penny_box_noise(
    const float x, const float y, const float z, const float w, int4 period, float distortion)
{
  /* Axis-sliced offsets: perlin on (x,0,z),(0,y,z),(0,0,z). [0,1] via *0.5+0.5. */
  float3 ox = penny_perlin_4d_tile(x, 0.0f, z, w, period) * 0.5f + make_float3(0.5f, 0.5f, 0.5f);
  float3 oy = penny_perlin_4d_tile(0.0f, y, z, w, period) * 0.5f + make_float3(0.5f, 0.5f, 0.5f);

  float oxc[3];
  oxc[0] = ox.x;
  oxc[1] = ox.y;
  oxc[2] = ox.z;
  float oyc[3];
  oyc[0] = oy.x;
  oyc[1] = oy.y;
  oyc[2] = oy.z;

  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    float offx = oxc[ch] * distortion;
    float offy = oyc[ch] * distortion;
    /* the_Point = position + offset; final sample uses warped x,y but ORIGINAL z. */
    float3 fin = penny_perlin_4d_tile(x + offx, y + offy, z, w, period) * 0.5f +
                 make_float3(0.5f, 0.5f, 0.5f);
    float finc[3];
    finc[0] = fin.x;
    finc[1] = fin.y;
    finc[2] = fin.z;
    res[ch] = finc[ch];
  }
  return make_float3(res[0], res[1], res[2]);
}

/* Penny Box noise — octave sum of box_Noise, signed /f falloff (roughness-driven),
 * boxstep(-1.05,1) to [0,1] (floor lifted off black). Self-blend. Tileable, RGB. */
ccl_device_noinline float3 penny_box_tile(const float x,
                                          const float y,
                                          const float z,
                                          const float w,
                                          int4 period,
                                          int octaves,
                                          float roughness,
                                          int blend_mode,
                                          float lvl_lo,
                                          float lvl_mid,
                                          float lvl_hi)
{
  octaves = max(octaves, 1);

  float3 value = make_float3(0.0f, 0.0f, 0.0f);
  float f = 1.0f;
  float amp = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    float off = (f - 1.0f) * 1000.0f;
    int4 op = make_int4(
        (int)(period.x * f), (int)(period.y * f), (int)(period.z * f), (int)(period.w * f));
    float3 b = penny_box_noise(x * f + off, y * f + off, z * f + off, w * f + off, op, 4.0f);
    value = value + (2.0f * b - make_float3(1.0f, 1.0f, 1.0f)) * amp;
    f *= 2.0f;
    amp *= roughness;
  }

  float3 base = make_float3(darf_boxstep(-1.05f, 1.0f, value.x),
                            darf_boxstep(-1.05f, 1.0f, value.y),
                            darf_boxstep(-1.05f, 1.0f, value.z));
  base = make_float3(penny_oct_levels(base.x, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(base.y, lvl_lo, lvl_mid, lvl_hi),
                     penny_oct_levels(base.z, lvl_lo, lvl_mid, lvl_hi));
  if (blend_mode == 0) {
    return base;
  }

  float3 value2 = make_float3(0.0f, 0.0f, 0.0f);
  f = 1.0f;
  amp = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    float off = (f - 1.0f) * 1000.0f;
    int4 op = make_int4(
        (int)(period.x * f), (int)(period.y * f), (int)(period.z * f), (int)(period.w * f));
    float3 b = penny_box_noise(
        (x + 0.5f) * f + off, (y + 0.5f) * f + off, (z + 0.5f) * f + off, w * f + off, op, 4.0f);
    value2 = value2 + (2.0f * b - make_float3(1.0f, 1.0f, 1.0f)) * amp;
    f *= 2.0f;
    amp *= roughness;
  }
  float3 base2 = make_float3(darf_boxstep(-1.05f, 1.0f, value2.x),
                             darf_boxstep(-1.05f, 1.0f, value2.y),
                             darf_boxstep(-1.05f, 1.0f, value2.z));
  base2 = make_float3(penny_oct_levels(base2.x, lvl_lo, lvl_mid, lvl_hi),
                      penny_oct_levels(base2.y, lvl_lo, lvl_mid, lvl_hi),
                      penny_oct_levels(base2.z, lvl_lo, lvl_mid, lvl_hi));
  return make_float3(
      clamp(penny_blend(blend_mode, clamp(base.x, 0.0f, 1.0f), clamp(base2.x, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.y, 0.0f, 1.0f), clamp(base2.y, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.z, 0.0f, 1.0f), clamp(base2.z, 0.0f, 1.0f)),
            0.0f,
            1.0f));
}

/* Penny Nutous — domain-warped abs-turbulence, tight Noxstep window, squared,
 * baked midtone gamma (== Level Mid 0.691616). Reuses penny_blist_turb. RGB. */
ccl_device_noinline float3 penny_nutous_tile(const float x,
                                             const float y,
                                             const float z,
                                             const float w,
                                             int4 period,
                                             int octaves,
                                             float roughness,
                                             int blend_mode,
                                             float lvl_lo,
                                             float lvl_mid,
                                             float lvl_hi)
{
  octaves = max(octaves, 1);

  /* displace = turb(co) * 0.5 (per-channel). */
  float3 disp = penny_blist_turb(x, y, z, w, period, octaves, roughness) * 0.5f;
  float dch[3];
  dch[0] = disp.x;
  dch[1] = disp.y;
  dch[2] = disp.z;

  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    float d = dch[ch];
    float3 inner = penny_blist_turb(x + d, y + d, z + d, w, period, octaves, roughness);
    float ich[3];
    ich[0] = inner.x;
    ich[1] = inner.y;
    ich[2] = inner.z;
    float v = darf_noxstep(-0.01f, 0.4f, fabsf(ich[ch]) * 0.5f);
    v = v * v;
    v = powf(v, 0.5316f); /* baked midtone (== Level Mid 0.691616) */
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float3 disp2 = penny_blist_turb(
                         x + 0.5f, y + 0.5f, z + 0.5f, w, period, octaves, roughness) *
                     0.5f;
      float d2ch[3];
      d2ch[0] = disp2.x;
      d2ch[1] = disp2.y;
      d2ch[2] = disp2.z;
      float d2 = d2ch[ch];
      float3 inner2 = penny_blist_turb(
          x + 0.5f + d2, y + 0.5f + d2, z + 0.5f + d2, w, period, octaves, roughness);
      float i2ch[3];
      i2ch[0] = inner2.x;
      i2ch[1] = inner2.y;
      i2ch[2] = inner2.z;
      float v2 = darf_noxstep(-0.01f, 0.4f, fabsf(i2ch[ch]) * 0.5f);
      v2 = v2 * v2;
      v2 = powf(v2, 0.5316f);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      res[ch] = clamp(
          penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

/* Luka — per-channel domain-warped abs-turbulence with adaptive window. */
/* Penny Cranal — x-shifted ridged biased turbulence average.
 * Inner turbulence hardcoded to 1 octave; outer detail = # x-shifted samples.
 * Baked 300% scale (coord/3), roughness drives bias (0.5==0.80), Mid 0.25 gamma. RGB. */
ccl_device_noinline float3 penny_cranal_tile(const float x_in,
                                             const float y_in,
                                             const float z_in,
                                             const float w_in,
                                             int4 period,
                                             int octaves,
                                             float roughness,
                                             int blend_mode,
                                             float lvl_lo,
                                             float lvl_mid,
                                             float lvl_hi)
{
  octaves = max(octaves, 1);

  /* Snap the 1/3 feature scale to the period so the pattern wraps at `period`
   * exactly (x/3 alone would only repeat every 3*period). */
  const int ppx = max(period.x, 1), ppy = max(period.y, 1), ppz = max(period.z, 1);
  const int lpx = max(1, (int)((float)ppx / 3.0f + 0.5f));
  const int lpy = max(1, (int)((float)ppy / 3.0f + 0.5f));
  const int lpz = max(1, (int)((float)ppz / 3.0f + 0.5f));
  const int4 lat_period = make_int4(lpx, lpy, lpz, period.w);
  const float cx = x_in * ((float)lpx / (float)ppx);
  const float cy0 = y_in * ((float)lpy / (float)ppy);
  const float cz0 = z_in * ((float)lpz / (float)ppz);

  /* Roughness drives bias, centered so 0.5 == faithful 0.80. */
  const float bias = clamp(0.80f + (roughness - 0.5f) * 0.3f, 0.5f, 0.95f);

  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    const float off = (float)ch * 1000.0f;
    float x1 = cx + off;
    const float cy = cy0 + off;
    const float cz = cz0 + off;

    float val = 0.0f;
    for (int i = 0; i <= octaves; ++i) {
      /* Per-octave Y/Z decorrelation to break lattice grid alignment. */
      float oy = (float)i * 7.793f;
      float oz = (float)i * 11.219f;
      float3 t = penny_blist_turb(x1, cy + oy, cz + oz, w_in, lat_period, 1, roughness);
      float tch[3];
      tch[0] = t.x;
      tch[1] = t.y;
      tch[2] = t.z;
      /* Penny snoise is [-1,1]; our single-octave signed perlin is NARROWER, so a
       * -0.5..0.5 window (not -1..1) restores the full contrast spread. */
      float n = darf_noxstep(-0.5f, 0.5f, tch[ch]);
      float folded = fabsf(1.0f - 2.0f * n);
      val += darf_bias(bias, folded);
      x1 += 3.0f;
    }
    val /= (float)(octaves + 1);
    /* Cranal output contrast stretch: black point 0.75, white 0.96, to match the
     * Penny reference's bright interiors + dark cell-edge lines (replaces a non-Penny
     * pow(val,2.0) that had washed the range out). */
    val = darf_noxstep(0.75f, 0.96f, val);
    /* Baked black-point lift: reproduces the correct Cranal tone at default Levels
     * (previously only achievable at user Min -1.5). Unclamped, matching the
     * non-clamping darf_noxstep the manual Min -1.5 used. */
    val = (val + 1.5f) / 2.5f;
    val = penny_oct_levels(val, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[ch] = val;
    }
    else {
      const float off2 = off + 0.5f;
      float x1b = cx + off2;
      const float cyb = cy0 + off2;
      const float czb = cz0 + off2;
      float val2 = 0.0f;
      for (int i = 0; i <= octaves; ++i) {
        float oy = (float)i * 7.793f;
        float oz = (float)i * 11.219f;
        float3 t2 = penny_blist_turb(x1b, cyb + oy, czb + oz, w_in, lat_period, 1, roughness);
        float t2ch[3];
        t2ch[0] = t2.x;
        t2ch[1] = t2.y;
        t2ch[2] = t2.z;
        /* -0.5..0.5 window: our perlin is narrower than Penny snoise (see val above). */
        float n2 = darf_noxstep(-0.5f, 0.5f, t2ch[ch]);
        float folded2 = fabsf(1.0f - 2.0f * n2);
        val2 += darf_bias(bias, folded2);
        x1b += 3.0f;
      }
      val2 /= (float)(octaves + 1);
      val2 = darf_noxstep(0.75f, 0.96f, val2); /* raise black point */
      val2 = (val2 + 1.5f) / 2.5f;             /* baked black-point lift */
      val2 = penny_oct_levels(val2, lvl_lo, lvl_mid, lvl_hi);
      res[ch] = clamp(penny_blend(blend_mode, val, val2), 0.0f, 1.0f);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

/** Buya — ridge-folded biased pfBm with octave-aware baked Levels.
 *  pfBm: H = (1-roughness)*0.4 spectral weights, weight-sum normalized.
 *  Weight-sum normalization is critical: without it the raw accumulation
 *  clips at high octave counts, producing flat-gray output (this was the
 *  root cause of the Buya normalization bug fixed in session). */

/** Per-octave baked Levels table for Buya normalization.
 *  Empirically tuned lo/mid/hi parameters per octave count to keep the
 *  pfBm output at consistent brightness as octaves change. Counts > 7
 *  fall through to a generic (0.5, 0.5, 1.5) mapping. */
ccl_device_inline float buya_octave_levels(int oc, float x)
{
  if (oc == 1) {
    return penny_levels(x, 0.85f, 0.5f, 1.75f);
  }
  else if (oc == 2) {
    return penny_levels(x, 0.90f, 0.5f, 1.80f);
  }
  else if (oc == 3) {
    return penny_levels(x, 0.85f, 0.5f, 2.00f);
  }
  else if (oc == 4) {
    return penny_levels(x, 0.95f, 0.5f, 1.75f);
  }
  else if (oc == 5) {
    return penny_levels(x, 1.00f, 0.5f, 1.26f);
  }
  else if (oc == 6) {
    return penny_levels(x, 1.00f, 0.5f, 1.20f);
  }
  else {
    return penny_levels(x, 0.5f, 0.5f, 1.5f);
  }
}

ccl_device_inline float buya_pfbm_channel(const float x,
                                          const float y,
                                          const float z,
                                          const float w,
                                          int ch,
                                          int octaves,
                                          int4 period,
                                          float roughness)
{
  float value = 0.0f;
  float scale = 1.0f;
  float wsum = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    int4 op = make_int4((int)(period.x * scale),
                        (int)(period.y * scale),
                        (int)(period.z * scale),
                        (int)(period.w * scale));
    float3 p = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w * scale + off, op);
    float pch[3];
    pch[0] = p.x;
    pch[1] = p.y;
    pch[2] = p.z;
    float toy = pch[ch] * 0.5f + 0.5f;
    float h = (1.0f - roughness) * 0.4f; /* inverted; 0.5 -> H 0.2 (faithful) */
    float weight = powf(2.0f, -h * (float)i);
    value += (2.0f * darf_noxstep(-1.0f, 1.0f, toy) - 1.0f) * weight;
    wsum += weight;
    scale *= 2.0f;
  }
  return value / fmaxf(wsum, 1e-6f);
}

ccl_device_noinline float3 penny_buya_tile(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int4 period,
                                           int octaves,
                                           float roughness,
                                           int blend_mode,
                                           float lvl_lo,
                                           float lvl_mid,
                                           float lvl_hi)
{
  octaves = max(octaves, 1);
  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    float pf = buya_pfbm_channel(x, y, z, w, ch, octaves, period, roughness);
    float v = darf_noxstep(0.1f, 0.5f, darf_bias(0.25f, fabsf(1.0f - 2.0f * pf * 0.5f)));
    v = buya_octave_levels(octaves, v);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float pf2 = buya_pfbm_channel(
          x + 0.5f, y + 0.5f, z + 0.5f, w, ch, octaves, period, roughness);
      float v2 = darf_noxstep(0.1f, 0.5f, darf_bias(0.25f, fabsf(1.0f - 2.0f * pf2 * 0.5f)));
      v2 = buya_octave_levels(octaves, v2);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      res[ch] = penny_blend(blend_mode, v, v2);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

/* Penny Cell - XY quantized unit-cell Perlin, amplitude-sum normalized octaves.
 * Z continuous. RGB. Self-blend. */
ccl_device_inline float cell_sample(float px, float py, float pz, float w, int4 per, int ch)
{
  float cx = px;
  if (cx < 0.0f) {
    cx = cx + 100.0f;
  }
  float cy = py;
  if (cy < 0.0f) {
    cy = cy + 100.0f;
  }
  /* BUGFIX: Z must be cell-snapped like X/Y. Passing Z raw gave anisotropic
   * vertical banding instead of isotropic blocky cells. */
  float cz = pz;
  if (cz < 0.0f) {
    cz = cz + 100.0f;
  }
  float qx = cx - (cx - floorf(cx)); /* floor to unit cell */
  float qy = cy - (cy - floorf(cy));
  float qz = cz - (cz - floorf(cz));
  float3 p = penny_perlin_4d_tile(qx + 1023.761f, qy + 31.875f, qz + 57.219f, w, per);
  float pch[3];
  pch[0] = p.x;
  pch[1] = p.y;
  pch[2] = p.z;
  return pch[ch] * 0.5f + 0.5f;
}

ccl_device_inline float cell_octaves(
    float bx, float by, float bz, float w, int4 period, int octaves, float roughness, int ch)
{
  float sum = 0.0f;
  float amp = 1.0f;
  float ampsum = 0.0f;
  float scale = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    int4 op = make_int4((int)(period.x * scale),
                        (int)(period.y * scale),
                        (int)(period.z * scale),
                        (int)(period.w * scale));
    sum += cell_sample(bx * scale, by * scale, bz * scale, w * scale, op, ch) * amp;
    ampsum += amp;
    amp *= roughness;
    scale *= 2.0f;
  }
  return clamp(sum / fmaxf(ampsum, 1e-6f), 0.0f, 1.0f);
}

ccl_device_noinline float3 penny_cell_tile(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int4 period,
                                           int octaves,
                                           float roughness,
                                           int blend_mode,
                                           float lvl_lo,
                                           float lvl_mid,
                                           float lvl_hi)
{
  octaves = max(octaves, 1);
  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = cell_octaves(x + dch, y + dch, z + dch, w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float v2 = cell_octaves(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, period, octaves, roughness, ch);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      res[ch] = clamp(penny_blend(blend_mode, v, v2), 0.0f, 1.0f);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

/** Displaced Turbulence — x-shifted displacement-warped abs-turbulence.
 *  Displace from 2-octave turb, temp from 1-octave turb, with percentile-based
 *  output windowing for octave-stable brightness/contrast. */

/** Empirically measured median (p50) and 98th-percentile (p98) of the raw
 *  Disturb accumulator at each octave count. Piecewise-linear interpolation
 *  between breakpoints (1, 4, 7, 10, 15). Mirrors EEVEE/blenlib
 *  `disturb_pctl` exactly (values bit-identical). */
ccl_device_inline void disturb_pctl(int oc, ccl_private float *p50, ccl_private float *p98)
{
  if (oc <= 1) {
    *p50 = 0.235f;
    *p98 = 0.784f;
  }
  else if (oc < 4) {
    float t = (float)(oc - 1) / 3.0f;
    *p50 = 0.235f + (1.176f - 0.235f) * t;
    *p98 = 0.784f + (1.961f - 0.784f) * t;
  }
  else if (oc < 7) {
    float t = (float)(oc - 4) / 3.0f;
    *p50 = 1.176f + (1.882f - 1.176f) * t;
    *p98 = 1.961f + (2.980f - 1.961f) * t;
  }
  else if (oc < 10) {
    float t = (float)(oc - 7) / 3.0f;
    *p50 = 1.882f + (2.510f - 1.882f) * t;
    *p98 = 2.980f + (3.843f - 2.980f) * t;
  }
  else if (oc < 15) {
    float t = (float)(oc - 10) / 5.0f;
    *p50 = 2.510f + (3.843f - 2.510f) * t;
    *p98 = 3.843f + (5.098f - 3.843f) * t;
  }
  else {
    float e = (float)(oc - 15);
    *p50 = 3.843f + 0.267f * e;
    *p98 = 5.098f + 0.251f * e;
  }
}

/* Measured bottom-2% of raw sum vs octave (sum units), for the black-point floor. */
ccl_device_inline float disturb_p02(int oc)
{
  if (oc <= 1) {
    return 0.000f;
  }
  else if (oc < 4) {
    return 0.000f + (0.314f - 0.000f) * ((float)(oc - 1) / 3.0f);
  }
  else if (oc < 7) {
    return 0.314f + (0.784f - 0.314f) * ((float)(oc - 4) / 3.0f);
  }
  else if (oc < 10) {
    return 0.784f + (1.412f - 0.784f) * ((float)(oc - 7) / 3.0f);
  }
  else if (oc < 15) {
    return 1.412f + (2.353f - 1.412f) * ((float)(oc - 10) / 5.0f);
  }
  else {
    return 2.353f + 0.188f * (float)(oc - 15);
  }
}

/** Octave-aware output window for Disturb: maps the raw accumulator into [0, ~1]
 *  using percentile tables. TMID=0.313, THI=0.537 set the target midtone/highlight
 *  placement. Quadratic lift term `LIFT × (1-t)²` prevents full black. */
ccl_device_inline float disturb_window(int oc, float sum)
{
  const float TMID = 0.313f, THI = 0.537f, FLOORMARGIN = 0.35f, LIFT = 0.2f;
  float p50, p98;
  disturb_pctl(oc, &p50, &p98);
  float span = (p98 - p50) / fmaxf(THI - TMID, 1e-4f);
  float lo = p50 - TMID * span;
  float hi = lo + span;
  lo = fminf(lo, disturb_p02(oc) - FLOORMARGIN);
  float t = darf_noxstep(lo, hi, sum);
  return t + LIFT * (1.0f - t) * (1.0f - t);
}

ccl_device_inline float disturb_accum(
    float bx, float by, float bz, float w, int4 period, int octaves, float roughness, int ch)
{
  /* roughness drives fBm-style amplitude falloff across x-shifted samples, NOT the inner
   * turb (fixed at 0.5 so distortion stays constant). Centered: 0.5 -> ramp 1.0 = original.
   * Renormalized to the unweighted magnitude the octave-tuned window expects. */
  float ramp = roughness * 2.0f;
  float x1 = bx;
  float sum = 0.0f;
  float amp = 1.0f;
  float ampsum = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    /* Per-octave Y/Z decorrelation to break lattice grid alignment. */
    float oy = (float)i * 7.793f;
    float oz = (float)i * 11.219f;
    float3 d = penny_abs_turb2(x1, by + oy, bz + oz, w, period, 2, 0.5f);
    float dch[3];
    dch[0] = d.x;
    dch[1] = d.y;
    dch[2] = d.z;
    float displace = 1.0f - 2.0f * dch[ch] * 0.8f;
    float3 tt = penny_abs_turb2(
        x1 - displace, by + displace + oy, bz + displace + oz, w, period, 1, 0.5f);
    float tch[3];
    tch[0] = tt.x;
    tch[1] = tt.y;
    tch[2] = tt.z;
    sum += tch[ch] * amp;
    ampsum += amp;
    amp *= ramp;
    x1 += 5.0f;
  }
  /* Roughness became an fBm-style amplitude falloff across the x-shifted samples
   * (inner turb fixed at 0.5 so distortion no longer scales with the slider).
   * Renormalize to the unweighted magnitude the octave-tuned window expects. */
  float norm_sum = (ampsum > 0.0f) ? (sum / ampsum) * (float)octaves : sum;
  return disturb_window(octaves, norm_sum);
}

/* Penny Hama - per-octave PRODUCT of narrow-windowed signed turbulence. Seed 2.0;
 * each octave multiplies in noxstep(0,0.3, abs(noxstep(0,1, turb))). i*10 shift. */
ccl_device_inline float penny_hama(const float x,
                                   const float y,
                                   const float z,
                                   const float w,
                                   int4 period,
                                   int octaves,
                                   float roughness,
                                   int ch)
{
  /* roughness drives gate width, inverted so high roughness tightens. 0.5 -> 0.3. */
  /* Roughness INVERTED here (high roughness -> tighter gate), per the Penny feel.
   * rough_inv centers on 0.5 so the gate default (0.3) is unchanged. */
  float rough_inv = 1.0f - roughness;
  float gate_hi = clamp(0.3f + (rough_inv - 0.5f) * 0.2f, 0.08f, 0.6f);
  float the_value = 2.0f;
  for (int i = 0; i < octaves; ++i) {
    float fi = (float)i * 10.0f;
    float3 t = penny_reg_turb2_signed(x + fi, y + fi, z + fi, w, period, 1, roughness);
    float turb = (ch == 0) ? t.x : ((ch == 1) ? t.y : t.z);
    float temp = darf_noxstep(0.0f, gate_hi, fabsf(darf_noxstep(0.0f, 1.0f, turb)));
    the_value *= temp;
  }
  return the_value;
}

ccl_device_noinline float3 penny_hama_tile(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int4 period,
                                           int octaves,
                                           float roughness,
                                           int blend_mode,
                                           float lvl_lo,
                                           float lvl_mid,
                                           float lvl_hi)
{
  octaves = max(octaves, 1);
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = penny_hama(x + dch, y + dch, z + dch, w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);
    float r;
    if (blend_mode == 0) {
      r = v;
    } /* HDR passthrough (match EEVEE + other noises) */
    else {
      float v2 = penny_hama(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, period, octaves, roughness, ch);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      r = clamp(penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    if (ch == 0)
      result.x = r;
    else if (ch == 1)
      result.y = r;
    else
      result.z = r;
  }
  return result;
}

/* Penny Naki - abs-turb displacement warp, single Perlin sample, smoothstep-biased output.
 * d = abs_turb(x,y,z,w, octaves, 0.5)[ch]*0.5; warp (x+d,y-d,z+d);
 * turb = perlin(warp)*0.5+0.5; out = noxstep(0.45,1, (turb + smoothstep(0.325,0.63,
 * d*0.25+0.5))*0.5). */
ccl_device_inline float penny_naki(const float x,
                                   const float y,
                                   const float z,
                                   const float w,
                                   int4 period,
                                   int octaves,
                                   float roughness,
                                   int ch)
{
  /* roughness drives displacement scale, centered: 0.5 -> 0.5 (original). */
  /* Roughness drives displacement magnitude (Naki's only spectral knob).
   * Centered so 0.5 == the original 0.5 scale (faithful default). */
  float disp_scale = clamp(0.5f + (roughness - 0.5f) * 0.3f, 0.2f, 0.85f);
  float3 ad = penny_abs_turb2(x, y, z, w, period, octaves, 0.5f);
  float d = ((ch == 0) ? ad.x : ((ch == 1) ? ad.y : ad.z)) * disp_scale;
  float3 p = penny_perlin_4d_tile(x + d + 7.793f, y - d + 11.219f, z + d + 13.547f, w, period);
  float turb = ((ch == 0) ? p.x : ((ch == 1) ? p.y : p.z)) * 0.5f + 0.5f;
  float bias = darf_smoothstep(0.325f, 0.63f, d * 0.25f + 0.5f);
  return darf_noxstep(0.45f, 1.0f, (turb + bias) * 0.5f);
}

ccl_device_noinline float3 penny_naki_tile(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int4 period,
                                           int octaves,
                                           float roughness,
                                           int blend_mode,
                                           float lvl_lo,
                                           float lvl_mid,
                                           float lvl_hi)
{
  octaves = max(octaves, 1);
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = penny_naki(x + dch, y + dch, z + dch, w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);
    float r;
    if (blend_mode == 0) {
      r = v;
    } /* HDR passthrough (match EEVEE + other noises) */
    else {
      float v2 = penny_naki(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, period, octaves, roughness, ch);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      r = clamp(penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    if (ch == 0)
      result.x = r;
    else if (ch == 1)
      result.y = r;
    else
      result.z = r;
  }
  return result;
}

/* Faithful Penny noise_ABS_Turbulence_Function for Poxo: coherent coord doubling
 * (coord += coord, NO decorrelation offset -> curved islands), pre-loop sample + octaves
 * loop samples, abs(perlin*0.73)*scale, normalized by scale_sum. Per-channel.
 * Inner octaves = 4 (not Penny's 3) to compensate tileable-perlin high-freq rolloff. */
ccl_device_inline float penny_poxo_absturb(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int octaves,
                                           float dampening,
                                           int4 period,
                                           int ch)
{
  float scale = 1.0f, scale_sum = 0.0f;
  float3 s = penny_perlin_4d_tile(x, y, z, w, period);
  float value = fabsf(((ch == 0) ? s.x : ((ch == 1) ? s.y : s.z)) * 0.73f) * scale;
  float cx = x + x, cy = y + y, cz = z + z;
  scale_sum += scale;
  scale *= dampening;
  for (int i = 0; i < octaves; ++i) {
    float3 si = penny_perlin_4d_tile(cx, cy, cz, w, period);
    value += fabsf(((ch == 0) ? si.x : ((ch == 1) ? si.y : si.z)) * 0.73f) * scale;
    cx += cx;
    cy += cy;
    cz += cz;
    scale_sum += scale;
    scale *= dampening;
  }
  return (scale_sum != 0.0f) ? (value / scale_sum) : 0.0f;
}

/* Penny Poxo - recursive abs-turb running-blend with fractional octave support. */
ccl_device_inline float penny_poxo(const float x,
                                   const float y,
                                   const float z,
                                   const float w,
                                   float detail,
                                   float roughness,
                                   int4 period,
                                   int ch)
{
  float scalar = -roughness; /* rough 0.5 -> -0.5 = Penny default; slider live */
  float size = 1.0f, value = 0.0f;
  int ioct = (int)detail;
  for (int i = 0; i < ioct; ++i) {
    float v = penny_poxo_absturb(x * size, y * size, z * size, w, 4, scalar, period, ch);
    value += fabsf(v - value) * 0.5f;
    size *= 2.0f;
  }
  float v = penny_poxo_absturb(x * size, y * size, z * size, w, 4, scalar, period, ch);
  value += fabsf(v - value) * 0.5f * (detail - floorf(detail));
  if (detail != 0.0f)
    value /= detail;
  value = clamp(powf(value, 0.7369656f), 0.0f, 1.0f);
  value = value * value * 2.0f;
  value = darf_noxstep(0.0f, 0.5f, value);
  value = darf_noxstep(0.01f, 0.9f, (detail != 0.0f) ? (value / detail) : value);
  return value;
}

ccl_device_noinline float3 penny_poxo_tile(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int4 period,
                                           float detail,
                                           float roughness,
                                           int blend_mode,
                                           float lvl_lo,
                                           float lvl_mid,
                                           float lvl_hi)
{
  float d = fmaxf(detail, 1.0f);
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = penny_poxo(x + dch, y + dch, z + dch, w, d, roughness, period, ch);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);
    float r;
    if (blend_mode == 0) {
      r = v;
    } /* HDR passthrough (match EEVEE + other noises) */
    else {
      float v2 = penny_poxo(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, d, roughness, period, ch);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      r = clamp(penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    if (ch == 0)
      result.x = r;
    else if (ch == 1)
      result.y = r;
    else
      result.z = r;
  }
  return result;
}

/* Penny Pezo abs-turb variant (ABS_Turbulence_Function_2): abs(2*p01-1)/scale summed,
 * decorrelation offset kept, scale doubling, no normalization. Per-channel. */
ccl_device_inline float penny_pezo_absturb2(
    const float x, const float y, const float z, const float w, int octaves, int4 period, int ch)
{
  float value = 0.0f, scale = 1.0f;
  int4 op = period;
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 p = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w * scale + off, op) *
                   0.5f +
               make_float3(0.5f, 0.5f, 0.5f);
    float p01 = (ch == 0) ? p.x : ((ch == 1) ? p.y : p.z);
    value += fabsf(2.0f * p01 - 1.0f) / scale;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
  }
  return value;
}

/* Penny Pezo signed turbulence variant (Turbulence_Function_3): snoise/f summed
 * (offset on x,y only - z unshifted), then noxstep(-0.62,0.62) (widened). Per-channel. */
ccl_device_inline float penny_pezo_turb3(
    const float x, const float y, const float z, const float w, int octaves, int4 period, int ch)
{
  float value = 0.0f, f = 1.0f;
  int4 op = period;
  for (int i = 0; i < octaves; ++i) {
    float off = (f - 1.0f) * 1000.0f;
    float3 s3 = penny_perlin_4d_tile(x * f + off, y * f + off, z * f, w * f + off, op);
    float s = (ch == 0) ? s3.x : ((ch == 1) ? s3.y : s3.z);
    value += s / f;
    f *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
  }
  /* Penny snoise range: turb3 accumulates signed perlin then maps [-1,1]->[0,1].
   * (An earlier -0.62..0.62 hand-tuning was compensating for a range mismatch;
   * restored to the Penny -1..1. Shared by Pezo and Sema.) */
  return darf_noxstep(-1.0f, 1.0f, value);
}

/* Penny Pezo - smoothstep-gated turbulence product, masked by abs-turb. Per-channel.
 * gate smoothstep(0.4,1.0) (opened from 0.6 - key range fix); output distribution-fit
 * to Penny: smoothstep(0,0.40) -> noxstep(0.08,0.65). REVISIT: midtone fizz runs bright. */
ccl_device_inline float penny_pezo(const float x,
                                   const float y,
                                   const float z,
                                   const float w,
                                   int octaves,
                                   int4 period,
                                   int ch,
                                   float roughness)
{
  /* roughness drives gate spread, centered: 0.5 -> 0.45 (softer onset for larger soft features).
   */
  /* Pezo per-octave gate onset. Penny used a fixed 0.6; lowered to 0.45 to soften
   * feature edges to match the reference. Roughness centers on it. */
  float gate_lo = clamp(0.45f + (roughness - 0.5f) * 0.3f, 0.2f, 0.7f);
  float value = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    float fi = (float)i;
    /* Large-feature freq: coord * 0.5 (Penny-faithful features were ~4x too small
     * at raw coord; halving the frequency doubled their size to match). */
    float t = penny_pezo_turb3(x * 0.5f + fi * 19.55f,
                               y * 0.5f + fi * 20.612f,
                               z * 0.5f + fi * 21.175f,
                               w,
                               1,
                               period,
                               ch);
    float temp = 1.0f - fabsf(1.0f - 2.0f * t);
    value *= darf_smoothstep(gate_lo, 1.0f, temp);
  }
  /* Fine-detail freq: coord * 15 (Penny's *10 left the background grain too coarse). */
  float temp = penny_pezo_absturb2(x * 15.0f, y * 15.0f, z * 15.0f, w, octaves, period, ch);
  temp = darf_noxstep(0.32f, 1.0f, darf_noxstep(-1.25f, 1.25f, temp));
  value = mix(0.25f, 1.0f, value) * darf_noxstep(-0.2f, 1.2f, temp);
  value = darf_smoothstep(0.0f, 0.40f, value);
  return value;
}

ccl_device_noinline float3 penny_pezo_tile(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int4 period,
                                           int octaves,
                                           float roughness,
                                           int blend_mode,
                                           float lvl_lo,
                                           float lvl_mid,
                                           float lvl_hi)
{
  octaves = max(octaves, 1);
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = penny_pezo(x + dch, y + dch, z + dch, w, octaves, period, ch, roughness);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);
    float r;
    if (blend_mode == 0) {
      r = v;
    } /* HDR passthrough (match EEVEE + other noises) */
    else {
      float v2 = penny_pezo(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, octaves, period, ch, roughness);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      r = clamp(penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    if (ch == 0)
      result.x = r;
    else if (ch == 1)
      result.y = r;
    else
      result.z = r;
  }
  return result;
}

/* Penny Dents - MAX across octaves of folded 1-octave turbulence. Per octave:
 * boxstep(0,0.56, abs(1 - 2*noxstep(-1,1, turb))), keep running max. Fractional tail.
 * Non-integer per-axis octave offsets (grid fix vs integer +(i+1) on tileable turb).
 * Output: boxstep(0.25,1)*v -> pow(0.8) -> noxstep(0,1.15) (distribution-fit to Penny).
 * REVISIT: highlights run hot (>=0.8 11.4% vs Penny 6.0%). Per-channel. */
ccl_device_inline float penny_dents(const float x,
                                    const float y,
                                    const float z,
                                    const float w,
                                    float detail,
                                    int4 period,
                                    int ch,
                                    float roughness)
{
  int ioct = (int)detail;
  /* roughness drives fold sharpness, centered: 0.5 -> 0.56 (original). */
  /* Dents fold sharpness (Penny 0.56). Roughness centers on it. */
  float fold_hi = clamp(0.56f + (roughness - 0.5f) * 0.5f, 0.12f, 0.95f);
  float the_value = 0.0f;
  int i;
  for (i = 0; i < ioct; ++i) {
    float o = (float)(i + 1);
    float3 t3 = penny_reg_turb2_signed(
        x + o * 12.793f, y + o * 13.547f, z + o * 14.219f, w, period, 1, 0.5f);
    float turb = (ch == 0) ? t3.x : ((ch == 1) ? t3.y : t3.z);
    float folded = fabsf(1.0f - 2.0f * darf_noxstep(-1.0f, 1.0f, turb));
    float temp = darf_boxstep(0.0f, fold_hi, folded);
    the_value = fmaxf(the_value, temp);
  }
  float o = (float)(i + 1);
  float3 t3 = penny_reg_turb2_signed(
      x + o * 12.793f, y + o * 13.547f, z + o * 14.219f, w, period, 1, 0.5f);
  float turb = (ch == 0) ? t3.x : ((ch == 1) ? t3.y : t3.z);
  float folded = fabsf(1.0f - 2.0f * darf_noxstep(-1.0f, 1.0f, turb));
  float temp = (detail - floorf(detail)) * darf_boxstep(0.0f, fold_hi, folded);
  the_value = fmaxf(the_value, temp);

  the_value = darf_boxstep(0.25f, 1.0f, the_value) * the_value;
  the_value = powf(the_value, 0.8f);
  the_value = darf_noxstep(0.0f, 1.15f, the_value);
  return the_value;
}

ccl_device_noinline float3 penny_dents_tile(const float x,
                                            const float y,
                                            const float z,
                                            const float w,
                                            int4 period,
                                            float detail,
                                            float roughness,
                                            int blend_mode,
                                            float lvl_lo,
                                            float lvl_mid,
                                            float lvl_hi)
{
  float d = fmaxf(detail, 1.0f);
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = penny_dents(x + dch, y + dch, z + dch, w, d, period, ch, roughness);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);
    float r;
    if (blend_mode == 0) {
      r = v;
    } /* HDR passthrough (match EEVEE + other noises) */
    else {
      float v2 = penny_dents(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, d, period, ch, roughness);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      r = clamp(penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    if (ch == 0)
      result.x = r;
    else if (ch == 1)
      result.y = r;
    else
      result.z = r;
  }
  return result;
}

/* Penny Sema - tightly-gated MAX of folded turbulence + fine absturb2 dither. Reuses Pezo
 * helpers. Per octave: smoothstep(0.8,0.999, 1-abs(1-2*turb3)) running max; output
 * boxstep(0.95,1) + ((1-2*absturb2(*75))*0.25+0.5)*0.05 grain. Per-channel. */
ccl_device_inline float penny_sema(const float x,
                                   const float y,
                                   const float z,
                                   const float w,
                                   int octaves,
                                   int4 period,
                                   int ch,
                                   float roughness)
{
  /* roughness drives gate tightness, centered: 0.5 -> 0.8 (original). */
  /* Sema gate tightness (Penny 0.8). Roughness centers on it; higher = sparser. */
  float gate_lo = clamp(0.8f + (roughness - 0.5f) * 0.3f, 0.5f, 0.95f);
  float the_value = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    float fi = (float)i;
    float t = penny_pezo_turb3(x + fi * 21.7f, y + fi * 11.3f, z + fi * 13.9f, w, 1, period, ch);
    float temp = 1.0f - fabsf(1.0f - 2.0f * t);
    float displace = darf_smoothstep(gate_lo, 0.999f, temp);
    the_value = fmaxf(the_value, displace);
  }
  float ab = penny_pezo_absturb2(x * 75.0f, y * 75.0f, z * 75.0f, w, 1, period, ch);
  float dither = ((1.0f - 2.0f * ab) * 0.25f + 0.5f) * 0.05f;
  the_value = darf_boxstep(0.95f, 1.0f, the_value) + dither;
  return the_value;
}

ccl_device_noinline float3 penny_sema_tile(const float x,
                                           const float y,
                                           const float z,
                                           const float w,
                                           int4 period,
                                           int octaves,
                                           float roughness,
                                           int blend_mode,
                                           float lvl_lo,
                                           float lvl_mid,
                                           float lvl_hi)
{
  octaves = max(octaves, 1);
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = penny_sema(x + dch, y + dch, z + dch, w, octaves, period, ch, roughness);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);
    float r;
    if (blend_mode == 0) {
      r = v;
    } /* HDR passthrough (match EEVEE + other noises) */
    else {
      float v2 = penny_sema(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, octaves, period, ch, roughness);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      r = clamp(penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    if (ch == 0)
      result.x = r;
    else if (ch == 1)
      result.y = r;
    else
      result.z = r;
  }
  return result;
}

/* Penny vl_Noise - Perlin domain-warped by Perlin (variable lacunarity). Build signed offset
 * vector from three Perlin samples at fixed offsets, scale by distortion, warp point, sample
 * Perlin there. Returns p01. Per-channel. (Dead time/period args from Penny dropped.) */
ccl_device_inline float penny_vl_noise(const float x,
                                       const float y,
                                       const float z,
                                       const float w,
                                       float distortion,
                                       int4 period,
                                       int ch)
{
  float3 a = penny_perlin_4d_tile(x - 2.0f, y + 7.0f, z + 8.0f, w, period);
  float3 b = penny_perlin_4d_tile(x - 13.0f, y - 1.0f, z + 4.0f, w, period);
  float3 c = penny_perlin_4d_tile(x + 17.0f, y + 8.0f, z - 4.0f, w, period);
  float ox = 1.0f - 2.0f * (((ch == 0) ? a.x : ((ch == 1) ? a.y : a.z)) * 0.5f + 0.5f);
  float oy = 1.0f - 2.0f * (((ch == 0) ? b.x : ((ch == 1) ? b.y : b.z)) * 0.5f + 0.5f);
  float oz = 1.0f - 2.0f * (((ch == 0) ? c.x : ((ch == 1) ? c.y : c.z)) * 0.5f + 0.5f);
  float3 tp = penny_perlin_4d_tile(
      x + ox * distortion, y + oy * distortion, z + oz * distortion, w, period);
  return ((ch == 0) ? tp.x : ((ch == 1) ? tp.y : tp.z)) * 0.5f + 0.5f;
}

/* Penny VL Noise - fBm sum of vl_Noise octaves (un-normalized /scale), noxstep(-1,1) (widened
 * from Penny's -0.65 to remove clipping). distortion=1.0. Per-channel. */
ccl_device_inline float penny_vl(const float x,
                                 const float y,
                                 const float z,
                                 const float w,
                                 float roughness,
                                 int octaves,
                                 int4 period,
                                 int ch)
{
  float the_value = 0.0f, scale = 1.0f, amp = 1.0f; /* roughness drives falloff; 0.5 == /scale */
  for (int i = 0; i < octaves; ++i) {
    float off = (scale - 1.0f) * 1000.0f;
    float v = penny_vl_noise(
        x * scale + off, y * scale + off, z * scale + off, w, 1.0f, period, ch);
    /* Roughness drives octave amplitude falloff (amp *= roughness); at 0.5 this
     * reproduces the original fixed /scale (2^-i) rolloff exactly. */
    the_value += (2.0f * v - 1.0f) * amp;
    scale *= 2.0f;
    amp *= roughness;
  }
  return darf_noxstep(-1.0f, 1.0f, the_value);
}

ccl_device_noinline float3 penny_vl_tile(const float x,
                                         const float y,
                                         const float z,
                                         const float w,
                                         int4 period,
                                         int octaves,
                                         float roughness,
                                         int blend_mode,
                                         float lvl_lo,
                                         float lvl_mid,
                                         float lvl_hi)
{
  octaves = max(octaves, 1);
  float3 result = make_float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = penny_vl(x + dch, y + dch, z + dch, w, roughness, octaves, period, ch);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);
    float r;
    if (blend_mode == 0) {
      r = v;
    } /* HDR passthrough (match EEVEE + other noises) */
    else {
      float v2 = penny_vl(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, roughness, octaves, period, ch);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      r = clamp(penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    if (ch == 0)
      result.x = r;
    else if (ch == 1)
      result.y = r;
    else
      result.z = r;
  }
  return result;
}

ccl_device_noinline float3 penny_disturb_tile(const float x,
                                              const float y,
                                              const float z,
                                              const float w,
                                              int4 period,
                                              int octaves,
                                              float roughness,
                                              int blend_mode,
                                              float lvl_lo,
                                              float lvl_mid,
                                              float lvl_hi)
{
  octaves = max(octaves, 1);
  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    float dch = (float)ch * 1000.0f;
    float v = disturb_accum(x + dch, y + dch, z + dch, w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[ch] = v; /* HDR passthrough (match EEVEE + other noises) */
    }
    else {
      float v2 = disturb_accum(
          x + dch + 0.5f, y + dch + 0.5f, z + dch + 0.5f, w, period, octaves, roughness, ch);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      res[ch] = clamp(
          penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

ccl_device_inline float3 penny_luka_tile(const float x,
                                         const float y,
                                         const float z,
                                         const float w,
                                         int4 period,
                                         int octaves,
                                         float roughness,
                                         int blend_mode,
                                         float lvl_lo,
                                         float lvl_mid,
                                         float lvl_hi)
{
  octaves = max(octaves, 1);

  /* Faithful pre-transform: scale 0.8 + decorrelation offset (xyz; w passes through). */
  const int ppx = max(period.x, 1), ppy = max(period.y, 1), ppz = max(period.z, 1);
  const int4 lat_period = make_int4(max(1, (int)(0.8f * (float)ppx + 0.5f)),
                                    max(1, (int)(0.8f * (float)ppy + 0.5f)),
                                    max(1, (int)(0.8f * (float)ppz + 0.5f)),
                                    period.w);
  float cx = x * ((float)lat_period.x / (float)ppx) + 127.1f;
  float cy = y * ((float)lat_period.y / (float)ppy) + 64.8f;
  float cz = z * ((float)lat_period.z / (float)ppz) + 26.81f;

  float3 temp = penny_luka_turb(cx, cy, cz, w, lat_period, octaves, roughness);
  /* Displacement: single raw Perlin * 0.275 (== 0.55 * 0.5). */
  float3 disp3 = penny_perlin_4d_tile(cx, cy, cz, w, lat_period) * 0.275f;

  float tch[3];
  tch[0] = temp.x;
  tch[1] = temp.y;
  tch[2] = temp.z;
  float dch[3];
  dch[0] = disp3.x;
  dch[1] = disp3.y;
  dch[2] = disp3.z;
  float res[3];
  for (int ch = 0; ch < 3; ++ch) {
    float d = dch[ch];
    /* Warp #1: (x+d, y-d, z+d). */
    float w1x = cx + d, w1y = cy - d, w1z = cz + d;
    /* Inner sample warps again by +d on all axes. */
    float3 inner3 = penny_luka_turb(w1x + d, w1y + d, w1z + d, w, lat_period, octaves, roughness);
    float ich[3];
    ich[0] = inner3.x;
    ich[1] = inner3.y;
    ich[2] = inner3.z;
    float v = darf_noxstep(-2.0f, 2.0f, ich[ch]);
    v = darf_noxstep(0.47f - tch[ch], 0.525f + tch[ch], v);
    v = darf_noxstep(0.5f, 0.78f, v);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi);

    if (blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float3 inner3b = penny_luka_turb(
          w1x + d + 0.5f, w1y + d + 0.5f, w1z + d + 0.5f, w, lat_period, octaves, roughness);
      float ichb[3];
      ichb[0] = inner3b.x;
      ichb[1] = inner3b.y;
      ichb[2] = inner3b.z;
      float v2 = darf_noxstep(-2.0f, 2.0f, ichb[ch]);
      v2 = darf_noxstep(0.47f - tch[ch], 0.525f + tch[ch], v2);
      v2 = darf_noxstep(0.5f, 0.78f, v2);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      res[ch] = clamp(
          penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
  }
  return make_float3(res[0], res[1], res[2]);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SVM gradient helper
 *
 * Dispatches to the correct penny_*_tile() and returns Factor (.x channel).
 * Used by the central-difference gradient computation. Only called when the
 * Gradient output socket is connected (stack_valid check).
 * \{ */
ccl_device_inline float penny_eval_fac(float x,
                                       float y,
                                       float z,
                                       float w,
                                       int noise_type,
                                       float detail,
                                       float roughness,
                                       const int4 period,
                                       int blend_mode,
                                       float level_min,
                                       float level_mid,
                                       float level_max)
{
  int octaves = max((int)floorf(detail), 1);
  float3 rgb;
  if (noise_type == 1) {
    rgb = penny_turbulence_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 2) {
    rgb = penny_stupl_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 3) {
    rgb = penny_luka_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 4) {
    rgb = penny_blistered_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 5) {
    rgb = penny_box_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 6) {
    rgb = penny_mod_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 7) {
    rgb = penny_nutous_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 8) {
    rgb = penny_cranal_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 9) {
    rgb = penny_buya_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 10) {
    rgb = penny_cell_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 11) {
    rgb = penny_disturb_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 12) {
    rgb = penny_hama_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 13) {
    rgb = penny_naki_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 14) {
    rgb = penny_poxo_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 15) {
    rgb = penny_pezo_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 16) {
    rgb = penny_dents_tile(
        x, y, z, w, period, detail, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 17) {
    rgb = penny_sema_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else if (noise_type == 18) {
    rgb = penny_vl_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  else {
    rgb = penny_fbm_tile(
        x, y, z, w, period, octaves, roughness, blend_mode, level_min, level_mid, level_max);
  }
  return rgb.x;
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name SVM dispatcher
 *
 * Unpacks the SVM bytecode stack, loads socket values with defaults, applies
 * Scale conversion (percentage → factor), assembles the coordinate per
 * dimensionality (1D/2D/3D/4D), and dispatches to the appropriate
 * `penny_*_tile()` function based on `noise_type`.
 *
 * SVM stack layout follows the same packed-uchar4 convention as
 * `svm_node_tex_noise`. The data nodes that follow the main op carry:
 *
 *   - `defaults1`: default W, Scale, Detail, Roughness.
 *   - `period_offsets`: Period stack offsets + blend_mode, noise_type, dimensions.
 *   - `period_defaults`: default Period X/Y/Z/W.
 *   - `scale_offsets`: Scale X/Y/Z/W stack offsets.
 *   - `scale_defaults`: default Scale X/Y/Z/W.
 *   - `level_defaults`: default Level Min/Mid/Max.
 *
 * Factor output = `rgb.x`; Color output = `rgb`. Both are stored to the
 * SVM stack only if their respective output sockets are connected
 * (`stack_valid()`).
 * \{ */
ccl_device_noinline void svm_node_tex_penny(ccl_private float *ccl_restrict stack,
                                            const ccl_global SVMNodeTexPenny &ccl_restrict node)
{
  const int blend_mode = (int)node.blend_mode;
  const int noise_type = (int)node.noise_type;
  const int dimensions = (int)node.dimensions;

  /* Load Vector and W. */
  float3 vector = stack_load_float3(stack, node.coord);
  float w_in = stack_load(stack, node.w);

  /* Scale values are percentages (display = 100 means factor 1.0). */
  const float scale_pct = stack_load(stack, node.scale);
  const float scale_x_pct = stack_load(stack, node.scale_x);
  const float scale_y_pct = stack_load(stack, node.scale_y);
  const float scale_z_pct = stack_load(stack, node.scale_z);
  const float scale_w_pct = stack_load(stack, node.scale_w);

  float detail_f = stack_load(stack, node.detail);
  float roughness = stack_load(stack, node.roughness);
  float level_min = stack_load(stack, node.level_min);
  float level_mid = stack_load(stack, node.level_mid);
  float level_max = stack_load(stack, node.level_max);
  detail_f = clamp(detail_f, 0.0f, 15.0f);
  roughness = fmaxf(roughness, 0.0f);

  /* Combine global × per-axis: actual_scale = (Scale/100) × (Scale_axis/100).
   * Bigger Scale = larger features, so divide input by the scale.
   * Avoid div-by-zero with a small floor. */
  const float global_factor = scale_pct * 0.01f;
  const float fx_scale = fmaxf(global_factor * scale_x_pct * 0.01f, 1e-6f) * 0.25f;
  const float fy_scale = fmaxf(global_factor * scale_y_pct * 0.01f, 1e-6f) * 0.25f;
  const float fz_scale = fmaxf(global_factor * scale_z_pct * 0.01f, 1e-6f) * 0.25f;
  const float fw_scale = fmaxf(global_factor * scale_w_pct * 0.01f, 1e-6f) * 0.25f;

  /* Assemble the sampled coordinate per dimension (matches Blender Noise + GN + EEVEE):
   * 1D -> (0,0,0,W); 2D -> (X,Y,0,0); 3D -> (X,Y,Z,0); 4D -> (X,Y,Z,W). */
  float w_scaled;
  if (dimensions == 1) {
    w_scaled = w_in / fw_scale;
    vector.x = 0.0f;
    vector.y = 0.0f;
    vector.z = 0.0f;
  }
  else if (dimensions == 2) {
    vector.x /= fx_scale;
    vector.y /= fy_scale;
    vector.z = 0.0f;
    w_scaled = 0.0f;
  }
  else if (dimensions == 3) {
    vector.x /= fx_scale;
    vector.y /= fy_scale;
    vector.z /= fz_scale;
    w_scaled = 0.0f;
  }
  else { /* 4D */
    vector.x /= fx_scale;
    vector.y /= fy_scale;
    vector.z /= fz_scale;
    w_scaled = w_in / fw_scale;
  }

  const float period_x_f = stack_load(stack, node.period_x);
  const float period_y_f = stack_load(stack, node.period_y);
  const float period_z_f = stack_load(stack, node.period_z);
  const float period_w_f = stack_load(stack, node.period_w);

  const int4 period = make_int4(clamp((int)period_x_f, 1, 256),
                                clamp((int)period_y_f, 1, 256),
                                clamp((int)period_z_f, 1, 256),
                                clamp((int)period_w_f, 1, 256));

  int octaves = max((int)floorf(detail_f), 1);

  float3 rgb;
  if (noise_type == 1) {
    rgb = penny_turbulence_tile(vector.x,
                                vector.y,
                                vector.z,
                                w_scaled,
                                period,
                                octaves,
                                roughness,
                                blend_mode,
                                level_min,
                                level_mid,
                                level_max);
  }
  else if (noise_type == 2) {
    rgb = penny_stupl_tile(vector.x,
                           vector.y,
                           vector.z,
                           w_scaled,
                           period,
                           octaves,
                           roughness,
                           blend_mode,
                           level_min,
                           level_mid,
                           level_max);
  }
  else if (noise_type == 3) {
    rgb = penny_luka_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          octaves,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 4) {
    rgb = penny_blistered_tile(vector.x,
                               vector.y,
                               vector.z,
                               w_scaled,
                               period,
                               octaves,
                               roughness,
                               blend_mode,
                               level_min,
                               level_mid,
                               level_max);
  }
  else if (noise_type == 5) {
    rgb = penny_box_tile(vector.x,
                         vector.y,
                         vector.z,
                         w_scaled,
                         period,
                         octaves,
                         roughness,
                         blend_mode,
                         level_min,
                         level_mid,
                         level_max);
  }
  else if (noise_type == 6) {
    rgb = penny_mod_tile(vector.x,
                         vector.y,
                         vector.z,
                         w_scaled,
                         period,
                         octaves,
                         roughness,
                         blend_mode,
                         level_min,
                         level_mid,
                         level_max);
  }
  else if (noise_type == 7) {
    rgb = penny_nutous_tile(vector.x,
                            vector.y,
                            vector.z,
                            w_scaled,
                            period,
                            octaves,
                            roughness,
                            blend_mode,
                            level_min,
                            level_mid,
                            level_max);
  }
  else if (noise_type == 8) {
    rgb = penny_cranal_tile(vector.x,
                            vector.y,
                            vector.z,
                            w_scaled,
                            period,
                            octaves,
                            roughness,
                            blend_mode,
                            level_min,
                            level_mid,
                            level_max);
  }
  else if (noise_type == 9) {
    rgb = penny_buya_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          octaves,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 10) {
    rgb = penny_cell_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          octaves,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 11) {
    rgb = penny_disturb_tile(vector.x,
                             vector.y,
                             vector.z,
                             w_scaled,
                             period,
                             octaves,
                             roughness,
                             blend_mode,
                             level_min,
                             level_mid,
                             level_max);
  }
  else if (noise_type == 12) {
    rgb = penny_hama_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          octaves,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 13) {
    rgb = penny_naki_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          octaves,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 14) {
    rgb = penny_poxo_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          detail_f,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 15) {
    rgb = penny_pezo_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          octaves,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 16) {
    rgb = penny_dents_tile(vector.x,
                           vector.y,
                           vector.z,
                           w_scaled,
                           period,
                           detail_f,
                           roughness,
                           blend_mode,
                           level_min,
                           level_mid,
                           level_max);
  }
  else if (noise_type == 17) {
    rgb = penny_sema_tile(vector.x,
                          vector.y,
                          vector.z,
                          w_scaled,
                          period,
                          octaves,
                          roughness,
                          blend_mode,
                          level_min,
                          level_mid,
                          level_max);
  }
  else if (noise_type == 18) {
    rgb = penny_vl_tile(vector.x,
                        vector.y,
                        vector.z,
                        w_scaled,
                        period,
                        octaves,
                        roughness,
                        blend_mode,
                        level_min,
                        level_mid,
                        level_max);
  }
  else {
    rgb = penny_fbm_tile(vector.x,
                         vector.y,
                         vector.z,
                         w_scaled,
                         period,
                         octaves,
                         roughness,
                         blend_mode,
                         level_min,
                         level_mid,
                         level_max);
  }

  /* Final global Levels REMOVED and output left unclamped (HDR passthrough).
   * level_min/mid/max are now threaded into each tile and applied per-noise
   * (output Levels), so the sliders act before the noise's own tone shaping. */
  /* Noise functions already return display-range; pass through unclamped. */
  float fac = rgb.x;
  float3 col = make_float3(rgb.x, rgb.y, rgb.z);

  if (stack_valid(node.fac_offset)) {
    stack_store_float(stack, node.fac_offset, fac);
  }
  if (stack_valid(node.color_offset)) {
    stack_store_float3(stack, node.color_offset, col);
  }
  if (stack_valid(node.gradient_offset)) {
    /* Raw pre-Levels gradient, central differences, converted to co-space so
     * Cycles matches the EEVEE/GLSL Penny gradient exactly. Only evaluated when
     * the Gradient output is connected. Identity Levels (0, 0.5, 1) hit the
     * tiles' passthrough fast-path -> underlying continuous field, noise-type
     * agnostic. vector is already scaled noise space; dividing each axis by its
     * f*_scale converts d/d(noise) to d/d(co). W held fixed; xyz only. Raw and
     * unnormalized: magnitude carries local slope. */
    const float delta = 0.01f;
    const float inv_2d = 1.0f / (2.0f * delta);
    float gx = penny_eval_fac(vector.x + delta,
                              vector.y,
                              vector.z,
                              w_scaled,
                              noise_type,
                              detail_f,
                              roughness,
                              period,
                              blend_mode,
                              0.0f,
                              0.5f,
                              1.0f) -
               penny_eval_fac(vector.x - delta,
                              vector.y,
                              vector.z,
                              w_scaled,
                              noise_type,
                              detail_f,
                              roughness,
                              period,
                              blend_mode,
                              0.0f,
                              0.5f,
                              1.0f);
    float gy = penny_eval_fac(vector.x,
                              vector.y + delta,
                              vector.z,
                              w_scaled,
                              noise_type,
                              detail_f,
                              roughness,
                              period,
                              blend_mode,
                              0.0f,
                              0.5f,
                              1.0f) -
               penny_eval_fac(vector.x,
                              vector.y - delta,
                              vector.z,
                              w_scaled,
                              noise_type,
                              detail_f,
                              roughness,
                              period,
                              blend_mode,
                              0.0f,
                              0.5f,
                              1.0f);
    float gz = penny_eval_fac(vector.x,
                              vector.y,
                              vector.z + delta,
                              w_scaled,
                              noise_type,
                              detail_f,
                              roughness,
                              period,
                              blend_mode,
                              0.0f,
                              0.5f,
                              1.0f) -
               penny_eval_fac(vector.x,
                              vector.y,
                              vector.z - delta,
                              w_scaled,
                              noise_type,
                              detail_f,
                              roughness,
                              period,
                              blend_mode,
                              0.0f,
                              0.5f,
                              1.0f);
    stack_store_float3(stack,
                       node.gradient_offset,
                       make_float3(gx / fx_scale, gy / fy_scale, gz / fz_scale) * inv_2d);
  }
}

/** \} */

CCL_NAMESPACE_END
