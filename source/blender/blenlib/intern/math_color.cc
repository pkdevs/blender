/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_math_color.hh"
#include "BLI_math_color_c.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_simd.hh"
#include "BLI_utildefines.hh"

#include <algorithm>
#include <cstring>

#include "BLI_strict_flags.hh" /* IWYU pragma: keep. Keep last. */

namespace blender {

void hsv_to_rgb(float h, float s, float v, float *r_r, float *r_g, float *r_b)
{
  float nr, ng, nb;

  nr = fabsf(h * 6.0f - 3.0f) - 1.0f;
  ng = 2.0f - fabsf(h * 6.0f - 2.0f);
  nb = 2.0f - fabsf(h * 6.0f - 4.0f);

  CLAMP(nr, 0.0f, 1.0f);
  CLAMP(nb, 0.0f, 1.0f);
  CLAMP(ng, 0.0f, 1.0f);

  *r_r = ((nr - 1.0f) * s + 1.0f) * v;
  *r_g = ((ng - 1.0f) * s + 1.0f) * v;
  *r_b = ((nb - 1.0f) * s + 1.0f) * v;
}

void hsl_to_rgb(float h, float s, float l, float *r_r, float *r_g, float *r_b)
{
  float nr, ng, nb, chroma;

  nr = fabsf(h * 6.0f - 3.0f) - 1.0f;
  ng = 2.0f - fabsf(h * 6.0f - 2.0f);
  nb = 2.0f - fabsf(h * 6.0f - 4.0f);

  CLAMP(nr, 0.0f, 1.0f);
  CLAMP(nb, 0.0f, 1.0f);
  CLAMP(ng, 0.0f, 1.0f);

  chroma = (1.0f - fabsf(2.0f * l - 1.0f)) * s;

  *r_r = (nr - 0.5f) * chroma + l;
  *r_g = (ng - 0.5f) * chroma + l;
  *r_b = (nb - 0.5f) * chroma + l;
}

void hsv_to_rgb_v(const float hsv[3], float r_rgb[3])
{
  hsv_to_rgb(hsv[0], hsv[1], hsv[2], &r_rgb[0], &r_rgb[1], &r_rgb[2]);
}

void hsl_to_rgb_v(const float hsl[3], float r_rgb[3])
{
  hsl_to_rgb(hsl[0], hsl[1], hsl[2], &r_rgb[0], &r_rgb[1], &r_rgb[2]);
}

void oklab_to_rgb(float l, float a, float b, float *r_r, float *r_g, float *r_b)
{
  float lo = l + 0.3963377774f * a + 0.2158037573f * b;
  float me = l - 0.1055613458f * a - 0.0638541728f * b;
  float sh = l - 0.0894841775f * a - 1.2914855480f * b;

  lo = lo * lo * lo;
  me = me * me * me;
  sh = sh * sh * sh;

  *r_r = 4.0767416621f * lo - 3.3077115913f * me + 0.2309699292f * sh;
  *r_g = -1.2684380046f * lo + 2.6097574011f * me - 0.3413193965f * sh;
  *r_b = -0.0041960863f * lo - 0.7034186147f * me + 1.7076147010f * sh;
}

void oklch_to_rgb(float l, float c, float h, float *r_r, float *r_g, float *r_b)
{
  float a = c * cos(h * 6.2831853072f);
  float b = c * sin(h * 6.2831853072f);
  oklab_to_rgb(l, a, b, r_r, r_g, r_b);
}

static float oklab_compute_max_saturation(float a, float b)
{
  /* Max saturation will be when one of r, g or b goes below zero. */

  /* Select different coefficients depending on which component goes below zero first. */
  float k0, k1, k2, k3, k4, wl, wm, ws;

  if (-1.88170328f * a - 0.80936493f * b > 1) {
    /* Red component. */
    k0 = +1.19086277f;
    k1 = +1.76576728f;
    k2 = +0.59662641f;
    k3 = +0.75515197f;
    k4 = +0.56771245f;
    wl = +4.0767416621f;
    wm = -3.3077115913f;
    ws = +0.2309699292f;
  }
  else if (1.81444104f * a - 1.19445276f * b > 1) {
    /* Green component. */
    k0 = +0.73956515f;
    k1 = -0.45954404f;
    k2 = +0.08285427f;
    k3 = +0.12541070f;
    k4 = +0.14503204f;
    wl = -1.2684380046f;
    wm = +2.6097574011f;
    ws = -0.3413193965f;
  }
  else {
    /* Blue component. */
    k0 = +1.35733652f;
    k1 = -0.00915799f;
    k2 = -1.15130210f;
    k3 = -0.50559606f;
    k4 = +0.00692167f;
    wl = -0.0041960863f;
    wm = -0.7034186147f;
    ws = +1.7076147010f;
  }

  /* Approximate max saturation using a polynomial. */
  float S = k0 + k1 * a + k2 * b + k3 * a * a + k4 * a * b;

  /* Do one step Halley's method to get closer.
   * this gives an error less than 10e6, except for some blue hues where the dS/dh is close to
   * infinite this should be sufficient for most applications, otherwise do two/three steps. */
  float k_l = +0.3963377774f * a + 0.2158037573f * b;
  float k_m = -0.1055613458f * a - 0.0638541728f * b;
  float k_s = -0.0894841775f * a - 1.2914855480f * b;

  {
    float l_ = 1.0f + S * k_l;
    float m_ = 1.0f + S * k_m;
    float s_ = 1.0f + S * k_s;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    float l_dS = 3.0f * k_l * l_ * l_;
    float m_dS = 3.0f * k_m * m_ * m_;
    float s_dS = 3.0f * k_s * s_ * s_;

    float l_dS2 = 6.0f * k_l * k_l * l_;
    float m_dS2 = 6.0f * k_m * k_m * m_;
    float s_dS2 = 6.0f * k_s * k_s * s_;

    float f = wl * l + wm * m + ws * s;
    float f1 = wl * l_dS + wm * m_dS + ws * s_dS;
    float f2 = wl * l_dS2 + wm * m_dS2 + ws * s_dS2;

    S = S - f * f1 / (f1 * f1 - 0.5f * f * f2);
  }

  return S;
}

static void oklab_find_cusp(float a, float b, float *r_l, float *r_c)
{
  float s_cusp = oklab_compute_max_saturation(a, b);
  float r_at_max, g_at_max, b_at_max;
  oklab_to_rgb(1, s_cusp * a, s_cusp * b, &r_at_max, &g_at_max, &b_at_max);
  *r_l = cbrtf(1.0f / fmaxf(fmaxf(r_at_max, g_at_max), b_at_max));
  *r_c = *r_l * s_cusp;
}

static float oklab_toe(float x)
{
  constexpr float k_1 = 0.206f;
  constexpr float k_2 = 0.03f;
  constexpr float k_3 = (1.0f + k_1) / (1.0f + k_2);
  return 0.5f * (k_3 * x - k_1 + sqrtf((k_3 * x - k_1) * (k_3 * x - k_1) + 4 * k_2 * k_3 * x));
}

static float oklab_toe_inverse(float x)
{
  constexpr float k_1 = 0.206f;
  constexpr float k_2 = 0.03f;
  constexpr float k_3 = (1.0f + k_1) / (1.0f + k_2);
  return (x * x + k_1 * x) / (k_3 * (x + k_2));
}

void okhsv_to_rgb(float h, float s, float v, float *r_r, float *r_g, float *r_b)
{
  float a_ = cosf(2.0f * 3.1415926536f * h);
  float b_ = sinf(2.0f * 3.1415926536f * h);

  float l_cusp, c_cusp;
  oklab_find_cusp(a_, b_, &l_cusp, &c_cusp);
  float s_max = c_cusp / l_cusp;
  float t_max = c_cusp / (1 - l_cusp);
  float s_0 = 0.5f;
  float k = 1 - s_0 / s_max;

  float l_v = 1 - s * s_0 / (s_0 + t_max - t_max * k * s);
  float c_v = s * t_max * s_0 / (s_0 + t_max - t_max * k * s);

  float l = v * l_v;
  float c = v * c_v;

  float l_vt = oklab_toe_inverse(l_v);
  float c_vt = c_v * l_vt / l_v;

  float l_new = oklab_toe_inverse(l);
  c = c * l_new / l;
  l = l_new;

  float r_scale, g_scale, b_scale;
  oklab_to_rgb(l_vt, a_ * c_vt, b_ * c_vt, &r_scale, &g_scale, &b_scale);
  float scale_l = cbrtf(1.0f / fmax(fmax(r_scale, g_scale), fmax(b_scale, 0.0f)));

  l = l * scale_l;
  c = c * scale_l;

  oklab_to_rgb(l, c * a_, c * b_, r_r, r_g, r_b);
}

static float oklab_find_gamut_intersection(
    float a, float b, float L1, float C1, float L0, float l_cusp, float c_cusp)
{
  float t;
  if (((L1 - L0) * c_cusp - (l_cusp - L0) * C1) <= 0.0f) {
    t = c_cusp * L0 / (C1 * l_cusp + c_cusp * (L0 - L1));
  }
  else {
    t = c_cusp * (L0 - 1.0f) / (C1 * (l_cusp - 1.0f) + c_cusp * (L0 - L1));

    float dL = L1 - L0;
    float dC = C1;

    float k_l = +0.3963377774f * a + 0.2158037573f * b;
    float k_m = -0.1055613458f * a - 0.0638541728f * b;
    float k_s = -0.0894841775f * a - 1.2914855480f * b;

    float l_dt = dL + dC * k_l;
    float m_dt = dL + dC * k_m;
    float s_dt = dL + dC * k_s;

    /* If higher accuracy is required, 2 or 3 iterations of the following block can be used. */
    {
      float L = L0 * (1.0f - t) + t * L1;
      float C = t * C1;

      float l_ = L + C * k_l;
      float m_ = L + C * k_m;
      float s_ = L + C * k_s;

      float l = l_ * l_ * l_;
      float m = m_ * m_ * m_;
      float s = s_ * s_ * s_;

      float ldt = 3 * l_dt * l_ * l_;
      float mdt = 3 * m_dt * m_ * m_;
      float sdt = 3 * s_dt * s_ * s_;

      float ldt2 = 6 * l_dt * l_dt * l_;
      float mdt2 = 6 * m_dt * m_dt * m_;
      float sdt2 = 6 * s_dt * s_dt * s_;

      float r_0 = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s - 1;
      float r_1 = 4.0767416621f * ldt - 3.3077115913f * mdt + 0.2309699292f * sdt;
      float r_2 = 4.0767416621f * ldt2 - 3.3077115913f * mdt2 + 0.2309699292f * sdt2;

      float u_r = r_1 / (r_1 * r_1 - 0.5f * r_0 * r_2);
      float t_r = -r_0 * u_r;

      float g_0 = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s - 1;
      float g_1 = -1.2684380046f * ldt + 2.6097574011f * mdt - 0.3413193965f * sdt;
      float g_2 = -1.2684380046f * ldt2 + 2.6097574011f * mdt2 - 0.3413193965f * sdt2;

      float u_g = g_1 / (g_1 * g_1 - 0.5f * g_0 * g_2);
      float t_g = -g_0 * u_g;

      float b_0 = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s - 1;
      float b_1 = -0.0041960863f * ldt - 0.7034186147f * mdt + 1.7076147010f * sdt;
      float b_2 = -0.0041960863f * ldt2 - 0.7034186147f * mdt2 + 1.7076147010f * sdt2;

      float u_b = b_1 / (b_1 * b_1 - 0.5f * b_0 * b_2);
      float t_b = -b_0 * u_b;

      t_r = u_r >= 0.0f ? t_r : FLT_MAX;
      t_g = u_g >= 0.0f ? t_g : FLT_MAX;
      t_b = u_b >= 0.0f ? t_b : FLT_MAX;

      t += fminf(t_r, fminf(t_g, t_b));
    }
  }

  return t;
}

static void oklab_get_st_mid(float a_, float b_, float *s_mid, float *t_mid)
{
  *s_mid = 0.11516993f +
           1.0f / (+7.44778970f + 4.15901240f * b_ +
                  a_ * (-2.19557347f + 1.75198401f * b_ +
                        a_ * (-2.13704948f - 10.02301043f * b_ +
                              a_ * (-4.24894561f + 5.38770819f * b_ + 4.69891013f * a_))));
  *t_mid = 0.11239642f +
           1.0f / (+1.61320320f - 0.68124379f * b_ +
                  a_ * (+0.40370612f + 0.90148123f * b_ +
                        a_ * (-0.27087943f + 0.61223990f * b_ +
                              a_ * (+0.00299215f - 0.45399568f * b_ - 0.14661872f * a_))));
}

static void oklab_get_cs(float L, float a_, float b_, float *c_0, float *c_mid, float *c_max)
{
  float l_cusp, c_cusp;
  oklab_find_cusp(a_, b_, &l_cusp, &c_cusp);

  *c_max = oklab_find_gamut_intersection(a_, b_, L, 1, L, l_cusp, c_cusp);
  float s_max = c_cusp / l_cusp;
  float t_max = c_cusp / (1 - l_cusp);

  float k = *c_max / fminf((L * s_max), (1 - L) * t_max);

  float s_mid, t_mid;
  oklab_get_st_mid(a_, b_, &s_mid, &t_mid);
  float c_mid_a = L * s_mid;
  float c_mid_b = (1.0f - L) * t_mid;
  *c_mid = 0.9f * k * sqrtf(sqrtf(1.0f / (1.0f / powf(c_mid_a, 4) + 1.0f / powf(c_mid_b, 4))));

  float c_0_a = L * 0.4f;
  float c_0_b = (1.0f - L) * 0.8f;
  *c_0 = sqrtf(1.0f / (1.0f / (c_0_a * c_0_a) + 1.0f / (c_0_b * c_0_b)));
}

void okhsl_to_rgb(float h, float s, float l, float *r_r, float *r_g, float *r_b)
{
  if (l == 1.0f) {
    *r_r = 1.0f;
    *r_g = 1.0f;
    *r_b = 1.0f;
    return;
  }

  else if (l == 0.0f) {
    *r_r = 0.0f;
    *r_g = 0.0f;
    *r_b = 0.0f;
    return;
  }

  float a_ = cosf(2.0f * 3.1415926536f * h);
  float b_ = sinf(2.0f * 3.1415926536f * h);
  float L = oklab_toe_inverse(l);

  float c_0, c_mid, c_max;
  oklab_get_cs(L, a_, b_, &c_0, &c_mid, &c_max);

  float mid = 0.8f;
  float mid_inv = 1.25f;

  float C, t, k_0, k_1, k_2;

  if (s < mid) {
    t = mid_inv * s;
    k_1 = mid * c_0;
    k_2 = (1.0f - k_1 / c_mid);
    C = t * k_1 / (1.0f - k_2 * t);
  }
  else {
    t = (s - mid) / (1 - mid);
    k_0 = c_mid;
    k_1 = (1.0f - mid) * c_mid * c_mid * mid_inv * mid_inv / c_0;
    k_2 = (1.0f - (k_1) / (c_max - c_mid));
    C = k_0 + t * k_1 / (1.0f - k_2 * t);
  }

  oklab_to_rgb(L, C * a_, C * b_, r_r, r_g, r_b);
}

void rgb_to_yuv(float r, float g, float b, float *r_y, float *r_u, float *r_v, int colorspace)
{
  float y, u, v;

  switch (colorspace) {
    case BLI_YUV_ITU_BT601:
      y = 0.299f * r + 0.587f * g + 0.114f * b;
      u = -0.147f * r - 0.289f * g + 0.436f * b;
      v = 0.615f * r - 0.515f * g - 0.100f * b;
      break;
    case BLI_YUV_ITU_BT709:
    default:
      BLI_assert(colorspace == BLI_YUV_ITU_BT709);
      y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      u = -0.09991f * r - 0.33609f * g + 0.436f * b;
      v = 0.615f * r - 0.55861f * g - 0.05639f * b;
      break;
  }

  *r_y = y;
  *r_u = u;
  *r_v = v;
}

void yuv_to_rgb(float y, float u, float v, float *r_r, float *r_g, float *r_b, int colorspace)
{
  float r, g, b;

  switch (colorspace) {
    case BLI_YUV_ITU_BT601:
      r = y + 1.140f * v;
      g = y - 0.394f * u - 0.581f * v;
      b = y + 2.032f * u;
      break;
    case BLI_YUV_ITU_BT709:
    default:
      BLI_assert(colorspace == BLI_YUV_ITU_BT709);
      r = y + 1.28033f * v;
      g = y - 0.21482f * u - 0.38059f * v;
      b = y + 2.12798f * u;
      break;
  }

  *r_r = r;
  *r_g = g;
  *r_b = b;
}

void rgb_to_ycc(float r, float g, float b, float *r_y, float *r_cb, float *r_cr, int colorspace)
{
  float sr, sg, sb;
  float y = 128.0f, cr = 128.0f, cb = 128.0f;

  sr = 255.0f * r;
  sg = 255.0f * g;
  sb = 255.0f * b;

  switch (colorspace) {
    case BLI_YCC_ITU_BT601:
      y = (0.257f * sr) + (0.504f * sg) + (0.098f * sb) + 16.0f;
      cb = (-0.148f * sr) - (0.291f * sg) + (0.439f * sb) + 128.0f;
      cr = (0.439f * sr) - (0.368f * sg) - (0.071f * sb) + 128.0f;
      break;
    case BLI_YCC_ITU_BT709:
      y = (0.183f * sr) + (0.614f * sg) + (0.062f * sb) + 16.0f;
      cb = (-0.101f * sr) - (0.338f * sg) + (0.439f * sb) + 128.0f;
      cr = (0.439f * sr) - (0.399f * sg) - (0.040f * sb) + 128.0f;
      break;
    case BLI_YCC_JFIF_0_255:
      y = (0.299f * sr) + (0.587f * sg) + (0.114f * sb);
      cb = (-0.16874f * sr) - (0.33126f * sg) + (0.5f * sb) + 128.0f;
      cr = (0.5f * sr) - (0.41869f * sg) - (0.08131f * sb) + 128.0f;
      break;
    default:
      BLI_assert_msg(0, "invalid colorspace");
      break;
  }

  *r_y = y;
  *r_cb = cb;
  *r_cr = cr;
}

void ycc_to_rgb(float y, float cb, float cr, float *r_r, float *r_g, float *r_b, int colorspace)
{
  /* FIXME the following comment must be wrong because:
   * BLI_YCC_ITU_BT601 y 16.0 cr 16.0 -> r -0.7009. */

  /* YCC input have a range of 16-235 and 16-240 except with JFIF_0_255 where the range is 0-255
   * RGB outputs are in the range 0 - 1.0f. */

  float r = 128.0f, g = 128.0f, b = 128.0f;

  switch (colorspace) {
    case BLI_YCC_ITU_BT601:
      r = 1.164f * (y - 16.0f) + 1.596f * (cr - 128.0f);
      g = 1.164f * (y - 16.0f) - 0.813f * (cr - 128.0f) - 0.392f * (cb - 128.0f);
      b = 1.164f * (y - 16.0f) + 2.017f * (cb - 128.0f);
      break;
    case BLI_YCC_ITU_BT709:
      r = 1.164f * (y - 16.0f) + 1.793f * (cr - 128.0f);
      g = 1.164f * (y - 16.0f) - 0.534f * (cr - 128.0f) - 0.213f * (cb - 128.0f);
      b = 1.164f * (y - 16.0f) + 2.115f * (cb - 128.0f);
      break;
    case BLI_YCC_JFIF_0_255:
      r = y + 1.402f * cr - 179.456f;
      g = y - 0.34414f * cb - 0.71414f * cr + 135.45984f;
      b = y + 1.772f * cb - 226.816f;
      break;
    default:
      BLI_assert_unreachable();
      break;
  }
  *r_r = r / 255.0f;
  *r_g = g / 255.0f;
  *r_b = b / 255.0f;
}

void hex_to_rgb(const char *hexcol, float *r_r, float *r_g, float *r_b)
{
  hex_to_rgba(hexcol, r_r, r_g, r_b, nullptr);
}

/**
 * Parse a hex color string into RGBA float values.
 *
 * \param hexcol: The hex string to parse (e.g. "#RRGGBB", "RRGGBB", "#RGB").
 * \param r_r, r_g, r_b: Pointers to store the parsed RGB values (0.0 - 1.0).
 * \param r_a: Pointer to store the parsed Alpha value (0.0 - 1.0). Can be NULL.
 *             If the hex string does not contain alpha, this value is NOT modified.
 * \return True if the hex string was successfully parsed, false otherwise.
 *         If false is returned, the output values are left unchanged.
 */
bool hex_to_rgba(const char *hexcol, float *r_r, float *r_g, float *r_b, float *r_a)
{
  if (hexcol[0] == '#') {
    hexcol++;
  }

  size_t hex_len = strlen(hexcol);
  uint r, g, b, a;
  bool alpha_parsed = false;

  switch (hex_len) {
    case 8: /* #RRGGBBAA */
      if (sscanf(hexcol, "%2x%2x%2x%2x", &r, &g, &b, &a) != 4) {
        return false;
      }
      alpha_parsed = true;
      break;
    case 7: /* #RRGGBBA -> #RRGGBBA0 */
      if (sscanf(hexcol, "%2x%2x%2x%1x", &r, &g, &b, &a) != 4) {
        return false;
      }
      a <<= 4;
      alpha_parsed = true;
      break;
    case 6: /* #RRGGBB */
      if (sscanf(hexcol, "%2x%2x%2x", &r, &g, &b) != 3) {
        return false;
      }
      break;
    case 5: /* #RGBAA -> #RRGGBBAA */
      if (sscanf(hexcol, "%1x%1x%1x%2x", &r, &g, &b, &a) != 4) {
        return false;
      }
      r = (r << 4) | r;
      g = (g << 4) | g;
      b = (b << 4) | b;
      alpha_parsed = true;
      break;
    case 4: /* #RGBA -> #RRGGBBAA */
      if (sscanf(hexcol, "%1x%1x%1x%1x", &r, &g, &b, &a) != 4) {
        return false;
      }
      r = (r << 4) | r;
      g = (g << 4) | g;
      b = (b << 4) | b;
      a = (a << 4) | a;
      alpha_parsed = true;
      break;
    case 3: /* #RGB -> #RRGGBB */
      if (sscanf(hexcol, "%1x%1x%1x", &r, &g, &b) != 3) {
        return false;
      }
      r = (r << 4) | r;
      g = (g << 4) | g;
      b = (b << 4) | b;
      break;
    case 2: /* #AB -> #ABABAB */
      if (sscanf(hexcol, "%2x", &r) != 1) {
        return false;
      }
      g = r;
      b = r;
      break;
    case 1: /* #A -> #AAAAAA */
      if (sscanf(hexcol, "%1x", &r) != 1) {
        return false;
      }
      r = (r << 4) | r;
      g = r;
      b = r;
      break;
    default:
      /* Invalid hex color length - leave color unchanged. */
      return false;
  }

  /* Convert integer color channels to float. */
  const float scale = 1.0f / 255.0f;
  *r_r = float(r) * scale;
  *r_g = float(g) * scale;
  *r_b = float(b) * scale;

  CLAMP(*r_r, 0.0f, 1.0f);
  CLAMP(*r_g, 0.0f, 1.0f);
  CLAMP(*r_b, 0.0f, 1.0f);

  /* Assign alpha if present. */
  if (r_a && alpha_parsed) {
    *r_a = float(a) * scale;
    CLAMP(*r_a, 0.0f, 1.0f);
  }
  return true;
}

void rgb_to_hsv(float r, float g, float b, float *r_h, float *r_s, float *r_v)
{
  float k = 0.0f;
  float chroma;
  float min_gb;

  if (g < b) {
    SWAP(float, g, b);
    k = -1.0f;
  }
  min_gb = b;
  if (r < g) {
    SWAP(float, r, g);
    k = -2.0f / 6.0f - k;
    min_gb = min_ff(g, b);
  }

  chroma = r - min_gb;

  *r_h = fabsf(k + (g - b) / (6.0f * chroma + 1e-20f));
  *r_s = chroma / (r + 1e-20f);
  *r_v = r;
}

void rgb_to_hsv_v(const float rgb[3], float r_hsv[3])
{
  rgb_to_hsv(rgb[0], rgb[1], rgb[2], &r_hsv[0], &r_hsv[1], &r_hsv[2]);
}

void rgb_to_hsl(float r, float g, float b, float *r_h, float *r_s, float *r_l)
{
  const float cmax = std::max({r, g, b});
  const float cmin = std::min({r, g, b});
  float h, s, l = min_ff(1.0f, (cmax + cmin) / 2.0f);

  if (cmax == cmin) {
    h = s = 0.0f; /* achromatic */
  }
  else {
    float d = cmax - cmin;
    s = l > 0.5f ? d / (2.0f - cmax - cmin) : d / (cmax + cmin);
    if (cmax == r) {
      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    }
    else if (cmax == g) {
      h = (b - r) / d + 2.0f;
    }
    else {
      h = (r - g) / d + 4.0f;
    }
  }
  h /= 6.0f;

  *r_h = h;
  *r_s = s;
  *r_l = l;
}

void rgb_to_hsl_compat(float r, float g, float b, float *r_h, float *r_s, float *r_l)
{
  /* Convert RGB to HSL, while staying as close as possible to existing HSL values.
   * Uses a threshold as there can be small errors introduced by color space conversions
   * or other operations. */
  const float orig_s = *r_s;
  const float orig_h = *r_h;
  const float threshold = 1e-5f;

  rgb_to_hsl(r, g, b, r_h, r_s, r_l);

  /* For (near) zero lightness or saturation, keep the other values unchanged,
   * as they are either undefined or very sensitive to small lightness changes. */
  if (*r_l <= threshold) {
    *r_h = orig_h;
    *r_s = orig_s;
  }
  else if (*r_s <= threshold) {
    *r_h = orig_h;
    *r_s = orig_s;
  }

  /* Hue wraps around, keep it on the same side. */
  if (fabsf(*r_h) <= threshold && fabsf(orig_h - 1.0f) <= threshold) {
    *r_h = 1.0f;
  }
  else if (fabsf(*r_h - 1.0f) <= threshold && fabsf(orig_h) <= threshold) {
    *r_h = 0.0f;
  }
}

void rgb_to_hsl_compat_v(const float rgb[3], float r_hsl[3])
{
  rgb_to_hsl_compat(rgb[0], rgb[1], rgb[2], &r_hsl[0], &r_hsl[1], &r_hsl[2]);
}

void rgb_to_hsl_v(const float rgb[3], float r_hsl[3])
{
  rgb_to_hsl(rgb[0], rgb[1], rgb[2], &r_hsl[0], &r_hsl[1], &r_hsl[2]);
}

void rgb_to_hsv_compat(float r, float g, float b, float *r_h, float *r_s, float *r_v)
{
  /* Convert RGB to HSV, while staying as close as possible to existing HSV values.
   * Uses a threshold as there can be small errors introduced by color space conversions
   * or other operations. */
  const float orig_h = *r_h;
  const float orig_s = *r_s;
  const float threshold = 1e-5f;

  rgb_to_hsv(r, g, b, r_h, r_s, r_v);

  /* For (near) zero values or saturation, keep the other values unchanged,
   * as they are either undefined or very sensitive to small value changes. */
  if (*r_v <= threshold) {
    *r_h = orig_h;
    *r_s = orig_s;
  }
  else if (*r_s <= threshold) {
    *r_h = orig_h;
  }

  /* Hue wraps around, keep it on the same side. */
  if (fabsf(*r_h) <= threshold && fabsf(orig_h - 1.0f) <= threshold) {
    *r_h = 1.0f;
  }
  else if (fabsf(*r_h - 1.0f) <= threshold && fabsf(orig_h) <= threshold) {
    *r_h = 0.0f;
  }
}

void rgb_to_hsv_compat_v(const float rgb[3], float r_hsv[3])
{
  rgb_to_hsv_compat(rgb[0], rgb[1], rgb[2], &r_hsv[0], &r_hsv[1], &r_hsv[2]);
}

void rgb_to_oklab(float r, float g, float b, float *r_l, float *r_a, float *r_b)
{
  float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
  float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
  float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

  l = cbrtf(l);
  m = cbrtf(m);
  s = cbrtf(s);

  *r_l = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
  *r_a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
  *r_b = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

void rgb_to_oklch(float r, float g, float b, float *r_l, float *r_c, float *r_h)
{
  float lab_l, lab_a, lab_b;
  rgb_to_oklab(r, g, b, &lab_l, &lab_a, &lab_b);
  *r_l = lab_l;
  *r_c = sqrt(lab_a * lab_a + lab_b * lab_b);
  *r_h = atan2(lab_b, lab_a) / 6.2831853072f;
}

void rgb_to_okhsv(float r, float g, float b, float *r_h, float *r_s, float *r_v)
{
  float lab_l, lab_a, lab_b;
  rgb_to_oklab(r, g, b, &lab_l, &lab_a, &lab_b);

  float c = sqrtf(lab_a * lab_a + lab_b * lab_b);
  float a_ = lab_a / c;
  float b_ = lab_b / c;

  float l_cusp, c_cusp;
  oklab_find_cusp(a_, b_, &l_cusp, &c_cusp);
  float s_max = c_cusp / l_cusp;
  float t_max = c_cusp / (1 - l_cusp);
  float s_0 = 0.5f;
  float k = 1 - s_0 / s_max;

  float l = lab_l;
  float t = t_max / (c + l * t_max);
  float l_v = t * l;
  float c_v = t * c;

  float l_vt = oklab_toe_inverse(l_v);
  float c_vt = c_v * l_vt / l_v;

  float r_scale, g_scale, b_scale;
  oklab_to_rgb(l_vt, a_ * c_vt, b_ * c_vt, &r_scale, &g_scale, &b_scale);
  float scale_l = cbrtf(1.0f / fmaxf(fmaxf(r_scale, g_scale), fmaxf(b_scale, 0.0f)));

  l = l / scale_l;
  c = c / scale_l;

  c = c * oklab_toe(l) / l;
  l = oklab_toe(l);

  *r_h = 0.5f + 0.5f * atan2f(-lab_b, -lab_a) / 3.1415926536f;
  *r_s = (s_0 + t_max) * c_v / ((t_max * s_0) + t_max * k * c_v);
  *r_v = l / l_v;
}

void rgb_to_okhsl(float r, float g, float b, float *r_h, float *r_s, float *r_l)
{
  float lab_l, lab_a, lab_b;
  rgb_to_oklab(r, g, b, &lab_l, &lab_a, &lab_b);

  float c = sqrtf(lab_a * lab_a + lab_b * lab_b);
  float a_ = lab_a / c;
  float b_ = lab_b / c;

  float c_0, c_mid, c_max;
  oklab_get_cs(lab_l, a_, b_, &c_0, &c_mid, &c_max);

  float mid = 0.8f;
  float mid_inv = 1.25f;

  float s;
  if (c < c_mid) {
    float k_1 = mid * c_0;
    float k_2 = (1.0f - k_1 / c_mid);

    float t = c / (k_1 + k_2 * c);
    s = t * mid;
  }
  else {
    float k_0 = c_mid;
    float k_1 = (1.0f - mid) * c_mid * c_mid * mid_inv * mid_inv / c_0;
    float k_2 = (1.0f - (k_1) / (c_max - c_mid));

    float t = (c - k_0) / (k_1 + k_2 * (c - k_0));
    s = mid + (1.0f - mid) * t;
  }

  *r_h = 0.5f + 0.5f * atan2f(-lab_b, -lab_a) / 3.1415926536f;
  *r_s = s;
  *r_l = oklab_toe(lab_l);
}

void hsv_clamp_v(float hsv[3], float v_max)
{
  if (hsv[0] < 0.0f || hsv[0] > 1.0f) [[unlikely]] {
    hsv[0] = hsv[0] - floorf(hsv[0]);
  }
  CLAMP(hsv[1], 0.0f, 1.0f);
  CLAMP(hsv[2], 0.0f, v_max);
}

uint hsv_to_cpack(float h, float s, float v)
{
  uint r, g, b;
  float rf, gf, bf;
  uint col;

  hsv_to_rgb(h, s, v, &rf, &gf, &bf);

  r = uint(rf * 255.0f);
  g = uint(gf * 255.0f);
  b = uint(bf * 255.0f);

  col = (r + (g * 256) + (b * 256 * 256));
  return col;
}

uint rgb_to_cpack(float r, float g, float b)
{
  uint ir, ig, ib;

  ir = uint(floorf(255.0f * max_ff(r, 0.0f)));
  ig = uint(floorf(255.0f * max_ff(g, 0.0f)));
  ib = uint(floorf(255.0f * max_ff(b, 0.0f)));

  ir = std::min<uint>(ir, 255);
  ig = std::min<uint>(ig, 255);
  ib = std::min<uint>(ib, 255);

  return (ir + (ig * 256) + (ib * 256 * 256));
}

void cpack_to_rgb(uint col, float *r_r, float *r_g, float *r_b)
{
  *r_r = float(col & 0xFF) * (1.0f / 255.0f);
  *r_g = float((col >> 8) & 0xFF) * (1.0f / 255.0f);
  *r_b = float((col >> 16) & 0xFF) * (1.0f / 255.0f);
}

/* -------------------------------------------------------------------- */
/** \name Color Transforms
 * \{ */

float srgb_to_linearrgb(float c)
{
  if (c < 0.04045f) {
    return (c < 0.0f) ? 0.0f : c * (1.0f / 12.92f);
  }

  return powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

float linearrgb_to_srgb(float c)
{
  if (c < 0.0031308f) {
    return (c < 0.0f) ? 0.0f : c * 12.92f;
  }

  return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* SIMD code path, with pow 2.4 and 1/2.4 approximations. */
#if BLI_HAVE_SSE2

/**
 * Calculate initial guess for `arg^exp` based on float representation
 * This method gives a constant bias, which can be easily compensated by
 * multiplying with bias_coeff.
 * Gives better results for exponents near 1 (e.g. `4/5`).
 * exp = exponent, encoded as uint32_t
 * `e2coeff = 2^(127/exponent - 127) * bias_coeff^(1/exponent)`, encoded as `uint32_t`.
 *
 * We hope that exp and e2coeff gets properly inlined.
 */

/**
 * Fast version of _mm_cvtps_epi32 for arm64, as sse2neon must set the rounding
 * mode each time while we can assume it is already set to nearest for Blender.
 */
MALWAYS_INLINE __m128i _bli_float_to_int_nearest(const __m128 a)
{
#  if BLI_HAVE_ARM_NEON
  return vreinterpretq_m128i_s32(vcvtnq_s32_f32(a));
#  else
  return _mm_cvtps_epi32(a);
#  endif
}

MALWAYS_INLINE __m128 _bli_math_fastpow(const int exp, const int e2coeff, const __m128 arg)
{
  __m128 ret;
  ret = _mm_mul_ps(arg, _mm_castsi128_ps(_mm_set1_epi32(e2coeff)));
  ret = _mm_cvtepi32_ps(_mm_castps_si128(ret));
  ret = _mm_mul_ps(ret, _mm_castsi128_ps(_mm_set1_epi32(exp)));
  ret = _mm_castsi128_ps(_bli_float_to_int_nearest(ret));
  return ret;
}

/** Improve `x ^ 1.0f/5.0f` solution with Newton-Raphson method */
MALWAYS_INLINE __m128 _bli_math_improve_5throot_solution(const __m128 old_result, const __m128 x)
{
  __m128 approx2 = _mm_mul_ps(old_result, old_result);
  __m128 approx4 = _mm_mul_ps(approx2, approx2);
  __m128 t = _mm_div_ps(x, approx4);
  __m128 summ = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(4.0f), old_result), t); /* FMA. */
  return _mm_mul_ps(summ, _mm_set1_ps(1.0f / 5.0f));
}

/** Calculate `powf(x, 2.4)`. Working domain: `1e-10 < x < 1e+10`. */
MALWAYS_INLINE __m128 _bli_math_fastpow24(const __m128 arg)
{
  /* max, avg and |avg| errors were calculated in GCC without FMA instructions
   * The final precision should be better than `powf` in GLIBC. */

  /* Calculate x^4/5, coefficient 0.994 was constructed manually to minimize
   * avg error.
   */
  /* 0x3F4CCCCD = 4/5 */
  /* 0x4F55A7FB = 2^(127/(4/5) - 127) * 0.994^(1/(4/5)) */
  /* error max = 0.17, avg = 0.0018, |avg| = 0.05 */
  __m128 x = _bli_math_fastpow(0x3F4CCCCD, 0x4F55A7FB, arg);
  __m128 arg2 = _mm_mul_ps(arg, arg);
  __m128 arg4 = _mm_mul_ps(arg2, arg2);
  /* error max = 0.018        avg = 0.0031    |avg| = 0.0031 */
  x = _bli_math_improve_5throot_solution(x, arg4);
  /* error max = 0.00021    avg = 1.6e-05    |avg| = 1.6e-05 */
  x = _bli_math_improve_5throot_solution(x, arg4);
  /* error max = 6.1e-07    avg = 5.2e-08    |avg| = 1.1e-07 */
  x = _bli_math_improve_5throot_solution(x, arg4);
  return _mm_mul_ps(x, _mm_mul_ps(x, x));
}

MALWAYS_INLINE __m128 _bli_math_rsqrt(__m128 in)
{
  __m128 r = _mm_rsqrt_ps(in);
  /* Only do additional Newton-Raphson iterations when using actual SSE
   * code path. When we are emulating SSE on NEON via sse2neon, the
   * additional NR iterations are already done inside _mm_rsqrt_ps
   * emulation. */
#  if defined(__SSE2__)
  r = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(1.5f), r),
                 _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(in, _mm_set1_ps(-0.5f)), r), _mm_mul_ps(r, r)));
#  endif
  return r;
}

/* Calculate `powf(x, 1.0f / 2.4)`. */
MALWAYS_INLINE __m128 _bli_math_fastpow512(const __m128 arg)
{
  /* 5/12 is too small, so compute the 4th root of 20/12 instead.
   * 20/12 = 5/3 = 1 + 2/3 = 2 - 1/3. 2/3 is a suitable argument for fastpow.
   * weighting coefficient: a^-1/2 = 2 a; a = 2^-2/3
   */
  __m128 xf = _bli_math_fastpow(0x3f2aaaab, 0x5eb504f3, arg);
  __m128 xover = _mm_mul_ps(arg, xf);
  __m128 xfm1 = _bli_math_rsqrt(xf);
  __m128 x2 = _mm_mul_ps(arg, arg);
  __m128 xunder = _mm_mul_ps(x2, xfm1);
  /* sqrt2 * over + 2 * sqrt2 * under */
  __m128 xavg = _mm_mul_ps(_mm_set1_ps(1.0f / (3.0f * 0.629960524947437f) * 0.999852f),
                           _mm_add_ps(xover, xunder));
  xavg = _mm_mul_ps(xavg, _bli_math_rsqrt(xavg));
  xavg = _mm_mul_ps(xavg, _bli_math_rsqrt(xavg));
  return xavg;
}

MALWAYS_INLINE __m128 _bli_math_blend_sse(const __m128 mask, const __m128 a, const __m128 b)
{
#  if BLI_HAVE_SSE4
  return _mm_blendv_ps(b, a, mask);
#  else
  return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
#  endif
}

MALWAYS_INLINE __m128 srgb_to_linearrgb_v4_simd(const __m128 c)
{
  __m128 cmp = _mm_cmplt_ps(c, _mm_set1_ps(0.04045f));
  __m128 lt = _mm_max_ps(_mm_mul_ps(c, _mm_set1_ps(1.0f / 12.92f)), _mm_set1_ps(0.0f));
  __m128 gtebase = _mm_mul_ps(_mm_add_ps(c, _mm_set1_ps(0.055f)),
                              _mm_set1_ps(1.0f / 1.055f)); /* FMA. */
  __m128 gte = _bli_math_fastpow24(gtebase);
  return _bli_math_blend_sse(cmp, lt, gte);
}

MALWAYS_INLINE __m128 linearrgb_to_srgb_v4_simd(const __m128 c)
{
  __m128 cmp = _mm_cmplt_ps(c, _mm_set1_ps(0.0031308f));
  __m128 lt = _mm_max_ps(_mm_mul_ps(c, _mm_set1_ps(12.92f)), _mm_set1_ps(0.0f));
  __m128 gte = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(1.055f), _bli_math_fastpow512(c)),
                          _mm_set1_ps(-0.055f));
  return _bli_math_blend_sse(cmp, lt, gte);
}

void srgb_to_linearrgb_v3_v3(float linear[3], const float srgb[3])
{
  float r[4] = {srgb[0], srgb[1], srgb[2], 1.0f};
  __m128 *rv = reinterpret_cast<__m128 *>(&r);
  *rv = srgb_to_linearrgb_v4_simd(*rv);
  linear[0] = r[0];
  linear[1] = r[1];
  linear[2] = r[2];
}

void linearrgb_to_srgb_v3_v3(float srgb[3], const float linear[3])
{
  float r[4] = {linear[0], linear[1], linear[2], 1.0f};
  __m128 *rv = reinterpret_cast<__m128 *>(&r);
  *rv = linearrgb_to_srgb_v4_simd(*rv);
  srgb[0] = r[0];
  srgb[1] = r[1];
  srgb[2] = r[2];
}

/** Convert float pixels values into byte range, matching #rgba_float_to_uchar rounding. */
MALWAYS_INLINE __m128i _bli_float_to_uchar_simd(const __m128 v)
{
  const __m128 clamped = _mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(1.0f));
  const __m128 scaled = _mm_add_ps(_mm_mul_ps(clamped, _mm_set1_ps(255.0f)), _mm_set1_ps(0.5f));
  return _mm_cvttps_epi32(scaled);
}

/** Convert block of 4 RGBA pixels with SIMD. */
MALWAYS_INLINE __m128i linearrgb_to_srgb_uchar4_block(const float (*linear)[4],
                                                      const float (*matrix)[3])
{
  __m128 c0 = _mm_loadu_ps(linear[0]);
  __m128 c1 = _mm_loadu_ps(linear[1]);
  __m128 c2 = _mm_loadu_ps(linear[2]);
  __m128 c3 = _mm_loadu_ps(linear[3]);

  /* Transpose from AoS to SoA. */
  _MM_TRANSPOSE4_PS(c0, c1, c2, c3);

  if (matrix) {
    /* Apply matrix to RGB channels. */
    const __m128 r = c0, g = c1, b = c2;
    c0 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(matrix[0][0]), r),
                               _mm_mul_ps(_mm_set1_ps(matrix[1][0]), g)),
                    _mm_mul_ps(_mm_set1_ps(matrix[2][0]), b));
    c1 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(matrix[0][1]), r),
                               _mm_mul_ps(_mm_set1_ps(matrix[1][1]), g)),
                    _mm_mul_ps(_mm_set1_ps(matrix[2][1]), b));
    c2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(matrix[0][2]), r),
                               _mm_mul_ps(_mm_set1_ps(matrix[1][2]), g)),
                    _mm_mul_ps(_mm_set1_ps(matrix[2][2]), b));
  }

  /* Convert RGB channels. */
  c0 = linearrgb_to_srgb_v4_simd(c0);
  c1 = linearrgb_to_srgb_v4_simd(c1);
  c2 = linearrgb_to_srgb_v4_simd(c2);

  /* Transpose from SoA to AoS. */
  _MM_TRANSPOSE4_PS(c0, c1, c2, c3);

  /* Float to byte. */
  const __m128i lo = _mm_packs_epi32(_bli_float_to_uchar_simd(c0), _bli_float_to_uchar_simd(c1));
  const __m128i hi = _mm_packs_epi32(_bli_float_to_uchar_simd(c2), _bli_float_to_uchar_simd(c3));
  return _mm_packus_epi16(lo, hi);
}

#else /* BLI_HAVE_SSE2 */

/* Non-SIMD code path, with the same pow approximations as SIMD one. */

MALWAYS_INLINE float int_as_float(int32_t v)
{
  float r;
  memcpy(&r, &v, sizeof(v));
  return r;
}

MALWAYS_INLINE int32_t float_as_int(float v)
{
  int32_t r;
  memcpy(&r, &v, sizeof(v));
  return r;
}

MALWAYS_INLINE float _bli_math_fastpow(const int exp, const int e2coeff, const float arg)
{
  float ret = arg * int_as_float(e2coeff);
  ret = float(float_as_int(ret));
  ret = ret * int_as_float(exp);
  ret = int_as_float(int(ret));
  return ret;
}

MALWAYS_INLINE float _bli_math_improve_5throot_solution(const float old_result, const float x)
{
  float approx2 = old_result * old_result;
  float approx4 = approx2 * approx2;
  float t = x / approx4;
  float summ = 4.0f * old_result + t;
  return summ * (1.0f / 5.0f);
}

MALWAYS_INLINE float _bli_math_fastpow24(const float arg)
{
  float x = _bli_math_fastpow(0x3F4CCCCD, 0x4F55A7FB, arg);
  float arg2 = arg * arg;
  float arg4 = arg2 * arg2;
  x = _bli_math_improve_5throot_solution(x, arg4);
  x = _bli_math_improve_5throot_solution(x, arg4);
  x = _bli_math_improve_5throot_solution(x, arg4);
  return x * x * x;
}

MALWAYS_INLINE float _bli_math_rsqrt(float in)
{
  return 1.0f / sqrtf(in);
}

MALWAYS_INLINE float _bli_math_fastpow512(const float arg)
{
  float xf = _bli_math_fastpow(0x3f2aaaab, 0x5eb504f3, arg);
  float xover = arg * xf;
  float xfm1 = _bli_math_rsqrt(xf);
  float x2 = arg * arg;
  float xunder = x2 * xfm1;
  float xavg = (1.0f / (3.0f * 0.629960524947437f) * 0.999852f) * (xover + xunder);
  xavg = xavg * _bli_math_rsqrt(xavg);
  xavg = xavg * _bli_math_rsqrt(xavg);
  return xavg;
}

MALWAYS_INLINE float srgb_to_linearrgb_approx(float c)
{
  if (c < 0.04045f) {
    return (c < 0.0f) ? 0.0f : c * (1.0f / 12.92f);
  }

  return _bli_math_fastpow24((c + 0.055f) * (1.0f / 1.055f));
}

MALWAYS_INLINE float linearrgb_to_srgb_approx(float c)
{
  if (c < 0.0031308f) {
    return (c < 0.0f) ? 0.0f : c * 12.92f;
  }

  return 1.055f * _bli_math_fastpow512(c) - 0.055f;
}

void srgb_to_linearrgb_v3_v3(float linear[3], const float srgb[3])
{
  linear[0] = srgb_to_linearrgb_approx(srgb[0]);
  linear[1] = srgb_to_linearrgb_approx(srgb[1]);
  linear[2] = srgb_to_linearrgb_approx(srgb[2]);
}

void linearrgb_to_srgb_v3_v3(float srgb[3], const float linear[3])
{
  srgb[0] = linearrgb_to_srgb_approx(linear[0]);
  srgb[1] = linearrgb_to_srgb_approx(linear[1]);
  srgb[2] = linearrgb_to_srgb_approx(linear[2]);
}

#endif /* BLI_HAVE_SSE2 */

void linearrgb_to_srgb_uchar4_n(uchar (*__restrict srgb)[4],
                                const float (*__restrict linear)[4],
                                const int size,
                                const float (*__restrict matrix)[3])
{
#if BLI_HAVE_SSE2
  if (size >= 4) {
    /* Process 4 pixels per SIMD operation. */
    int i = 0;
    for (; i + 4 <= size; i += 4) {
      const __m128i bytes = linearrgb_to_srgb_uchar4_block(&linear[i], matrix);
      _mm_storeu_si128(reinterpret_cast<__m128i *>(srgb[i]), bytes);
    }
    /* Remainder with SIMD too, knowing we can safely overwrite already processed pixels again. */
    if (i < size) {
      const int remainder = size - 4;
      const __m128i bytes = linearrgb_to_srgb_uchar4_block(&linear[remainder], matrix);
      _mm_storeu_si128(reinterpret_cast<__m128i *>(srgb[remainder]), bytes);
    }
    return;
  }
#endif

  /* Scalar path. */
  if (matrix) {
    for (int i = 0; i < size; i++) {
      float s[4];
      mul_v3_m3v3(s, matrix, linear[i]);
      linearrgb_to_srgb_v3_v3(s, s);
      s[3] = linear[i][3];
      rgba_float_to_uchar(srgb[i], s);
    }
  }
  else {
    for (int i = 0; i < size; i++) {
      float s[4];
      linearrgb_to_srgb_v3_v3(s, linear[i]);
      s[3] = linear[i][3];
      rgba_float_to_uchar(srgb[i], s);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Other
 * \{ */

void rgb_float_set_hue_float_offset(float rgb[3], float hue_offset)
{
  float hsv[3];

  rgb_to_hsv(rgb[0], rgb[1], rgb[2], hsv, hsv + 1, hsv + 2);

  hsv[0] += hue_offset;
  if (hsv[0] > 1.0f) {
    hsv[0] -= 1.0f;
  }
  else if (hsv[0] < 0.0f) {
    hsv[0] += 1.0f;
  }

  hsv_to_rgb(hsv[0], hsv[1], hsv[2], rgb, rgb + 1, rgb + 2);
}

void rgb_byte_set_hue_float_offset(uchar rgb[3], float hue_offset)
{
  float rgb_float[3];

  rgb_uchar_to_float(rgb_float, rgb);
  rgb_float_set_hue_float_offset(rgb_float, hue_offset);
  rgb_float_to_uchar(rgb, rgb_float);
}

float BLI_color_from_srgb_table[256];

void BLI_init_srgb_conversion()
{
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;

  /* Fill in the lookup table to convert bytes to float: */
  for (int b = 0; b <= 255; b++) {
    float f = srgb_to_linearrgb(float(b) * (1.0f / 255.0f));
    BLI_color_from_srgb_table[b] = f;
  }
}

namespace math {

struct locus_entry_t {
  float mired; /* Inverse temperature */
  float2 uv;   /* CIE 1960 uv coordinates */
  float t;     /* Isotherm parameter */
  float dist(const float2 p) const
  {
    const float2 diff = p - uv;
    return diff.y - t * diff.x;
  }
};

/* Tabulated approximation of the Planckian locus.
 * Based on http://www.brucelindbloom.com/Eqn_XYZ_to_T.html.
 * Original source:
 * "Color Science: Concepts and Methods, Quantitative Data and Formulae", Second Edition,
 * Gunter Wyszecki and W. S. Stiles, John Wiley & Sons, 1982, pp. 227, 228. */
static const std::array<locus_entry_t, 31> planck_locus{{
    {0.0f, {0.18006f, 0.26352f}, -0.24341f},   {10.0f, {0.18066f, 0.26589f}, -0.25479f},
    {20.0f, {0.18133f, 0.26846f}, -0.26876f},  {30.0f, {0.18208f, 0.27119f}, -0.28539f},
    {40.0f, {0.18293f, 0.27407f}, -0.30470f},  {50.0f, {0.18388f, 0.27709f}, -0.32675f},
    {60.0f, {0.18494f, 0.28021f}, -0.35156f},  {70.0f, {0.18611f, 0.28342f}, -0.37915f},
    {80.0f, {0.18740f, 0.28668f}, -0.40955f},  {90.0f, {0.18880f, 0.28997f}, -0.44278f},
    {100.0f, {0.19032f, 0.29326f}, -0.47888f}, {125.0f, {0.19462f, 0.30141f}, -0.58204f},
    {150.0f, {0.19962f, 0.30921f}, -0.70471f}, {175.0f, {0.20525f, 0.31647f}, -0.84901f},
    {200.0f, {0.21142f, 0.32312f}, -1.0182f},  {225.0f, {0.21807f, 0.32909f}, -1.2168f},
    {250.0f, {0.22511f, 0.33439f}, -1.4512f},  {275.0f, {0.23247f, 0.33904f}, -1.7298f},
    {300.0f, {0.24010f, 0.34308f}, -2.0637f},  {325.0f, {0.24792f, 0.34655f}, -2.4681f},
    {350.0f, {0.25591f, 0.34951f}, -2.9641f},  {375.0f, {0.26400f, 0.35200f}, -3.5814f},
    {400.0f, {0.27218f, 0.35407f}, -4.3633f},  {425.0f, {0.28039f, 0.35577f}, -5.3762f},
    {450.0f, {0.28863f, 0.35714f}, -6.7262f},  {475.0f, {0.29685f, 0.35823f}, -8.5955f},
    {500.0f, {0.30505f, 0.35907f}, -11.324f},  {525.0f, {0.31320f, 0.35968f}, -15.628f},
    {550.0f, {0.32129f, 0.36011f}, -23.325f},  {575.0f, {0.32931f, 0.36038f}, -40.770f},
    {600.0f, {0.33724f, 0.36051f}, -116.45f},
}};

bool whitepoint_to_temp_tint(const float3 &white, float &temperature, float &tint)
{
  /* Convert XYZ -> CIE 1960 uv. */
  const float2 uv = float2{4.0f * white.x, 6.0f * white.y} / dot(white, {1.0f, 15.0f, 3.0f});

  /* Find first entry that's "to the right" of the white point. */
  auto check = [uv](const float val, const locus_entry_t &entry) { return entry.dist(uv) < val; };
  std::array<locus_entry_t, 31>::const_iterator entry = std::upper_bound(
      planck_locus.begin(), planck_locus.end(), 0.0f, check);
  if (entry == planck_locus.begin() || entry == planck_locus.end()) {
    return false;
  }
  const size_t i = size_t(entry - planck_locus.begin());
  const locus_entry_t &low = planck_locus[i - 1], high = planck_locus[i];

  /* Find closest point on locus. */
  const float d_low = low.dist(uv) / sqrtf(1.0f + low.t * low.t);
  const float d_high = high.dist(uv) / sqrtf(1.0f + high.t * high.t);
  const float f = d_low / (d_low - d_high);

  /* Find tint based on distance to closest point on locus. */
  const float2 uv_temp = interpolate(low.uv, high.uv, f);
  const float abs_tint = length(uv - uv_temp) * 3000.0f;
  if (abs_tint > 150.0f) {
    return false;
  }

  temperature = 1e6f / interpolate(low.mired, high.mired, f);
  tint = abs_tint * ((uv.x < uv_temp.x) ? 1.0f : -1.0f);
  return true;
}

float3 whitepoint_from_temp_tint(const float temperature, const float tint)
{
  /* Find table entry. */
  const float mired = clamp(
      1e6f / temperature, planck_locus[0].mired, planck_locus[planck_locus.size() - 1].mired);
  auto check = [](const locus_entry_t &entry, const float val) { return entry.mired < val; };
  std::array<locus_entry_t, 31>::const_iterator entry = std::lower_bound(
      planck_locus.begin(), planck_locus.end(), mired, check);
  const size_t i = size_t(entry - planck_locus.begin());
  const locus_entry_t &low = planck_locus[i - 1], high = planck_locus[i];

  /* Find interpolation factor. */
  const float f = (mired - low.mired) / (high.mired - low.mired);

  /* Interpolate point along Planckian locus. */
  float2 uv = interpolate(low.uv, high.uv, f);

  /* Compute and interpolate isotherm. */
  const float2 isotherm0 = normalize(float2(1.0f, low.t));
  const float2 isotherm1 = normalize(float2(1.0f, high.t));
  const float2 isotherm = normalize(interpolate(isotherm0, isotherm1, f));

  /* Offset away from the Planckian locus according to the tint.
   * Tint is parameterized such that +-3000 tint corresponds to +-1 delta UV. */
  uv -= isotherm * tint / 3000.0f;

  /* Convert CIE 1960 uv -> xyY. */
  const float x = 3.0f * uv.x / (2.0f * uv.x - 8.0f * uv.y + 4.0f);
  const float y = 2.0f * uv.y / (2.0f * uv.x - 8.0f * uv.y + 4.0f);

  /* Convert xyY -> XYZ (assuming Y=1). */
  return float3{x / y, 1.0f, (1.0f - x - y) / y};
}

float3x3 chromatic_adaption_matrix(const float3 &from_XYZ, const float3 &to_XYZ)
{
  /* Bradford transformation matrix (XYZ -> LMS). */
  static const float3x3 bradford{
      {0.8951f, -0.7502f, 0.0389f},
      {0.2664f, 1.7135f, -0.0685f},
      {-0.1614f, 0.0367f, 1.0296f},
  };

  /* Compute white points in LMS space. */
  const float3 from_LMS = bradford * from_XYZ / from_XYZ.y;
  const float3 to_LMS = bradford * to_XYZ / to_XYZ.y;

  /* Assemble full transform: XYZ -> LMS -> adapted LMS -> adapted XYZ. */
  return invert(bradford) * from_scale<float3x3>(to_LMS / from_LMS) * bradford;
}

/** \} */

}  // namespace math
}  // namespace blender
