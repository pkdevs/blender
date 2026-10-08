/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_object_info(float mat_index,
                      [[resource_table]] KernelGlobals &kg,
                      ShadingData &sd,
                      float3 &location,
                      float3 &scale,
                      float4 &color,
                      float &alpha,
                      float &object_index,
                      float &material_index,
                      float &random)
{
  const ObjectMatrices obj = kg.object_matrices_get(sd);

  float3 ob_scale;
  ob_scale.x = length(obj.model[0].xyz);
  ob_scale.y = length(obj.model[1].xyz);
  ob_scale.z = length(obj.model[2].xyz);

  location = obj.model[3].xyz;
  scale = ob_scale;
  ObjectInfos info = kg.object_infos_get(sd);
  color = info.ob_color;
  alpha = info.ob_color.a;
  object_index = info.index;
  /* TODO(fclem): Put that inside the Material UBO. */
  material_index = mat_index;
  random = info.random;
}
