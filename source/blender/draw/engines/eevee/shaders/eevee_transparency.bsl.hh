/* SPDX-FileCopyrightText: 2017-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * Surface-stable stochastic transparency implementation. Multiple underlying sampling hashes can
 * be used.
 *
 * References:
 *
 *  [wymn2017]  Chris Wyman and Morgan McGuire
 *              Hashed Alpha Testing
 *              ACM I3D 2017
 *              https://research.nvidia.com/labs/rtr/publication/wyman2017hashed/
 */

#pragma once

#include "eevee_sampling_lib.bsl.hh"
#include "gpu_shader_compat.hh"

namespace eevee::hashed_transparency {

/* For the underlying hashes, refer to `eevee_sampling_lib.bsl.hh`. */
enum class HashType : uchar {
  /* Sine hash used in the original implementation. Deprecated. */
  Sine,
  /* PCG hash, less artifact-ey on imprecise world-space inputs. */
  PCG
};

/**
 * Wrapper for a scaled, hashed alpha threshold used below, with a selectable underlying hash.
 */
template<HashType Hash> float2 alpha_hash(float2 /* pixel_scale */, float3 /* P */)
{
  return float2(0.0f);
}
template<> float2 alpha_hash<HashType::Sine>(float2 pixel_scale, float3 P)
{
  float2 alpha;
  alpha.x = random::sine(floor(pixel_scale.x * P));
  alpha.y = random::sine(floor(pixel_scale.y * P));
  return alpha;
}
template<> float2 alpha_hash<HashType::PCG>(float2 pixel_scale, float3 P)
{
  float2 alpha;
  alpha.x = random::pcg(floor(pixel_scale.x * P));
  alpha.y = random::pcg(floor(pixel_scale.y * P));
  return alpha;
}

/**
 * Surface-stable alpha testing [wymn2017].
 */
template<HashType Hash> float alpha_threshold(float hash_scale, float hash_offset, float3 P)
{
  /* Find the discretized derivatives of our coordinates. */
  float max_deriv = max(length(gpu_dfdx(P)), length(gpu_dfdy(P)));
  float pix_scale = 1.0f / (hash_scale * max_deriv);
  /* Find two nearest log-discretized noise scales. */
  float pix_scale_log = log2(pix_scale);
  float2 pix_scales;
  pix_scales.x = exp2(floor(pix_scale_log));
  pix_scales.y = exp2(ceil(pix_scale_log));
  /* Compute alpha thresholds at our two noise scales. */
  float2 alpha = alpha_hash<Hash>(pix_scales, P);
  /* Factor to interpolate lerp with. */
  float fac = fract(log2(pix_scale));
  /* Interpolate alpha threshold from noise at two scales. */
  float x = mix(alpha.x, alpha.y, fac);
  /* Pass into CDF to compute uniformly distributed threshold. */
  float a = min(fac, 1.0f - fac);
  float one_a = 1.0f - a;
  float denom = 1.0f / (2 * a * one_a);
  float one_x = (1 - x);
  float3 cases = float3((x * x) * denom, (x - 0.5f * a) / one_a, 1.0f - (one_x * one_x * denom));
  /* Find our final, uniformly distributed alpha threshold. */
  float threshold = (x < one_a) ? ((x < a) ? cases.x : cases.y) : cases.z;
  /* Jitter the threshold for TAA accumulation. */
  threshold = fract(threshold + hash_offset);
  /* Avoids threshold == 0. */
  threshold = clamp(threshold, 1.0e-6f, 1.0f);
  return threshold;
}
template float alpha_threshold<HashType::Sine>(float, float, float3);
template float alpha_threshold<HashType::PCG>(float, float, float3);

}  // namespace eevee::hashed_transparency
