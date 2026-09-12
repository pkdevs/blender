/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_shadow_raycast(const float light_index,
                         float3 position,
                         float softness,
                         float4 &color,
                         [[resource_table]] KernelGlobals &kg,
                         const ShadingData &sd)
{
  node_shadow_raycast_impl(kg, sd, int(light_index), position, softness, color);
}
