/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_safe_lib.glsl"

[[node]]
void node_function_project_point(const float3 point,
                                 const float4x4 transform,
                                 float3 &projected_point)
{
  float4 tmp = transform * float4(point, 1.0f);
  projected_point = safe_divide(tmp.xyz, abs(tmp.w));
}
