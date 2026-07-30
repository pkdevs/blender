/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Penny Noise EEVEE GLSL implementation. Mirrors the Cycles SVM kernel
 * (penny.h) for visual consistency across backends: same algorithms, blend
 * modes, Levels, and hash. */

#include "gpu_shader_common_hash.glsl"
#include "gpu_shader_material_noise.glsl"

/* ---- Foundation: gradient + tileable 4D Perlin ----
 * Custom gradient must match penny_grad in penny.h and the OSL shader
 * exactly, or EEVEE won't match Cycles. Uses Blender's fade() from
 * gpu_shader_material_noise.glsl (same quintic polynomial as penny_fade). */
float penny_grad(uint hash, float x, float y, float z, float w)
{
  uint h = hash & 31u;
  uint sel = h >> 3u;
  float a, b, c;
  if (sel == 0u) {
    a = y;
    b = z;
    c = w;
  }
  else if (sel == 1u) {
    a = x;
    b = z;
    c = w;
  }
  else if (sel == 2u) {
    a = x;
    b = y;
    c = w;
  }
  else {
    a = x;
    b = y;
    c = z;
  }
  float sa = ((h & 4u) != 0u) ? a : -a;
  float sb = ((h & 2u) != 0u) ? b : -b;
  float sc = ((h & 1u) != 0u) ? c : -c;
  return sa + sb + sc;
}

float3 penny_perlin_4d_tile(float4 vec, int4 period)
{
  int X, Y, Z, W;
  float fx, fy, fz, fw;

  FLOORFRAC(vec.x, X, fx);
  FLOORFRAC(vec.y, Y, fy);
  FLOORFRAC(vec.z, Z, fz);
  FLOORFRAC(vec.w, W, fw);

  int px = max(period.x, 1);
  int py = max(period.y, 1);
  int pz = max(period.z, 1);
  int pw = max(period.w, 1);
  X = X - (X / px) * px;
  if (X < 0) {
    X += px;
  }
  Y = Y - (Y / py) * py;
  if (Y < 0) {
    Y += py;
  }
  Z = Z - (Z / pz) * pz;
  if (Z < 0) {
    Z += pz;
  }
  W = W - (W / pw) * pw;
  if (W < 0) {
    W += pw;
  }
  int X1 = (X + 1 == px) ? 0 : X + 1;
  int Y1 = (Y + 1 == py) ? 0 : Y + 1;
  int Z1 = (Z + 1 == pz) ? 0 : Z + 1;
  int W1 = (W + 1 == pw) ? 0 : W + 1;

  float u = fade(fx);
  float v = fade(fy);
  float t = fade(fz);
  float s = fade(fw);

  float fxm = fx - 1.0f;
  float fym = fy - 1.0f;
  float fzm = fz - 1.0f;
  float fwm = fw - 1.0f;

  uint h0 = uint(hash_int4(X, Y, Z, W));
  uint h1 = uint(hash_int4(X1, Y, Z, W));
  uint h2 = uint(hash_int4(X, Y1, Z, W));
  uint h3 = uint(hash_int4(X1, Y1, Z, W));
  uint h4 = uint(hash_int4(X, Y, Z1, W));
  uint h5 = uint(hash_int4(X1, Y, Z1, W));
  uint h6 = uint(hash_int4(X, Y1, Z1, W));
  uint h7 = uint(hash_int4(X1, Y1, Z1, W));
  uint h8 = uint(hash_int4(X, Y, Z, W1));
  uint h9 = uint(hash_int4(X1, Y, Z, W1));
  uint h10 = uint(hash_int4(X, Y1, Z, W1));
  uint h11 = uint(hash_int4(X1, Y1, Z, W1));
  uint h12 = uint(hash_int4(X, Y, Z1, W1));
  uint h13 = uint(hash_int4(X1, Y, Z1, W1));
  uint h14 = uint(hash_int4(X, Y1, Z1, W1));
  uint h15 = uint(hash_int4(X1, Y1, Z1, W1));

  float channels[3];
  for (int c = 0; c < 3; c = c + 1) {
    uint sh = uint(c * 5);
    float n0 = penny_grad(h0 >> sh, fx, fy, fz, fw);
    float n1 = penny_grad(h1 >> sh, fxm, fy, fz, fw);
    float n2 = penny_grad(h2 >> sh, fx, fym, fz, fw);
    float n3 = penny_grad(h3 >> sh, fxm, fym, fz, fw);
    float n4 = penny_grad(h4 >> sh, fx, fy, fzm, fw);
    float n5 = penny_grad(h5 >> sh, fxm, fy, fzm, fw);
    float n6 = penny_grad(h6 >> sh, fx, fym, fzm, fw);
    float n7 = penny_grad(h7 >> sh, fxm, fym, fzm, fw);
    float n8 = penny_grad(h8 >> sh, fx, fy, fz, fwm);
    float n9 = penny_grad(h9 >> sh, fxm, fy, fz, fwm);
    float n10 = penny_grad(h10 >> sh, fx, fym, fz, fwm);
    float n11 = penny_grad(h11 >> sh, fxm, fym, fz, fwm);
    float n12 = penny_grad(h12 >> sh, fx, fy, fzm, fwm);
    float n13 = penny_grad(h13 >> sh, fxm, fy, fzm, fwm);
    float n14 = penny_grad(h14 >> sh, fx, fym, fzm, fwm);
    float n15 = penny_grad(h15 >> sh, fxm, fym, fzm, fwm);

    float x00 = mix(n0, n1, u);
    float x01 = mix(n2, n3, u);
    float x10 = mix(n4, n5, u);
    float x11 = mix(n6, n7, u);
    float x20 = mix(n8, n9, u);
    float x21 = mix(n10, n11, u);
    float x30 = mix(n12, n13, u);
    float x31 = mix(n14, n15, u);
    float y0 = mix(x00, x01, v);
    float y1 = mix(x10, x11, v);
    float y2 = mix(x20, x21, v);
    float y3 = mix(x30, x31, v);
    float z0 = mix(y0, y1, t);
    float z1 = mix(y2, y3, t);
    channels[c] = mix(z0, z1, s);
  }

  return float3(channels[0], channels[1], channels[2]);
}

/* ---- Blend modes (Photoshop-style, 24 modes) ----
 * Operate on [0,1] scalars per-channel. Mode 0 = Normal (passthrough).
 * Integer mode IDs must match penny.h, OSL, and the RNA enum exactly. */
float penny_blendReflect(float base, float blend)
{
  return (blend == 1.0f) ? blend : min(base * base / (1.0f - blend), 1.0f);
}
float penny_blendAdd(float base, float blend)
{
  return base + blend;
}
float penny_blendMultiply(float base, float blend)
{
  return base * blend;
}
float penny_blendDifference(float base, float blend)
{
  return abs(base - blend);
}
float penny_blendAverage(float base, float blend)
{
  return (base + blend) * 0.5f;
}
float penny_blendColorBurn(float base, float blend)
{
  return (blend == 0.0f) ? blend : (1.0f - ((1.0f - base) / blend));
}
float penny_blendColorDodge(float base, float blend)
{
  return (blend == 1.0f) ? blend : base / (1.0f - blend);
}
float penny_blendDarken(float base, float blend)
{
  return min(blend, base);
}
float penny_blendLighten(float base, float blend)
{
  return max(blend, base);
}
float penny_blendExclusion(float base, float blend)
{
  return base + blend - 2.0f * base * blend;
}
float penny_blendGlow(float base, float blend)
{
  return penny_blendReflect(blend, base);
}
float penny_blendOverlay(float base, float blend)
{
  return (base < 0.5f) ? (2.0f * base * blend) : (1.0f - 2.0f * (1.0f - base) * (1.0f - blend));
}
float penny_blendHardLight(float base, float blend)
{
  return penny_blendOverlay(blend, base);
}
float penny_blendVividLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendColorBurn(base, 2.0f * blend) :
                          penny_blendColorDodge(base, 2.0f * (blend - 0.5f));
}
float penny_blendHardMix(float base, float blend)
{
  return (penny_blendVividLight(base, blend) < 0.5f) ? 0.0f : 1.0f;
}
float penny_blendLinearBurn(float base, float blend)
{
  return base + blend - 1.0f;
}
float penny_blendLinearDodge(float base, float blend)
{
  return base + blend;
}
float penny_blendLinearLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendLinearBurn(base, 2.0f * blend) :
                          penny_blendLinearDodge(base, 2.0f * (blend - 0.5f));
}
float penny_blendNegation(float base, float blend)
{
  return 1.0f - abs(1.0f - base - blend);
}
float penny_blendNormal(float base, float blend)
{
  return blend;
}
float penny_blendPhoenix(float base, float blend)
{
  return min(base, blend) - max(base, blend) + 1.0f;
}
float penny_blendPinLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendDarken(base, 2.0f * blend) :
                          penny_blendLighten(base, 2.0f * (blend - 0.5f));
}
float penny_blendScreen(float base, float blend)
{
  return base + blend - base * blend;
}
float penny_blendSoftLight(float base, float blend)
{
  return (blend < 0.5f) ? (2.0f * base * blend + base * base * (1.0f - 2.0f * blend)) :
                          (sqrt(base) * (2.0f * blend - 1.0f) + 2.0f * base * (1.0f - blend));
}
float penny_blendSubtract(float base, float blend)
{
  return base + blend - 1.0f;
}

/* Dispatcher. blend_mode arrives as a constant, so the GLSL->Metal compiler
 * folds this to the single selected branch (no runtime switch cost). */
float penny_blend(int mode, float base, float blend)
{
  if (mode == 1) {
    return penny_blendAdd(base, blend);
  }
  else if (mode == 2) {
    return penny_blendMultiply(base, blend);
  }
  else if (mode == 3) {
    return penny_blendDifference(base, blend);
  }
  else if (mode == 4) {
    return penny_blendDarken(base, blend);
  }
  else if (mode == 5) {
    return penny_blendLighten(base, blend);
  }
  else if (mode == 6) {
    return penny_blendColorDodge(base, blend);
  }
  else if (mode == 7) {
    return penny_blendColorBurn(base, blend);
  }
  else if (mode == 8) {
    return penny_blendOverlay(base, blend);
  }
  else if (mode == 9) {
    return penny_blendAverage(base, blend);
  }
  else if (mode == 10) {
    return penny_blendExclusion(base, blend);
  }
  else if (mode == 11) {
    return penny_blendGlow(base, blend);
  }
  else if (mode == 12) {
    return penny_blendHardLight(base, blend);
  }
  else if (mode == 13) {
    return penny_blendHardMix(base, blend);
  }
  else if (mode == 14) {
    return penny_blendLinearBurn(base, blend);
  }
  else if (mode == 15) {
    return penny_blendLinearDodge(base, blend);
  }
  else if (mode == 16) {
    return penny_blendLinearLight(base, blend);
  }
  else if (mode == 17) {
    return penny_blendNegation(base, blend);
  }
  else if (mode == 18) {
    return penny_blendPhoenix(base, blend);
  }
  else if (mode == 19) {
    return penny_blendPinLight(base, blend);
  }
  else if (mode == 20) {
    return penny_blendReflect(base, blend);
  }
  else if (mode == 21) {
    return penny_blendScreen(base, blend);
  }
  else if (mode == 22) {
    return penny_blendSoftLight(base, blend);
  }
  else if (mode == 23) {
    return penny_blendSubtract(base, blend);
  }
  else if (mode == 24) {
    return penny_blendVividLight(base, blend);
  }
  else {
    return penny_blendNormal(base, blend);
  } /* 0 = Normal */
}

/* ---- darf_* helpers — shaping primitives ----
 * Unclamped remap (noxstep), clamped remap (boxstep), Hermite smoothstep,
 * power-curve bias, linear map, and clip. Must match penny.h, OSL, and
 * blenlib (noise_penny.cc) exactly. */
float darf_mix(float a, float b, float l)
{
  return ((1.0f - l) * a) + (b * l);
}
float darf_clip(float n, float lower, float upper)
{
  return max(lower, min(n, upper));
}
float darf_boxstep(float a, float b, float x)
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
float darf_smoothstep(float a, float b, float x)
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
/* Unclamped linear remap: maps x from [a,b] → [0,1] WITHOUT clamping.
 * Extrapolation beyond [0,1] is intentional (Buya HDR recovery depends on it). */
float darf_noxstep(float a, float b, float x)
{
  if (b == a) {
    return 0.0f;
  }
  return (x - a) / (b - a);
}
float darf_bias(float b, float x)
{
  return pow(x, log(b) / log(0.5f));
}
float darf_map(float value, float old_lo, float old_hi, float new_lo, float new_hi)
{
  float old_range = old_hi - old_lo;
  if (old_range == 0.0f) {
    return new_lo;
  }
  float new_range = new_hi - new_lo;
  return (((value - old_lo) * new_range) / old_range) + new_lo;
}

/* ---- Levels — three-point output remap ----
 * penny_levels: clamped (used by Buya's baked table).
 * penny_oct_levels: unclamped noxstep (used by all 19 noise types).
 * Identity at (0, 0.5, 1) — noises unchanged unless sliders move. */

/* Clamped Levels. lo/hi = black/white points, mid = gamma midpoint. */
float penny_levels(float x, float lo, float mid, float hi)
{
  /* identity at default (lo=0, mid=0.5, hi=1): pass through UNCLAMPED. Levels only
   * engages (and clamps, for pow-safety) when the user adjusts the sliders. */
  if (lo == 0.0f && mid == 0.5f && hi == 1.0f) {
    return x;
  }
  float n = clamp((x - lo) / max(hi - lo, 1e-6f), 0.0f, 1.0f);
  float g = log(clamp(mid, 0.001f, 0.999f)) / log(0.5f);
  return pow(n, g);
}

/* Per-octave Levels: sign-safe, NON-CLIPPING. Identity at default (0/0.5/1) returns x
 * unchanged (preserves every noise at default sliders). When adjusted, remaps without
 * clamping and applies gamma symmetrically around 0 so out-of-range values never NaN. */
float penny_oct_levels(float x, float low, float mid, float high)
{
  /* Per-octave Levels (David's model): noxstep(low,high) remap + gamma(mid).
   * Identity at default (0/0.5/1) - noises unchanged unless sliders move.
   * Input expected in [0,1]; noxstep guarantees n in [0,1] so pow is NaN-safe. */
  if (low == 0.0f && mid == 0.5f && high == 1.0f) {
    return x;
  }
  float n = darf_noxstep(low, high, x);
  float g = log(clamp(mid, 0.001f, 0.999f)) / log(0.5f);
  return pow(n, g);
}

/* Per-octave Levels globals: set once in node_tex_penny, read inside turb helpers
 * (avoids threading params through the shared-helper web). Identity at default. */
float g_lvl_lo = 0.0f;
float g_lvl_mid = 0.5f;
float g_lvl_hi = 1.0f;

/* ---- Noise type implementations ----
 * Each penny_*_tile() function implements one of the 19 noise types.
 * Common pipeline: evaluate per-channel → penny_oct_levels → optional blend.
 * Building blocks (penny_turb_additive, penny_abs_turb2, penny_reg_turb2,
 * penny_blist_turb, penny_luka_turb, etc.) are shared helpers. */

/* fBm — fractal Brownian motion. */
float3 penny_fbm_tile(float4 co,
                      float detail,
                      float roughness,
                      int4 period,
                      int blend_mode,
                      float lvl_lo,
                      float lvl_mid,
                      float lvl_hi)
{
  int octaves = max(int(detail), 1);

  if (blend_mode == 0) {
    /* Classic additive fBm — preserves the original Penny look. */
    float amp = 1.0f;
    float freq = 1.0f;
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float norm = 0.0f;
    int4 oct_period = period;
    for (int i = 0; i < octaves; i = i + 1) {
      float3 _p = penny_perlin_4d_tile(co * freq, oct_period);
      sum = sum + amp * _p;
      norm += amp;
      amp *= roughness;
      freq *= 2.0f;
      oct_period *= 2;
    }
    float3 _fac = (norm > 0.0f) ? (sum / norm) * 0.5f + 0.5f : float3(0.5f, 0.5f, 0.5f);
    return float3(penny_oct_levels(_fac.x, lvl_lo, lvl_mid, lvl_hi),
                  penny_oct_levels(_fac.y, lvl_lo, lvl_mid, lvl_hi),
                  penny_oct_levels(_fac.z, lvl_lo, lvl_mid, lvl_hi));
  }

  /* Blend-mode path: single blend between two decorrelated normalized fBm samples
   * (matches every other tile's pattern; no per-octave clamp compounding). */
  float amp1 = 1.0f, freq1 = 1.0f, norm1 = 0.0f;
  float3 sum1 = float3(0.0f, 0.0f, 0.0f);
  int4 op1 = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float3 _p = penny_perlin_4d_tile(co * freq1, op1);
    sum1 = sum1 + amp1 * _p;
    norm1 += amp1;
    amp1 *= roughness;
    freq1 *= 2.0f;
    op1 *= 2;
  }
  float3 base = (norm1 > 0.0f) ? (sum1 / norm1) * 0.5f + 0.5f : float3(0.5f, 0.5f, 0.5f);

  float4 co2 = float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w);
  float amp2 = 1.0f, freq2 = 1.0f, norm2 = 0.0f;
  float3 sum2 = float3(0.0f, 0.0f, 0.0f);
  int4 op2 = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float3 _p2 = penny_perlin_4d_tile(co2 * freq2, op2);
    sum2 = sum2 + amp2 * _p2;
    norm2 += amp2;
    amp2 *= roughness;
    freq2 *= 2.0f;
    op2 *= 2;
  }
  float3 base2 = (norm2 > 0.0f) ? (sum2 / norm2) * 0.5f + 0.5f : float3(0.5f, 0.5f, 0.5f);

  /* Second layer (base2) blends at 30% strength at 1 octave only; 2+ octaves blend at 100%. */
  float3 blended = float3(penny_blend(blend_mode, base.x, base2.x),
                          penny_blend(blend_mode, base.y, base2.y),
                          penny_blend(blend_mode, base.z, base2.z));
  float blend_strength = (octaves == 1) ? 0.3f : 1.0f;
  float3 _fb = mix(base, blended, blend_strength);
  return float3(penny_oct_levels(_fb.x, lvl_lo, lvl_mid, lvl_hi),
                penny_oct_levels(_fb.y, lvl_lo, lvl_mid, lvl_hi),
                penny_oct_levels(_fb.z, lvl_lo, lvl_mid, lvl_hi));
}

/* Turbulence additive core: returns the Normal-path result for a coordinate.
 * Roughness-driven, normalized, floor-lifted. Reused for self-blend. */
float3 penny_turb_additive(float4 co, int octaves, float roughness, int4 period)
{
  float3 value = float3(0.0f, 0.0f, 0.0f);
  float f = 1.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (f - 1.0f) * 1000.0f;
    float4 sample_co = float4(co.x * f + off, co.y * f + off, co.z * f + off, co.w * f + off);
    int4 oct_period = period * int(f);
    float3 n = penny_perlin_4d_tile(sample_co, oct_period);
    float3 t = 0.7f * (n * 0.5f + 0.5f) + 0.3f;
    value = value + (2.0f * t - 1.0f) * amp;
    norm += amp;
    f *= 2.0f;
    amp *= roughness;
  }
  value = value * (0.5f / norm);
  return float3(darf_noxstep(-0.12f, 0.34f, value.x),
                darf_noxstep(-0.12f, 0.34f, value.y),
                darf_noxstep(-0.12f, 0.34f, value.z));
}

/* Penny Turbulence — RGB, tileable. Self-blend for rich blend modes. */
float3 penny_turbulence_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  float3 base = penny_turb_additive(co, octaves, roughness, period);
  if (blend_mode == 0) {
    /* Turbulence uses OUTPUT Levels (same self-cancel reason as fBm). */
    return float3(penny_oct_levels(base.x, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                  penny_oct_levels(base.y, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                  penny_oct_levels(base.z, g_lvl_lo, g_lvl_mid, g_lvl_hi));
  }
  /* Self-blend: same additive Turbulence at +0.5 shifted coord. */
  float4 co2 = float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w);
  float3 base2 = penny_turb_additive(co2, octaves, roughness, period);
  float3 _tb = float3(
      clamp(penny_blend(blend_mode, clamp(base.x, 0.0f, 1.0f), clamp(base2.x, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.y, 0.0f, 1.0f), clamp(base2.y, 0.0f, 1.0f)),
            0.0f,
            1.0f),
      clamp(penny_blend(blend_mode, clamp(base.z, 0.0f, 1.0f), clamp(base2.z, 0.0f, 1.0f)),
            0.0f,
            1.0f));
  return float3(penny_oct_levels(_tb.x, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                penny_oct_levels(_tb.y, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                penny_oct_levels(_tb.z, g_lvl_lo, g_lvl_mid, g_lvl_hi));
}
/* Inner: absolute-value turbulence, tileable, RGB. Stupl building block.
 * blend_mode 0 = original additive; 1-24 = per-octave blend (scale = opacity). */
/* Inner: absolute-value turbulence, tileable, RGB. Stupl building block. */
/* Maxon Stupl helper: abs-turbulence. Per channel:
 *   p01 = perlin*0.5+0.5;  sum += abs(2*p01 - 1) * amp   (== abs(perlin)*amp)
 * Roughness drives amp; normalized for brightness stability. Tileable. */
float3 penny_abs_turb2(float4 co, int octaves, float roughness, int4 period)
{
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  int4 oct_period = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 sc = float4(
        co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w * scale + off);
    float3 p01 = penny_perlin_4d_tile(sc, oct_period) * 0.5f + 0.5f;
    sum = sum + abs(2.0f * p01 - 1.0f) * amp;
    norm += amp;
    scale *= 2.0f;
    oct_period *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
}

/* Maxon Stupl helper: regular (signed) turbulence. Per channel:
 *   sum += (2*p01 - 1) * amp   (== perlin*amp);  then darf_noxstep(-1,1).
 * Roughness-driven, normalized, tileable. */
float3 penny_reg_turb2(float4 co, int octaves, float roughness, int4 period)
{
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  int4 oct_period = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 sc = float4(
        co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w * scale + off);
    float3 p01 = penny_perlin_4d_tile(sc, oct_period) * 0.5f + 0.5f;
    sum = sum + (2.0f * p01 - 1.0f) * amp;
    norm += amp;
    scale *= 2.0f;
    oct_period *= 2;
    amp *= roughness;
  }
  sum = (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
  return float3(darf_noxstep(-1.0f, 1.0f, sum.x),
                darf_noxstep(-1.0f, 1.0f, sum.y),
                darf_noxstep(-1.0f, 1.0f, sum.z));
}

/* Signed (pre-noxstep) variant of reg_turb2: returns the normalized fractal sum (~[-1,1]).
 * Matches Penny noise_Turbulence's raw output, which Hama's remap chain expects. */
float3 penny_reg_turb2_signed(float4 co, int octaves, float roughness, int4 period)
{
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  int4 oct_period = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 sc = float4(
        co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w * scale + off);
    float3 p01 = penny_perlin_4d_tile(sc, oct_period) * 0.5f + 0.5f;
    sum = sum + (2.0f * p01 - 1.0f) * amp;
    norm += amp;
    scale *= 2.0f;
    oct_period *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
}

/* Maxon Stupl: per-channel domain-warped turbulence. */
float3 penny_stupl_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  float3 turb = penny_abs_turb2(co, octaves, roughness, period);
  /* Displacement: 1 - 2 * reg_turb(co, 1 octave). */
  float3 disp3 = float3(1.0f, 1.0f, 1.0f) - 2.0f * penny_reg_turb2(co, 1, roughness, period);

  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int c = 0; c < 3; c = c + 1) {
    float d = disp3[c];
    float4 warped = float4(co.x - d, co.y + d, co.z + d, co.w);
    float3 inner3 = float3(1.0f, 1.0f, 1.0f) -
                    2.0f * penny_reg_turb2(warped, octaves, roughness, period);
    float inner = inner3[c];
    float v = darf_noxstep(-2.45f, 2.5f, inner);
    v = darf_noxstep(0.5f - turb[c], 0.5f + turb[c], v);
    v = darf_smoothstep(0.0f, 1.0f, v);
    v = darf_bias(0.45f, v);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    float blended_result;
    if (blend_mode == 0) {
      blended_result = v;
    }
    else {
      /* Second operand: same Stupl chain at a phase-shifted coordinate,
       * giving a same-kind correlated signal for classic blend behavior. */
      float4 warped2 = float4(co.x - d + 0.5f, co.y + d + 0.5f, co.z + d + 0.5f, co.w);
      float3 inner3b = float3(1.0f, 1.0f, 1.0f) -
                       2.0f * penny_reg_turb2(warped2, octaves, roughness, period);
      float v2 = darf_noxstep(-2.45f, 2.5f, inner3b[c]);
      v2 = darf_noxstep(0.5f - turb[c], 0.5f + turb[c], v2);
      v2 = darf_smoothstep(0.0f, 1.0f, v2);
      v2 = darf_bias(0.45f, v2);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      blended_result = clamp(
          penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    result[c] = blended_result;
  }
  return result;
}

/* Luka abs-turbulence: abs(snoise * 0.73) per octave. Roughness-driven,
 * normalized, tileable. (Distinct from Stupl's turb2 helpers.) */
float3 penny_luka_turb(float4 co, int octaves, float roughness, int4 period)
{
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  int4 oct_period = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 sc = float4(
        co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w * scale + off);
    float3 n = penny_perlin_4d_tile(sc, oct_period);
    sum = sum + abs(n * 0.73f) * amp;
    norm += amp;
    scale *= 2.0f;
    oct_period *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
}

/* Penny "noise_Turbulence" base — signed fractal sum. Dampening = roughness
 * (0.5 reproduces the original). Normalized by scale-sum, tileable, RGB. */
float3 penny_blist_turb(float4 co, int octaves, float roughness, int4 period)
{
  float3 result = float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float scale_sum = 0.0f;
  float f = 1.0f;

  /* First octave: signed perlin at scale 1. scale_sum += scale BEFORE damping. */
  float3 n0 = penny_perlin_4d_tile(co, period);
  result = n0 * scale;
  scale_sum += scale;
  scale *= roughness;
  f *= 2.0f;

  /* Remaining octaves: coord doubles (freq doubling), scale damps. */
  for (int i = 1; i < octaves; i = i + 1) {
    float off = (f - 1.0f) * 1000.0f;
    float4 sample_co = float4(co.x * f + off, co.y * f + off, co.z * f + off, co.w * f + off);
    int4 oct_period = period * int(f);
    float3 n = penny_perlin_4d_tile(sample_co, oct_period);
    result = result + n * scale;
    scale *= roughness;
    scale_sum += scale;
    f *= 2.0f;
  }
  return result * (1.0f / scale_sum);
}

/* Penny Blistered Turbulence — adaptive contrast window. Self-blend for modes. */
float3 penny_blistered_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  /* temp = boxstep(-1.5,1.5, 1 - 2*boxstep(-1,1, turb(co))) — per-channel width. */
  float3 t0 = penny_blist_turb(co, octaves, roughness, period);
  float3 temp = float3(darf_boxstep(-1.5f, 1.5f, 1.0f - 2.0f * darf_boxstep(-1.0f, 1.0f, t0.x)),
                       darf_boxstep(-1.5f, 1.5f, 1.0f - 2.0f * darf_boxstep(-1.0f, 1.0f, t0.y)),
                       darf_boxstep(-1.5f, 1.5f, 1.0f - 2.0f * darf_boxstep(-1.0f, 1.0f, t0.z)));

  /* Inner sample at 2x coordinate (higher frequency). */
  float4 co2 = float4(co.x * 2.0f, co.y * 2.0f, co.z * 2.0f, co.w);
  float3 inner = penny_blist_turb(co2, octaves, roughness, period);

  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float s = darf_boxstep(-1.0f, 1.0f, inner[ch]);
    float v = darf_boxstep(0.5f - temp[ch], 0.5f + temp[ch], s);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      /* Self-blend: same chain at +0.5 phase-shifted coord. */
      float3 disp2 = penny_blist_turb(float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w),
                                      octaves,
                                      roughness,
                                      period) *
                     0.5f;
      float d2 = disp2[ch];
      float3 inner2 = penny_blist_turb(
          float4(co.x + 0.5f + d2, co.y + 0.5f + d2, co.z + 0.5f + d2, co.w),
          octaves,
          roughness,
          period);
      float v2 = darf_noxstep(-0.01f, 0.4f, abs(inner2[ch]) * 0.5f);
      v2 = v2 * v2;
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Penny box_Noise — domain distortion via axis-sliced Perlin offsets.
 * Each RGB channel computed independently. Returns [0,1]. Tileable.
 * distortion hardcoded 4.0 (matches original caller). w passes through. */
float3 penny_box_noise(float4 co, int4 period, float distortion)
{
  /* Axis-sliced offsets (original uses perlin on (x,0,z),(0,y,z),(0,0,z)).
   * penny_perlin returns signed RGB; original used [0,1] Toy form == *0.5+0.5. */
  float3 ox = penny_perlin_4d_tile(float4(co.x, 0.0f, co.z, co.w), period) * 0.5f + 0.5f;
  float3 oy = penny_perlin_4d_tile(float4(0.0f, co.y, co.z, co.w), period) * 0.5f + 0.5f;
  float3 oz = penny_perlin_4d_tile(float4(0.0f, 0.0f, co.z, co.w), period) * 0.5f + 0.5f;

  float3 result;
  for (int ch = 0; ch < 3; ch = ch + 1) {
    /* Per-channel offset vector (offset.x from ox, offset.y from oy, offset.z from oz). */
    float offx = ox[ch] * distortion;
    float offy = oy[ch] * distortion;
    /* the_Point = position + offset; final sample uses warped x,y but ORIGINAL z. */
    float px = co.x + offx;
    float py = co.y + offy;
    float3 fin = penny_perlin_4d_tile(float4(px, py, co.z, co.w), period) * 0.5f + 0.5f;
    result[ch] = fin[ch];
  }
  return result;
}

/* Penny Box noise — octave sum of box_Noise, signed /f falloff (roughness-driven),
 * boxstep to [0,1]. Self-blend for blend modes. Tileable, RGB. */
float3 penny_box_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  float3 value = float3(0.0f, 0.0f, 0.0f);
  float f = 1.0f;
  float amp = 1.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (f - 1.0f) * 1000.0f;
    float4 sample_co = float4(co.x * f + off, co.y * f + off, co.z * f + off, co.w * f + off);
    int4 oct_period = period * int(f);
    float3 b = penny_box_noise(sample_co, oct_period, 4.0f);
    /* (2*p01 - 1) * amp ; amp = roughness^i reproduces /f at roughness 0.5. */
    value = value + (2.0f * b - float3(1.0f, 1.0f, 1.0f)) * amp;
    f *= 2.0f;
    amp *= roughness;
  }

  float3 base = float3(darf_boxstep(-1.05f, 1.0f, value.x),
                       darf_boxstep(-1.05f, 1.0f, value.y),
                       darf_boxstep(-1.05f, 1.0f, value.z));
  base = float3(penny_oct_levels(base.x, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                penny_oct_levels(base.y, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                penny_oct_levels(base.z, g_lvl_lo, g_lvl_mid, g_lvl_hi));

  if (blend_mode == 0) {
    return base;
  }
  /* Self-blend: same octave sum at +0.5 shifted coord. */
  float3 value2 = float3(0.0f, 0.0f, 0.0f);
  f = 1.0f;
  amp = 1.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (f - 1.0f) * 1000.0f;
    float4 sample_co = float4(
        (co.x + 0.5f) * f + off, (co.y + 0.5f) * f + off, (co.z + 0.5f) * f + off, co.w * f + off);
    int4 oct_period = period * int(f);
    float3 b = penny_box_noise(sample_co, oct_period, 4.0f);
    value2 = value2 + (2.0f * b - float3(1.0f, 1.0f, 1.0f)) * amp;
    f *= 2.0f;
    amp *= roughness;
  }

  float3 base2 = float3(darf_boxstep(-1.05f, 1.0f, value2.x),
                        darf_boxstep(-1.05f, 1.0f, value2.y),
                        darf_boxstep(-1.05f, 1.0f, value2.z));
  base2 = float3(penny_oct_levels(base2.x, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                 penny_oct_levels(base2.y, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                 penny_oct_levels(base2.z, g_lvl_lo, g_lvl_mid, g_lvl_hi));

  return float3(
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

/* Penny Mod — snap coord to integer cell center (C-fmod truncation), sample Perlin.
 * snap(p) = trunc(p) + 0.5 + (p<0 ? -1.001 : 1.001).  Returns [0,1] RGB. */
float3 penny_mod_sample(float4 p, int4 period)
{
  /* Per-axis snap. fmod(p,1) == p - trunc(p); so p - fmod(p,1) == trunc(p). */
  float sx = trunc(p.x) + 0.5f + (p.x < 0.0f ? -1.001f : 1.001f);
  float sy = trunc(p.y) + 0.5f + (p.y < 0.0f ? -1.001f : 1.001f);
  float sz = trunc(p.z) + 0.5f + (p.z < 0.0f ? -1.001f : 1.001f);
  return penny_perlin_4d_tile(float4(sx, sy, sz, p.w), period) * 0.5f + 0.5f;
}

/* Penny Mod noise — summed snapped-Perlin octaves (blocky). Centered roughness
 * (0.5 = original, no falloff). Final: 2*sum - octaves + 0.5. Self-blend, RGB. */
float3 penny_mod_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  /* Fixed coordinate offset (original adds (1.11, 1.54, 1.82)). */
  float4 c = float4(co.x + 1.11f, co.y + 1.54f, co.z + 1.82f, co.w);
  float ramp = roughness * 2.0f; /* centered: 0.5 -> 1.0 (no falloff = original) */

  float3 value = float3(0.0f, 0.0f, 0.0f);
  float scale = 1.0f;
  float amp = 1.0f;
  float ampsum = 0.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 p = float4(c.x * scale + off, c.y * scale + off, c.z * scale + off, c.w * scale + off);
    int4 oct_period = period * int(scale);
    value = value + penny_mod_sample(p, oct_period) * amp;
    ampsum += amp;
    scale *= 2.0f;
    amp *= ramp;
  }
  /* Final remap (original): 2*sum - octaves + 0.5. */
  float3 base = float3(2.0f * (value.x / max(ampsum, 1e-6f)) - 0.5f,
                       2.0f * (value.y / max(ampsum, 1e-6f)) - 0.5f,
                       2.0f * (value.z / max(ampsum, 1e-6f)) - 0.5f);
  base = clamp(base, 0.0f, 1.0f);
  base = float3(penny_oct_levels(base.x, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                penny_oct_levels(base.y, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                penny_oct_levels(base.z, g_lvl_lo, g_lvl_mid, g_lvl_hi));
  if (blend_mode == 0) {
    return base;
  }
  /* Self-blend: same chain at +0.5 shifted coord. */
  float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
  float3 value2 = float3(0.0f, 0.0f, 0.0f);
  scale = 1.0f;
  amp = 1.0f;
  float ampsum2 = 0.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 p = float4(
        c2.x * scale + off, c2.y * scale + off, c2.z * scale + off, c2.w * scale + off);
    int4 oct_period = period * int(scale);
    value2 = value2 + penny_mod_sample(p, oct_period) * amp;
    ampsum2 += amp;
    scale *= 2.0f;
    amp *= ramp;
  }
  float3 base2 = float3(2.0f * (value2.x / max(ampsum2, 1e-6f)) - 0.5f,
                        2.0f * (value2.y / max(ampsum2, 1e-6f)) - 0.5f,
                        2.0f * (value2.z / max(ampsum2, 1e-6f)) - 0.5f);
  base2 = clamp(base2, 0.0f, 1.0f);
  base2 = float3(penny_oct_levels(base2.x, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                 penny_oct_levels(base2.y, g_lvl_lo, g_lvl_mid, g_lvl_hi),
                 penny_oct_levels(base2.z, g_lvl_lo, g_lvl_mid, g_lvl_hi));
  return float3(
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

/* Penny Nutous — domain-warped abs-turbulence, tight Noxstep window, squared.
 * Reuses penny_blist_turb (= Penny noise_Turbulence). Self-blend, RGB. */
float3 penny_nutous_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  /* displace = turb(co) * 0.5  (per-channel). */
  float3 disp = penny_blist_turb(co, octaves, roughness, period) * 0.5f;

  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float d = disp[ch];
    /* Warp all axes by the same scalar displace, sample turb again. */
    float3 inner = penny_blist_turb(
        float4(co.x + d, co.y + d, co.z + d, co.w), octaves, roughness, period);
    float v = darf_noxstep(-0.01f, 0.4f, abs(inner[ch]) * 0.5f);
    v = v * v;           /* square */
    v = pow(v, 0.5316f); /* baked midtone (== Level Mid 0.691616) */
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      /* Self-blend: same chain at +0.5 phase-shifted coord. */
      float3 disp2 = penny_blist_turb(float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w),
                                      octaves,
                                      roughness,
                                      period) *
                     0.5f;
      float d2 = disp2[ch];
      float3 inner2 = penny_blist_turb(
          float4(co.x + 0.5f + d2, co.y + 0.5f + d2, co.z + 0.5f + d2, co.w),
          octaves,
          roughness,
          period);
      float v2 = darf_noxstep(-0.01f, 0.4f, abs(inner2[ch]) * 0.5f);
      v2 = v2 * v2;
      v2 = pow(v2, 0.5316f);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Penny Cranal — x-shifted ridged biased turbulence average.
 * Inner turbulence hardcoded to 1 octave; outer detail = # of x-shifted samples. RGB. */
float3 penny_cranal_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  /* Snap the 1/3 feature scale to the period so the pattern wraps at `period`
   * exactly (co/3 alone would only repeat every 3*period). */
  int4 lat_period = int4(max(1, int(round(float(period.x) / 3.0f))),
                         max(1, int(round(float(period.y) / 3.0f))),
                         max(1, int(round(float(period.z) / 3.0f))),
                         period.w);
  co = float4(co.x * (float(lat_period.x) / float(max(period.x, 1))),
              co.y * (float(lat_period.y) / float(max(period.y, 1))),
              co.z * (float(lat_period.z) / float(max(period.z, 1))),
              co.w);

  /* Roughness drives bias, centered so 0.5 == faithful 0.80. */
  float bias = clamp(0.80f + (roughness - 0.5f) * 0.3f, 0.5f, 0.95f);

  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    /* Per-channel decorrelation offset on the coord. */
    float off = float(ch) * 1000.0f;
    float x1 = co.x + off;
    float cy = co.y + off;
    float cz = co.z + off;

    float val = 0.0f;
    /* Loop runs `octaves` times, plus one more sample after (octaves+1 total). */
    for (int i = 0; i <= octaves; i = i + 1) {
      /* Per-octave Y/Z decorrelation to break lattice grid alignment. */
      float oy = float(i) * 7.793f;
      float oz = float(i) * 11.219f;
      float3 t = penny_blist_turb(float4(x1, cy + oy, cz + oz, co.w), 1, roughness, lat_period);
      float tv = t[ch];
      float n = darf_noxstep(-0.5f, 0.5f, tv);
      float folded = abs(1.0f - 2.0f * n);
      val = val + darf_bias(bias, folded);
      x1 = x1 + 3.0f;
    }
    val = val / float(octaves + 1);

    val = darf_noxstep(0.75f, 0.96f, val); /* contrast: black point + white */
    val = (val + 1.5f) / 2.5f;             /* baked black-point lift (was user Min -1.5) */
    val = penny_oct_levels(val, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = val;
    }
    else {
      float off2 = off + 0.5f;
      float x1b = co.x + off2;
      float cyb = co.y + off2;
      float czb = co.z + off2;
      float val2 = 0.0f;
      for (int i = 0; i <= octaves; i = i + 1) {
        float oy = float(i) * 7.793f;
        float oz = float(i) * 11.219f;
        float3 t2 = penny_blist_turb(
            float4(x1b, cyb + oy, czb + oz, co.w), 1, roughness, lat_period);
        float tv2 = t2[ch];
        float n2 = darf_noxstep(-0.5f, 0.5f, tv2);
        float folded2 = abs(1.0f - 2.0f * n2);
        val2 = val2 + darf_bias(bias, folded2);
        x1b = x1b + 3.0f;
      }
      val2 = val2 / float(octaves + 1);
      val2 = darf_noxstep(0.75f, 0.96f, val2); /* contrast */
      val2 = (val2 + 1.5f) / 2.5f;             /* baked black-point lift */
      val2 = penny_oct_levels(val2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = clamp(penny_blend(blend_mode, val, val2), 0.0f, 1.0f);
    }
  }
  return result;
}

/* Luka — per-channel domain-warped abs-turbulence with adaptive window. */
/* Penny Buya — ridge-folded biased pfBm with per-octave-count output remap.
 * pfBm uses fixed H=1.0 spectral weights (2^-i). RGB. Self-blend. */
float buya_octave_remap(int oc, float x)
{
  /* Windows fit to our [0,1] pre-remap scale. Tune LO/HI per octave by eye. */
  if (oc == 1) {
    return darf_noxstep(0.55f, 0.80f, x);
  }
  else if (oc == 2) {
    return darf_noxstep(0.45f, 0.78f, x);
  }
  else if (oc == 3) {
    return darf_noxstep(0.65f, 1.00f, x);
  }
  else if (oc == 4) {
    return darf_noxstep(0.40f, 0.72f, x);
  }
  else {
    return darf_noxstep(0.38f, 0.70f, x);
  }
}

float buya_pfbm_channel(float4 co, int ch, int octaves, int4 period, float roughness)
{
  float value = 0.0f;
  float scale = 1.0f;
  float wsum = 0.0f;
  int4 per = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 c = float4(
        co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w * scale + off);
    float3 p = penny_perlin_4d_tile(c, per);
    float toy = p[ch] * 0.5f + 0.5f;         /* Improved_Toy [0,1] */
    float h = (1.0f - roughness) * 0.4f;     /* inverted; 0.5 -> H 0.2 (faithful) */
    float weight = pow(2.0f, -h * float(i)); /* live H spectral weight */
    value = value + (2.0f * darf_noxstep(-1.0f, 1.0f, toy) - 1.0f) * weight;
    wsum = wsum + weight;
    scale = scale * 2.0f;
    per = per * 2;
  }
  return value / max(wsum, 1e-6f); /* normalize so range is bounded, no clip */
}

/* Buya per-octave baked Levels (lo, 0.5, hi). Tune per octave by eye. */
float buya_octave_levels(int oc, float x)
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

float3 penny_buya_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float pf = buya_pfbm_channel(co, ch, octaves, period, roughness);
    float v = darf_noxstep(0.1f, 0.5f, darf_bias(0.25f, abs(1.0f - 2.0f * pf * 0.5f)));
    v = buya_octave_levels(octaves, v); /* baked per-octave Levels */
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 cob = float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w);
      float pf2 = buya_pfbm_channel(cob, ch, octaves, period, roughness);
      float v2 = darf_noxstep(0.1f, 0.5f, darf_bias(0.25f, abs(1.0f - 2.0f * pf2 * 0.5f)));
      v2 = buya_octave_levels(octaves, v2);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, v, v2);
    }
  }
  return result;
}

/* Penny Cell - XY quantized cell Perlin, octave-summed (Mod-style accumulate/remap,
 * simple roughness falloff, scale x2/octave, period doubling). Z continuous. RGB. */
float cell_sample(float px, float py, float pz, float w, int4 per, int ch)
{
  float cx = px;
  if (cx < 0.0f) {
    cx = cx + 100.0f;
  }
  float cy = py;
  if (cy < 0.0f) {
    cy = cy + 100.0f;
  }
  float cz = pz;
  if (cz < 0.0f) {
    cz = cz + 100.0f;
  }
  float qx = cx - mod(cx, 1.0f);
  float qy = cy - mod(cy, 1.0f);
  float qz = cz - mod(cz, 1.0f);
  float3 p = penny_perlin_4d_tile(float4(qx + 1023.761f, qy + 31.875f, qz + 57.219f, w), per);
  return p[ch] * 0.5f + 0.5f;
}

float cell_octaves(
    float bx, float by, float bz, float w, int4 period, int octaves, float roughness, int ch)
{
  float sum = 0.0f;
  float amp = 1.0f;
  float ampsum = 0.0f;
  float scale = 1.0f;
  int4 per = period;
  for (int i = 0; i < octaves; i = i + 1) {
    sum = sum + cell_sample(bx * scale, by * scale, bz * scale, w * scale, per, ch) * amp;
    ampsum = ampsum + amp;
    amp = amp * roughness;
    scale = scale * 2.0f;
    per = per * 2;
  }
  return clamp(sum / max(ampsum, 1e-6f), 0.0f, 1.0f);
}

float3 penny_cell_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float v = cell_octaves(
        co.x + dch, co.y + dch, co.z + dch, co.w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float v2 = cell_octaves(co.x + dch + 0.5f,
                              co.y + dch + 0.5f,
                              co.z + dch + 0.5f,
                              co.w,
                              period,
                              octaves,
                              roughness,
                              ch);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = clamp(penny_blend(blend_mode, v, v2), 0.0f, 1.0f);
    }
  }
  return result;
}

/* Displaced Turbulence per-octave-count noxstep window.
 * Replaces single c*oct linear window. Anchored on oct 7 (0.35, 5.25 = confirmed good);
 * seeded from sqrt(oct) law so width tracks signal spread (~sqrt) not mean (~linear).
 * Tune 1-4 by eye one at a time; 5+ should be close. */
/* Measured percentiles of raw disturb sum vs octave count (p50, p98), interpolated.
 * The distribution is non-parametric (skew collapses with octave, percentile spread
 * grows sub-sqrt even though std ~ sqrt(n)), so we table the percentiles directly. */
float2 disturb_pctl(int oc)
{
  float p50, p98;
  if (oc <= 1) {
    p50 = 0.235f;
    p98 = 0.784f;
  }
  else if (oc < 4) {
    float t = float(oc - 1) / 3.0f;
    p50 = mix(0.235f, 1.176f, t);
    p98 = mix(0.784f, 1.961f, t);
  }
  else if (oc < 7) {
    float t = float(oc - 4) / 3.0f;
    p50 = mix(1.176f, 1.882f, t);
    p98 = mix(1.961f, 2.980f, t);
  }
  else if (oc < 10) {
    float t = float(oc - 7) / 3.0f;
    p50 = mix(1.882f, 2.510f, t);
    p98 = mix(2.980f, 3.843f, t);
  }
  else if (oc < 15) {
    float t = float(oc - 10) / 5.0f;
    p50 = mix(2.510f, 3.843f, t);
    p98 = mix(3.843f, 5.098f, t);
  }
  else {
    float e = float(oc - 15);
    p50 = 3.843f + 0.267f * e;
    p98 = 5.098f + 0.251f * e;
  }
  return float2(p50, p98);
}

/* Place the window so every octave's median -> TMID and p98 -> THI output.
 * One tonal balance across all octaves. Defaults reproduce validated oct-7 [0.35, 5.25].
 * TMID = brightness (up = lighter, toward the oct-1 look); THI = highlight/contrast. */
/* Measured bottom-2% of raw sum vs octave (sum units). Anchors the black point
 * BELOW the deep displacement creases so they stay dark grey, never hard-clip to black. */
float disturb_p02(int oc)
{
  if (oc <= 1) {
    return 0.000f;
  }
  else if (oc < 4) {
    return mix(0.000f, 0.314f, float(oc - 1) / 3.0f);
  }
  else if (oc < 7) {
    return mix(0.314f, 0.784f, float(oc - 4) / 3.0f);
  }
  else if (oc < 10) {
    return mix(0.784f, 1.412f, float(oc - 7) / 3.0f);
  }
  else if (oc < 15) {
    return mix(1.412f, 2.353f, float(oc - 10) / 5.0f);
  }
  else {
    return 2.353f + 0.188f * float(oc - 15);
  }
}

float disturb_window(int oc, float sum)
{
  const float TMID = 0.313f;       /* median output = brightness */
  const float THI = 0.537f;        /* p98 output    = highlight/contrast */
  const float FLOORMARGIN = 0.35f; /* floor below bottom-2% (broad-basin clip guard) */
  const float LIFT = 0.2f;         /* shadow lift, all octaves */
  float2 pc = disturb_pctl(oc);
  float span = (pc.y - pc.x) / max(THI - TMID, 1e-4f);
  float lo = pc.x - TMID * span;
  float hi = lo + span;
  lo = min(lo, disturb_p02(oc) - FLOORMARGIN);
  float t = darf_noxstep(lo, hi, sum);
  return t + LIFT * (1.0f - t) *
                 (1.0f - t); /* lift shadows uniformly; fades to 0 by white, mids ~untouched */
}

/* Penny Displaced Turbulence - x-shifted sum of displacement-warped turbulence.
 * displace from 2-octave turb, temp from 1-octave turb. Octave-scaled noxstep output. RGB. */
float disturb_accum(
    float bx, float by, float bz, float w, int4 period, int octaves, float roughness, int ch)
{
  /* Roughness now drives fBm-style amplitude falloff across the x-shifted samples, NOT the
   * inner turbulence (which is fixed at 0.5 so the displacement/distortion stays constant).
   * Centered: roughness 0.5 -> ramp 1.0 = no falloff = original look (sum range preserved,
   * disturb_window tuning still valid). Normalized back to the unweighted magnitude so the
   * octave-tuned window sees a stable range at any roughness. */
  float ramp = roughness * 2.0f;
  float x1 = bx;
  float sum = 0.0f;
  float amp = 1.0f;
  float ampsum = 0.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    /* Per-octave Y/Z decorrelation to break lattice grid alignment. */
    float oy = float(i) * 7.793f;
    float oz = float(i) * 11.219f;
    float3 d = penny_abs_turb2(float4(x1, by + oy, bz + oz, w), 2, 0.5f, period);
    float displace = 1.0f - 2.0f * d[ch] * 0.8f;
    float3 t = penny_abs_turb2(
        float4(x1 - displace, by + displace + oy, bz + displace + oz, w), 1, 0.5f, period);
    sum = sum + t[ch] * amp;
    ampsum += amp;
    amp *= ramp;
    x1 = x1 + 5.0f;
  }
  /* Renormalize to the unweighted magnitude the window expects (sum ~ mean*octaves). */
  float norm_sum = (ampsum > 0.0f) ? (sum / ampsum) * float(octaves) : sum;
  return disturb_window(octaves, norm_sum);
}

/* Penny Hama - per-octave PRODUCT (not sum) of narrow-windowed signed turbulence.
 * Seed 2.0; each octave multiplies in noxstep(0,0.3, abs(noxstep(0,1, turb))).
 * coord shifted by i*10 on all axes per octave. Uses signed turb (penny_reg_turb2). */
/* ROUGHNESS INERT (by design): per-octave PRODUCT structure, inner turb is 1-octave -
 * no amplitude-accumulation term where falloff could apply faithfully. */
float penny_hama(float4 co, int octaves, float roughness, int4 period, int ch)
{
  /* roughness drives gate width; inverted so high roughness tightens. 0.5 -> 0.3. */
  float rough_inv = 1.0f - roughness;
  float gate_hi = clamp(0.3f + (rough_inv - 0.5f) * 0.2f, 0.08f, 0.6f);
  float the_value = 2.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float fi = float(i) * 10.0f;
    float4 c = float4(co.x + fi, co.y + fi, co.z + fi, co.w);
    float turb = penny_reg_turb2_signed(c, 1, roughness, period)[ch];
    float temp = darf_noxstep(0.0f, gate_hi, abs(darf_noxstep(0.0f, 1.0f, turb)));
    the_value = the_value * temp;
  }
  return the_value;
}

float3 penny_hama_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float4 c = float4(co.x + dch, co.y + dch, co.z + dch, co.w);
    float v = penny_hama(c, octaves, roughness, period, ch);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
      float v2 = penny_hama(c2, octaves, roughness, period, ch);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Penny Naki - abs-turb displacement warp, single Perlin sample, smoothstep-biased output.
 * d = abs_turb(co, octaves, 0.5)[ch]*0.5; warp (x+d,y-d,z+d); turb = perlin(warp)*0.5+0.5;
 * out = noxstep(0.45,1, (turb + smoothstep(0.325,0.63, d*0.25+0.5)) * 0.5). Per-channel. */
/* ROUGHNESS INERT (by design): abs_turb fixed at 0.5; wiring roughness here would only
 * scale the displacement (distortion), not octave amplitude - deliberately avoided. */
float penny_naki(float4 co, int octaves, float roughness, int4 period, int ch)
{
  /* roughness drives displacement scale, centered: 0.5 -> 0.5 (original). */
  float disp_scale = clamp(0.5f + (roughness - 0.5f) * 0.3f, 0.2f, 0.85f);
  float d = penny_abs_turb2(co, octaves, 0.5f, period)[ch] * disp_scale;
  /* Prime-ish offsets on the displaced Perlin sample break lattice alignment. */
  float4 wc = float4(co.x + d + 7.793f, co.y - d + 11.219f, co.z + d + 13.547f, co.w);
  float turb = penny_perlin_4d_tile(wc, period)[ch] * 0.5f + 0.5f;
  float bias = darf_smoothstep(0.325f, 0.63f, d * 0.25f + 0.5f);
  return darf_noxstep(0.45f, 1.0f, (turb + bias) * 0.5f);
}

float3 penny_naki_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float4 c = float4(co.x + dch, co.y + dch, co.z + dch, co.w);
    float v = penny_naki(c, octaves, roughness, period, ch);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
      float v2 = penny_naki(c2, octaves, roughness, period, ch);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Faithful Penny noise_ABS_Turbulence_Function for Poxo: coherent coord doubling
 * (coord += coord, NO decorrelation offset -> curved islands), one pre-loop sample +
 * f_Octaves loop samples, abs(snoise*0.73)*scale, normalized by scale_sum. Per-channel. */
float penny_poxo_absturb(float4 co, int octaves, float dampening, int4 period, int ch)
{
  float scale = 1.0f;
  float scale_sum = 0.0f;
  float3 s = penny_perlin_4d_tile(co, period);
  float value = abs(s[ch] * 0.73f) * scale;
  float4 c = float4(co.x + co.x, co.y + co.y, co.z + co.z, co.w);
  scale_sum += scale;
  scale *= dampening;
  for (int i = 0; i < octaves; i = i + 1) {
    float3 si = penny_perlin_4d_tile(c, period);
    value += abs(si[ch] * 0.73f) * scale;
    c = float4(c.x + c.x, c.y + c.y, c.z + c.z, c.w);
    scale_sum += scale;
    scale *= dampening;
  }
  return (scale_sum != 0.0f) ? (value / scale_sum) : 0.0f;
}

/* Penny Poxo - recursive abs-turb running-blend with fractional octave support.
 * Inner abs-turb fixed at 3 octaves, roughness -0.5 (l_Scalar = 1/(-1*2)). Per octave:
 * value += abs(turb - value) * 0.5, frequency doubling. Fractional tail + pow/noxstep output.
 * detail kept as float (honors fractional octaves). Verbatim; dead l_Scale_Sum dropped. */
float penny_poxo(float4 co, float detail, float roughness, int4 period, int ch)
{
  float scalar = -roughness; /* rough 0.5 -> -0.5 = Penny default; slider now live */
  float size = 1.0f;
  float value = 0.0f;
  int ioct = int(detail);
  for (int i = 0; i < ioct; i = i + 1) {
    float4 sc = float4(co.x * size, co.y * size, co.z * size, co.w);
    float temp = penny_poxo_absturb(sc, 4, scalar, period, ch);
    value += abs(temp - value) * 0.5f;
    size *= 2.0f;
  }
  float4 sc = float4(co.x * size, co.y * size, co.z * size, co.w);
  float temp = penny_poxo_absturb(sc, 4, scalar, period, ch);
  value += abs(temp - value) * 0.5f * (detail - floor(detail));
  if (detail != 0.0f) {
    value /= detail;
  }
  value = clamp(pow(value, 0.7369656f), 0.0f, 1.0f);
  value = value * value * 2.0f;
  value = darf_noxstep(0.0f, 0.5f, value);
  value = darf_noxstep(0.01f, 0.9f, (detail != 0.0f) ? (value / detail) : value);
  return value;
}

float3 penny_poxo_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  float d = max(detail, 1.0f); /* keep fractional; floor of 1 like other tiles' min */
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float4 c = float4(co.x + dch, co.y + dch, co.z + dch, co.w);
    float v = penny_poxo(c, d, roughness, period, ch);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
      float v2 = penny_poxo(c2, d, roughness, period, ch);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Penny Pezo abs-turb variant (noise_ABS_Turbulence_Function_2): abs(2*p01-1)/scale summed,
 * decorrelation offset KEPT, scale doubling, no final normalization. Per-channel. */
float penny_pezo_absturb2(float4 co, int octaves, int4 period, int ch)
{
  float value = 0.0f;
  float scale = 1.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 sc = float4(
        co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w * scale + off);
    float p01 = penny_perlin_4d_tile(sc, op)[ch] * 0.5f + 0.5f;
    value += abs(2.0f * p01 - 1.0f) / scale;
    scale *= 2.0f;
    op *= 2;
  }
  return value;
}

/* Penny Pezo signed turbulence variant (noise_Turbulence_Function_3): snoise/f summed
 * (offset on x,y only - z unshifted), then noxstep(-1,1). Per-channel. */
float penny_pezo_turb3(float4 co, int octaves, int4 period, int ch)
{
  float value = 0.0f;
  float f = 1.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (f - 1.0f) * 1000.0f;
    float4 sc = float4(co.x * f + off, co.y * f + off, co.z * f, co.w * f + off);
    float s = penny_perlin_4d_tile(sc, op)[ch]; /* 2*(s*0.5+0.5)-1 == s */
    value += s / f;
    f *= 2.0f;
    op *= 2;
  }
  return darf_noxstep(-1.0f, 1.0f, value); /* Penny original range */
}

/* Penny Pezo - per-octave smoothstep-gated turbulence product, masked by abs-turb. Per-channel. */
float penny_pezo(float4 co, int octaves, float roughness, int4 period, int ch)
{
  /* roughness drives gate spread, centered: 0.5 -> 0.45. */
  float gate_lo = clamp(0.45f + (roughness - 0.5f) * 0.3f, 0.2f, 0.7f);
  float value = 1.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float fi = float(i);
    float4 c = float4(
        co.x * 0.5f + fi * 19.55f, co.y * 0.5f + fi * 20.612f, co.z * 0.5f + fi * 21.175f, co.w);
    float t = penny_pezo_turb3(c, 1, period, ch);
    float temp = 1.0f - abs(1.0f - 2.0f * t);
    value *= darf_smoothstep(gate_lo, 1.0f, temp); /* gate centered on 0.4 via roughness */
  }
  float4 c10 = float4(co.x * 15.0f, co.y * 15.0f, co.z * 15.0f, co.w);
  float temp = penny_pezo_absturb2(c10, octaves, period, ch);
  temp = darf_noxstep(0.32f, 1.0f, darf_noxstep(-1.25f, 1.25f, temp));
  value = mix(0.25f, 1.0f, value) * darf_noxstep(-0.2f, 1.2f, temp);
  /* Pezo output tone, distribution-fit to Penny. LOCKED but REVISIT: blacks (12.1% vs
   * Penny 11.4%) and sparkles (2.7% vs 2.4%) match; midtone fizz runs bright (p50 0.204
   * vs Penny 0.165) - residual of tileable-perlin vs snoise amplitude, lives in internal
   * windows not this curve. Compensations elsewhere: turb3 noxstep(-0.62,0.62), gate
   * smoothstep(0.4,1.0) (opened from 0.6 - the key range fix). */
  value = darf_smoothstep(0.0f, 0.40f, value);
  return value;
}

float3 penny_pezo_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float4 c = float4(co.x + dch, co.y + dch, co.z + dch, co.w);
    float v = penny_pezo(c, octaves, roughness, period, ch);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
      float v2 = penny_pezo(c2, octaves, roughness, period, ch);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Penny Dents - MAX across octaves of folded 1-octave turbulence. Per octave:
 * boxstep(0,0.65, abs(1 - 2*noxstep(-1,1, turb))), keep running max. Fractional tail.
 * Uses signed pre-noxstep turb so Dents' own noxstep(-1,1) applies once. Per-channel. */
float penny_dents(float4 co,
                  float detail,
                  float roughness,
                  int4 period,
                  int ch,
                  float lvl_lo,
                  float lvl_mid,
                  float lvl_hi)
{
  int ioct = int(detail);
  /* roughness drives fold sharpness, centered: 0.5 -> 0.56 (original). */
  float fold_hi = clamp(0.56f + (roughness - 0.5f) * 0.5f, 0.12f, 0.95f);
  float the_value = 0.0f;
  int i;
  for (i = 0; i < ioct; i = i + 1) {
    float o = float(i + 1);
    /* non-integer per-axis offset: avoids tile-period alignment that caused a grid
     * (Penny's +(i+1) is just octave decorrelation; integer steps hit our tileable seams). */
    float4 c = float4(co.x + o * 12.793f, co.y + o * 13.547f, co.z + o * 14.219f, co.w);
    float turb = penny_reg_turb2_signed(c, 1, 0.5f, period)[ch];
    float folded = abs(1.0f - 2.0f * darf_noxstep(-1.0f, 1.0f, turb));
    float temp = darf_boxstep(0.0f, fold_hi, folded);
    the_value = max(the_value, temp);
  }
  float o = float(i + 1);
  float4 c = float4(co.x + o * 12.793f, co.y + o * 13.547f, co.z + o * 14.219f, co.w);
  float turb = penny_reg_turb2_signed(c, 1, 0.5f, period)[ch];
  float folded = abs(1.0f - 2.0f * darf_noxstep(-1.0f, 1.0f, turb));
  float temp = (detail - floor(detail)) * darf_boxstep(0.0f, fold_hi, folded);
  the_value = max(the_value, temp);

  /* Dents output, distribution-fit to Penny. LOCKED but REVISIT: blacks (27.0% vs Penny
   * 27.6%), mids (>=0.5 28.6% vs 27.7%), mean (0.336 vs 0.327) all near-exact; highlights
   * run hot (>=0.8 11.4% vs Penny 6.0%) - residual perlin-vs-snoise amplitude. Compensations:
   * non-integer per-axis octave offsets (grid fix), fold boxstep(0,0.56) from Penny's 0.65. */
  the_value = darf_boxstep(0.25f, 1.0f, the_value) * the_value;
  the_value = pow(the_value, 0.8f);                 /* lift mids */
  the_value = darf_noxstep(0.0f, 1.15f, the_value); /* highlight rolloff */
  return the_value;
}

float3 penny_dents_tile(float4 co,
                        float detail,
                        float roughness,
                        int4 period,
                        int blend_mode,
                        float lvl_lo,
                        float lvl_mid,
                        float lvl_hi)
{
  float d = max(detail, 1.0f);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float4 c = float4(co.x + dch, co.y + dch, co.z + dch, co.w);
    float v = penny_dents(c, d, roughness, period, ch, lvl_lo, lvl_mid, lvl_hi);
    v = penny_oct_levels(v, lvl_lo, lvl_mid, lvl_hi); /* output Levels (match kernel/OSL) */

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
      float v2 = penny_dents(c2, d, roughness, period, ch, lvl_lo, lvl_mid, lvl_hi);
      v2 = penny_oct_levels(v2, lvl_lo, lvl_mid, lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Penny Sema - tightly-gated MAX of folded turbulence + fine absturb2 dither. Reuses Pezo
 * helpers. Per octave: smoothstep(0.8,0.999, 1-abs(1-2*turb3)) kept as running max; output
 * boxstep(0.95,1) + faint (1-2*absturb2(*75))*0.25+0.5)*0.05 grain. Non-integer per-axis
 * offsets (grid fix). Noise_Scale dropped (node scales via input coords). Per-channel. */
float penny_sema(float4 co, int octaves, float roughness, int4 period, int ch)
{
  /* roughness drives gate tightness, centered: 0.5 -> 0.8 (original). */
  float gate_lo = clamp(0.8f + (roughness - 0.5f) * 0.3f, 0.5f, 0.95f);
  float the_value = 0.0f;
  for (int i = 0; i < octaves; i = i + 1) {
    float fi = float(i);
    float4 c = float4(co.x + fi * 21.7f, co.y + fi * 11.3f, co.z + fi * 13.9f, co.w);
    float t = penny_pezo_turb3(c, 1, period, ch);
    float temp = 1.0f - abs(1.0f - 2.0f * t);
    float displace = darf_smoothstep(gate_lo, 0.999f, temp);
    the_value = max(the_value, displace);
  }
  float4 c75 = float4(co.x * 75.0f, co.y * 75.0f, co.z * 75.0f, co.w);
  float dither = ((1.0f - 2.0f * penny_pezo_absturb2(c75, 1, period, ch)) * 0.25f + 0.5f) * 0.05f;
  the_value = darf_boxstep(0.95f, 1.0f, the_value) + dither;
  return the_value;
}

float3 penny_sema_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float4 c = float4(co.x + dch, co.y + dch, co.z + dch, co.w);
    float v = penny_sema(c, octaves, roughness, period, ch);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
      float v2 = penny_sema(c2, octaves, roughness, period, ch);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* Penny vl_Noise - Perlin domain-warped by Perlin (variable lacunarity). Build a signed
 * offset vector from three Perlin samples at fixed offsets, scale by distortion, warp
 * the point, sample Perlin there. Returns p01 ([0,1]) so caller's 2*v-1 maps to signed.
 * Per-channel. (Dead time/period args from Penny dropped; only p + distortion used.) */
float penny_vl_noise(float4 p, float distortion, int4 period, int ch)
{
  float ox = 1.0f - 2.0f * (penny_perlin_4d_tile(float4(p.x - 2.0f, p.y + 7.0f, p.z + 8.0f, p.w),
                                                 period)[ch] *
                                0.5f +
                            0.5f);
  float oy = 1.0f - 2.0f * (penny_perlin_4d_tile(float4(p.x - 13.0f, p.y - 1.0f, p.z + 4.0f, p.w),
                                                 period)[ch] *
                                0.5f +
                            0.5f);
  float oz = 1.0f - 2.0f * (penny_perlin_4d_tile(float4(p.x + 17.0f, p.y + 8.0f, p.z - 4.0f, p.w),
                                                 period)[ch] *
                                0.5f +
                            0.5f);
  float4 the_point = float4(
      p.x + ox * distortion, p.y + oy * distortion, p.z + oz * distortion, p.w);
  return penny_perlin_4d_tile(the_point, period)[ch] * 0.5f + 0.5f;
}

/* Penny VL Noise - fBm sum of vl_Noise octaves (un-normalized /scale), noxstep(-0.65,0.65).
 * Noise_Scale dropped (node scales via input coords). distortion=1.0 (full warp). Per-channel. */
float penny_vl(float4 co, int octaves, int4 period, int ch, float roughness)
{
  float the_value = 0.0f;
  float scale = 1.0f;
  float amp = 1.0f; /* roughness drives octave falloff; 0.5 == original /scale (2^-i) */
  for (int i = 0; i < octaves; i = i + 1) {
    float off = (scale - 1.0f) * 1000.0f;
    float4 sc = float4(co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w);
    float v = penny_vl_noise(sc, 1.0f, period, ch);
    the_value += (2.0f * v - 1.0f) * amp;
    scale *= 2.0f;
    amp *= roughness;
  }
  return darf_noxstep(
      -1.0f, 1.0f, the_value); /* widened to +-1.0: remove clipping, lower contrast */
}

float3 penny_vl_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float4 c = float4(co.x + dch, co.y + dch, co.z + dch, co.w);
    float v = penny_vl(c, octaves, period, ch, roughness);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float4 c2 = float4(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f, c.w);
      float v2 = penny_vl(c2, octaves, period, ch, roughness);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

float3 penny_disturb_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float dch = float(ch) * 1000.0f;
    float v = disturb_accum(
        co.x + dch, co.y + dch, co.z + dch, co.w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      float v2 = disturb_accum(co.x + dch + 0.5f,
                               co.y + dch + 0.5f,
                               co.z + dch + 0.5f,
                               co.w,
                               period,
                               octaves,
                               roughness,
                               ch);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

float3 penny_luka_tile(float4 co, float detail, float roughness, int4 period, int blend_mode)
{
  int octaves = max(int(detail), 1);

  /* Faithful pre-transform: scale 0.8 + decorrelation offset (xyz; w passes through). */
  /* Snap the 0.8 feature scale to the period so the pattern wraps at `period`
   * exactly (a non-integer 0.8 prescale would not tile). */
  int4 lat_period = int4(max(1, int(round(0.8f * float(period.x)))),
                         max(1, int(round(0.8f * float(period.y)))),
                         max(1, int(round(0.8f * float(period.z)))),
                         period.w);
  float kx = float(lat_period.x) / float(max(period.x, 1));
  float ky = float(lat_period.y) / float(max(period.y, 1));
  float kz = float(lat_period.z) / float(max(period.z, 1));
  float4 c = float4(co.x * kx + 127.1f, co.y * ky + 64.8f, co.z * kz + 26.81f, co.w);

  float3 temp = penny_luka_turb(c, octaves, roughness, lat_period);
  /* Displacement: single raw Perlin * 0.275 (== 0.55 * 0.5). */
  float3 disp3 = penny_perlin_4d_tile(c, lat_period) * 0.275f;

  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch = ch + 1) {
    float d = disp3[ch];
    /* Warp #1: (x+d, y-d, z+d). */
    float4 w1 = float4(c.x + d, c.y - d, c.z + d, c.w);
    /* Inner sample warps again by +d on all axes. */
    float4 w2 = float4(w1.x + d, w1.y + d, w1.z + d, c.w);
    float3 inner3 = penny_luka_turb(w2, octaves, roughness, lat_period);
    float inner = inner3[ch];
    float v = darf_noxstep(-2.0f, 2.0f, inner);
    v = darf_noxstep(0.47f - temp[ch], 0.525f + temp[ch], v);
    v = darf_noxstep(0.5f, 0.78f, v);
    v = penny_oct_levels(v, g_lvl_lo, g_lvl_mid, g_lvl_hi);

    if (blend_mode == 0) {
      result[ch] = v;
    }
    else {
      /* Self-blend: same Luka chain at a phase-shifted coord for classic modes. */
      float4 w2b = float4(w1.x + d + 0.5f, w1.y + d + 0.5f, w1.z + d + 0.5f, c.w);
      float3 inner3b = penny_luka_turb(w2b, octaves, roughness, lat_period);
      float v2 = darf_noxstep(-2.0f, 2.0f, inner3b[ch]);
      v2 = darf_noxstep(0.47f - temp[ch], 0.525f + temp[ch], v2);
      v2 = darf_noxstep(0.5f, 0.78f, v2);
      v2 = penny_oct_levels(v2, g_lvl_lo, g_lvl_mid, g_lvl_hi);
      result[ch] = penny_blend(blend_mode, clamp(v, 0.0f, 1.0f), clamp(v2, 0.0f, 1.0f));
    }
  }
  return result;
}

/* ---- EEVEE node entry point ----
 * node_tex_penny: main entry point — unpacks parameters, applies Scale
 *   conversion (percentage → factor), assembles coordinate per dimensionality,
 *   dispatches to the appropriate penny_*_tile(), writes Factor and Color. */

void penny_field(float3 co,
                 float w,
                 float detail,
                 float scale_pct,
                 float scale_x_pct,
                 float scale_y_pct,
                 float scale_z_pct,
                 float scale_w_pct,
                 float roughness,
                 float level_min,
                 float level_mid,
                 float level_max,
                 float period_x,
                 float period_y,
                 float period_z,
                 float period_w,
                 float blend_mode,
                 float noise_type,
                 float dimensions,
                 float tileable,
                 out float3 out_rgb)
{
  g_lvl_lo = level_min;
  g_lvl_mid = level_mid;
  g_lvl_hi = level_max;

  float3 vec = co;

  detail = clamp(detail, 0.0f, 15.0f);
  roughness = max(roughness, 0.0f);

  float global_factor = scale_pct * 0.01f;
  float fx_scale = max(global_factor * scale_x_pct * 0.01f, 1e-6f) * 0.25f;
  float fy_scale = max(global_factor * scale_y_pct * 0.01f, 1e-6f) * 0.25f;
  float fz_scale = max(global_factor * scale_z_pct * 0.01f, 1e-6f) * 0.25f;
  float fw_scale = max(global_factor * scale_w_pct * 0.01f, 1e-6f) * 0.25f;

  /* Assemble the sampled coordinate per dimension (matches Blender Noise + GN):
   * 1D -> (0,0,0,W); 2D -> (X,Y,0,0); 3D -> (X,Y,Z,0); 4D -> (X,Y,Z,W). */
  /* Period is an object-space tile size, independent of Scale. Snap the integer
   * lattice period to round(Period / scale), then sample at vec * (lattice /
   * Period): the pattern repeats exactly every Period units (exact regardless
   * of the rounding) while Scale still controls feature size. */
  float qx, qy, qz, qw;
  int4 period;
  if (tileable > 0.5f) {
    /* Tileable: snap the lattice period to Period/scale and sample at
     * vec * (lattice / Period) so the pattern repeats exactly every Period. */
    int Px = clamp(int(period_x), 1, 256);
    int Py = clamp(int(period_y), 1, 256);
    int Pz = clamp(int(period_z), 1, 256);
    int Pw = clamp(int(period_w), 1, 256);
    int Lx = clamp(int(round(float(Px) / fx_scale)), 1, 4096);
    int Ly = clamp(int(round(float(Py) / fy_scale)), 1, 4096);
    int Lz = clamp(int(round(float(Pz) / fz_scale)), 1, 4096);
    int Lw = clamp(int(round(float(Pw) / fw_scale)), 1, 4096);
    qx = vec.x * (float(Lx) / float(Px));
    qy = vec.y * (float(Ly) / float(Py));
    qz = vec.z * (float(Lz) / float(Pz));
    qw = w * (float(Lw) / float(Pw));
    period = int4(Lx, Ly, Lz, Lw);
  }
  else {
    /* Non-tiling: continuous Scale; large fixed lattice so it effectively never
     * repeats in normal use. */
    qx = vec.x / fx_scale;
    qy = vec.y / fy_scale;
    qz = vec.z / fz_scale;
    qw = w / fw_scale;
    period = int4(256, 256, 256, 256);
  }

  int dims = int(dimensions);
  float4 p;
  if (dims == 1) {
    p = float4(0.0f, 0.0f, 0.0f, qw);
  }
  else if (dims == 2) {
    p = float4(qx, qy, 0.0f, 0.0f);
  }
  else if (dims == 3) {
    p = float4(qx, qy, qz, 0.0f);
  }
  else {
    p = float4(qx, qy, qz, qw);
  }

  int nt = int(noise_type);
  int bm = int(blend_mode);
  float3 rgb;
  if (nt == 1) {
    rgb = penny_turbulence_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 2) {
    rgb = penny_stupl_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 3) {
    rgb = penny_luka_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 4) {
    rgb = penny_blistered_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 5) {
    rgb = penny_box_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 6) {
    rgb = penny_mod_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 7) {
    rgb = penny_nutous_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 8) {
    rgb = penny_cranal_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 9) {
    rgb = penny_buya_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 10) {
    rgb = penny_cell_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 11) {
    rgb = penny_disturb_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 12) {
    rgb = penny_hama_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 13) {
    rgb = penny_naki_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 14) {
    rgb = penny_poxo_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 15) {
    rgb = penny_pezo_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 16) {
    rgb = penny_dents_tile(p, detail, roughness, period, bm, level_min, level_mid, level_max);
  }
  else if (nt == 17) {
    rgb = penny_sema_tile(p, detail, roughness, period, bm);
  }
  else if (nt == 18) {
    rgb = penny_vl_tile(p, detail, roughness, period, bm);
  }
  else {
    rgb = penny_fbm_tile(p, detail, roughness, period, bm, level_min, level_mid, level_max);
  }

  /* Photoshop-style Levels on final output (identity at 0/0.5/1). */
  /* final Levels removed - level_min/mid/max now drive per-octave Levels in tiles. */
  out_rgb = rgb;
}

[[node]]
void node_tex_penny(float3 co,
                    float w,
                    float detail,
                    float scale_pct,
                    float scale_x_pct,
                    float scale_y_pct,
                    float scale_z_pct,
                    float scale_w_pct,
                    float roughness,
                    float level_min,
                    float level_mid,
                    float level_max,
                    float period_x,
                    float period_y,
                    float period_z,
                    float period_w,
                    float blend_mode,
                    float noise_type,
                    float dimensions,
                    float tileable,
                    float compute_gradient,
                    out float fac,
                    out float4 color,
                    out float3 gradient)
{
  /* Recenter the default generated coordinate so Scale pivots at the box center
   * (= object origin for a centered mesh) instead of the corner. */
  co -= float3(0.5f, 0.5f, 0.5f);

  /* Fac/Color use the user Levels. */
  float3 rgb;
  penny_field(co,
              w,
              detail,
              scale_pct,
              scale_x_pct,
              scale_y_pct,
              scale_z_pct,
              scale_w_pct,
              roughness,
              level_min,
              level_mid,
              level_max,
              period_x,
              period_y,
              period_z,
              period_w,
              blend_mode,
              noise_type,
              dimensions,
              tileable,
              rgb);
  fac = rgb.x;
  color = float4(rgb.x, rgb.y, rgb.z, 1.0f);

  /* Gradient: raw pre-Levels field, differentiated numerically.
   * Identity Levels (0,0.5,1) hit the penny_oct_levels passthrough fast-path, so
   * every tile collapses to its underlying continuous field - noise-type
   * agnostic, no analytic derivatives. Central differences on xyz (w held
   * fixed); per-axis step is a fixed fraction of a cell so finite-difference
   * accuracy is Scale-independent. Raw and unnormalized: magnitude carries slope. */
  float gf = scale_pct * 0.01f;
  float3 eps3 = float3(max(gf * scale_x_pct * 0.01f, 1e-6f),
                       max(gf * scale_y_pct * 0.01f, 1e-6f),
                       max(gf * scale_z_pct * 0.01f, 1e-6f)) *
                0.25f * 0.01f;
  float3 grad = float3(0.0f);
  if (compute_gradient > 0.5f) {
    for (int a = 0; a < 3; a++) {
      float3 dp = float3(0.0f);
      dp[a] = eps3[a];
      float3 fp, fm;
      penny_field(co + dp,
                  w,
                  detail,
                  scale_pct,
                  scale_x_pct,
                  scale_y_pct,
                  scale_z_pct,
                  scale_w_pct,
                  roughness,
                  0.0f,
                  0.5f,
                  1.0f,
                  period_x,
                  period_y,
                  period_z,
                  period_w,
                  blend_mode,
                  noise_type,
                  dimensions,
                  tileable,
                  fp);
      penny_field(co - dp,
                  w,
                  detail,
                  scale_pct,
                  scale_x_pct,
                  scale_y_pct,
                  scale_z_pct,
                  scale_w_pct,
                  roughness,
                  0.0f,
                  0.5f,
                  1.0f,
                  period_x,
                  period_y,
                  period_z,
                  period_w,
                  blend_mode,
                  noise_type,
                  dimensions,
                  tileable,
                  fm);
      grad[a] = (fp.x - fm.x) / (2.0f * eps3[a]);
    }
  }
  gradient = grad;
}
