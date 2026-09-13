/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_color_utils.bsl.hh"

/* ** Combine/Separate XYZ ** */

[[node]]
void node_composite_combine_xyz(float x, float y, float z, float3 &vector)
{
  vector = float3(x, y, z);
}

[[node]]
void node_composite_separate_xyz(float3 vector, float &x, float &y, float &z)
{
  x = vector.x;
  y = vector.y;
  z = vector.z;
}

/* ** Combine/Separate RGBA ** */

[[node]]
void node_composite_combine_rgba(float r, float g, float b, float a, float4 &color)
{
  color = float4(r, g, b, a);
}

[[node]]
void node_composite_separate_rgba(float4 color, float &r, float &g, float &b, float &a)
{
  r = color.r;
  g = color.g;
  b = color.b;
  a = color.a;
}

/* ** Combine/Separate HSVA ** */

[[node]]
void node_composite_combine_hsva(float h, float s, float v, float a, float4 &color)
{
  hsv_to_rgb(float4(h, s, v, a), color);
}

[[node]]
void node_composite_separate_hsva(float4 color, float &h, float &s, float &v, float &a)
{
  float4 hsva;
  rgb_to_hsv(color, hsva);
  h = hsva.x;
  s = hsva.y;
  v = hsva.z;
  a = hsva.a;
}

/* ** Combine/Separate HSLA ** */

[[node]]
void node_composite_combine_hsla(float h, float s, float l, float a, float4 &color)
{
  hsl_to_rgb(float4(h, s, l, a), color);
  color.rgb = max(color.rgb, float3(0.0f));
}

[[node]]
void node_composite_separate_hsla(float4 color, float &h, float &s, float &l, float &a)
{
  float4 hsla;
  rgb_to_hsl(color, hsla);
  h = hsla.x;
  s = hsla.y;
  l = hsla.z;
  a = hsla.a;
}

/* ** Combine/Separate LAB using Oklab ** */

[[node]]
void node_composite_combine_oklab(float l, float a, float b, float alpha, float4 &color)
{
  oklab_to_rgb(float4(l, a, b, alpha), color);
}

[[node]]
void node_composite_separate_oklab(float4 color, float &l, float &a, float &b, float &alpha)
{
  float4 lab;
  rgb_to_oklab(color, lab);
  l = lab.x;
  a = lab.y;
  b = lab.z;
  alpha = lab.a;
}

/* ** Combine/Separate LCH using Oklab ** */

[[node]]
void node_composite_combine_oklch(float l, float c, float h, float alpha, float4 &color)
{
  oklch_to_rgb(float4(l, c, h, alpha), color);
}

[[node]]
void node_composite_separate_oklch(float4 color, float &l, float &c, float &h, float &alpha)
{
  float4 lch;
  rgb_to_oklch(color, lch);
  l = lch.x;
  c = lch.y;
  h = lch.z;
  alpha = lch.a;
}

/* ** Combine/Separate HSV using Oklab ** */

[[node]]
void node_composite_combine_okhsv(float h, float s, float v, float alpha, float4 &color)
{
  okhsv_to_rgb(float4(h, s, v, alpha), color);
}

[[node]]
void node_composite_separate_okhsv(float4 color, float &h, float &s, float &v, float &alpha)
{
  float4 hsv;
  rgb_to_okhsv(color, hsv);
  h = hsv.x;
  s = hsv.y;
  v = hsv.z;
  alpha = hsv.a;
}

/* ** Combine/Separate HSL using Oklab ** */

[[node]]
void node_composite_combine_okhsl(float h, float s, float l, float alpha, float4 &color)
{
  okhsl_to_rgb(float4(h, s, l, alpha), color);
}

[[node]]
void node_composite_separate_okhsl(float4 color, float &h, float &s, float &l, float &alpha)
{
  float4 hsl;
  rgb_to_okhsl(color, hsl);
  h = hsl.x;
  s = hsl.y;
  l = hsl.z;
  alpha = hsl.a;
}

/* ** Combine/Separate YCCA ** */

[[node]]
void node_composite_combine_ycca_itu_601(float y, float cb, float cr, float a, float4 &color)
{
  ycca_to_rgba_itu_601(float4(y, cb, cr, a), color);
}

[[node]]
void node_composite_combine_ycca_itu_709(float y, float cb, float cr, float a, float4 &color)
{
  ycca_to_rgba_itu_709(float4(y, cb, cr, a), color);
}

[[node]]
void node_composite_combine_ycca_jpeg(float y, float cb, float cr, float a, float4 &color)
{
  ycca_to_rgba_jpeg(float4(y, cb, cr, a), color);
}

[[node]]
void node_composite_separate_ycca_itu_601(float4 color, float &y, float &cb, float &cr, float &a)
{
  float4 ycca;
  rgba_to_ycca_itu_601(color, ycca);
  y = ycca.x;
  cb = ycca.y;
  cr = ycca.z;
  a = ycca.a;
}

[[node]]
void node_composite_separate_ycca_itu_709(float4 color, float &y, float &cb, float &cr, float &a)
{
  float4 ycca;
  rgba_to_ycca_itu_709(color, ycca);
  y = ycca.x;
  cb = ycca.y;
  cr = ycca.z;
  a = ycca.a;
}

[[node]]
void node_composite_separate_ycca_jpeg(float4 color, float &y, float &cb, float &cr, float &a)
{
  float4 ycca;
  rgba_to_ycca_jpeg(color, ycca);
  y = ycca.x;
  cb = ycca.y;
  cr = ycca.z;
  a = ycca.a;
}

/* ** Combine/Separate YUVA ** */

[[node]]
void node_composite_combine_yuva_itu_709(float y, float u, float v, float a, float4 &color)
{
  yuva_to_rgba_itu_709(float4(y, u, v, a), color);
}

[[node]]
void node_composite_separate_yuva_itu_709(float4 color, float &y, float &u, float &v, float &a)
{
  float4 yuva;
  rgba_to_yuva_itu_709(color, yuva);
  y = yuva.x;
  u = yuva.y;
  v = yuva.z;
  a = yuva.a;
}
