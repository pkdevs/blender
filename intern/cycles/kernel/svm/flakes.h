/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"
#include "util/hash.h"
#include "util/math.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline float mx_flake_density_to_probability(float x)
{
  float a = -26.19771808f;
  float b = 26.39663835f;
  float c = 85.53857017f;
  float d = -102.35069432f;
  float e = -101.42634862f;
  float f = 118.45082288f;
  float xx = x * x;
  return (a * xx + b * x) / (c * xx * x + d * xx + e * x + f);
}

ccl_device_inline float3 mx_rotate_flake(const float3 p, const float3 rot_noise)
{
  float theta = M_PI_F * 2.0f * rot_noise.x;
  float phi = M_PI_F * 2.0f * rot_noise.y;
  float z = rot_noise.z * 2.0f;

  float r = sqrtf(z);
  float vx = sinf(phi) * r;
  float vy = cosf(phi) * r;
  float vz = sqrtf(2.0f - z);

  float s_theta = sinf(theta);
  float c_theta = cosf(theta);
  float sx = vx * c_theta - vy * s_theta;
  float sy = vx * s_theta + vy * c_theta;

  float3 result;
  result.x = (vx * sx - c_theta) * p.x + (vy * sx + s_theta) * p.y + (vz * sx) * p.z;
  result.y = (vx * sy - s_theta) * p.x + (vy * sy - c_theta) * p.y + (vz * sy) * p.z;
  result.z = (vx * vz) * p.x + (vy * vz) * p.y + (1.0f - z) * p.z;

  return result;
}

ccl_device_noinline void svm_node_tex_flakes_math(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexFlakes &ccl_restrict node,
    const float3 p,
    const float roughness,
    const float coverage)
{
  float probability = mx_flake_density_to_probability(coverage);
  float flake_diameter = 1.5f / sqrtf(3.0f);
  float flake_radius2 = (flake_diameter * flake_diameter) * 3.0f;

  float3 cell_center = floor(p) + make_float3(0.5f, 0.5f, 0.5f);

  float3 closest_flake_pos = make_float3(0.0f, 0.0f, 0.0f);
  float3 closest_flake_normal = make_float3(0.0f, 0.0f, 1.0f);
  float closest_flake_dist2 = 1e5f;
  float closest_flake_priority = -1.0f;
  int flake_found = 0;

  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      for (int z = -1; z <= 1; z++) {
        float3 current_cell = cell_center + make_float3((float)x, (float)y, (float)z);
        
        float cell_presence = hash_float3_to_float(current_cell);
        if (cell_presence > probability) {
          continue;
        }

        float priority = hash_float3_to_float(current_cell + make_float3(3.0f, 3.0f, 3.0f));
        float3 flake_pos = current_cell + hash_float3_to_float3(current_cell + make_float3(1.0f, 1.0f, 1.0f)) - make_float3(0.5f, 0.5f, 0.5f);
        
        float3 delta = p - flake_pos;
        float dist2 = len_squared(delta);

        if (dist2 < flake_radius2) {
          if (priority > closest_flake_priority) {
            closest_flake_priority = priority;
            closest_flake_dist2 = dist2;
            closest_flake_pos = current_cell;
            
            float3 rot_noise = hash_float3_to_float3(current_cell);
            closest_flake_normal = mx_rotate_flake(make_float3(0.0f, 0.0f, 1.0f), rot_noise);
            flake_found = 1;
          }
        }
      }
    }
  }

  float3 final_color = make_float3(0.0f, 0.0f, 0.0f);
  float3 final_normal = make_float3(0.0f, 0.0f, 1.0f);
  float final_fac = 0.0f;
  float final_presence = 0.0f;

  if (flake_found) {
    float3 rot_noise = hash_float3_to_float3(closest_flake_pos + make_float3(2.0f, 2.0f, 2.0f));
    
    float alpha2 = roughness * roughness;
    float theta_noise = rot_noise.x;
    float phi_noise = rot_noise.y;
    float tan_theta = sqrtf(theta_noise * alpha2 / (1.0f - theta_noise));
    float sin_theta = tan_theta / sqrtf(1.0f + tan_theta * tan_theta);
    float cos_theta = 1.0f / sqrtf(1.0f + tan_theta * tan_theta);
    float phi = 2.0f * M_PI_F * phi_noise;
    
    float3 local_dir = make_float3(cosf(phi) * sin_theta, sinf(phi) * sin_theta, cos_theta);
    
    float3 t = make_float3(1.0f, 0.0f, 0.0f);
    float3 b = make_float3(0.0f, 1.0f, 0.0f);
    
    float3 perturbed_normal = local_dir.x * t + local_dir.y * b + local_dir.z * closest_flake_normal;
    final_normal = normalize(perturbed_normal);
    
    final_fac = hash_float3_to_float(closest_flake_pos + make_float3(4.0f, 4.0f, 4.0f));
    final_color = hash_float3_to_float3(closest_flake_pos + make_float3(5.0f, 5.0f, 5.0f));
    final_presence = 1.0f;
  }

  if (stack_valid(node.color_offset))
    stack_store_float3(stack, node.color_offset, final_color);
  if (stack_valid(node.normal_offset))
    stack_store_float3(stack, node.normal_offset, final_normal);
  if (stack_valid(node.fac_offset))
    stack_store_float(stack, node.fac_offset, final_fac);
  if (stack_valid(node.presence_offset))
    stack_store_float(stack, node.presence_offset, final_presence);
}

ccl_device_noinline void svm_node_tex_flakes(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexFlakes &ccl_restrict node)
{
  const float3 vector = stack_load(stack, node.vector);
  const float w = stack_load(stack, node.w);
  const float scale = stack_load(stack, node.scale);
  const float roughness = stack_load(stack, node.roughness);
  const float coverage = stack_load(stack, node.coverage);

  float3 p = make_float3(0.0f, 0.0f, 0.0f);

  if (node.dimensions == 1) {
    p = make_float3(w * scale, 0.0f, 0.0f);
  }
  else if (node.dimensions == 2) {
    p = make_float3(vector.x, vector.y, 0.0f) * scale;
  }
  else if (node.dimensions == 3) {
    p = vector * scale;
  }
  else if (node.dimensions == 4) {
    p = make_float3(vector.x, vector.y, vector.z + w) * scale;
  }

  svm_node_tex_flakes_math(stack, node, p, roughness, coverage);
}

CCL_NAMESPACE_END
