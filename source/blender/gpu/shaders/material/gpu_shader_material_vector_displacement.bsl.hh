/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_transform_utils.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"

[[node]]
void node_vector_displacement_tangent(float4 vector,
                                      float midlevel,
                                      float scale,
                                      float4 T,
                                      float3 &result,
                                      [[resource_table]] KernelGlobals &kg,
                                      ShadingData &sd)
{
  float3 oN, oT, oB;
  normal_transform_world_to_object(g_data.N, oN, kg, sd);
  normal_transform_world_to_object(T.xyz, oT, kg, sd);
  oN = normalize(oN);
  oT = normalize(oT);
  oB = T.w * safe_normalize(cross(oN, oT));

  float3 disp = (vector.xyz - midlevel) * scale;
  disp = disp.x * oT + disp.y * oN + disp.z * oB;
  direction_transform_object_to_world(disp, result, kg, sd);
}

[[node]]
void node_vector_displacement_object(float4 vector,
                                     float midlevel,
                                     float scale,
                                     float3 &result,
                                     [[resource_table]] KernelGlobals &kg,
                                     ShadingData &sd)
{
  float3 disp = (vector.xyz - midlevel) * scale;
  direction_transform_object_to_world(disp, result, kg, sd);
}

[[node]]
void node_vector_displacement_world(float4 vector, float midlevel, float scale, float3 &result)
{
  result = (vector.xyz - midlevel) * scale;
}
