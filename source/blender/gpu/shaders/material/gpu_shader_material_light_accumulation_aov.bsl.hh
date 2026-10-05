/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

[[node]]
void node_light_accumulation_aov(float4 light_accumulated, float4 light, float4 &result)
{
  light = max(light, float4(0.0f));
  result = light + light_accumulated;
  result.a = 1.0f;
}
