/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_realize_on_domain_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_realize_on_domain_float4)

#include "gpu_shader_math_matrix_transform.bsl.hh"
#include "gpu_shader_math_sample_rect.glsl"

#if defined(ENTRY_POINT_realize_on_domain_texture) || defined(GLSL_CPP_STUBS)
void realize_on_domain_texture()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 coordinates = transform_point(to_float3x3(transformation), float2(texel));
  imageStore(domain_img, texel, texture(input_tx, coordinates));
}
#endif

#if defined(ENTRY_POINT_realize_on_domain_float4x4) || defined(GLSL_CPP_STUBS)
void realize_on_domain_float4x4()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 coordinates = transform_point(to_float3x3(transformation), float2(texel));
  /* Each column of the matrix is stored in one layer of the texture. */
  for (int i = 0; i < 4; i++) {
    imageStore(domain_img, int3(texel, i), texture(input_tx, float3(coordinates, float(i))));
  }
}
#endif

template<enum Sampler sampler> void realize_on_domain()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 coordinates = transform_point(to_float3x3(transformation), float2(texel));
  imageStore(domain_img, texel, sample_rect<sampler>(input_tx, coordinates, wh));
}

#if defined(ENTRY_POINT_realize_on_domainTSampler_Box) || defined(GLSL_CPP_STUBS)
template void realize_on_domain<Sampler::Box>();
#endif

#if defined(ENTRY_POINT_realize_on_domainTSampler_Bspline) || defined(GLSL_CPP_STUBS)
template void realize_on_domain<Sampler::Bspline>();
#endif

#if defined(ENTRY_POINT_realize_on_domain_anisotropic) || defined(GLSL_CPP_STUBS)
void realize_on_domain_anisotropic()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 coordinates = transform_point(to_float3x3(transformation), float2(texel));
  imageStore(domain_img,
             texel,
             textureGrad(input_tx, coordinates, transformation[0].xy, transformation[1].xy));
}
#endif
