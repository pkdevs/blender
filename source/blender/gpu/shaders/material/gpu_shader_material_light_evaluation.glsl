/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_evaluation(const float light_index,
                           float3 position,
                           float3 normal,
                           float roughness,
                           float4 &color,
                           float &factor,
                           float &energy_scale)
{
  node_light_evaluation_impl(
      int(light_index), position, normal, roughness, color, factor, energy_scale);
}
