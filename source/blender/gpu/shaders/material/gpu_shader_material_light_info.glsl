/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_info(
    float4 &color, float &power, float3 &position, float3 &direction, float &distance, float &mask)
{
  node_light_info_impl(color, power, position, direction, distance, mask);
}
