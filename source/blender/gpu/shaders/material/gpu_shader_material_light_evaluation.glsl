/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_evaluation_common(
    const float light_index, float3 position, float &mask, float3 &direction, float &distance)
{
  node_light_evaluation_common_impl(light_index, position, direction, distance, mask);
}

[[node]]
void node_light_evaluation_diffuse(const float light_index,
                                   float3 position,
                                   float3 normal,
                                   float roughness,
                                   float &factor,
                                   float &mask,
                                   float3 &direction,
                                   float &distance)
{
  node_light_evaluation_common_impl(light_index, position, direction, distance, mask);
  node_light_evaluation_impl<true>(light_index, position, normal, roughness, factor);
}

[[node]]
void node_light_evaluation_glossy(const float light_index,
                                  float3 position,
                                  float3 normal,
                                  float roughness,
                                  float &factor,
                                  float &mask,
                                  float3 &direction,
                                  float &distance)
{
  node_light_evaluation_common_impl(light_index, position, direction, distance, mask);
  node_light_evaluation_impl<false>(light_index, position, normal, roughness, factor);
}
