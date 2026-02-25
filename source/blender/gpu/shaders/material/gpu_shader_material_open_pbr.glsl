/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_safe_lib.glsl"

// TODO (OpenPBR): implement the correct GLSL shader

[[node]]
void node_bsdf_open_pbr(float base_weight,
                        float4 base_color,
                        float base_metalness,
                        float diffuse_roughness,
                        float3 N,
                        float weight,
                        Closure &result)
{
  ClosureDiffuse diffuse_data;
  diffuse_data.weight = weight;
  diffuse_data.color = base_color.rgb;
  diffuse_data.N = safe_normalize(N);

  result = closure_eval(diffuse_data);
}
