/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

enum Sampler : uchar { Box, Bspline };

template<enum Sampler sampler> static inline float weight(float x)
{
  return x;
}

/* Match function in BLI_math_interp.h */
float2 fast_hypot(const float2x2 &jacobian)
{
  const float4 aj = abs(float4(jacobian[0], jacobian[1]));
  const float a = aj[0];
  const float b = aj[2];
  const float c = aj[1];
  const float d = aj[3];
  return float2(a < b ? b + 0.375f * a : a + 0.375f * b, c < d ? d + 0.375f * c : c + 0.375f * d);
}

/* Sample orthogonal rectangle of size wh centered on uv.
 * This is intended to work with any filter function.
 * This does not integrate the filter function with the pixel, instead it assumes the value
 * at the center of the pixel is correct. This results in a small sharpening effect that looks
 * better, and is also simpler to calculate.
 * This differs from the C implementation in math_interp as it relies on texture() to do the
 * wrapping and bilinear sample 4 pixels at a time.
 * todo: using bilinear sampling does not work if one of the samples is negative
 * todo: fix for filters with radius != 2
 */
template<enum Sampler sampler>
float4 sample_rect(const sampler2D &source, const float2 &uv, const float2 &wh)
{
  const float2 w1 = max(wh, 1.0f);
  const float2 r = 2 * w1;
  const float2 a = floor(uv - r + 1.0f);
  const float2 d = ceil(r / 16.0f);
  const float2 scale = 1.0f / float2(textureSize(source, 0));
  /* precompute the horizontal filter so it can be reused */
  float2 xfilter[33];  /* pairs of u,weight */
  float divx = 0.0f;
  int nx = 0;
  for (float x = a.x - uv.x; x < r.x; x += 2 * d.x) {
    float wt = weight<sampler>(abs(x / w1.x));
    float x2 = x + 1.0f;
    float wt2 = x2 < r.x ? weight<sampler>(abs(x2 / w1.x)) : 0.0f;
    wt += wt2;
    x2 = (x + uv.x + wt2 / wt + 0.5f) * scale.x;
    xfilter[nx++] = float2(x2, wt);
    divx += wt;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float y = a.y - uv.y; y < r.y; y += 2 * d.y) {
    float wt = weight<sampler>(abs(y / w1.y));
    float y2 = y + 1.0f;
    float wt2 = y2 < r.y ? weight<sampler>(abs(y2 / w1.y)) : 0.0f;
    wt += wt2;
    y2 = (y + uv.y + wt2 / wt + 0.5f) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(source, float2(xfilter[j].x, y2)) * xfilter[j].y;
    }
    sum += sumx * wt;
    div += wt;
  }
  return sum / (div * divx);
}

/* specialized as r is smaller and weight function needs to know size of a pixel */
template<>
float4 sample_rect<Sampler::Box>(const sampler2D &source, const float2 &uv, const float2 &wh)
{
  const float2 r = max((wh + 1) / 2.0f, 1.0f);
  const float2 a = floor(uv - r + 1.0f);
  const float2 d = ceil(r / 8.0f);
  const float2 scale = 1.0f / float2(textureSize(source, 0));
  /* precompute the horizontal filter so it can be reused */
  float2 xfilter[33];
  float divx = 0.0f;
  int nx = 0;
  for (float x = a.x - uv.x; x < r.x; x += 2 * d.x) {
    float wt = min(r.x - abs(x), 1.0f);
    float x2 = x + 1.0f;
    float wt2 = clamp(r.x - abs(x2), 0.0f, 1.0f);
    wt += wt2;
    x2 = (x + uv.x + wt2 / wt + 0.5f) * scale.x;
    xfilter[nx++] = float2(x2, wt);
    divx += wt;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float y = a.y - uv.y; y < r.y; y += 2 * d.y) {
    float wt = min(r.y - abs(y), 1.0f);
    float y2 = y + 1.0f;
    float wt2 = clamp(r.y - abs(y2), 0.0f, 1.0f);
    wt += wt2;
    y2 = (y + uv.y + wt2 / wt + 0.5f) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(source, float2(xfilter[j].x, y2)) * xfilter[j].y;
    }
    sum += sumx * wt;
    div += wt;
  }
  return sum / (div * divx);
}

template<> float weight<Sampler::Bspline>(float x)
{
  return x < 1.0f ? (0.5f * x - 1.0f) * x * x + 4.0f / 6.0f :
                    ((-1.0f / 6.0f * x + 1.0f) * x - 2.0f) * x + 4.0f / 3.0f;
}
template float4 sample_rect<Sampler::Bspline>(const sampler2D &source,
                                              const float2 &uv,
                                              const float2 &wh);
