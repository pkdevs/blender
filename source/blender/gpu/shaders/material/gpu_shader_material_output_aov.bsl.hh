/* SPDX-FileCopyrightText: 2020-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_output_aov(float4 color,
                     float value,
                     float hash,
                     [[resource_table]] KernelGlobals &kg,
                     const ShadingData &sd,
                     Closure & /*dummy*/)
{
#if defined(GPU_FRAGMENT_SHADER) || defined(GLSL_CPP_STUBS)
#  if defined(OBINFO_LIB) || defined(GLSL_CPP_STUBS)
  output_aov(int2(sd.frag_co.xy),
             color,
             value,
             floatBitsToUint(hash),
             sd.holdout, /* TODO(fclem): This is not supposed to be read. */
             kg.object_infos_get(sd).flag);
#  else
  output_aov(int2(sd.frag_co.xy), color, value, floatBitsToUint(hash), 0.0f, 0u);
#  endif
#endif
}
