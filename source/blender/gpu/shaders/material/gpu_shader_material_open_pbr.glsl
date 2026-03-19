/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_safe_lib.glsl"

// TODO (OpenPBR): implement the correct GLSL shader

[[node]]
void node_bsdf_open_pbr(float weight,
                        float base_weight,
                        float4 base_color,
                        float base_metalness,
                        float base_diffuse_roughness,
                        float specular_weight,
                        float4 specular_color,
                        float specular_roughness,
                        float specular_roughness_anisotropy,
                        float specular_ior,
                        float transmission_weight,
                        float4 transmission_color,
                        float transmission_depth,
                        float4 transmission_scatter,
                        float transmission_scatter_anisotropy,
                        float transmission_dispersion_scale,
                        float transmission_dispersion_abbe_number,
                        float subsurface_weight,
                        float4 subsurface_color,
                        float subsurface_radius,
                        float4 subsurface_radius_scale,
                        float subsurface_scatter_anisotropy,
                        float coat_weight,
                        float4 coat_color,
                        float coat_roughness,
                        float coat_roughness_anisotropy,
                        float coat_ior,
                        float coat_darkening,
                        float fuzz_weight,
                        float4 fuzz_color,
                        float fuzz_roughness,
                        float emission_luminance,
                        float4 emission_color,
                        float thin_film_weight,
                        float thin_film_thickness,
                        float thin_film_ior,
                        float geometry_opacity,
                        float geometry_thin_walled,  //should be bool but this is not supported yet (bool is converted to float internally)
                        float3 geometry_normal,
                        float3 geometry_tangent,
                        float3 geometry_coat_normal,
                        float3 geometry_coat_tangent,
                        Closure &result)
{
  ClosureDiffuse diffuse_data;
  diffuse_data.weight = weight;
  diffuse_data.color = base_color.rgb;
  diffuse_data.N = safe_normalize(geometry_normal);

  result = closure_eval(diffuse_data);
}
