/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

[[node]]
void rgb_to_hsv(float4 rgb, float4 &outcol)
{
  float cmax, cmin, h, s, v, cdelta;
  float3 c;

  cmax = max(rgb[0], max(rgb[1], rgb[2]));
  cmin = min(rgb[0], min(rgb[1], rgb[2]));
  cdelta = cmax - cmin;

  v = cmax;
  if (cmax != 0.0f) {
    s = cdelta / cmax;
  }
  else {
    s = 0.0f;
    h = 0.0f;
  }

  if (s == 0.0f) {
    h = 0.0f;
  }
  else {
    c = (float3(cmax) - rgb.xyz) / cdelta;

    if (rgb.x == cmax) {
      h = c[2] - c[1];
    }
    else if (rgb.y == cmax) {
      h = 2.0f + c[0] - c[2];
    }
    else {
      h = 4.0f + c[1] - c[0];
    }

    h /= 6.0f;

    if (h < 0.0f) {
      h += 1.0f;
    }
  }

  outcol = float4(h, s, v, rgb.w);
}

[[node]]
void hsv_to_rgb(float4 hsv, float4 &outcol)
{
  float i, f, p, q, t, h, s, v;
  float3 rgb;

  h = hsv[0];
  s = hsv[1];
  v = hsv[2];

  if (s == 0.0f) {
    rgb = float3(v, v, v);
  }
  else {
    if (h == 1.0f) {
      h = 0.0f;
    }

    h *= 6.0f;
    i = floor(h);
    f = h - i;
    rgb = float3(f, f, f);
    p = v * (1.0f - s);
    q = v * (1.0f - (s * f));
    t = v * (1.0f - (s * (1.0f - f)));

    if (i == 0.0f) {
      rgb = float3(v, t, p);
    }
    else if (i == 1.0f) {
      rgb = float3(q, v, p);
    }
    else if (i == 2.0f) {
      rgb = float3(p, v, t);
    }
    else if (i == 3.0f) {
      rgb = float3(p, q, v);
    }
    else if (i == 4.0f) {
      rgb = float3(t, p, v);
    }
    else {
      rgb = float3(v, p, q);
    }
  }

  outcol = float4(rgb, hsv.w);
}

[[node]]
void rgb_to_hsl(float4 rgb, float4 &outcol)
{
  float cmax, cmin, h, s, l;

  cmax = max(rgb[0], max(rgb[1], rgb[2]));
  cmin = min(rgb[0], min(rgb[1], rgb[2]));
  l = min(1.0f, (cmax + cmin) / 2.0f);

  if (cmax == cmin) {
    h = s = 0.0f; /* achromatic */
  }
  else {
    float cdelta = cmax - cmin;
    s = l > 0.5f ? cdelta / (2.0f - cmax - cmin) : cdelta / (cmax + cmin);
    if (cmax == rgb[0]) {
      h = (rgb[1] - rgb[2]) / cdelta + (rgb[1] < rgb[2] ? 6.0f : 0.0f);
    }
    else if (cmax == rgb[1]) {
      h = (rgb[2] - rgb[0]) / cdelta + 2.0f;
    }
    else {
      h = (rgb[0] - rgb[1]) / cdelta + 4.0f;
    }
  }
  h /= 6.0f;

  outcol = float4(h, s, l, rgb.w);
}

[[node]]
void hsl_to_rgb(float4 hsl, float4 &outcol)
{
  float nr, ng, nb, chroma, h, s, l;

  h = hsl[0];
  s = hsl[1];
  l = hsl[2];

  nr = abs(h * 6.0f - 3.0f) - 1.0f;
  ng = 2.0f - abs(h * 6.0f - 2.0f);
  nb = 2.0f - abs(h * 6.0f - 4.0f);

  nr = clamp(nr, 0.0f, 1.0f);
  nb = clamp(nb, 0.0f, 1.0f);
  ng = clamp(ng, 0.0f, 1.0f);

  chroma = (1.0f - abs(2.0f * l - 1.0f)) * s;

  outcol = float4(
      (nr - 0.5f) * chroma + l, (ng - 0.5f) * chroma + l, (nb - 0.5f) * chroma + l, hsl.w);
}

[[node]]
void rgb_to_oklab(float4 rgb, float4 &outcol)
{
  float l, m, s;

  l = 0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2];
  m = 0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2];
  s = 0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2];

  l = pow(l, 1.0f / 3.0f);
  m = pow(m, 1.0f / 3.0f);
  s = pow(s, 1.0f / 3.0f);

  outcol = float4(0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
                  1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
                  0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
                  rgb[3]);
}

[[node]]
void oklab_to_rgb(float4 lab, float4 &outcol)
{
  float l, m, s;

  l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
  m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
  s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];

  l = l * l * l;
  m = m * m * m;
  s = s * s * s;

  outcol = float4(+4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                  -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                  -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
                  lab[3]);
}

[[node]]
void rgb_to_oklch(float4 rgb, float4 &outcol)
{
  float c, h;
  float4 lab;
  rgb_to_oklab(rgb, lab);

  c = sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
  h = atan(lab[2], lab[1]) / radians(360.0f);

  outcol = float4(lab[0], c, h, lab[3]);
}

[[node]]
void oklch_to_rgb(float4 lch, float4 &outcol)
{
  float a = lch[1] * cos(lch[2] * radians(360.0f));
  float b = lch[1] * sin(lch[2] * radians(360.0f));

  oklab_to_rgb(float4(lch[0], a, b, lch[3]), outcol);
}

float oklab_compute_max_saturation(float a, float b)
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

void oklab_find_cusp(float a, float b, float &l, float &c)
{
  float s_cusp = oklab_compute_max_saturation(a, b);
  float4 rgb_at_max;
  oklab_to_rgb(float4(1.0f, s_cusp * a, s_cusp * b, 1.0f), rgb_at_max);
  l = pow(1.0f / max(max(rgb_at_max.x, rgb_at_max.y), rgb_at_max.z), 1.0f / 3.0f);
  c = l * s_cusp;
}

float oklab_toe(float x)
{
  const float k_1 = 0.206f;
  const float k_2 = 0.03f;
  const float k_3 = (1.0f + k_1) / (1.0f + k_2);
  return 0.5f * (k_3 * x - k_1 + sqrt((k_3 * x - k_1) * (k_3 * x - k_1) + 4 * k_2 * k_3 * x));
}

float oklab_toe_inverse(float x)
{
  const float k_1 = 0.206f;
  const float k_2 = 0.03f;
  const float k_3 = (1.0f + k_1) / (1.0f + k_2);
  return (x * x + k_1 * x) / (k_3 * (x + k_2));
}

[[node]]
void rgb_to_okhsv(float4 rgb, float4 &outcol)
{
  float4 lab;
  rgb_to_oklab(rgb, lab);
  float l = lab[0];
  float a = lab[1];
  float b = lab[2];

  float c = sqrt(a * a + b * b);
  float a_ = a / c;
  float b_ = b / c;

  float l_cusp, c_cusp;
  oklab_find_cusp(a_, b_, l_cusp, c_cusp);
  float s_max = c_cusp / l_cusp;
  float t_max = c_cusp / (1 - l_cusp);
  float s_0 = 0.5f;
  float k = 1 - s_0 / s_max;

  float t = t_max / (c + l * t_max);
  float l_v = t * l;
  float c_v = t * c;

  float l_vt = oklab_toe_inverse(l_v);
  float c_vt = c_v * l_vt / l_v;

  float4 rgb_scale;
  oklab_to_rgb(float4(l_vt, a_ * c_vt, b_ * c_vt, rgb[3]), rgb_scale);
  float scale_l = pow(1.0f / max(max(rgb_scale.x, rgb_scale.y), max(rgb_scale.z, 0.0f)),
                      1.0f / 3.0f);

  l = l / scale_l;
  c = c / scale_l;

  c = c * oklab_toe(l) / l;
  l = oklab_toe(l);

  float h = 0.5f + 0.5f * atan(-b, -a) / 3.1415926536f;
  float v = l / l_v;
  float s = (s_0 + t_max) * c_v / ((t_max * s_0) + t_max * k * c_v);
  outcol = float4(h, s, v, rgb[3]);
}

[[node]]
void okhsv_to_rgb(float4 hsv, float4 &outcol)
{
  float h = hsv[0];
  float s = hsv[1];
  float v = hsv[2];

  float a_ = cos(2.0f * 3.1415926536f * h);
  float b_ = sin(2.0f * 3.1415926536f * h);

  float l_cusp, c_cusp;
  oklab_find_cusp(a_, b_, l_cusp, c_cusp);
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

  float4 rgb_scale;
  oklab_to_rgb(float4(l_vt, a_ * c_vt, b_ * c_vt, hsv[3]), rgb_scale);
  float scale_l = pow(1.0f / max(max(rgb_scale.x, rgb_scale.y), max(rgb_scale.z, 0.0f)),
                      1.0f / 3.0f);

  l = l * scale_l;
  c = c * scale_l;

  oklab_to_rgb(float4(l, c * a_, c * b_, hsv[3]), outcol);
}

float oklab_find_gamut_intersection(
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

      t_r = u_r >= 0.0f ? t_r : 1e38;
      t_g = u_g >= 0.0f ? t_g : 1e38;
      t_b = u_b >= 0.0f ? t_b : 1e38;

      t += min(t_r, min(t_g, t_b));
    }
  }

  return t;
}

void oklab_get_st_mid(float a_, float b_, float &s_mid, float &t_mid)
{
  s_mid = 0.11516993f +
          1.0f / (+7.44778970f + 4.15901240f * b_ +
                 a_ * (-2.19557347f + 1.75198401f * b_ +
                       a_ * (-2.13704948f - 10.02301043f * b_ +
                             a_ * (-4.24894561f + 5.38770819f * b_ + 4.69891013f * a_))));
  t_mid = 0.11239642f +
          1.0f / (+1.61320320f - 0.68124379f * b_ +
                 a_ * (+0.40370612f + 0.90148123f * b_ +
                       a_ * (-0.27087943f + 0.61223990f * b_ +
                             a_ * (+0.00299215f - 0.45399568f * b_ - 0.14661872f * a_))));
}

void oklab_get_cs(float L, float a_, float b_, float &c_0, float &c_mid, float &c_max)
{
  float l_cusp, c_cusp;
  oklab_find_cusp(a_, b_, l_cusp, c_cusp);

  c_max = oklab_find_gamut_intersection(a_, b_, L, 1, L, l_cusp, c_cusp);
  float s_max = c_cusp / l_cusp;
  float t_max = c_cusp / (1 - l_cusp);

  float k = c_max / min((L * s_max), (1 - L) * t_max);

  float s_mid, t_mid;
  oklab_get_st_mid(a_, b_, s_mid, t_mid);
  float c_mid_a = L * s_mid;
  float c_mid_b = (1.0f - L) * t_mid;
  c_mid = 0.9f * k * sqrt(sqrt(1.0f / (1.0f / pow(c_mid_a, 4) + 1.0f / pow(c_mid_b, 4))));

  float c_0_a = L * 0.4f;
  float c_0_b = (1.0f - L) * 0.8f;
  c_0 = sqrt(1.0f / (1.0f / (c_0_a * c_0_a) + 1.0f / (c_0_b * c_0_b)));
}

[[node]]
void rgb_to_okhsl(float4 rgb, float4 &outcol)
{
  float4 lab;
  rgb_to_oklab(float4(rgb[0], rgb[1], rgb[2], rgb[3]), lab);
  float lab_l = lab[0];
  float lab_a = lab[1];
  float lab_b = lab[2];

  float c = sqrt(lab_a * lab_a + lab_b * lab_b);
  float a_ = lab_a / c;
  float b_ = lab_b / c;

  float c_0, c_mid, c_max;
  oklab_get_cs(lab_l, a_, b_, c_0, c_mid, c_max);

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

  float h = (0.5f + 0.5f * atan(-lab_b, -lab_a) / 3.1415926536f);
  float l = oklab_toe(lab_l);
  outcol = float4(h, s, l, rgb[3]);
}

[[node]]
void okhsl_to_rgb(float4 hsl, float4 &outcol)
{
  float h = hsl[0];
  float s = hsl[1];
  float l = hsl[2];

  if (l == 1.0f) {
    outcol = float4(1.0f, 1.0f, 1.0f, hsl[3]);
    return;
  }

  else if (l == 0.0f) {
    outcol = float4(0.0f, 0.0f, 0.0f, hsl[3]);
    return;
  }

  float a_ = cos(2.0f * 3.1415926536f * h);
  float b_ = sin(2.0f * 3.1415926536f * h);
  float L = oklab_toe_inverse(l);

  float c_0, c_mid, c_max;
  oklab_get_cs(L, a_, b_, c_0, c_mid, c_max);

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

  oklab_to_rgb(float4(L, C * a_, C * b_, hsl[3]), outcol);
}

/* ** YCCA to RGBA ** */

[[node]]
void ycca_to_rgba_itu_601(float4 ycca, float4 &color)
{
  ycca.xyz *= 255.0f;
  ycca.xyz -= float3(16.0f, 128.0f, 128.0f);
  color.rgb = float3x3(1.164f, 1.164f, 1.164f, 0.0f, -0.392f, 2.017f, 1.596f, -0.813f, 0.0f) *
              ycca.xyz;
  color.rgb /= 255.0f;
  color.a = ycca.a;
}

[[node]]
void ycca_to_rgba_itu_709(float4 ycca, float4 &color)
{
  ycca.xyz *= 255.0f;
  ycca.xyz -= float3(16.0f, 128.0f, 128.0f);
  color.rgb = float3x3(1.164f, 1.164f, 1.164f, 0.0f, -0.213f, 2.115f, 1.793f, -0.534f, 0.0f) *
              ycca.xyz;
  color.rgb /= 255.0f;
  color.a = ycca.a;
}

[[node]]
void ycca_to_rgba_jpeg(float4 ycca, float4 &color)
{
  ycca.xyz *= 255.0f;
  color.rgb = float3x3(1.0f, 1.0f, 1.0f, 0.0f, -0.34414f, 1.772f, 1.402f, -0.71414f, 0.0f) *
              ycca.xyz;
  color.rgb += float3(-179.456f, 135.45984f, -226.816f);
  color.rgb /= 255.0f;
  color.a = ycca.a;
}

/* ** RGBA to YCCA ** */

[[node]]
void rgba_to_ycca_itu_601(float4 rgba, float4 &ycca)
{
  rgba.rgb *= 255.0f;
  ycca.xyz = float3x3(0.257f, -0.148f, 0.439f, 0.504f, -0.291f, -0.368f, 0.098f, 0.439f, -0.071f) *
             rgba.rgb;
  ycca.xyz += float3(16.0f, 128.0f, 128.0f);
  ycca.xyz /= 255.0f;
  ycca.a = rgba.a;
}

[[node]]
void rgba_to_ycca_itu_709(float4 rgba, float4 &ycca)
{
  rgba.rgb *= 255.0f;
  ycca.xyz = float3x3(0.183f, -0.101f, 0.439f, 0.614f, -0.338f, -0.399f, 0.062f, 0.439f, -0.040f) *
             rgba.rgb;
  ycca.xyz += float3(16.0f, 128.0f, 128.0f);
  ycca.xyz /= 255.0f;
  ycca.a = rgba.a;
}

[[node]]
void rgba_to_ycca_jpeg(float4 rgba, float4 &ycca)
{
  rgba.rgb *= 255.0f;
  ycca.xyz = float3x3(
                 0.299f, -0.16874f, 0.5f, 0.587f, -0.33126f, -0.41869f, 0.114f, 0.5f, -0.08131f) *
             rgba.rgb;
  ycca.xyz += float3(0.0f, 128.0f, 128.0f);
  ycca.xyz /= 255.0f;
  ycca.a = rgba.a;
}

/* ** YUVA to RGBA ** */

[[node]]
void yuva_to_rgba_itu_709(float4 yuva, float4 &color)
{
  color.rgb = float3x3(1.0f, 1.0f, 1.0f, 0.0f, -0.21482f, 2.12798f, 1.28033f, -0.38059f, 0.0f) *
              yuva.xyz;
  color.a = yuva.a;
}

/* ** RGBA to YUVA ** */

[[node]]
void rgba_to_yuva_itu_709(float4 rgba, float4 &yuva)
{
  yuva.xyz =
      float3x3(
          0.2126f, -0.09991f, 0.615f, 0.7152f, -0.33609f, -0.55861f, 0.0722f, 0.436f, -0.05639f) *
      rgba.rgb;
  yuva.a = rgba.a;
}

/* ** Alpha Handling ** */

[[node]]
void color_alpha_clear(float4 color, float4 &result)
{
  result = float4(color.rgb, 1.0f);
}

[[node]]
void color_alpha_premultiply(float4 color, float4 &result)
{
  result = float4(color.rgb * color.a, color.a);
}

[[node]]
void color_alpha_unpremultiply(float4 color, float4 &result)
{
  if (color.a == 0.0f || color.a == 1.0f) {
    result = color;
  }
  else {
    result = float4(color.rgb / color.a, color.a);
  }
}

float linear_rgb_to_srgb(float color)
{
  if (color < 0.0031308f) {
    return (color < 0.0f) ? 0.0f : color * 12.92f;
  }

  return 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
}

float3 linear_rgb_to_srgb(float3 color)
{
  return float3(
      linear_rgb_to_srgb(color.r), linear_rgb_to_srgb(color.g), linear_rgb_to_srgb(color.b));
}

float srgb_to_linear_rgb(float color)
{
  if (color < 0.04045f) {
    return (color < 0.0f) ? 0.0f : color * (1.0f / 12.92f);
  }

  return pow((color + 0.055f) * (1.0f / 1.055f), 2.4f);
}

float3 srgb_to_linear_rgb(float3 color)
{
  return float3(
      srgb_to_linear_rgb(color.r), srgb_to_linear_rgb(color.g), srgb_to_linear_rgb(color.b));
}

float get_luminance(float3 color, float3 luminance_coefficients)
{
  return dot(color, luminance_coefficients);
}
