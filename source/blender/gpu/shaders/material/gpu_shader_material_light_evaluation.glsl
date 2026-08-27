/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_evaluation(const float light_index,
                           float3 position,
                           float3 normal,
                           float roughness,
                           float3 &direction,
                           float &distance,
                           float &mask,
                           float &factor)
{
  node_light_evaluation_impl(int(light_index), position, direction, distance, mask);
}

[[node]]
void node_light_evaluation_with_factor(const float light_index,
                                       float3 position,
                                       float3 normal,
                                       float roughness,
                                       float3 &direction,
                                       float &distance,
                                       float &mask,
                                       float &factor)
{
  node_light_evaluation_with_factor_impl(
      int(light_index), position, normal, roughness, direction, distance, mask, factor);
}
