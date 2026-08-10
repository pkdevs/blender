/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_hash.glsl"

float mx_flake_density_to_probability(float x)
{
  float a = -26.19771808;
  float b = 26.39663835;
  float c = 85.53857017;
  float d = -102.35069432;
  float e = -101.42634862;
  float f = 118.45082288;
  float xx = x * x;
  return (a * xx + b * x) / (c * xx * x + d * xx + e * x + f);
}

vec3 mx_rotate_flake(vec3 p, vec3 rot_noise)
{
  float theta = 3.14159265358979323846 * 2.0 * rot_noise.x;
  float phi = 3.14159265358979323846 * 2.0 * rot_noise.y;
  float z = rot_noise.z * 2.0;

  float r = sqrt(z);
  float vx = sin(phi) * r;
  float vy = cos(phi) * r;
  float vz = sqrt(2.0 - z);

  float s_theta = sin(theta);
  float c_theta = cos(theta);
  float sx = vx * c_theta - vy * s_theta;
  float sy = vx * s_theta + vy * c_theta;

  vec3 result;
  result.x = (vx * sx - c_theta) * p.x + (vy * sx + s_theta) * p.y + (vz * sx) * p.z;
  result.y = (vx * sy - s_theta) * p.x + (vy * sy - c_theta) * p.y + (vz * sy) * p.z;
  result.z = (vx * vz) * p.x + (vy * vz) * p.y + (1.0 - z) * p.z;

  return result;
}

void node_tex_flakes_math(vec3 p, float roughness, float coverage, out vec4 out_color, out vec3 out_normal, out float out_fac, out float out_presence)
{
  float probability = mx_flake_density_to_probability(coverage);
  float flake_diameter = 1.5 / sqrt(3.0);
  float flake_radius2 = (flake_diameter * flake_diameter) * 3.0;

  vec3 cell_center = floor(p) + vec3(0.5);

  vec3 closest_flake_pos = vec3(0.0);
  vec3 closest_flake_normal = vec3(0.0, 0.0, 1.0);
  float closest_flake_dist2 = 1e5;
  float closest_flake_priority = -1.0;
  bool flake_found = false;

  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      for (int z = -1; z <= 1; z++) {
        vec3 current_cell = cell_center + vec3(float(x), float(y), float(z));
        
        float cell_presence = hash_vec3_to_float(current_cell);
        if (cell_presence > probability) {
          continue;
        }

        float priority = hash_vec3_to_float(current_cell + vec3(3.0));
        vec3 flake_pos = current_cell + hash_vec3_to_vec3(current_cell + vec3(1.0)) - vec3(0.5);
        
        vec3 delta = p - flake_pos;
        float dist2 = dot(delta, delta);

        if (dist2 < flake_radius2) {
          if (priority > closest_flake_priority) {
            closest_flake_priority = priority;
            closest_flake_dist2 = dist2;
            closest_flake_pos = current_cell;
            
            vec3 rot_noise = hash_vec3_to_vec3(current_cell);
            closest_flake_normal = mx_rotate_flake(vec3(0.0, 0.0, 1.0), rot_noise);
            flake_found = true;
          }
        }
      }
    }
  }

  if (flake_found) {
    vec3 rot_noise = hash_vec3_to_vec3(closest_flake_pos + vec3(2.0));
    
    float alpha2 = roughness * roughness;
    float theta_noise = rot_noise.x;
    float phi_noise = rot_noise.y;
    float tan_theta = sqrt(theta_noise * alpha2 / (1.0 - theta_noise));
    float sin_theta = tan_theta / sqrt(1.0 + tan_theta * tan_theta);
    float cos_theta = 1.0 / sqrt(1.0 + tan_theta * tan_theta);
    float phi = 2.0 * 3.14159265358979323846 * phi_noise;
    
    vec3 local_dir = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
    
    vec3 t = vec3(1.0, 0.0, 0.0);
    vec3 b = vec3(0.0, 1.0, 0.0);
    
    vec3 perturbed_normal = local_dir.x * t + local_dir.y * b + local_dir.z * closest_flake_normal;
    out_normal = normalize(perturbed_normal);
    
    out_fac = hash_vec3_to_float(closest_flake_pos + vec3(4.0));
    out_color = vec4(hash_vec3_to_vec3(closest_flake_pos + vec3(5.0)), 1.0);
    out_presence = 1.0;
  }
  else {
    out_normal = vec3(0.0, 0.0, 1.0);
    out_fac = 0.0;
    out_color = vec4(0.0);
    out_presence = 0.0;
  }
}

/* Flakes 1D */
[[node]]
void node_tex_flakes_1d(vec3 coord,
                        float w,
                        float scale,
                        float roughness,
                        float coverage,
                        out vec4 color,
                        out vec3 normal,
                        out float fac,
                        out float presence)
{
  vec3 p = vec3(w * scale, 0.0, 0.0);
  node_tex_flakes_math(p, roughness, coverage, color, normal, fac, presence);
}

/* Flakes 2D */
[[node]]
void node_tex_flakes_2d(vec3 coord,
                        float w,
                        float scale,
                        float roughness,
                        float coverage,
                        out vec4 color,
                        out vec3 normal,
                        out float fac,
                        out float presence)
{
  vec3 p = vec3(coord.xy * scale, 0.0);
  node_tex_flakes_math(p, roughness, coverage, color, normal, fac, presence);
}

/* Flakes 3D */
[[node]]
void node_tex_flakes_3d(vec3 coord,
                        float w,
                        float scale,
                        float roughness,
                        float coverage,
                        out vec4 color,
                        out vec3 normal,
                        out float fac,
                        out float presence)
{
  vec3 p = coord * scale;
  node_tex_flakes_math(p, roughness, coverage, color, normal, fac, presence);
}

/* Flakes 4D */
[[node]]
void node_tex_flakes_4d(vec3 coord,
                        float w,
                        float scale,
                        float roughness,
                        float coverage,
                        out vec4 color,
                        out vec3 normal,
                        out float fac,
                        out float presence)
{
  vec3 p = vec3(coord.xy, coord.z + w) * scale;
  node_tex_flakes_math(p, roughness, coverage, color, normal, fac, presence);
}
