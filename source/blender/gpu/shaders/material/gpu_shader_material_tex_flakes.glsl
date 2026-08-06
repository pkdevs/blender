/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_hash.glsl"

/* Flakes 1D */
[[node]]
void node_tex_flakes_1d(float3 coord,
                        float w,
                        float scale,
                        float density,
                        float flake_size,
                        float randomness,
                        out float4 color,
                        out float3 normal,
                        out float fac)
{
  float p = w * scale;
  float cell = floor(p);

  float best_fac = 0.0;
  float3 best_normal = float3(0.0, 0.0, 1.0);
  float4 best_color = float4(0.0);
  float highest_z = -1000.0;

  for (int i = -1; i <= 1; i++) {
    float neighbor = cell + float(i);
    float hash_id = hash_float_to_float(neighbor);

    if (hash_id < density) {
      float h = hash_float_to_float(neighbor + 0.5);
      float pos = neighbor + h * randomness;
      float dist = abs(p - pos);

      if (dist < flake_size / 2.0) {
        float flake_z = hash_float_to_float(neighbor + 12.34);
        if (flake_z > highest_z) {
          highest_z = flake_z;
          best_fac = 1.0;
          best_normal = normalize(hash_float_to_vec3(neighbor + 56.78) * 2.0 - 1.0);
          best_color = float4(hash_float_to_vec3(neighbor + 90.12), 1.0);
        }
      }
    }
  }

  color = best_color;
  normal = best_normal;
  fac = best_fac;
}

/* Flakes 2D */
[[node]]
void node_tex_flakes_2d(float3 coord,
                        float w,
                        float scale,
                        float density,
                        float flake_size,
                        float randomness,
                        out float4 color,
                        out float3 normal,
                        out float fac)
{
  float2 p = coord.xy * scale;
  float2 cell = floor(p);

  float best_fac = 0.0;
  float3 best_normal = float3(0.0, 0.0, 1.0);
  float4 best_color = float4(0.0);
  float highest_z = -1000.0;

  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      float2 neighbor = cell + float2(i, j);
      float hash_id = hash_vec2_to_float(neighbor);

      if (hash_id < density) {
        float2 h = hash_vec2_to_vec2(neighbor + 0.5);
        float2 pos = neighbor + h * randomness;
        float dist = length(p - pos);

        if (dist < flake_size / 2.0) {
          float flake_z = hash_vec2_to_float(neighbor + 12.34);
          if (flake_z > highest_z) {
            highest_z = flake_z;
            best_fac = 1.0;
            best_normal = normalize(hash_vec2_to_vec3(neighbor + 56.78) * 2.0 - 1.0);
            best_color = float4(hash_vec2_to_vec3(neighbor + 90.12), 1.0);
          }
        }
      }
    }
  }

  color = best_color;
  normal = best_normal;
  fac = best_fac;
}

/* Flakes 3D */
[[node]]
void node_tex_flakes_3d(float3 coord,
                        float w,
                        float scale,
                        float density,
                        float flake_size,
                        float randomness,
                        out float4 color,
                        out float3 normal,
                        out float fac)
{
  float3 p = coord * scale;
  float3 cell = floor(p);

  float best_fac = 0.0;
  float3 best_normal = float3(0.0, 0.0, 1.0);
  float4 best_color = float4(0.0);
  float highest_z = -1000.0;

  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      for (int k = -1; k <= 1; k++) {
        float3 neighbor = cell + float3(i, j, k);
        float hash_id = hash_vec3_to_float(neighbor);

        if (hash_id < density) {
          float3 h = hash_vec3_to_vec3(neighbor + 0.5);
          float3 pos = neighbor + h * randomness;
          float dist = length(p - pos);

          if (dist < flake_size / 2.0) {
            float flake_z = hash_vec3_to_float(neighbor + 12.34);
            if (flake_z > highest_z) {
              highest_z = flake_z;
              best_fac = 1.0;
              best_normal = normalize(hash_vec3_to_vec3(neighbor + 56.78) * 2.0 - 1.0);
              best_color = float4(hash_vec3_to_vec3(neighbor + 90.12), 1.0);
            }
          }
        }
      }
    }
  }

  color = best_color;
  normal = best_normal;
  fac = best_fac;
}

/* Flakes 4D */
[[node]]
void node_tex_flakes_4d(float3 coord,
                        float w,
                        float scale,
                        float density,
                        float flake_size,
                        float randomness,
                        out float4 color,
                        out float3 normal,
                        out float fac)
{
  float4 p = float4(coord, w) * scale;
  float4 cell = floor(p);

  float best_fac = 0.0;
  float3 best_normal = float3(0.0, 0.0, 1.0);
  float4 best_color = float4(0.0);
  float highest_z = -1000.0;

  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      for (int k = -1; k <= 1; k++) {
        for (int l = -1; l <= 1; l++) {
          float4 neighbor = cell + float4(i, j, k, l);
          float hash_id = hash_vec4_to_float(neighbor);

          if (hash_id < density) {
            float4 h = hash_vec4_to_vec4(neighbor + 0.5);
            float4 pos = neighbor + h * randomness;
            float dist = length(p - pos);

            if (dist < flake_size / 2.0) {
              float flake_z = hash_vec4_to_float(neighbor + 12.34);
              if (flake_z > highest_z) {
                highest_z = flake_z;
                best_fac = 1.0;
                best_normal = normalize(hash_vec4_to_vec3(neighbor + 56.78) * 2.0 - 1.0);
                best_color = float4(hash_vec4_to_vec3(neighbor + 90.12), 1.0);
              }
            }
          }
        }
      }
    }
  }

  color = best_color;
  normal = best_normal;
  fac = best_fac;
}
