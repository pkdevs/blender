/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Penny Noise CPU implementation, used by the Geometry Nodes multi-function and
 * the Penny F-Curve modifier. Mirrors the Cycles SVM kernel
 * (intern/cycles/kernel/svm/penny.h); the hash is bit-identical to Cycles'
 * hash_uint4 so all backends produce identical results. */

#include "BLI_noise_penny.hh"

#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"

namespace blender::noise::penny {

/* -------------------------------------------------------------------- */
/** \name Foundation: fade, gradient, tileable 4D Perlin
 *
 * These functions implement a standard improved Perlin noise in 4D with
 * tileable lattice coordinates. The tiling is achieved by wrapping the
 * integer lattice indices modulo the period, so the pattern repeats every
 * `period` units along each axis.
 * \{ */

/**
 * Quintic fade curve (Perlin's improved interpolant).
 * Maps t ∈ [0,1] → [0,1] with zero first and second derivatives at endpoints,
 * eliminating visible lattice artifacts in the interpolation.
 *   f(t) = 6t^5 - 15t^4 + 10t^3
 */
static float penny_fade(float t)
{
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/**
 * 4D gradient function for Perlin noise.
 *
 * Selects 3 of the 4 input coordinates based on the hash, then applies sign
 * flips from the low bits. This gives 32 distinct gradient directions in 4D
 * (chosen from projections of (±x, ±y, ±z, ±w) onto 3-axis subspaces).
 *
 * \param hash_val  Hash of the lattice point (from `blender::noise::hash`).
 * \param x,y,z,w   Fractional offset from the lattice point to the sample point.
 * \return Dot product of the pseudo-random gradient with the offset vector.
 */
static float penny_grad(uint32_t hash_val, float x, float y, float z, float w)
{
  const uint32_t h = hash_val & 31u;
  /* Bits 3-4 select which 3 of 4 axes to use. */
  const uint32_t sel = h >> 3;
  float a, b, c;
  if (sel == 0u) {
    a = y;
    b = z;
    c = w; /* project onto YZW */
  }
  else if (sel == 1u) {
    a = x;
    b = z;
    c = w; /* project onto XZW */
  }
  else if (sel == 2u) {
    a = x;
    b = y;
    c = w; /* project onto XYW */
  }
  else {
    a = x;
    b = y;
    c = z; /* project onto XYZ */
  }
  /* Bits 0-2 control sign of each selected axis. */
  const float sa = (h & 4u) ? a : -a;
  const float sb = (h & 2u) ? b : -b;
  const float sc = (h & 1u) ? c : -c;
  return sa + sb + sc;
}

/**
 * Tileable 4D Perlin noise with 3-channel output.
 *
 * Evaluates improved Perlin noise at (x, y, z, w) with periodic boundary
 * conditions defined by \a period. The lattice coordinates wrap modulo the
 * period along each axis, making the output seamlessly tileable.
 *
 * Three decorrelated channels are produced from a single set of 16 lattice
 * hashes by right-shifting each hash by `cc * 5` bits before passing to
 * `penny_grad`. This avoids 3× the hashing cost while producing visually
 * independent R, G, B channels.
 *
 * \param x,y,z,w   Sample coordinate in noise space.
 * \param period     Tiling period per axis (minimum 1).
 * \return float3 with independently-seeded noise channels in approximately [-1, 1].
 */
static float3 penny_perlin_4d_tile(float x, float y, float z, float w, const int4 &period)
{
  /* Integer lattice coordinates. */
  const int X_floor = int(std::floor(x));
  const int Y_floor = int(std::floor(y));
  const int Z_floor = int(std::floor(z));
  const int W_floor = int(std::floor(w));
  /* Fractional offsets within the unit hypercube [0, 1)^4. */
  const float fx = x - float(X_floor);
  const float fy = y - float(Y_floor);
  const float fz = z - float(Z_floor);
  const float fw = w - float(W_floor);

  /* Clamp periods to >= 1 and wrap lattice indices for tiling.
   * The `(X >> 31) & px` trick adds `px` when X is negative (arithmetic
   * right-shift fills with sign bit), converting C's truncation-toward-zero
   * modulo into a true floor-modulo without branching. */
  const int px = math::max(period.x, 1);
  const int py = math::max(period.y, 1);
  const int pz = math::max(period.z, 1);
  const int pw = math::max(period.w, 1);
  int X = X_floor % px;
  X += (X >> 31) & px;
  int Y = Y_floor % py;
  Y += (Y >> 31) & py;
  int Z = Z_floor % pz;
  Z += (Z >> 31) & pz;
  int W = W_floor % pw;
  W += (W >> 31) & pw;
  /* Neighbor lattice indices, wrapping at the period boundary. */
  const int X1 = (X + 1 == px) ? 0 : X + 1;
  const int Y1 = (Y + 1 == py) ? 0 : Y + 1;
  const int Z1 = (Z + 1 == pz) ? 0 : Z + 1;
  const int W1 = (W + 1 == pw) ? 0 : W + 1;

  /* Quintic interpolation weights. */
  const float u = penny_fade(fx);
  const float v = penny_fade(fy);
  const float t = penny_fade(fz);
  const float s = penny_fade(fw);

  /* Offsets from the far corner (used for gradients at +1 lattice points). */
  const float fxm = fx - 1.0f, fym = fy - 1.0f, fzm = fz - 1.0f, fwm = fw - 1.0f;

  /* Hash all 16 corners of the 4D unit hypercube. Each hash is a uint32_t
   * derived from `blender::noise::hash(X, Y, Z, W)`, which is bit-identical
   * to Cycles' `hash_uint4`. */
  const uint32_t h0 = hash(uint32_t(X), uint32_t(Y), uint32_t(Z), uint32_t(W));
  const uint32_t h1 = hash(uint32_t(X1), uint32_t(Y), uint32_t(Z), uint32_t(W));
  const uint32_t h2 = hash(uint32_t(X), uint32_t(Y1), uint32_t(Z), uint32_t(W));
  const uint32_t h3 = hash(uint32_t(X1), uint32_t(Y1), uint32_t(Z), uint32_t(W));
  const uint32_t h4 = hash(uint32_t(X), uint32_t(Y), uint32_t(Z1), uint32_t(W));
  const uint32_t h5 = hash(uint32_t(X1), uint32_t(Y), uint32_t(Z1), uint32_t(W));
  const uint32_t h6 = hash(uint32_t(X), uint32_t(Y1), uint32_t(Z1), uint32_t(W));
  const uint32_t h7 = hash(uint32_t(X1), uint32_t(Y1), uint32_t(Z1), uint32_t(W));
  const uint32_t h8 = hash(uint32_t(X), uint32_t(Y), uint32_t(Z), uint32_t(W1));
  const uint32_t h9 = hash(uint32_t(X1), uint32_t(Y), uint32_t(Z), uint32_t(W1));
  const uint32_t h10 = hash(uint32_t(X), uint32_t(Y1), uint32_t(Z), uint32_t(W1));
  const uint32_t h11 = hash(uint32_t(X1), uint32_t(Y1), uint32_t(Z), uint32_t(W1));
  const uint32_t h12 = hash(uint32_t(X), uint32_t(Y), uint32_t(Z1), uint32_t(W1));
  const uint32_t h13 = hash(uint32_t(X1), uint32_t(Y), uint32_t(Z1), uint32_t(W1));
  const uint32_t h14 = hash(uint32_t(X), uint32_t(Y1), uint32_t(Z1), uint32_t(W1));
  const uint32_t h15 = hash(uint32_t(X1), uint32_t(Y1), uint32_t(Z1), uint32_t(W1));

  /* Evaluate 3 independent channels by shifting the hash bits. Shifting by
   * `cc * 5` gives each channel a different 5-bit slice of the 32-bit hash,
   * producing independent gradient selections without additional hash calls. */
  float channels[3];
  for (int cc = 0; cc < 3; cc++) {
    const uint32_t sh = uint32_t(cc * 5);

    /* Gradient dot-products at all 16 hypercube corners. */
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

    /* 4D trilinear-style interpolation (nested lerps along X, Y, Z, W). */
    const float x00 = math::interpolate(n0, n1, u);
    const float x01 = math::interpolate(n2, n3, u);
    const float x10 = math::interpolate(n4, n5, u);
    const float x11 = math::interpolate(n6, n7, u);
    const float x20 = math::interpolate(n8, n9, u);
    const float x21 = math::interpolate(n10, n11, u);
    const float x30 = math::interpolate(n12, n13, u);
    const float x31 = math::interpolate(n14, n15, u);
    const float y0 = math::interpolate(x00, x01, v);
    const float y1 = math::interpolate(x10, x11, v);
    const float y2 = math::interpolate(x20, x21, v);
    const float y3 = math::interpolate(x30, x31, v);
    const float z0 = math::interpolate(y0, y1, t);
    const float z1 = math::interpolate(y2, y3, t);
    channels[cc] = math::interpolate(z0, z1, s);
  }
  return float3(channels[0], channels[1], channels[2]);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name darf_* helpers — shaping primitives
 *
 * Named after the DARF Noise Displace addon these originated from. These are
 * the basic tonal shaping tools used throughout the noise types: unclamped
 * linear remap (noxstep), clamped remap (boxstep), Hermite smoothstep, and
 * power-curve bias.
 * \{ */

/**
 * Unclamped linear remap (noxstep).
 * Maps x from [a, b] → [0, 1] but does NOT clamp the result, allowing
 * extrapolation beyond [0, 1]. This is intentional: Buya feeds values > 1.0
 * into `buya_octave_levels` for HDR detail recovery, and other noise types
 * rely on the extrapolation for their full dynamic range.
 *
 * \param a  Low end of input range.
 * \param b  High end of input range (clamped to avoid division by zero).
 * \param x  Input value to remap.
 * \return (x - a) / (b - a), unclamped.
 */
static float darf_noxstep(float a, float b, float x)
{
  return (x - a) / math::max(b - a, 1e-6f);
}

/**
 * Clamped linear remap (boxstep).
 * Same as `darf_noxstep` but clamps the output to [0, 1].
 */
static float darf_boxstep(float a, float b, float x)
{
  return math::clamp((x - a) / math::max(b - a, 1e-6f), 0.0f, 1.0f);
}

/**
 * Hermite smoothstep with clamped input.
 * Maps x from [a, b] → [0, 1] with smooth (C1-continuous) transitions at
 * the boundaries. Used for soft gating in noise types like Stupl and Naki.
 *   f(t) = 3t^2 - 2t^3
 */
static float darf_smoothstep(float a, float b, float x)
{
  float t = math::clamp((x - a) / math::max(b - a, 1e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/**
 * Power-curve bias function.
 * Remaps x ∈ [0, 1] using a gamma derived from \a bval:
 *   gamma = log(bval) / log(0.5)
 *   result = x^gamma
 *
 * When bval = 0.5, gamma = 1 (identity). Values < 0.5 brighten; > 0.5 darken.
 * This is the classic Schlick bias used in procedural shading.
 */
static float darf_bias(float bval, float x)
{
  return std::pow(x, std::log(bval) / std::log(0.5f));
}

/**
 * Three-point output level remap (Levels).
 *
 * Remaps the noise value \a x using the user's Level Min / Mid / Max controls:
 *   1. Linear remap from [low, high] → [0, 1] via `darf_noxstep` (unclamped,
 *      allowing HDR values from e.g. Buya).
 *   2. Gamma correction derived from the mid-point:
 *        gamma = log(mid) / log(0.5)
 *      When mid = 0.5, gamma = 1 (identity). Values < 0.5 compress the
 *      darks; > 0.5 compresses the highlights.
 *
 * The identity shortcut (low=0, mid=0.5, high=1) returns x unchanged,
 * avoiding the log/pow cost on every sample when Levels are at defaults.
 */
static float penny_oct_levels(float x, float low, float mid, float high)
{
  /* Fast path: default levels are a no-op. */
  if (low == 0.0f && mid == 0.5f && high == 1.0f) {
    return x;
  }
  float n = darf_noxstep(low, high, x);
  float g = std::log(math::clamp(mid, 0.001f, 0.999f)) / std::log(0.5f);
  return std::pow(n, g);
}

/* Silence unused-function warnings for helpers not yet called by all noise
 * types. These references ensure the compiler sees a use, preventing
 * -Wunused-function from firing during incremental development. */
[[maybe_unused]] static float darf_smoothstep_ref(float a, float b, float x)
{
  return darf_smoothstep(a, b, x);
}
[[maybe_unused]] static float darf_bias_ref(float b, float x)
{
  return darf_bias(b, x);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend modes (Photoshop-style)
 *
 * 24 blend modes matching the SVM kernel's `penny_blend*` functions exactly.
 * These are used when `PennyParams::blend_mode != 0`: the noise type generates
 * two decorrelated samples (base and blend) and combines them through the
 * selected mode. Mode 0 = Normal (passthrough of base, no second sample needed).
 *
 * Each function operates on scalar float values in [0, 1] per-channel.
 * The dispatch function `penny_blend()` maps integer mode IDs to functions.
 * \{ */

/** Reflect: base²/(1-blend), clamped to [0, 1]. */
static float penny_blendReflect(float base, float blend)
{
  return (blend == 1.0f) ? blend : math::min(base * base / (1.0f - blend), 1.0f);
}

/** Add: base + blend (can exceed 1.0; caller clamps). */
static float penny_blendAdd(float base, float blend)
{
  return base + blend;
}

/** Multiply: base × blend. */
static float penny_blendMultiply(float base, float blend)
{
  return base * blend;
}

/** Difference: |base - blend|. */
static float penny_blendDifference(float base, float blend)
{
  return math::abs(base - blend);
}

/** Average: (base + blend) / 2. */
static float penny_blendAverage(float base, float blend)
{
  return (base + blend) * 0.5f;
}

/** Color Burn: darkens base by increasing contrast proportional to blend. */
static float penny_blendColorBurn(float base, float blend)
{
  return (blend == 0.0f) ? blend : math::max(1.0f - (1.0f - base) / blend, 0.0f);
}

/** Color Dodge: brightens base proportional to blend. */
static float penny_blendColorDodge(float base, float blend)
{
  return (blend == 1.0f) ? blend : math::min(base / (1.0f - blend), 1.0f);
}

/** Darken: min(base, blend). */
static float penny_blendDarken(float base, float blend)
{
  return math::min(blend, base);
}

/** Lighten: max(base, blend). */
static float penny_blendLighten(float base, float blend)
{
  return math::max(blend, base);
}

/** Exclusion: base + blend - 2·base·blend (inverted Multiply). */
static float penny_blendExclusion(float base, float blend)
{
  return base + blend - 2.0f * base * blend;
}

/** Glow: Reflect with arguments swapped (highlights base where blend is bright). */
static float penny_blendGlow(float base, float blend)
{
  return penny_blendReflect(blend, base);
}

/** Overlay: Multiply in darks, Screen in lights (threshold at base = 0.5). */
static float penny_blendOverlay(float base, float blend)
{
  return (base < 0.5f) ? (2.0f * base * blend) : (1.0f - 2.0f * (1.0f - base) * (1.0f - blend));
}

/** Hard Light: Overlay with arguments swapped. */
static float penny_blendHardLight(float base, float blend)
{
  return penny_blendOverlay(blend, base);
}

/** Vivid Light: Color Burn in darks, Color Dodge in lights (threshold at blend = 0.5). */
static float penny_blendVividLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendColorBurn(base, 2.0f * blend) :
                          penny_blendColorDodge(base, 2.0f * (blend - 0.5f));
}

/** Hard Mix: posterizes to 0 or 1 based on Vivid Light threshold. */
static float penny_blendHardMix(float base, float blend)
{
  return (penny_blendVividLight(base, blend) < 0.5f) ? 0.0f : 1.0f;
}

/** Linear Burn: base + blend - 1. */
static float penny_blendLinearBurn(float base, float blend)
{
  return base + blend - 1.0f;
}

/** Linear Dodge: base + blend (same as Add). */
static float penny_blendLinearDodge(float base, float blend)
{
  return base + blend;
}

/** Linear Light: Linear Burn in darks, Linear Dodge in lights. */
static float penny_blendLinearLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendLinearBurn(base, 2.0f * blend) :
                          penny_blendLinearDodge(base, 2.0f * (blend - 0.5f));
}

/** Negation: 1 - |1 - base - blend|. */
static float penny_blendNegation(float base, float blend)
{
  return 1.0f - math::abs(1.0f - base - blend);
}

/** Normal: replace base with blend (passthrough). */
static float penny_blendNormal(float /*base*/, float blend)
{
  return blend;
}

/** Phoenix: min(base,blend) - max(base,blend) + 1. */
static float penny_blendPhoenix(float base, float blend)
{
  return math::min(base, blend) - math::max(base, blend) + 1.0f;
}

/** Pin Light: Darken in darks, Lighten in lights. */
static float penny_blendPinLight(float base, float blend)
{
  return (blend < 0.5f) ? penny_blendDarken(base, 2.0f * blend) :
                          penny_blendLighten(base, 2.0f * (blend - 0.5f));
}

/** Screen: base + blend - base·blend (inverse Multiply). */
static float penny_blendScreen(float base, float blend)
{
  return base + blend - base * blend;
}

/** Soft Light: gentle contrast adjustment using a quadratic/sqrt curve. */
static float penny_blendSoftLight(float base, float blend)
{
  return (blend < 0.5f) ? (2.0f * base * blend + base * base * (1.0f - 2.0f * blend)) :
                          (2.0f * base * (1.0f - blend) + std::sqrt(base) * (2.0f * blend - 1.0f));
}

/** Subtract: base + blend - 1 (same as Linear Burn). */
static float penny_blendSubtract(float base, float blend)
{
  return base + blend - 1.0f;
}

/**
 * Blend mode dispatch.
 * Maps integer mode ID (from `PennyParams::blend_mode`) to the corresponding
 * blend function. Mode 0 (Normal/passthrough) is handled by the caller
 * skipping the second sample entirely; the default case here catches it as
 * a safety net.
 *
 * Mode assignments match `penny.h` and the RNA enum in `rna_nodetree.cc`:
 *   1=Add, 2=Multiply, 3=Difference, 4=Darken, 5=Lighten, 6=Screen,
 *   7=Overlay, 8=Hard Light, 9=Soft Light, 10=Color Dodge, 11=Color Burn,
 *   12=Linear Dodge, 13=Linear Burn, 14=Linear Light, 15=Vivid Light,
 *   16=Pin Light, 17=Hard Mix, 18=Reflect, 19=Glow, 20=Phoenix,
 *   21=Negation, 22=Exclusion, 23=Subtract, 24=Average.
 */
[[maybe_unused]] static float penny_blend(int mode, float base, float blend)
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
      return penny_blendScreen(base, blend);
    case 7:
      return penny_blendOverlay(base, blend);
    case 8:
      return penny_blendHardLight(base, blend);
    case 9:
      return penny_blendSoftLight(base, blend);
    case 10:
      return penny_blendColorDodge(base, blend);
    case 11:
      return penny_blendColorBurn(base, blend);
    case 12:
      return penny_blendLinearDodge(base, blend);
    case 13:
      return penny_blendLinearBurn(base, blend);
    case 14:
      return penny_blendLinearLight(base, blend);
    case 15:
      return penny_blendVividLight(base, blend);
    case 16:
      return penny_blendPinLight(base, blend);
    case 17:
      return penny_blendHardMix(base, blend);
    case 18:
      return penny_blendReflect(base, blend);
    case 19:
      return penny_blendGlow(base, blend);
    case 20:
      return penny_blendPhoenix(base, blend);
    case 21:
      return penny_blendNegation(base, blend);
    case 22:
      return penny_blendExclusion(base, blend);
    case 23:
      return penny_blendSubtract(base, blend);
    case 24:
      return penny_blendAverage(base, blend);
    default:
      return penny_blendNormal(base, blend);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Turbulence building blocks
 *
 * These are the reusable octave-summing functions that the individual noise
 * types are composed from. They all share the same pattern:
 *   - Loop over octaves, doubling frequency and scaling period each iteration.
 *   - Accumulate with amplitude weighting controlled by roughness.
 *   - Normalize by the accumulated weight sum.
 *
 * The `(f - 1.0f) * 1000.0f` offset on each octave decorrelates successive
 * octave contributions so they don't visually align at the origin. This is a
 * standard trick in multi-octave noise; the specific factor (1000) just needs
 * to be large relative to typical feature sizes.
 *
 * Variants differ in how they process each octave's raw Perlin sample:
 *   - **Additive** (`penny_turb_additive`): 70% Perlin + 30% bias, signed sum.
 *   - **Abs-turb** (`penny_abs_turb2`): |2·p01 - 1| (creased/billowy).
 *   - **Reg-turb** (`penny_reg_turb2`): signed (2·p01 - 1), normalized to [0,1].
 *   - **Reg-turb-signed** (`penny_reg_turb2_signed`): same but returns [-1,1].
 *   - **Blist-turb** (`penny_blist_turb`): signed Perlin, scale-sum normalization.
 *   - **Luka-turb** (`penny_luka_turb`): |perlin × 0.73|, amp-sum normalization.
 * \{ */

/**
 * Additive turbulence.
 * Each octave blends 70% Perlin (mapped to [0,1]) with 30% constant, then
 * converts to signed [-1,1] for accumulation. The noxstep(-0.12, 0.34) output
 * remap expands the usable range and shifts the black/white points for a
 * brighter, higher-contrast result. Levels are applied by the caller.
 */
static float3 penny_turb_additive(const float4 &co,
                                  const int4 &period,
                                  int octaves,
                                  float roughness)
{
  float3 value(0.0f, 0.0f, 0.0f);
  float f = 1.0f, amp = 1.0f, norm = 0.0f;
  for (int i = 0; i < octaves; i++) {
    float off = (f - 1.0f) * 1000.0f; /* decorrelation offset */
    int4 op(int(period.x * f), int(period.y * f), int(period.z * f), int(period.w * f));
    float3 n = penny_perlin_4d_tile(co.x * f + off, co.y * f + off, co.z * f + off, co.w, op);
    /* 70/30 blend: mostly noise, with a constant floor to prevent full-black octaves. */
    float3 t = 0.7f * (n * 0.5f + float3(0.5f, 0.5f, 0.5f)) + float3(0.3f, 0.3f, 0.3f);
    value = value + (2.0f * t - float3(1.0f, 1.0f, 1.0f)) * amp;
    norm += amp;
    f *= 2.0f;
    amp *= roughness;
  }
  /* Normalize and remap to [0, ~1] via noxstep. */
  value = value * (0.5f / norm);
  return float3(darf_noxstep(-0.12f, 0.34f, value.x),
                darf_noxstep(-0.12f, 0.34f, value.y),
                darf_noxstep(-0.12f, 0.34f, value.z));
}

/**
 * Absolute-value turbulence (abs-turb).
 * Takes |2·p01 - 1| of each octave (where p01 is Perlin mapped to [0,1]),
 * producing the classic Turbulence look with creased zero-crossings.
 * Used by Stupl, Naki, and Displaced Turbulence as an inner building block.
 *
 * Unlike `penny_turb_additive`, the period is doubled per octave by directly
 * scaling the int4 (op.x *= 2) rather than reconstructing from the base period.
 */
static float3 penny_abs_turb2(const float4 &co, const int4 &period, int octaves, float roughness)
{
  float3 sum(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 p01 = penny_perlin_4d_tile(
                     co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w, op) *
                     0.5f +
                 float3(0.5f, 0.5f, 0.5f);
    /* Absolute value of the signed remap — folds the waveform at zero. */
    sum = sum + float3(math::abs(2.0f * p01.x - 1.0f),
                       math::abs(2.0f * p01.y - 1.0f),
                       math::abs(2.0f * p01.z - 1.0f)) *
                    amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
}

/**
 * Regular signed turbulence, remapped to [0, 1].
 * Accumulates signed (2·p01 - 1) octaves, normalizes, then applies
 * noxstep(-1, 1) to map the result from [-1,1] into [0,1].
 * Used by Stupl (for inner domain warp) and indirectly by Hama and Dents.
 */
static float3 penny_reg_turb2(const float4 &co, const int4 &period, int octaves, float roughness)
{
  float3 sum(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 p01 = penny_perlin_4d_tile(
                     co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w, op) *
                     0.5f +
                 float3(0.5f, 0.5f, 0.5f);
    sum = sum + float3(2.0f * p01.x - 1.0f, 2.0f * p01.y - 1.0f, 2.0f * p01.z - 1.0f) * amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  sum = (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
  return float3(darf_noxstep(-1.0f, 1.0f, sum.x),
                darf_noxstep(-1.0f, 1.0f, sum.y),
                darf_noxstep(-1.0f, 1.0f, sum.z));
}

/**
 * Regular signed turbulence, returned in [-1, 1] (no noxstep remap).
 * Same accumulation as `penny_reg_turb2` but without the final [0,1] mapping.
 * Used by Hama and Dents which need the signed range for their own shaping.
 */
static float3 penny_reg_turb2_signed(const float4 &co,
                                     const int4 &period,
                                     int octaves,
                                     float roughness)
{
  float3 sum(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 p01 = penny_perlin_4d_tile(
                     co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w, op) *
                     0.5f +
                 float3(0.5f, 0.5f, 0.5f);
    sum = sum + float3(2.0f * p01.x - 1.0f, 2.0f * p01.y - 1.0f, 2.0f * p01.z - 1.0f) * amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
}

/**
 * Blistered Turbulence base.
 * Accumulates raw signed Perlin octaves (not abs or remapped) with
 * scale-sum normalization (divides by accumulated scale rather than
 * accumulated amplitude). The first octave is handled separately to
 * match the original algorithm's structure. Used by Blistered Turbulence, Nutous,
 * and Cranal.
 */
static float3 penny_blist_turb(const float4 &co, const int4 &period, int octaves, float roughness)
{
  float3 result(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, scale_sum = 0.0f, f = 1.0f;
  /* First octave: raw Perlin at base frequency. */
  float3 n0 = penny_perlin_4d_tile(co.x, co.y, co.z, co.w, period);
  result = n0 * scale;
  scale_sum += scale;
  scale *= roughness;
  f *= 2.0f;
  /* Subsequent octaves at doubling frequency. */
  for (int i = 1; i < octaves; i++) {
    float off = (f - 1.0f) * 1000.0f;
    int4 op(int(period.x * f), int(period.y * f), int(period.z * f), int(period.w * f));
    float3 n = penny_perlin_4d_tile(co.x * f + off, co.y * f + off, co.z * f + off, co.w, op);
    result = result + n * scale;
    scale *= roughness;
    scale_sum += scale;
    f *= 2.0f;
  }
  return result * (1.0f / scale_sum);
}

/**
 * Luka turbulence.
 * Accumulates |perlin × 0.73| per octave. The 0.73 scaling factor slightly
 * compresses the raw Perlin range before taking the absolute value, softening
 * the crease lines compared to standard abs-turbulence. Amplitude-sum
 * normalized.
 */
static float3 penny_luka_turb(const float4 &co, const int4 &period, int octaves, float roughness)
{
  float3 sum(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, norm = 0.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 n = penny_perlin_4d_tile(
        co.x * scale + off, co.y * scale + off, co.z * scale + off, co.w, op);
    sum = sum +
          float3(math::abs(n.x * 0.73f), math::abs(n.y * 0.73f), math::abs(n.z * 0.73f)) * amp;
    norm += amp;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
    amp *= roughness;
  }
  return (norm > 0.0f) ? sum * (1.0f / norm) : float3(0.0f, 0.0f, 0.0f);
}

/**
 * Pezo turb3 — signed Perlin octave accumulation (per-channel).
 * Accumulates raw signed Perlin with 1/f amplitude weighting and maps the
 * result from [-1,1] to [0,1]. Shared by Pezo and Sema noise types.
 *
 * Note: Z coordinate is NOT scaled by frequency (intentional — creates a
 * directional bias that Pezo/Sema rely on for their characteristic look).
 */
static float penny_pezo_turb3(
    float x, float y, float z, float w, int octaves, const int4 &period, int ch)
{
  float value = 0.0f, f = 1.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i++) {
    float off = (f - 1.0f) * 1000.0f;
    float3 s3 = penny_perlin_4d_tile(x * f + off, y * f + off, z * f, w, op);
    float s = (ch == 0) ? s3.x : ((ch == 1) ? s3.y : s3.z);
    value += s / f; /* 1/f weighting (pink noise spectrum). */
    f *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
  }
  return darf_noxstep(-1.0f, 1.0f, value);
}

/**
 * Pezo abs-turb2 — high-frequency abs turbulence detail (per-channel).
 * Similar to `penny_abs_turb2` but per-channel (scalar), with 1/scale
 * amplitude weighting and no final normalization. Used by Pezo and Sema
 * for fine-detail masking.
 */
static float penny_pezo_absturb2(
    float x, float y, float z, float w, int octaves, const int4 &period, int ch)
{
  float value = 0.0f, scale = 1.0f;
  int4 op = period;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    float3 pp = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w, op) *
                    0.5f +
                float3(0.5f, 0.5f, 0.5f);
    float p01 = (ch == 0) ? pp.x : ((ch == 1) ? pp.y : pp.z);
    value += math::abs(2.0f * p01 - 1.0f) / scale;
    scale *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
  }
  return value;
}

/**
 * Cell noise sample.
 * Quantizes the input coordinate to integer lattice positions (floor), applies
 * a large offset to avoid lattice-aligned artifacts near the origin, then
 * evaluates the Perlin base to get a pseudo-random value per cell. The result
 * is in [0, 1]. Negative coordinates are shifted by +100 to avoid negative
 * floor issues.
 */
static float cell_sample(float px, float py, float pz, float w, const int4 &per, int ch)
{
  float cx = px;
  if (cx < 0.0f) {
    cx += 100.0f;
  }
  float cy = py;
  if (cy < 0.0f) {
    cy += 100.0f;
  }
  float cz = pz;
  if (cz < 0.0f) {
    cz += 100.0f;
  }
  float qx = cx - (cx - std::floor(cx)); /* = floor(cx), avoiding int conversion. */
  float qy = cy - (cy - std::floor(cy));
  float qz = cz - (cz - std::floor(cz));
  /* Large offsets (1023.761, 31.875, 57.219) decorrelate from the lattice. */
  float3 p = penny_perlin_4d_tile(qx + 1023.761f, qy + 31.875f, qz + 57.219f, w, per);
  float pch[3];
  pch[0] = p.x;
  pch[1] = p.y;
  pch[2] = p.z;
  return pch[ch] * 0.5f + 0.5f;
}

/**
 * Cell noise octave accumulation.
 * Sums `cell_sample` over octaves with doubling frequency and roughness-scaled
 * amplitude. Amplitude-sum normalized and clamped to [0, 1].
 */
static float cell_octaves(float bx,
                          float by,
                          float bz,
                          float w,
                          const int4 &period,
                          int octaves,
                          float roughness,
                          int ch)
{
  float sum = 0.0f, amp = 1.0f, ampsum = 0.0f, scale = 1.0f;
  for (int i = 0; i < octaves; i++) {
    int4 op(int(period.x * scale),
            int(period.y * scale),
            int(period.z * scale),
            int(period.w * scale));
    sum += cell_sample(bx * scale, by * scale, bz * scale, w, op, ch) * amp;
    ampsum += amp;
    amp *= roughness;
    scale *= 2.0f;
  }
  return math::clamp(sum / math::max(ampsum, 1e-6f), 0.0f, 1.0f);
}

/**
 * Box noise sample — axis-sliced domain distortion.
 * Evaluates Perlin in two axis-reduced slices (XZ and YZ), uses those as
 * per-channel distortion offsets, then evaluates a final Perlin sample at
 * the distorted coordinate. This creates a blockier, more structured pattern
 * than standard domain warp.
 *
 * \param distortion  Strength of the axis-slice warp (typically 4.0).
 */
static float3 penny_box_noise(
    float x, float y, float z, float w, const int4 &period, float distortion)
{
  /* XZ slice noise (Y=0) and YZ slice noise (X=0). */
  float3 ox = penny_perlin_4d_tile(x, 0.0f, z, w, period) * 0.5f + float3(0.5f, 0.5f, 0.5f);
  float3 oy = penny_perlin_4d_tile(0.0f, y, z, w, period) * 0.5f + float3(0.5f, 0.5f, 0.5f);
  float oxc[3];
  oxc[0] = ox.x;
  oxc[1] = ox.y;
  oxc[2] = ox.z;
  float oyc[3];
  oyc[0] = oy.x;
  oyc[1] = oy.y;
  oyc[2] = oy.z;
  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    float offx = oxc[ch] * distortion;
    float offy = oyc[ch] * distortion;
    /* Final Perlin at the distorted position. */
    float3 fin = penny_perlin_4d_tile(x + offx, y + offy, z, w, period) * 0.5f +
                 float3(0.5f, 0.5f, 0.5f);
    float finc[3];
    finc[0] = fin.x;
    finc[1] = fin.y;
    finc[2] = fin.z;
    res[ch] = finc[ch];
  }
  return float3(res[0], res[1], res[2]);
}

/**
 * Mod noise sample — snapped-coordinate Perlin.
 * Quantizes each axis to integer + 0.5 with a directional offset (±1.001),
 * creating a stepped/quantized noise pattern. The sign flip on negative
 * coordinates ensures symmetric quantization across the origin.
 */
static float3 penny_mod_sample(float x, float y, float z, float w, const int4 &period)
{
  float sx = std::trunc(x) + 0.5f + (x < 0.0f ? -1.001f : 1.001f);
  float sy = std::trunc(y) + 0.5f + (y < 0.0f ? -1.001f : 1.001f);
  float sz = std::trunc(z) + 0.5f + (z < 0.0f ? -1.001f : 1.001f);
  return penny_perlin_4d_tile(sx, sy, sz, w, period) * 0.5f + float3(0.5f, 0.5f, 0.5f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Noise type implementations
 *
 * Each `penny_*_tile()` function implements one of the 19 Penny Noise types.
 * They all follow the same structure:
 *   1. Unpack PennyParams (octaves, period, roughness, levels, blend_mode).
 *   2. Evaluate the noise algorithm per-channel.
 *   3. Apply `penny_oct_levels()` for the user's Level Min/Mid/Max remap.
 *   4. If blend_mode != 0, generate a second decorrelated sample (offset +0.5
 *      in XYZ) and combine via `penny_blend()`.
 *
 * The blend path clamps both samples to [0,1] before and after blending to
 * prevent out-of-range values from non-clamping blend modes (Add, Subtract).
 * \{ */

/* ---- fBm ---- */

/**
 * Core normalized fBm (fractal Brownian motion).
 * Standard multi-octave sum of signed Perlin noise with roughness-scaled
 * amplitude decay and lacunarity = 2. The result is mapped from [-1,1] to
 * [0,1] and then passed through `penny_oct_levels`.
 *
 * This is the simplest Penny noise type and the mathematical foundation that
 * most others build upon or modify.
 */
static float3 penny_fbm_norm(
    const float4 &co, const int4 &period, int octaves, float roughness, const float3 &lvl)
{
  float amp = 1.0f, freq = 1.0f, norm = 0.0f;
  float3 sum(0.0f, 0.0f, 0.0f);
  int4 op = period;
  for (int i = 0; i < octaves; i++) {
    sum = sum + amp * penny_perlin_4d_tile(co.x * freq, co.y * freq, co.z * freq, co.w * freq, op);
    norm += amp;
    amp *= roughness;
    freq *= 2.0f;
    op.x *= 2;
    op.y *= 2;
    op.z *= 2;
    op.w *= 2;
  }
  /* Map signed [-1,1] to unsigned [0,1]. */
  float3 r = (norm > 0.0f) ? (sum * (1.0f / norm)) * 0.5f + float3(0.5f, 0.5f, 0.5f) :
                             float3(0.5f, 0.5f, 0.5f);
  return float3(penny_oct_levels(r.x, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(r.y, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(r.z, lvl.x, lvl.y, lvl.z));
}

/**
 * fBm tile entry point.
 * Normal mode (blend_mode == 0): returns fBm directly.
 * Blend mode: generates a second fBm sample offset by +0.5 in XYZ and blends.
 * At 1 octave the blend contribution is reduced to 30% (bs = 0.3) to prevent
 * over-saturation of the blend effect at low detail levels.
 */
static float3 penny_fbm_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float3 lvl = p.levels;
  if (p.blend_mode == 0) {
    return penny_fbm_norm(co, period, octaves, p.roughness, lvl);
  }
  /* Blend path: two decorrelated fBm samples. */
  float3 base = penny_fbm_norm(co, period, octaves, p.roughness, lvl);
  float3 base2 = penny_fbm_norm(
      float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w), period, octaves, p.roughness, lvl);
  float3 blended(penny_blend(p.blend_mode, base.x, base2.x),
                 penny_blend(p.blend_mode, base.y, base2.y),
                 penny_blend(p.blend_mode, base.z, base2.z));
  /* Blend strength: 30% at 1 octave, 100% at 2+. */
  float bs = (octaves == 1) ? 0.3f : 1.0f;
  return math::interpolate(base, blended, bs);
}

/* ---- Cranal ---- */

/**
 * Cranal noise — folded blistered turbulence with bias shaping.
 *
 * Evaluates blistered turbulence at 1 octave per iteration (but loops
 * `octaves + 1` times with x-offset stepping), folds the result via
 * |1 - 2n|, applies roughness-dependent bias, and averages. The output is
 * remapped through noxstep(0.75, 0.96) and a baked black-point lift
 * `(val + 1.5) / 2.5` that was originally a user Min = -1.5 parameter.
 *
 * Produces a cracked/crazed organic pattern.
 */
static float3 penny_cranal_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;

  /* Scale input by 1/3 for a coarser base feature size. */
  const float cx = co.x / 3.0f, cy0 = co.y / 3.0f, cz0 = co.z / 3.0f;
  /* Roughness-dependent bias: controls fold sharpness. */
  const float bias = math::clamp(0.80f + (roughness - 0.5f) * 0.3f, 0.5f, 0.95f);

  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    const float off = float(ch) * 1000.0f; /* per-channel decorrelation */
    float x1 = cx + off;
    const float cy = cy0 + off, cz = cz0 + off;
    float val = 0.0f;
    for (int i = 0; i <= octaves; i++) {
      /* Per-octave Y/Z decorrelation: prime-ish offsets break lattice alignment
       * across octaves, eliminating the grid artifact caused by constant cy/cz. */
      float oy = float(i) * 7.793f;
      float oz = float(i) * 11.219f;
      float3 t = penny_blist_turb(float4(x1, cy + oy, cz + oz, co.w), period, 1, roughness);
      float tch[3];
      tch[0] = t.x;
      tch[1] = t.y;
      tch[2] = t.z;
      float n = darf_noxstep(-0.5f, 0.5f, tch[ch]);
      float folded = math::abs(1.0f - 2.0f * n); /* fold at midpoint */
      val += darf_bias(bias, folded);
      x1 += 3.0f; /* step along X for each "octave" */
    }
    val /= float(octaves + 1);
    val = darf_noxstep(0.75f, 0.96f, val);
    val = (val + 1.5f) / 2.5f; /* baked black-point lift (was user Min = -1.5) */
    val = penny_oct_levels(val, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[ch] = val;
    }
    else {
      const float off2 = off + 0.5f;
      float x1b = cx + off2;
      const float cyb = cy0 + off2, czb = cz0 + off2;
      float val2 = 0.0f;
      for (int i = 0; i <= octaves; i++) {
        float oy = float(i) * 7.793f;
        float oz = float(i) * 11.219f;
        float3 t2 = penny_blist_turb(float4(x1b, cyb + oy, czb + oz, co.w), period, 1, roughness);
        float t2ch[3];
        t2ch[0] = t2.x;
        t2ch[1] = t2.y;
        t2ch[2] = t2.z;
        float n2 = darf_noxstep(-0.5f, 0.5f, t2ch[ch]);
        float folded2 = math::abs(1.0f - 2.0f * n2);
        val2 += darf_bias(bias, folded2);
        x1b += 3.0f;
      }
      val2 /= float(octaves + 1);
      val2 = darf_noxstep(0.75f, 0.96f, val2);
      val2 = (val2 + 1.5f) / 2.5f; /* baked black-point lift */
      val2 = penny_oct_levels(val2, lvl.x, lvl.y, lvl.z);
      res[ch] = math::clamp(penny_blend(p.blend_mode, val, val2), 0.0f, 1.0f);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/* ---- Cell ---- */

/**
 * Cell noise — Cellular/Worley-derived pattern.
 * Multi-octave cell noise producing a stepped/quantized pattern where each
 * integer cell gets a pseudo-random value from the Perlin lattice.
 */
static float3 penny_cell_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = cell_octaves(
        co.x + dch, co.y + dch, co.z + dch, co.w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[ch] = v;
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
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      res[ch] = math::clamp(penny_blend(p.blend_mode, v, v2), 0.0f, 1.0f);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/** \} */

/* ---- Turbulence ---- */

/**
 * Turbulence — absolute-value variant of fBm.
 * Uses `penny_turb_additive` for the classic creased/billowy turbulence look
 * (Perlin's original Turbulence function). Levels applied after blending.
 */
static float3 penny_turbulence_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const float3 lvl = p.levels;
  float3 base = penny_turb_additive(co, p.period, octaves, p.roughness);
  if (p.blend_mode == 0) {
    return float3(penny_oct_levels(base.x, lvl.x, lvl.y, lvl.z),
                  penny_oct_levels(base.y, lvl.x, lvl.y, lvl.z),
                  penny_oct_levels(base.z, lvl.x, lvl.y, lvl.z));
  }
  float3 base2 = penny_turb_additive(
      float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w), p.period, octaves, p.roughness);
  float3 tb(math::clamp(penny_blend(p.blend_mode,
                                    math::clamp(base.x, 0.0f, 1.0f),
                                    math::clamp(base2.x, 0.0f, 1.0f)),
                        0.0f,
                        1.0f),
            math::clamp(penny_blend(p.blend_mode,
                                    math::clamp(base.y, 0.0f, 1.0f),
                                    math::clamp(base2.y, 0.0f, 1.0f)),
                        0.0f,
                        1.0f),
            math::clamp(penny_blend(p.blend_mode,
                                    math::clamp(base.z, 0.0f, 1.0f),
                                    math::clamp(base2.z, 0.0f, 1.0f)),
                        0.0f,
                        1.0f));
  return float3(penny_oct_levels(tb.x, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(tb.y, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(tb.z, lvl.x, lvl.y, lvl.z));
}

/* ---- Stupl ---- */

/**
 * Stupl noise — displacement-warped turbulence with bias shaping.
 *
 * Pipeline per channel:
 *   1. Compute abs-turbulence and a signed displacement vector (1 octave).
 *   2. Displace the coordinate by the signed turb and evaluate reg-turb at the
 *      warped position.
 *   3. Remap through noxstep(-2.45, 2.5), then gate by the abs-turb range
 *      [0.5 - turb, 0.5 + turb].
 *   4. Smoothstep + bias(0.45) for the final soft contrast curve.
 *
 * Produces an organic, blotchy pattern with soft edges.
 */
static float3 penny_stupl_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  const float3 one(1.0f, 1.0f, 1.0f);

  float3 turb = penny_abs_turb2(co, period, octaves, roughness);
  /* Single-octave signed displacement field. */
  float3 disp3 = one - 2.0f * penny_reg_turb2(co, period, 1, roughness);
  float tch[3];
  tch[0] = turb.x;
  tch[1] = turb.y;
  tch[2] = turb.z;
  float dch[3];
  dch[0] = disp3.x;
  dch[1] = disp3.y;
  dch[2] = disp3.z;
  float res[3];
  for (int c = 0; c < 3; c++) {
    float d = dch[c];
    /* Domain-warped inner turbulence. */
    float3 inner3 = one - 2.0f * penny_reg_turb2(float4(co.x - d, co.y + d, co.z + d, co.w),
                                                 period,
                                                 octaves,
                                                 roughness);
    float ich[3];
    ich[0] = inner3.x;
    ich[1] = inner3.y;
    ich[2] = inner3.z;
    float v = darf_noxstep(-2.45f, 2.5f, ich[c]);
    /* Turb-gated range: narrows the visible band based on outer turbulence. */
    v = darf_noxstep(0.5f - tch[c], 0.5f + tch[c], v);
    v = darf_smoothstep(0.0f, 1.0f, v);
    v = darf_bias(0.45f, v);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[c] = v;
    }
    else {
      float3 inner3b = one -
                       2.0f * penny_reg_turb2(
                                  float4(co.x - d + 0.5f, co.y + d + 0.5f, co.z + d + 0.5f, co.w),
                                  period,
                                  octaves,
                                  roughness);
      float ichb[3];
      ichb[0] = inner3b.x;
      ichb[1] = inner3b.y;
      ichb[2] = inner3b.z;
      float v2 = darf_noxstep(-2.45f, 2.5f, ichb[c]);
      v2 = darf_noxstep(0.5f - tch[c], 0.5f + tch[c], v2);
      v2 = darf_smoothstep(0.0f, 1.0f, v2);
      v2 = darf_bias(0.45f, v2);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      res[c] = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/* ---- Box ---- */

/**
 * Box noise — axis-sliced domain distortion over octaves.
 * Multi-octave sum of `penny_box_noise` (distortion = 4.0), mapped through
 * boxstep(-1.05, 1.0) to [0,1]. Produces a boxy, grid-influenced pattern.
 */
static float3 penny_box_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  const float3 one(1.0f, 1.0f, 1.0f);

  float3 value(0.0f, 0.0f, 0.0f);
  float f = 1.0f, amp = 1.0f;
  for (int i = 0; i < octaves; i++) {
    float off = (f - 1.0f) * 1000.0f;
    int4 op(int(period.x * f), int(period.y * f), int(period.z * f), int(period.w * f));
    float3 b = penny_box_noise(co.x * f + off, co.y * f + off, co.z * f + off, co.w, op, 4.0f);
    value = value + (2.0f * b - one) * amp;
    f *= 2.0f;
    amp *= roughness;
  }
  float3 base(darf_boxstep(-1.05f, 1.0f, value.x),
              darf_boxstep(-1.05f, 1.0f, value.y),
              darf_boxstep(-1.05f, 1.0f, value.z));
  base = float3(penny_oct_levels(base.x, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(base.y, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(base.z, lvl.x, lvl.y, lvl.z));
  if (p.blend_mode == 0) {
    return base;
  }

  float3 value2(0.0f, 0.0f, 0.0f);
  f = 1.0f;
  amp = 1.0f;
  for (int i = 0; i < octaves; i++) {
    float off = (f - 1.0f) * 1000.0f;
    int4 op(int(period.x * f), int(period.y * f), int(period.z * f), int(period.w * f));
    float3 b = penny_box_noise(
        (co.x + 0.5f) * f + off, (co.y + 0.5f) * f + off, (co.z + 0.5f) * f + off, co.w, op, 4.0f);
    value2 = value2 + (2.0f * b - one) * amp;
    f *= 2.0f;
    amp *= roughness;
  }
  float3 base2(darf_boxstep(-1.05f, 1.0f, value2.x),
               darf_boxstep(-1.05f, 1.0f, value2.y),
               darf_boxstep(-1.05f, 1.0f, value2.z));
  base2 = float3(penny_oct_levels(base2.x, lvl.x, lvl.y, lvl.z),
                 penny_oct_levels(base2.y, lvl.x, lvl.y, lvl.z),
                 penny_oct_levels(base2.z, lvl.x, lvl.y, lvl.z));
  return float3(math::clamp(penny_blend(p.blend_mode,
                                        math::clamp(base.x, 0.0f, 1.0f),
                                        math::clamp(base2.x, 0.0f, 1.0f)),
                            0.0f,
                            1.0f),
                math::clamp(penny_blend(p.blend_mode,
                                        math::clamp(base.y, 0.0f, 1.0f),
                                        math::clamp(base2.y, 0.0f, 1.0f)),
                            0.0f,
                            1.0f),
                math::clamp(penny_blend(p.blend_mode,
                                        math::clamp(base.z, 0.0f, 1.0f),
                                        math::clamp(base2.z, 0.0f, 1.0f)),
                            0.0f,
                            1.0f));
}

/* ---- Mod ---- */

/**
 * Mod noise — snapped-coordinate Perlin over octaves.
 * Uses `penny_mod_sample` for a quantized/stepped noise with roughness × 2
 * amplitude ramp, remapped to [0,1] via `2·(sum/ampsum) - 0.5`. Input
 * coordinates are shifted (+1.11, +1.54, +1.82) to avoid symmetry at origin.
 */
static float3 penny_mod_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  /* Offset to break origin symmetry. */
  float cx = co.x + 1.11f, cy = co.y + 1.54f, cz = co.z + 1.82f;
  float ramp = roughness * 2.0f; /* Amplitude ramp steeper than standard roughness. */

  float3 value(0.0f, 0.0f, 0.0f);
  float scale = 1.0f, amp = 1.0f, ampsum = 0.0f;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    int4 op(int(period.x * scale),
            int(period.y * scale),
            int(period.z * scale),
            int(period.w * scale));
    value = value +
            penny_mod_sample(cx * scale + off, cy * scale + off, cz * scale + off, co.w, op) * amp;
    ampsum += amp;
    scale *= 2.0f;
    amp *= ramp;
  }
  float3 base(math::clamp(2.0f * (value.x / math::max(ampsum, 1e-6f)) - 0.5f, 0.0f, 1.0f),
              math::clamp(2.0f * (value.y / math::max(ampsum, 1e-6f)) - 0.5f, 0.0f, 1.0f),
              math::clamp(2.0f * (value.z / math::max(ampsum, 1e-6f)) - 0.5f, 0.0f, 1.0f));
  base = float3(penny_oct_levels(base.x, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(base.y, lvl.x, lvl.y, lvl.z),
                penny_oct_levels(base.z, lvl.x, lvl.y, lvl.z));
  if (p.blend_mode == 0) {
    return base;
  }

  float c2x = cx + 0.5f, c2y = cy + 0.5f, c2z = cz + 0.5f;
  float3 value2(0.0f, 0.0f, 0.0f);
  scale = 1.0f;
  amp = 1.0f;
  float ampsum2 = 0.0f;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    int4 op(int(period.x * scale),
            int(period.y * scale),
            int(period.z * scale),
            int(period.w * scale));
    value2 = value2 +
             penny_mod_sample(c2x * scale + off, c2y * scale + off, c2z * scale + off, co.w, op) *
                 amp;
    ampsum2 += amp;
    scale *= 2.0f;
    amp *= ramp;
  }
  float3 base2(math::clamp(2.0f * (value2.x / math::max(ampsum2, 1e-6f)) - 0.5f, 0.0f, 1.0f),
               math::clamp(2.0f * (value2.y / math::max(ampsum2, 1e-6f)) - 0.5f, 0.0f, 1.0f),
               math::clamp(2.0f * (value2.z / math::max(ampsum2, 1e-6f)) - 0.5f, 0.0f, 1.0f));
  base2 = float3(penny_oct_levels(base2.x, lvl.x, lvl.y, lvl.z),
                 penny_oct_levels(base2.y, lvl.x, lvl.y, lvl.z),
                 penny_oct_levels(base2.z, lvl.x, lvl.y, lvl.z));
  return float3(math::clamp(penny_blend(p.blend_mode,
                                        math::clamp(base.x, 0.0f, 1.0f),
                                        math::clamp(base2.x, 0.0f, 1.0f)),
                            0.0f,
                            1.0f),
                math::clamp(penny_blend(p.blend_mode,
                                        math::clamp(base.y, 0.0f, 1.0f),
                                        math::clamp(base2.y, 0.0f, 1.0f)),
                            0.0f,
                            1.0f),
                math::clamp(penny_blend(p.blend_mode,
                                        math::clamp(base.z, 0.0f, 1.0f),
                                        math::clamp(base2.z, 0.0f, 1.0f)),
                            0.0f,
                            1.0f));
}

/* ---- Blistered Turbulence ---- */

/**
 * Blistered Turbulence noise — double-layered blistered turbulence.
 *
 * Pipeline:
 *   1. Outer layer: blistered turbulence → boxstep → fold (|1 - 2·boxstep|)
 *      to create a threshold mask (tch).
 *   2. Inner layer: blistered turbulence at 2× frequency → boxstep.
 *   3. Gate the inner through the outer mask: boxstep(0.5 - tch, 0.5 + tch).
 *
 * The result is a bubbly/blistered organic texture where the outer layer
 * controls the size and distribution of the blisters.
 */
static float3 penny_blistered_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;

  /* Outer turbulence → threshold mask. */
  float3 t0 = penny_blist_turb(co, period, octaves, roughness);
  float t0c[3];
  t0c[0] = t0.x;
  t0c[1] = t0.y;
  t0c[2] = t0.z;
  float tch[3];
  for (int ch = 0; ch < 3; ch++) {
    tch[ch] = darf_boxstep(-1.5f, 1.5f, 1.0f - 2.0f * darf_boxstep(-1.0f, 1.0f, t0c[ch]));
  }
  /* Inner turbulence at 2× frequency. */
  float3 inner = penny_blist_turb(
      float4(co.x * 2.0f, co.y * 2.0f, co.z * 2.0f, co.w), period, octaves, roughness);
  float ich[3];
  ich[0] = inner.x;
  ich[1] = inner.y;
  ich[2] = inner.z;
  /* Second inner sample for blend mode. */
  float3 innerb = penny_blist_turb(
      float4(co.x * 2.0f + 0.5f, co.y * 2.0f + 0.5f, co.z * 2.0f + 0.5f, co.w),
      period,
      octaves,
      roughness);
  float ichb[3];
  ichb[0] = innerb.x;
  ichb[1] = innerb.y;
  ichb[2] = innerb.z;

  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    float s = darf_boxstep(-1.0f, 1.0f, ich[ch]);
    float v = darf_boxstep(0.5f - tch[ch], 0.5f + tch[ch], s);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float sb = darf_boxstep(-1.0f, 1.0f, ichb[ch]);
      float v2 = darf_boxstep(0.5f - tch[ch], 0.5f + tch[ch], sb);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      res[ch] = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/* ---- Nutous ---- */

/**
 * Nutous noise — displacement-warped blistered turbulence.
 *
 * Uses blistered turbulence × 0.5 as a displacement field, evaluates a
 * second blistered turbulence at the displaced position, and shapes the
 * result through noxstep(-0.01, 0.4) → square → pow(0.5316). The power
 * exponent (0.5316) was empirically tuned for the desired contrast curve.
 *
 * Produces a fluid, vein-like organic pattern.
 */
static float3 penny_nutous_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;

  /* Displacement field: half-strength blistered turbulence. */
  float3 disp = penny_blist_turb(co, period, octaves, roughness) * 0.5f;
  float dch[3];
  dch[0] = disp.x;
  dch[1] = disp.y;
  dch[2] = disp.z;
  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    float d = dch[ch];
    float3 inner = penny_blist_turb(
        float4(co.x + d, co.y + d, co.z + d, co.w), period, octaves, roughness);
    float ich[3];
    ich[0] = inner.x;
    ich[1] = inner.y;
    ich[2] = inner.z;
    float v = darf_noxstep(-0.01f, 0.4f, math::abs(ich[ch]) * 0.5f);
    v = v * v;                /* contrast boost via squaring */
    v = std::pow(v, 0.5316f); /* empirical gamma for the desired curve */
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float3 disp2 = penny_blist_turb(float4(co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w),
                                      period,
                                      octaves,
                                      roughness) *
                     0.5f;
      float d2ch[3];
      d2ch[0] = disp2.x;
      d2ch[1] = disp2.y;
      d2ch[2] = disp2.z;
      float d2 = d2ch[ch];
      float3 inner2 = penny_blist_turb(
          float4(co.x + 0.5f + d2, co.y + 0.5f + d2, co.z + 0.5f + d2, co.w),
          period,
          octaves,
          roughness);
      float i2ch[3];
      i2ch[0] = inner2.x;
      i2ch[1] = inner2.y;
      i2ch[2] = inner2.z;
      float v2 = darf_noxstep(-0.01f, 0.4f, math::abs(i2ch[ch]) * 0.5f);
      v2 = v2 * v2;
      v2 = std::pow(v2, 0.5316f);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      res[ch] = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/* ---- Naki ---- */

/**
 * Naki inner helper — displacement-warped abs-turbulence with smoothstep bias.
 *
 * Pipeline:
 *   1. Abs-turbulence displacement scaled by roughness-dependent factor.
 *   2. Single Perlin evaluation at warped position → [0,1].
 *   3. Smoothstep(0.325, 0.63) bias from the displacement magnitude.
 *   4. Average of turb + bias, remapped through noxstep(0.45, 1.0).
 *
 * \param disp_scale  Roughness-dependent displacement strength [0.2, 0.85].
 */
static float penny_naki(
    float x, float y, float z, float w, const int4 &period, int octaves, float roughness, int ch)
{
  float disp_scale = math::clamp(0.5f + (roughness - 0.5f) * 0.3f, 0.2f, 0.85f);
  float3 ad = penny_abs_turb2(float4(x, y, z, w), period, octaves, 0.5f);
  float d = ((ch == 0) ? ad.x : ((ch == 1) ? ad.y : ad.z)) * disp_scale;
  /* Prime-ish offsets on the displaced Perlin sample break lattice alignment
   * that caused grid artifacts when d is symmetric across axes. */
  float3 pp = penny_perlin_4d_tile(x + d + 7.793f, y - d + 11.219f, z + d + 13.547f, w, period);
  float turb = ((ch == 0) ? pp.x : ((ch == 1) ? pp.y : pp.z)) * 0.5f + 0.5f;
  float bias = darf_smoothstep(0.325f, 0.63f, d * 0.25f + 0.5f);
  return darf_noxstep(0.45f, 1.0f, (turb + bias) * 0.5f);
}

/** Naki tile entry point. */
static float3 penny_naki_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float3 result(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = penny_naki(co.x + dch, co.y + dch, co.z + dch, co.w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    float r;
    if (p.blend_mode == 0) {
      r = v;
    }
    else {
      float v2 = penny_naki(co.x + dch + 0.5f,
                            co.y + dch + 0.5f,
                            co.z + dch + 0.5f,
                            co.w,
                            period,
                            octaves,
                            roughness,
                            ch);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      r = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
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

/* ---- Luka ---- */

/**
 * Luka noise — double-warped Luka turbulence with cascaded noxstep gating.
 *
 * Pipeline per channel:
 *   1. Luka turbulence at scaled/offset coordinate → outer shape.
 *   2. Perlin displacement × 0.275.
 *   3. Inner Luka turbulence at displaced position.
 *   4. Cascaded gating: noxstep(-2, 2) → noxstep(0.47-t, 0.525+t) → noxstep(0.5, 0.78).
 *
 * Input coordinates are scaled by 0.8 and offset (+127.1, +64.8, +26.81)
 * to break symmetry and adjust the base feature size.
 */
static float3 penny_luka_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;

  /* Coordinate pre-scaling and offset for feature size / symmetry breaking. */
  float cx = co.x * 0.8f + 127.1f, cy = co.y * 0.8f + 64.8f, cz = co.z * 0.8f + 26.81f;
  float3 temp = penny_luka_turb(float4(cx, cy, cz, co.w), period, octaves, roughness);
  float3 disp3 = penny_perlin_4d_tile(cx, cy, cz, co.w, period) * 0.275f;
  float tch[3];
  tch[0] = temp.x;
  tch[1] = temp.y;
  tch[2] = temp.z;
  float dch[3];
  dch[0] = disp3.x;
  dch[1] = disp3.y;
  dch[2] = disp3.z;
  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    float d = dch[ch];
    float w1x = cx + d, w1y = cy - d, w1z = cz + d;
    /* Inner Luka turbulence at doubly-displaced position. */
    float3 inner3 = penny_luka_turb(
        float4(w1x + d, w1y + d, w1z + d, co.w), period, octaves, roughness);
    float ich[3];
    ich[0] = inner3.x;
    ich[1] = inner3.y;
    ich[2] = inner3.z;
    float v = darf_noxstep(-2.0f, 2.0f, ich[ch]);
    /* Turb-gated range, then final contrast squeeze. */
    v = darf_noxstep(0.47f - tch[ch], 0.525f + tch[ch], v);
    v = darf_noxstep(0.5f, 0.78f, v);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float3 inner3b = penny_luka_turb(
          float4(w1x + d + 0.5f, w1y + d + 0.5f, w1z + d + 0.5f, co.w),
          period,
          octaves,
          roughness);
      float ichb[3];
      ichb[0] = inner3b.x;
      ichb[1] = inner3b.y;
      ichb[2] = inner3b.z;
      float v2 = darf_noxstep(-2.0f, 2.0f, ichb[ch]);
      v2 = darf_noxstep(0.47f - tch[ch], 0.525f + tch[ch], v2);
      v2 = darf_noxstep(0.5f, 0.78f, v2);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      res[ch] = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/* ---- Hama ---- */

/**
 * Hama inner helper — per-octave product of gated signed turbulence.
 *
 * Each octave evaluates signed turbulence (1 octave), gates through
 * noxstep(0, gate_hi) of the absolute value, and multiplies into a running
 * product (initialized at 2.0). The gate threshold is roughness-dependent:
 *   gate_hi = clamp(0.3 + (1-roughness - 0.5) × 0.2, 0.08, 0.6)
 *
 * The multiplicative accumulation creates a pattern where features only
 * survive where ALL octaves agree — producing a sparse, networked look.
 */
static float penny_hama(
    float x, float y, float z, float w, const int4 &period, int octaves, float roughness, int ch)
{
  float rough_inv = 1.0f - roughness;
  float gate_hi = math::clamp(0.3f + (rough_inv - 0.5f) * 0.2f, 0.08f, 0.6f);
  float the_value = 2.0f; /* Initial product seed > 1 to set the dynamic range. */
  for (int i = 0; i < octaves; i++) {
    float fi = float(i) * 10.0f; /* Per-octave coordinate offset. */
    float3 t = penny_reg_turb2_signed(float4(x + fi, y + fi, z + fi, w), period, 1, roughness);
    float turb = (ch == 0) ? t.x : ((ch == 1) ? t.y : t.z);
    float temp = darf_noxstep(0.0f, gate_hi, math::abs(darf_noxstep(0.0f, 1.0f, turb)));
    the_value *= temp; /* Multiplicative accumulation. */
  }
  return the_value;
}

/** Hama tile entry point. */
static float3 penny_hama_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float3 result(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = penny_hama(co.x + dch, co.y + dch, co.z + dch, co.w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    float r;
    if (p.blend_mode == 0) {
      r = v;
    }
    else {
      float v2 = penny_hama(co.x + dch + 0.5f,
                            co.y + dch + 0.5f,
                            co.z + dch + 0.5f,
                            co.w,
                            period,
                            octaves,
                            roughness,
                            ch);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      r = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
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

/* ---- Dents ---- */

/**
 * Dents inner helper — running-max of folded turbulence with fractional detail.
 *
 * Each octave evaluates single-octave signed turbulence at a prime-offset
 * coordinate, folds via |1 - 2·noxstep|, gates through boxstep(0, fold_hi),
 * and takes the running maximum. The last octave is fractionally weighted by
 * (detail - floor(detail)) for smooth LOD transitions.
 *
 * Post-processing: boxstep(0.25, 1.0) × value → pow(0.8) → noxstep(0, 1.15).
 *
 * \param fold_hi  Roughness-dependent fold threshold [0.12, 0.95].
 */
static float penny_dents(
    float x, float y, float z, float w, float detail, const int4 &period, int ch, float roughness)
{
  int ioct = int(detail);
  float fold_hi = math::clamp(0.56f + (roughness - 0.5f) * 0.5f, 0.12f, 0.95f);
  float the_value = 0.0f;
  int i;
  /* Integer octaves: full-weight running max. */
  for (i = 0; i < ioct; i++) {
    float o = float(i + 1);
    /* Prime-number offsets (12.793, 13.547, 14.219) for octave decorrelation. */
    float3 t3 = penny_reg_turb2_signed(
        float4(x + o * 12.793f, y + o * 13.547f, z + o * 14.219f, w), period, 1, 0.5f);
    float turb = (ch == 0) ? t3.x : ((ch == 1) ? t3.y : t3.z);
    float folded = math::abs(1.0f - 2.0f * darf_noxstep(-1.0f, 1.0f, turb));
    float temp = darf_boxstep(0.0f, fold_hi, folded);
    the_value = math::max(the_value, temp);
  }
  /* Fractional last octave for smooth detail transitions. */
  float o = float(i + 1);
  float3 t3 = penny_reg_turb2_signed(
      float4(x + o * 12.793f, y + o * 13.547f, z + o * 14.219f, w), period, 1, 0.5f);
  float turb = (ch == 0) ? t3.x : ((ch == 1) ? t3.y : t3.z);
  float folded = math::abs(1.0f - 2.0f * darf_noxstep(-1.0f, 1.0f, turb));
  float temp = (detail - std::floor(detail)) * darf_boxstep(0.0f, fold_hi, folded);
  the_value = math::max(the_value, temp);

  /* Contrast shaping: suppress low values, gamma adjust, expand range. */
  the_value = darf_boxstep(0.25f, 1.0f, the_value) * the_value;
  the_value = std::pow(the_value, 0.8f);
  the_value = darf_noxstep(0.0f, 1.15f, the_value);
  return the_value;
}

/** Dents tile entry point. Uses fractional detail (not integer-clamped). */
static float3 penny_dents_tile(const float4 &co, const PennyParams &p)
{
  const float d = math::max(p.detail, 1.0f);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float3 result(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = penny_dents(co.x + dch, co.y + dch, co.z + dch, co.w, d, period, ch, roughness);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    float r;
    if (p.blend_mode == 0) {
      r = v;
    }
    else {
      float v2 = penny_dents(
          co.x + dch + 0.5f, co.y + dch + 0.5f, co.z + dch + 0.5f, co.w, d, period, ch, roughness);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      r = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
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

/* ---- Sema ---- */

/**
 * Sema inner helper — gated folded turbulence with high-frequency dither.
 *
 * Each octave evaluates `penny_pezo_turb3` (1 octave), folds via
 * |1 - 2·turb|, gates through smoothstep(gate_lo, 0.999), and takes the
 * running maximum. After all octaves, a fine-detail dither from
 * `penny_pezo_absturb2` at 75× scale is added, and the result is gated
 * through boxstep(0.95, 1.0) to create a sharp, sparse pattern.
 *
 * \param gate_lo  Roughness-dependent gate threshold [0.5, 0.95].
 */
static float penny_sema(
    float x, float y, float z, float w, int octaves, const int4 &period, int ch, float roughness)
{
  float gate_lo = math::clamp(0.8f + (roughness - 0.5f) * 0.3f, 0.5f, 0.95f);
  float the_value = 0.0f;
  for (int i = 0; i < octaves; i++) {
    float fi = float(i);
    float t = penny_pezo_turb3(x + fi * 21.7f, y + fi * 11.3f, z + fi * 13.9f, w, 1, period, ch);
    float temp = 1.0f - math::abs(1.0f - 2.0f * t); /* fold */
    float displace = darf_smoothstep(gate_lo, 0.999f, temp);
    the_value = math::max(the_value, displace);
  }
  /* High-frequency dither from abs-turb at 75× scale. */
  float ab = penny_pezo_absturb2(x * 75.0f, y * 75.0f, z * 75.0f, w, 1, period, ch);
  float dither = ((1.0f - 2.0f * ab) * 0.25f + 0.5f) * 0.05f;
  the_value = darf_boxstep(0.95f, 1.0f, the_value) + dither;
  return the_value;
}

/** Sema tile entry point. */
static float3 penny_sema_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float3 result(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = penny_sema(co.x + dch, co.y + dch, co.z + dch, co.w, octaves, period, ch, roughness);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    float r;
    if (p.blend_mode == 0) {
      r = v;
    }
    else {
      float v2 = penny_sema(co.x + dch + 0.5f,
                            co.y + dch + 0.5f,
                            co.z + dch + 0.5f,
                            co.w,
                            octaves,
                            period,
                            ch,
                            roughness);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      r = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
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

/* ---- Pezo ---- */

/**
 * Pezo inner helper — gated turbulence product masked by fine abs-turb.
 *
 * Pipeline:
 *   1. Per-octave: evaluate `penny_pezo_turb3` at 0.5× scale with prime offsets,
 *      fold and gate through smoothstep(gate_lo, 1.0), accumulate as product.
 *   2. High-frequency mask from `penny_pezo_absturb2` at 15× scale, gated
 *      through cascaded noxstep.
 *   3. Combine: interpolate(0.25, 1.0, product) × noxstep(mask).
 *   4. Final smoothstep(0.0, 0.40) for soft contrast.
 *
 * \param gate_lo  Roughness-dependent gate [0.2, 0.7].
 */
static float penny_pezo(
    float x, float y, float z, float w, int octaves, const int4 &period, int ch, float roughness)
{
  float gate_lo = math::clamp(0.45f + (roughness - 0.5f) * 0.3f, 0.2f, 0.7f);
  float value = 1.0f;
  for (int i = 0; i < octaves; i++) {
    float fi = float(i);
    float t = penny_pezo_turb3(x * 0.5f + fi * 19.55f,
                               y * 0.5f + fi * 20.612f,
                               z * 0.5f + fi * 21.175f,
                               w,
                               1,
                               period,
                               ch);
    float temp = 1.0f - math::abs(1.0f - 2.0f * t);
    value *= darf_smoothstep(gate_lo, 1.0f, temp);
  }
  /* High-frequency abs-turb mask. */
  float temp = penny_pezo_absturb2(x * 15.0f, y * 15.0f, z * 15.0f, w, octaves, period, ch);
  temp = darf_noxstep(0.32f, 1.0f, darf_noxstep(-1.25f, 1.25f, temp));
  value = math::interpolate(0.25f, 1.0f, value) * darf_noxstep(-0.2f, 1.2f, temp);
  value = darf_smoothstep(0.0f, 0.40f, value);
  return value;
}

/** Pezo tile entry point. */
static float3 penny_pezo_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float3 result(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = penny_pezo(co.x + dch, co.y + dch, co.z + dch, co.w, octaves, period, ch, roughness);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    float r;
    if (p.blend_mode == 0) {
      r = v;
    }
    else {
      float v2 = penny_pezo(co.x + dch + 0.5f,
                            co.y + dch + 0.5f,
                            co.z + dch + 0.5f,
                            co.w,
                            octaves,
                            period,
                            ch,
                            roughness);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      r = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
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

/* ---- VL (Variable Lacunarity) ---- */

/**
 * VL noise sample — Perlin warped by Perlin.
 * Three independent Perlin evaluations at offset coordinates produce an XYZ
 * displacement field, which warps the final Perlin evaluation. This is
 * Lewis's Variable Lacunarity Noise (VLNoise): the distortion creates
 * anisotropic, swirling features that standard Perlin cannot produce.
 *
 * \param distortion  Warp strength (typically 1.0).
 */
static float penny_vl_noise(
    float x, float y, float z, float w, float distortion, const int4 &period, int ch)
{
  /* Three Perlin samples at decorrelated coordinates for XYZ displacement. */
  float3 a = penny_perlin_4d_tile(x - 2.0f, y + 7.0f, z + 8.0f, w, period);
  float3 b = penny_perlin_4d_tile(x - 13.0f, y - 1.0f, z + 4.0f, w, period);
  float3 c = penny_perlin_4d_tile(x + 17.0f, y + 8.0f, z - 4.0f, w, period);
  /* Extract per-channel displacement and convert [0,1] → [-1,1]. */
  float ox = 1.0f - 2.0f * (((ch == 0) ? a.x : ((ch == 1) ? a.y : a.z)) * 0.5f + 0.5f);
  float oy = 1.0f - 2.0f * (((ch == 0) ? b.x : ((ch == 1) ? b.y : b.z)) * 0.5f + 0.5f);
  float oz = 1.0f - 2.0f * (((ch == 0) ? c.x : ((ch == 1) ? c.y : c.z)) * 0.5f + 0.5f);
  /* Final Perlin at warped position. */
  float3 tp = penny_perlin_4d_tile(
      x + ox * distortion, y + oy * distortion, z + oz * distortion, w, period);
  return ((ch == 0) ? tp.x : ((ch == 1) ? tp.y : tp.z)) * 0.5f + 0.5f;
}

/**
 * VL inner — fBm over variable-lacunarity noise.
 * Multi-octave sum of VL noise with standard fBm amplitude/frequency scaling.
 * Output mapped from [-1,1] to [0,1] via noxstep.
 */
static float penny_vl(
    float x, float y, float z, float w, float roughness, int octaves, const int4 &period, int ch)
{
  float the_value = 0.0f, scale = 1.0f, amp = 1.0f;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    float v = penny_vl_noise(
        x * scale + off, y * scale + off, z * scale + off, w, 1.0f, period, ch);
    the_value += (2.0f * v - 1.0f) * amp;
    scale *= 2.0f;
    amp *= roughness;
  }
  return darf_noxstep(-1.0f, 1.0f, the_value);
}

/** VL tile entry point. */
static float3 penny_vl_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float3 result(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = penny_vl(co.x + dch, co.y + dch, co.z + dch, co.w, roughness, octaves, period, ch);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    float r;
    if (p.blend_mode == 0) {
      r = v;
    }
    else {
      float v2 = penny_vl(co.x + dch + 0.5f,
                          co.y + dch + 0.5f,
                          co.z + dch + 0.5f,
                          co.w,
                          roughness,
                          octaves,
                          period,
                          ch);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      r = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
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

/* ---- Poxo ---- */

/**
 * Poxo abs-turbulence — coordinate-doubling running blend base.
 *
 * Unlike standard octave loops, this doubles the coordinate directly
 * (cx += cx) rather than scaling by a frequency multiplier. This creates a
 * self-similar pattern with tighter spatial correlation between octaves.
 * The amplitude is controlled by \a dampening (typically negative roughness),
 * producing a scale-sum normalized result.
 */
static float penny_poxo_absturb(
    float x, float y, float z, float w, int octaves, float dampening, const int4 &period, int ch)
{
  float scale = 1.0f, scale_sum = 0.0f;
  float3 s = penny_perlin_4d_tile(x, y, z, w, period);
  float value = math::abs(((ch == 0) ? s.x : ((ch == 1) ? s.y : s.z)) * 0.73f) * scale;
  float cx = x + x, cy = y + y, cz = z + z; /* coordinate doubling */
  scale_sum += scale;
  scale *= dampening;
  for (int i = 0; i < octaves; i++) {
    float3 si = penny_perlin_4d_tile(cx, cy, cz, w, period);
    value += math::abs(((ch == 0) ? si.x : ((ch == 1) ? si.y : si.z)) * 0.73f) * scale;
    cx += cx;
    cy += cy;
    cz += cz;
    scale_sum += scale;
    scale *= dampening;
  }
  return (scale_sum != 0.0f) ? (value / scale_sum) : 0.0f;
}

/**
 * Poxo inner — recursive abs-turb running-blend with fractional octaves.
 *
 * Each octave evaluates `penny_poxo_absturb` (4 inner octaves, negative
 * roughness as dampening), blends via |v - value| × 0.5 (running difference),
 * and doubles the spatial frequency. The final fractional octave is weighted
 * by (detail - floor(detail)).
 *
 * Post-processing: pow(0.7369656) → square × 2 → noxstep(0, 0.5) →
 * noxstep(0.01, 0.9) normalized by detail count.
 */
static float penny_poxo(
    float x, float y, float z, float w, float detail, float roughness, const int4 &period, int ch)
{
  float scalar = -roughness; /* Negative dampening for the inner abs-turb. */
  float size = 1.0f, value = 0.0f;
  int ioct = int(detail);
  for (int i = 0; i < ioct; i++) {
    float v = penny_poxo_absturb(x * size, y * size, z * size, w, 4, scalar, period, ch);
    value += math::abs(v - value) * 0.5f; /* Running difference blend. */
    size *= 2.0f;
  }
  /* Fractional last octave. */
  float v = penny_poxo_absturb(x * size, y * size, z * size, w, 4, scalar, period, ch);
  value += math::abs(v - value) * 0.5f * (detail - std::floor(detail));
  if (detail != 0.0f)
    value /= detail;
  /* Empirical contrast shaping chain. */
  value = math::clamp(std::pow(value, 0.7369656f), 0.0f, 1.0f);
  value = value * value * 2.0f;
  value = darf_noxstep(0.0f, 0.5f, value);
  value = darf_noxstep(0.01f, 0.9f, (detail != 0.0f) ? (value / detail) : value);
  return value;
}

/** Poxo tile entry point. Uses fractional detail. */
static float3 penny_poxo_tile(const float4 &co, const PennyParams &p)
{
  const float d = math::max(p.detail, 1.0f);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float3 result(0.0f, 0.0f, 0.0f);
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = penny_poxo(co.x + dch, co.y + dch, co.z + dch, co.w, d, roughness, period, ch);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    float r;
    if (p.blend_mode == 0) {
      r = v;
    }
    else {
      float v2 = penny_poxo(
          co.x + dch + 0.5f, co.y + dch + 0.5f, co.z + dch + 0.5f, co.w, d, roughness, period, ch);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      r = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
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

/* ---- Buya ---- */

/**
 * Clamped three-point levels (used by Buya's per-octave normalization table).
 * Same math as `penny_oct_levels` but with a clamped noxstep instead of
 * unclamped, preventing out-of-range values from the Buya pfBm accumulator
 * from propagating through the baked table.
 */
static float penny_levels(float x, float lo, float mid, float hi)
{
  float n = math::clamp((x - lo) / math::max(hi - lo, 1e-6f), 0.0f, 1.0f);
  float g = std::log(math::clamp(mid, 0.001f, 0.999f)) / std::log(0.5f);
  return std::pow(n, g);
}

/**
 * Buya pfBm channel — spectral-weighted fBm with weight-sum normalization.
 *
 * Uses H = roughness × 0.4 (inverted, matching the working shader) for
 * spectral weight computation: weight_i = 2^(-H × i). The weight-sum
 * normalization (`value / wsum`) is critical for Buya — without it, the raw
 * accumulation clips at high octave counts, producing flat-gray output.
 * This was the root cause of the Buya normalization bug fixed in session.
 *
 * The `* 0.5f + 0.5f` maps signed Perlin [-1,1] to [0,1] before the
 * `darf_noxstep(-1, 1)` re-centering, which produces the characteristic
 * Buya contrast curve.
 */
static float buya_pfbm_channel(
    float x, float y, float z, float w, int ch, int octaves, const int4 &period, float roughness)
{
  float value = 0.0f, scale = 1.0f, wsum = 0.0f;
  for (int i = 0; i < octaves; i++) {
    float off = (scale - 1.0f) * 1000.0f;
    int4 op(int(period.x * scale),
            int(period.y * scale),
            int(period.z * scale),
            int(period.w * scale));
    float3 pp = penny_perlin_4d_tile(x * scale + off, y * scale + off, z * scale + off, w, op);
    float pch[3];
    pch[0] = pp.x;
    pch[1] = pp.y;
    pch[2] = pp.z;
    float toy = pch[ch] * 0.5f + 0.5f;
    float h = (1.0f - roughness) * 0.4f; /* Inverted H (matches working shader). */
    float weight = std::pow(2.0f, -h * float(i));
    value += (2.0f * darf_noxstep(-1.0f, 1.0f, toy) - 1.0f) * weight;
    wsum += weight;
    scale *= 2.0f;
  }
  /* Weight-sum normalization: critical for preventing clipping at high octave counts. */
  return value / math::max(wsum, 1e-6f);
}

/**
 * Buya octave-aware baked Levels table.
 *
 * Per-octave-count lo/hi/mid parameters empirically tuned to normalize
 * the pfBm output range. Without this table, the pfBm output shifts its
 * effective range as octave count changes, producing inconsistent brightness.
 * The table entries were calibrated from measured percentile distributions
 * of the pfBm output at each octave count.
 *
 * Octave counts > 7 fall through to a generic (0.5, 0.5, 1.5) mapping.
 */
static float buya_octave_levels(int oc, float x)
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

/**
 * Buya tile entry point.
 *
 * Pipeline per channel:
 *   1. pfBm with spectral weights → weight-sum normalized.
 *   2. Fold via |1 - 2·pfBm·0.5|, bias(0.25), noxstep(0, 0.35).
 *   3. Per-octave baked levels normalization.
 *   4. User levels remap.
 *   5. Optional blend mode with +0.5 offset second sample.
 */
static float3 penny_buya_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    float pf = buya_pfbm_channel(co.x, co.y, co.z, co.w, ch, octaves, period, roughness);
    /* Fold → bias → noxstep shaping chain. */
    float v = darf_noxstep(0.1f, 0.5f, darf_bias(0.25f, math::abs(1.0f - 2.0f * pf * 0.5f)));
    v = buya_octave_levels(octaves, v);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[ch] = v;
    }
    else {
      float pf2 = buya_pfbm_channel(
          co.x + 0.5f, co.y + 0.5f, co.z + 0.5f, co.w, ch, octaves, period, roughness);
      float v2 = darf_noxstep(0.1f, 0.5f, darf_bias(0.25f, math::abs(1.0f - 2.0f * pf2 * 0.5f)));
      v2 = buya_octave_levels(octaves, v2);
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      res[ch] = penny_blend(p.blend_mode, v, v2);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/* ---- Displaced Turbulence (Disturb) ---- */

/**
 * Disturb percentile tables.
 *
 * Empirically measured median (p50) and 98th-percentile (p98) values of the
 * Disturb accumulator output at each octave count. Used by `disturb_window`
 * to normalize the output range. The piecewise-linear interpolation between
 * measured octave breakpoints (1, 4, 7, 10, 15) gives smooth transitions.
 *
 * These tables are the Displaced Turbulence equivalent of Buya's
 * `buya_octave_levels` — without them, the noise brightness and contrast
 * shift dramatically as octave count changes.
 */
static void disturb_pctl(int oc, float &p50, float &p98)
{
  if (oc <= 1) {
    p50 = 0.235f;
    p98 = 0.784f;
  }
  else if (oc < 4) {
    float t = float(oc - 1) / 3.0f;
    p50 = 0.235f + (1.176f - 0.235f) * t;
    p98 = 0.784f + (1.961f - 0.784f) * t;
  }
  else if (oc < 7) {
    float t = float(oc - 4) / 3.0f;
    p50 = 1.176f + (1.882f - 1.176f) * t;
    p98 = 1.961f + (2.980f - 1.961f) * t;
  }
  else if (oc < 10) {
    float t = float(oc - 7) / 3.0f;
    p50 = 1.882f + (2.510f - 1.882f) * t;
    p98 = 2.980f + (3.843f - 2.980f) * t;
  }
  else if (oc < 15) {
    float t = float(oc - 10) / 5.0f;
    p50 = 2.510f + (3.843f - 2.510f) * t;
    p98 = 3.843f + (5.098f - 3.843f) * t;
  }
  else {
    float e = float(oc - 15);
    p50 = 3.843f + 0.267f * e;
    p98 = 5.098f + 0.251f * e;
  }
}

/**
 * Disturb 2nd-percentile table.
 * Piecewise-linear estimate of the floor (2nd percentile) of the accumulator
 * at each octave count, used to set the low end of the output window.
 */
static float disturb_p02(int oc)
{
  if (oc <= 1) {
    return 0.000f;
  }
  else if (oc < 4) {
    return 0.000f + (0.314f - 0.000f) * (float(oc - 1) / 3.0f);
  }
  else if (oc < 7) {
    return 0.314f + (0.784f - 0.314f) * (float(oc - 4) / 3.0f);
  }
  else if (oc < 10) {
    return 0.784f + (1.412f - 0.784f) * (float(oc - 7) / 3.0f);
  }
  else if (oc < 15) {
    return 1.412f + (2.353f - 1.412f) * (float(oc - 10) / 5.0f);
  }
  else {
    return 2.353f + 0.188f * float(oc - 15);
  }
}

/**
 * Disturb output window — normalizes the accumulator into [0, ~1].
 *
 * Uses the percentile tables to compute a window [lo, hi] that maps the
 * median to TMID (0.313) and the 98th percentile to THI (0.537). The low
 * end is further adjusted by the 2nd-percentile floor with a margin.
 * A quadratic lift term `LIFT × (1-t)²` prevents full black.
 *
 * Constants: TMID=0.313, THI=0.537, FLOORMARGIN=0.35, LIFT=0.2.
 */
static float disturb_window(int oc, float sum)
{
  const float TMID = 0.313f, THI = 0.537f, FLOORMARGIN = 0.35f, LIFT = 0.2f;
  float p50, p98;
  disturb_pctl(oc, p50, p98);
  float span = (p98 - p50) / math::max(THI - TMID, 1e-4f);
  float lo = p50 - TMID * span;
  float hi = lo + span;
  lo = math::min(lo, disturb_p02(oc) - FLOORMARGIN);
  float t = darf_noxstep(lo, hi, sum);
  return t + LIFT * (1.0f - t) * (1.0f - t); /* Quadratic lift to prevent full black. */
}

/**
 * Disturb accumulator — x-shifted displacement-warped abs-turbulence.
 *
 * Per octave:
 *   1. Evaluate 2-octave abs-turbulence for the displacement field.
 *   2. Compute displacement: `1 - 2 × turb × 0.8`.
 *   3. Evaluate 1-octave abs-turbulence at displaced position.
 *   4. Accumulate with roughness×2 amplitude ramp.
 *   5. X-coordinate steps by +5 each octave for decorrelation.
 *
 * The amplitude-sum normalization and `× octaves` re-scaling produce a
 * consistent output range regardless of octave count, which is then
 * windowed by `disturb_window` for final normalization.
 *
 * \note Requires `penny_abs_turb2` which matches `penny.h`'s `penny_abs_turb2`.
 */
static float disturb_accum(float bx,
                           float by,
                           float bz,
                           float w,
                           const int4 &period,
                           int octaves,
                           float roughness,
                           int ch)
{
  float ramp = roughness * 2.0f;
  float x1 = bx, sum = 0.0f, amp = 1.0f, ampsum = 0.0f;
  for (int i = 0; i < octaves; i++) {
    /* Per-octave Y/Z decorrelation: prime-ish offsets break lattice alignment
     * across octaves, eliminating the grid artifact caused by constant by/bz. */
    float oy = float(i) * 7.793f;
    float oz = float(i) * 11.219f;
    /* 2-octave abs-turb for displacement field. */
    float3 d = penny_abs_turb2(float4(x1, by + oy, bz + oz, w), period, 2, 0.5f);
    float dch[3];
    dch[0] = d.x;
    dch[1] = d.y;
    dch[2] = d.z;
    float displace = 1.0f - 2.0f * dch[ch] * 0.8f;
    /* 1-octave abs-turb at displaced position. */
    float3 tt = penny_abs_turb2(
        float4(x1 - displace, by + displace + oy, bz + displace + oz, w), period, 1, 0.5f);
    float tch[3];
    tch[0] = tt.x;
    tch[1] = tt.y;
    tch[2] = tt.z;
    sum += tch[ch] * amp;
    ampsum += amp;
    amp *= ramp;
    x1 += 5.0f; /* X-step for octave decorrelation. */
  }
  /* Normalize and re-scale by octave count, then window. */
  float norm_sum = (ampsum > 0.0f) ? (sum / ampsum) * float(octaves) : sum;
  return disturb_window(octaves, norm_sum);
}

/**
 * Displaced Turbulence tile entry point.
 * Turbulence with an internal domain-warp displacement pass, producing a
 * more complex and organic pattern than standard Turbulence.
 */
static float3 penny_disturb_tile(const float4 &co, const PennyParams &p)
{
  const int octaves = math::max(int(p.detail), 1);
  const int4 period = p.period;
  const float roughness = p.roughness;
  const float3 lvl = p.levels;
  float res[3];
  for (int ch = 0; ch < 3; ch++) {
    float dch = float(ch) * 1000.0f;
    float v = disturb_accum(
        co.x + dch, co.y + dch, co.z + dch, co.w, period, octaves, roughness, ch);
    v = penny_oct_levels(v, lvl.x, lvl.y, lvl.z);
    if (p.blend_mode == 0) {
      res[ch] = v;
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
      v2 = penny_oct_levels(v2, lvl.x, lvl.y, lvl.z);
      res[ch] = math::clamp(
          penny_blend(p.blend_mode, math::clamp(v, 0.0f, 1.0f), math::clamp(v2, 0.0f, 1.0f)),
          0.0f,
          1.0f);
    }
  }
  return float3(res[0], res[1], res[2]);
}

/* -------------------------------------------------------------------- */
/** \name Public entry point
 *
 * Single dispatch function that routes to the correct noise type based on
 * `PennyParams::type`. This is called by:
 *   - The Geometry Nodes multi-function (`PennyFunction::call`) in
 *     `node_shader_tex_penny.cc`.
 *   - The F-Curve modifier evaluation in the Graph Editor.
 *
 * All 19 noise types are evaluated through their respective `penny_*_tile()`
 * functions, which share the common pipeline described in the file header.
 *
 * \param position  4D coordinate (XYZ spatial + W for 4th dimension / animation).
 * \param params    Noise type, octaves, roughness, levels, blend mode, period.
 * \return float3 with RGB noise values, typically in [0, 1] per channel.
 * \{ */

float3 penny(const float4 &position, const PennyParams &params)
{
  switch (PennyType(params.type)) {
    case PennyType::fBm:
      return penny_fbm_tile(position, params);
    case PennyType::Turbulence:
      return penny_turbulence_tile(position, params);
    case PennyType::Stupl:
      return penny_stupl_tile(position, params);
    case PennyType::Blistered:
      return penny_blistered_tile(position, params);
    case PennyType::Nutous:
      return penny_nutous_tile(position, params);
    case PennyType::Naki:
      return penny_naki_tile(position, params);
    case PennyType::Luka:
      return penny_luka_tile(position, params);
    case PennyType::Hama:
      return penny_hama_tile(position, params);
    case PennyType::Dents:
      return penny_dents_tile(position, params);
    case PennyType::Sema:
      return penny_sema_tile(position, params);
    case PennyType::Pezo:
      return penny_pezo_tile(position, params);
    case PennyType::VL:
      return penny_vl_tile(position, params);
    case PennyType::Poxo:
      return penny_poxo_tile(position, params);
    case PennyType::Buya:
      return penny_buya_tile(position, params);
    case PennyType::DisplacedTurbulence:
      return penny_disturb_tile(position, params);
    case PennyType::Box:
      return penny_box_tile(position, params);
    case PennyType::Mod:
      return penny_mod_tile(position, params);
    case PennyType::Cranal:
      return penny_cranal_tile(position, params);
    case PennyType::Cell:
      return penny_cell_tile(position, params);
    default:
      return float3(0.5f, 0.5f, 0.5f);
  }
}

/** \} */

}  // namespace blender::noise::penny
