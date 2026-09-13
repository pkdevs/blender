/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_transform_utils.bsl.hh"

[[node]]
void node_tex_coord_position(float3 &out_pos)
{
  out_pos = g_data.P;
}

[[node]]
void node_tex_coord(float4x4 obmatinv,
                    float3 attr_orco,
                    float4 attr_uv,
                    [[resource_table]] KernelGlobals &kg,
                    ShadingData &sd,
                    float3 &generated,
                    float3 &normal,
                    float3 &uv,
                    float3 &object,
                    float3 &camera,
                    float3 &window,
                    float3 &reflection)
{
  Coordinates coords = coordinate_impl(kg, sd, g_data.P, g_data.N);
  generated = attr_orco;
  normal_transform_world_to_object(g_data.N, kg, sd, normal);
  uv = attr_uv.xyz;
  bool valid_mat = (obmatinv[3][3] != 0.0f);
  if (valid_mat) {
    object = (obmatinv * float4(g_data.P, 1.0f)).xyz;
  }
  else {
    point_transform_world_to_object(g_data.P, kg, sd, object);
  }
  camera = coords.camera;
  window = coords.screen;
  reflection = coords.reflect;
}
