/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_info(const float light_index,
                     float3 position,
                     float4 &color,
                     float &power,
                     float3 &light_position,
                     float3 &direction,
                     float &distance,
                     float &cutoff_distance,
                     float &mask,
                     float &is_sun)
{
  node_light_info_impl(int(light_index),
                       position,
                       color,
                       power,
                       light_position,
                       direction,
                       distance,
                       cutoff_distance,
                       mask,
                       is_sun);
}
