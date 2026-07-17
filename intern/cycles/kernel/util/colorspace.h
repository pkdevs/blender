/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"
#include "kernel/tables.h"

#include "util/types_spectrum.h"

CCL_NAMESPACE_BEGIN

ccl_device float3 xyz_to_rgb(KernelGlobals kg, const float3 xyz)
{
  return make_float3(dot(make_float3(kernel_data.film.xyz_to_r), xyz),
                     dot(make_float3(kernel_data.film.xyz_to_g), xyz),
                     dot(make_float3(kernel_data.film.xyz_to_b), xyz));
}

ccl_device float3 xyz_to_rgb_clamped(KernelGlobals kg, const float3 xyz)
{
  return max(xyz_to_rgb(kg, xyz), zero_float3());
}

ccl_device float3 rec709_to_rgb(KernelGlobals kg, const float3 rec709)
{
  return (kernel_data.film.is_rec709) ?
             rec709 :
             make_float3(dot(make_float3(kernel_data.film.rec709_to_r), rec709),
                         dot(make_float3(kernel_data.film.rec709_to_g), rec709),
                         dot(make_float3(kernel_data.film.rec709_to_b), rec709));
}

template<class T> ccl_device auto linear_rgb_to_gray(KernelGlobals kg, const T c)
{
  return dot(c, make_float3(kernel_data.film.rgb_to_y));
}

ccl_device_inline Spectrum rgb_to_spectrum(const float3 rgb)
{
  return rgb;
}

ccl_device_inline float3 spectrum_to_rgb(Spectrum s)
{
  return s;
}

ccl_device float spectrum_to_gray(KernelGlobals kg, Spectrum c)
{
  return linear_rgb_to_gray(kg, spectrum_to_rgb(c));
}

#ifdef __SPECTRAL__
/* Given a wavelength, convert it to rgb in the working color space, assuming D65 illuminant.
 * [Metameric: Spectral Uplifting via Controllable Color Constraints]
 * (https://markvanderuit.nl/files/2023-07-23-paper-metameric/metameric-paper.pdf)
 * by Mark van de Ruit and Elmar Eisemann, Eq. (1). */
ccl_device_inline float3 wavelength_to_rgb_d65(KernelGlobals kg, const float lambda_um)
{
  int i;
  const float f = floorfrac((lambda_um - WAVELENGTH_CIE_MIN) / WAVELENGTH_DLAMBDA, &i);

  if (i < 0 || i >= WAVELENGTH_RESOLUTION) {
    return zero_float3();
  }

  ccl_constant float *c = cie_color_match[i];
  float3 xyz = mix(make_float3(c[0], c[1], c[2]), make_float3(c[3], c[4], c[5]), f);
  xyz *= mix(cie_d65_spd[i], cie_d65_spd[i + 1], f);

  return xyz_to_rgb(kg, xyz * CIE_D65_NORMALIZATION);
}

/* Sample wavelength in um.
 * See `intern/cycles/app/cie_d65_luminance_fit.py` for the fitting. */
ccl_device_inline float sample_wavelength(float rand, ccl_private float *prob = nullptr)
{
  constexpr float a = 21.71348444564851f;
  constexpr float x0 = 0.5554867905834258f;
  constexpr float y0 = 0.021659159132699574f;
  constexpr float N = 0.9707633294863183f;

  rand = N * rand + y0;

  if (prob) {
    /* The derivative of F(x) = 1/(1+e^-(a(x-x0))) is F'(x) = a * F(x) * (1 - F(x)). */
    *prob = a * rand * (1.0f - rand);
    /* Normalization */
    *prob *= (WAVELENGTH_CIE_MAX - WAVELENGTH_CIE_MIN) / N;
  }

  const float wavelength = -fast_logf(1.0f / rand - 1.0f) / a + x0;
  return clamp(wavelength, WAVELENGTH_CIE_MIN, WAVELENGTH_CIE_MAX);
}

ccl_device_inline bool rgb2spec_fetch_mono(float3 rgb, ccl_private float3 &out)
{
  if (rgb.x != rgb.y || rgb.y != rgb.z) {
    return false;
  }

  // Black/white map to +/- 8192, which evaluate to 0/1 in single precision.
  // (we cannot use infinity as this would produce NaNs)
  const float v = rgb.x;
  float r;
  if (v <= 0.f) {
    r = -8192.f;
  }
  else if (v >= 1.f) {
    r = 8192.f;
  }
  else {
    r = (v - 0.5f) / sqrtf(v * (1.f - v));
  }

  out.x = out.y = 0.f;
  out.z = r;
  return true;
}

ccl_device_inline int rgb2spec_find_interval(int size_, float x)
{
  int left = 0, last_interval = size_ - 2, size = last_interval;

  while (size > 0) {
    int half = size >> 1, middle = left + half + 1;

    if (uplift_scale[middle] <= x) {
      left = middle;
      size -= half + 1;
    }
    else {
      size = half;
    }
  }

  return min(left, last_interval);
}

/// Convert an RGB value into a RGB2Spec coefficient representation
ccl_device float3 rgb2spec_fetch(float3 rgb)
{
  // Determine largest RGB component
  int i = 0;
  constexpr int res = 32;
  rgb = saturate(rgb);

  float3 out;
  // Use a closed form solution for monochromatic inputs
  if (rgb2spec_fetch_mono(rgb, out)) {
    return out;
  }

  for (int j = 1; j < 3; ++j) {
    if (rgb[j] >= rgb[i]) {
      i = j;
    }
  }

  float z = rgb[i], scale, x, y;

  if (z <= 0.f) {
    for (int j = 0; j < 3; ++j) {
      out[j] = uplift_data[j];
    }
    return out;
  }

  scale = (res - 1) / z;
  x = rgb[(i + 1) % 3] * scale;
  y = rgb[(i + 2) % 3] * scale;

  // Trilinearly interpolated lookup
  uint32_t xi = min((uint32_t)x, (uint32_t)(res - 2)), yi = min((uint32_t)y, (uint32_t)(res - 2)),
           zi = rgb2spec_find_interval(res, z),
           offset = (((i * res + zi) * res + yi) * res + xi) * 3, dx = 3, dy = 3 * res,
           dz = 3 * res * res;

  float x1 = x - xi, x0 = 1.f - x1, y1 = y - yi, y0 = 1.f - y1,
        z1 = (z - uplift_scale[zi]) / (uplift_scale[zi + 1] - uplift_scale[zi]), z0 = 1.f - z1;

  for (int j = 0; j < 3; ++j) {
    out[j] = ((uplift_data[offset] * x0 + uplift_data[offset + dx] * x1) * y0 +
              (uplift_data[offset + dy] * x0 + uplift_data[offset + dy + dx] * x1) * y1) *
                 z0 +
             ((uplift_data[offset + dz] * x0 + uplift_data[offset + dz + dx] * x1) * y0 +
              (uplift_data[offset + dz + dy] * x0 + uplift_data[offset + dz + dy + dx] * x1) *
                  y1) *
                 z1;
    offset++;
  }

  return out;
}

ccl_device_inline float sigmoid(float x)
{
  return 0.5f * x / sqrt(1.0f + sqr(x)) + 0.5f;
}

ccl_device_inline float S(float3 coeff, float wavelength)
{
  return sigmoid(dot(coeff, make_float3(sqr(wavelength), wavelength, 1.0f)));
}

/* Return equi-distant wavelengths in the range of [WAVELENGTH_CIE_MIN, WAVELENGTH_CIE_MAX]. */
ccl_device_inline float3 sample_wavelengths(const float rand)
{
  /* The first component is hero wavelength. */
  float3 offsets = make_float3(0.0f, 0.1f, 0.9f) + rand;
  /* Warp to [0, 1) */
  offsets = select(offsets >= one_float3(), offsets - 1.0f, offsets);
  return mix(WAVELENGTH_CIE_MIN, WAVELENGTH_CIE_MAX, offsets);
}

ccl_device_inline float3 uplift(const float3 rgb, const float rand)
{
  const float3 wavelength = sample_wavelengths(rand) * 1000.0f;
  const float3 coeff = rgb2spec_fetch(rgb);
  return make_float3(S(coeff, wavelength.x), S(coeff, wavelength.y), S(coeff, wavelength.z));
}

ccl_device_inline float3 uplift(const float3 rgb, const float3 wavelengths_um)
{
  const float3 coeff = rgb2spec_fetch(rgb);
  return make_float3(
      S(coeff, wavelengths_um.x), S(coeff, wavelengths_um.y), S(coeff, wavelengths_um.z));
}
#endif

CCL_NAMESPACE_END
