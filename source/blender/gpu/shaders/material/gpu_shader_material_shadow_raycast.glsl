/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_shadow_raycast(float3 position, float softness, float4 &color)
{
  node_shadow_raycast_impl(position, softness, color);
}
