/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_accumulation(float4 diffuse_light,
                             float4 diffuse_color,
                             float4 glossy_light,
                             float4 glossy_color,
                             float weight,
                             Closure &result)
{
  diffuse_light = max(diffuse_light, float4(0.0f));
  diffuse_color = saturate(diffuse_color);
  glossy_light = max(glossy_light, float4(0.0f));
  glossy_color = saturate(glossy_color);
#if 0
  transmission_light = max(transmission_light, float4(0.0f));
  transmission_color = saturate(transmission_color);
#endif

  node_light_accumulation_impl(
      diffuse_light, diffuse_color, glossy_light, glossy_color, weight, result);
}
