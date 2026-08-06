/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"
#include "util/hash.h"
#include "util/math.h"

CCL_NAMESPACE_BEGIN

ccl_device_noinline void svm_node_tex_flakes_1d(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexFlakes &ccl_restrict node,
    const float w, const float scale, const float density, 
    const float flake_size, const float randomness)
{
  float p = w * scale;
  float cell = floorf(p);

  float best_fac = 0.0f;
  float3 best_normal = make_float3(0.0f, 0.0f, 1.0f);
  float3 best_color = make_float3(0.0f, 0.0f, 0.0f);
  float highest_z = -1000.0f;

  for (int i = -1; i <= 1; i++) {
    float neighbor = cell + (float)i;
    float hash_id = hash_float_to_float(neighbor);

    if (hash_id < density) {
      float h = hash_float_to_float(neighbor + 0.5f);
      float pos = neighbor + h * randomness;
      float dist = fabsf(p - pos);

      if (dist < flake_size / 2.0f) {
        float flake_z = hash_float_to_float(neighbor + 12.34f);
        if (flake_z > highest_z) {
          highest_z = flake_z;
          best_fac = 1.0f;
          best_normal = normalize(hash_float_to_float3(neighbor + 56.78f) * 2.0f - make_float3(1.0f, 1.0f, 1.0f));
          best_color = hash_float_to_float3(neighbor + 90.12f);
        }
      }
    }
  }

  if (stack_valid(node.color_offset)) stack_store_float3(stack, node.color_offset, best_color);
  if (stack_valid(node.normal_offset)) stack_store_float3(stack, node.normal_offset, best_normal);
  if (stack_valid(node.fac_offset)) stack_store_float(stack, node.fac_offset, best_fac);
}

ccl_device_noinline void svm_node_tex_flakes_2d(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexFlakes &ccl_restrict node,
    const float3 vector, const float scale, const float density, 
    const float flake_size, const float randomness)
{
  float2 p = make_float2(vector.x, vector.y) * scale;
  float2 cell = floor(p);

  float best_fac = 0.0f;
  float3 best_normal = make_float3(0.0f, 0.0f, 1.0f);
  float3 best_color = make_float3(0.0f, 0.0f, 0.0f);
  float highest_z = -1000.0f;

  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      float2 neighbor = cell + make_float2((float)i, (float)j);
      float hash_id = hash_float2_to_float(neighbor);

      if (hash_id < density) {
        float2 h = hash_float2_to_float2(neighbor + make_float2(0.5f, 0.5f));
        float2 pos = neighbor + h * randomness;
        float dist = len(p - pos);

        if (dist < flake_size / 2.0f) {
          float flake_z = hash_float2_to_float(neighbor + make_float2(12.34f, 12.34f));
          if (flake_z > highest_z) {
            highest_z = flake_z;
            best_fac = 1.0f;
            best_normal = normalize(hash_float2_to_float3(neighbor + make_float2(56.78f, 56.78f)) * 2.0f - make_float3(1.0f, 1.0f, 1.0f));
            best_color = hash_float2_to_float3(neighbor + make_float2(90.12f, 90.12f));
          }
        }
      }
    }
  }

  if (stack_valid(node.color_offset)) stack_store_float3(stack, node.color_offset, best_color);
  if (stack_valid(node.normal_offset)) stack_store_float3(stack, node.normal_offset, best_normal);
  if (stack_valid(node.fac_offset)) stack_store_float(stack, node.fac_offset, best_fac);
}

ccl_device_noinline void svm_node_tex_flakes_3d(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexFlakes &ccl_restrict node,
    const float3 vector, const float scale, const float density, 
    const float flake_size, const float randomness)
{
  float3 p = vector * scale;
  float3 cell = floor(p);

  float best_fac = 0.0f;
  float3 best_normal = make_float3(0.0f, 0.0f, 1.0f);
  float3 best_color = make_float3(0.0f, 0.0f, 0.0f);
  float highest_z = -1000.0f;

  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      for (int k = -1; k <= 1; k++) {
        float3 neighbor = cell + make_float3((float)i, (float)j, (float)k);
        float hash_id = hash_float3_to_float(neighbor);

        if (hash_id < density) {
          float3 h = hash_float3_to_float3(neighbor + make_float3(0.5f, 0.5f, 0.5f));
          float3 pos = neighbor + h * randomness;
          float dist = len(p - pos);

          if (dist < flake_size / 2.0f) {
            float flake_z = hash_float3_to_float(neighbor + make_float3(12.34f, 12.34f, 12.34f));
            if (flake_z > highest_z) {
              highest_z = flake_z;
              best_fac = 1.0f;
              best_normal = normalize(hash_float3_to_float3(neighbor + make_float3(56.78f, 56.78f, 56.78f)) * 2.0f - make_float3(1.0f, 1.0f, 1.0f));
              best_color = hash_float3_to_float3(neighbor + make_float3(90.12f, 90.12f, 90.12f));
            }
          }
        }
      }
    }
  }

  if (stack_valid(node.color_offset)) stack_store_float3(stack, node.color_offset, best_color);
  if (stack_valid(node.normal_offset)) stack_store_float3(stack, node.normal_offset, best_normal);
  if (stack_valid(node.fac_offset)) stack_store_float(stack, node.fac_offset, best_fac);
}

ccl_device_noinline void svm_node_tex_flakes_4d(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexFlakes &ccl_restrict node,
    const float3 vector, const float w, const float scale, const float density, 
    const float flake_size, const float randomness)
{
  float4 p = make_float4(vector.x, vector.y, vector.z, w) * scale;
  float4 cell = floor(p);

  float best_fac = 0.0f;
  float3 best_normal = make_float3(0.0f, 0.0f, 1.0f);
  float3 best_color = make_float3(0.0f, 0.0f, 0.0f);
  float highest_z = -1000.0f;

  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      for (int k = -1; k <= 1; k++) {
        for (int l = -1; l <= 1; l++) {
          float4 neighbor = cell + make_float4((float)i, (float)j, (float)k, (float)l);
          float hash_id = hash_float4_to_float(neighbor);

          if (hash_id < density) {
            float4 h = hash_float4_to_float4(neighbor + make_float4(0.5f, 0.5f, 0.5f, 0.5f));
            float4 pos = neighbor + h * randomness;
            float dist = len(p - pos);

            if (dist < flake_size / 2.0f) {
              float flake_z = hash_float4_to_float(neighbor + make_float4(12.34f, 12.34f, 12.34f, 12.34f));
              if (flake_z > highest_z) {
                highest_z = flake_z;
                best_fac = 1.0f;
                best_normal = normalize(hash_float4_to_float3(neighbor + make_float4(56.78f, 56.78f, 56.78f, 56.78f)) * 2.0f - make_float3(1.0f, 1.0f, 1.0f));
                best_color = hash_float4_to_float3(neighbor + make_float4(90.12f, 90.12f, 90.12f, 90.12f));
              }
            }
          }
        }
      }
    }
  }

  if (stack_valid(node.color_offset)) stack_store_float3(stack, node.color_offset, best_color);
  if (stack_valid(node.normal_offset)) stack_store_float3(stack, node.normal_offset, best_normal);
  if (stack_valid(node.fac_offset)) stack_store_float(stack, node.fac_offset, best_fac);
}

ccl_device_noinline void svm_node_tex_flakes(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexFlakes &ccl_restrict node)
{
  const float3 vector = stack_load(stack, node.vector);
  const float w = stack_load(stack, node.w);
  const float scale = stack_load(stack, node.scale);
  const float density = stack_load(stack, node.density);
  const float flake_size = stack_load(stack, node.flake_size);
  const float randomness = stack_load(stack, node.randomness);

  switch (node.dimensions) {
    case 1:
      svm_node_tex_flakes_1d(stack, node, w, scale, density, flake_size, randomness);
      break;
    case 2:
      svm_node_tex_flakes_2d(stack, node, vector, scale, density, flake_size, randomness);
      break;
    case 3:
      svm_node_tex_flakes_3d(stack, node, vector, scale, density, flake_size, randomness);
      break;
    case 4:
      svm_node_tex_flakes_4d(stack, node, vector, w, scale, density, flake_size, randomness);
      break;
    default:
      kernel_assert(0);
      break;
  }
}

CCL_NAMESPACE_END
