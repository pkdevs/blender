/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

[[node]]
void node_light_accumulation_aov(const float light_index,
                                 float4 light,
                                 [[resource_table]] KernelGlobals &kg,
                                 ShadingData &sd,
                                 Closure &result)
{
  light = max(light, float4(0.0f));
  node_light_accumulation_aov_impl(kg, sd, int(light_index), light.rgb, result);
}
