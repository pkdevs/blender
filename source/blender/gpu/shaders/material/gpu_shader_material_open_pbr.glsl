/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_math.glsl"
#include "gpu_shader_math_fast_lib.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

// TODO (OpenPBR): implement the correct GLSL shader

[[node]]
void node_bsdf_open_pbr(
    float weight,                               // #0
    float base_weight,                          // #1
    float4 base_color,                          // #2
    float base_metalness,                       // #3
    float base_diffuse_roughness,               // #4
    float specular_weight,                      // #5
    float4 specular_color,                      // #6
    float specular_roughness,                   // #7
    float specular_roughness_anisotropy,        // #8
    float specular_ior,                         // #9
    float transmission_weight,                  // #10
    float4 transmission_color,                  // #11
    float transmission_depth,                   // #12
    float4 transmission_scatter,                // #13
    float transmission_scatter_anisotropy,      // #14
    float transmission_dispersion_scale,        // #15
    float transmission_dispersion_abbe_number,  // #16
    float subsurface_weight,                    // #17
    float4 subsurface_color,                    // #18
    float subsurface_radius,                    // #19
    float4 subsurface_radius_scale,             // #20
    float subsurface_scatter_anisotropy,        // #21
    float coat_weight,                          // #22
    float4 coat_color,                          // #23
    float coat_roughness,                       // #24
    float coat_roughness_anisotropy,            // #25
    float coat_ior,                             // #26
    float coat_darkening,                       // #27
    float fuzz_weight,                          // #28
    float4 fuzz_color,                          // #29
    float fuzz_roughness,                       // #30
    float emission_luminance,                   // #31
    float4 emission_color,                      // #32
    float thin_film_weight,                     // #33
    float thin_film_thickness,                  // #34
    float thin_film_ior,                        // #35
    float geometry_opacity,                     // #36
    float geometry_thin_walled,                 // should be bool but this is not supported
    //                             // yet (bool is converted to float internally)
    float3 geometry_normal,        // #38
    float3 geometry_tangent,       // #39
    float3 geometry_coat_normal,   // #40
    float3 geometry_coat_tangent,  // #41
    Closure &result)
{

  bool do_multiscatter = true;
  // base_color = max(base_color, float4(0.0f));
  // float4 clamped_base_color = min(base_color, float4(1.0f));
  float3 N = safe_normalize(geometry_normal);
  float3 CN = safe_normalize(geometry_coat_normal);
  float3 V = coordinate_incoming(g_data.P);
  float NV = dot(N, V);

  /////////////////////////////////////////////
  // temporary OpenPBR parameter
  ////////////////////////////////////////////

  // glossy component
  const float oneMinusSpecularIor = 1.f - specular_ior;
  const float onePlusSpecularIor = 1.f + specular_ior;
  const float oneMinusSpecularIOROveronePlusSpecularIOR = safe_divide(oneMinusSpecularIor,
                                                                      onePlusSpecularIor);
  const float Fs = oneMinusSpecularIOROveronePlusSpecularIOR *
                   oneMinusSpecularIOROveronePlusSpecularIOR;
  const float xiSpecular = clamp(specular_weight, 0.f, safe_divide(1.f, Fs));
  const float epsilonSpecular = sign(specular_ior - 1.f) * safe_sqrt(xiSpecular * Fs);
  const float modulated_specular_ior = safe_divide(1.f + epsilonSpecular, 1.f - epsilonSpecular);

  float3 reflection_color = float3(0.0f);
  if (base_metalness > 0.0f) {
    float3 F0 = base_color.rgb * base_weight;
    float3 F82 = min(specular_color.rgb * specular_weight, float3(1.0f));
    float3 metal_brdf;
    brdf_f82_tint_lut(F0, F82, NV, specular_roughness, do_multiscatter, metal_brdf);
    reflection_color = weight * base_metalness * metal_brdf;
    /* Attenuate lower layers */
    weight *= max((1.0f - base_metalness), 0.0f);

    ClosureReflection reflection_data;
    reflection_data.N = N;
    reflection_data.roughness = specular_roughness;
    reflection_data.color = reflection_color.rgb;
    reflection_data.weight = base_metalness;
    closure_eval(reflection_data);
  }

  if (specular_weight > 0.0f && modulated_specular_ior != 1.0f) {
    float f0 = F0_from_ior(modulated_specular_ior);
    float3 F0 = float3(f0) * specular_color.rgb;
    F0 = clamp(F0, float3(0.0f), float3(1.0f));
    float3 F90 = float3(1.0f);
    float3 specular_brdf, unused;
    bsdf_lut(F0,
             F90,
             float3(0.0f),
             NV,
             specular_roughness,
             modulated_specular_ior,
             do_multiscatter,
             specular_brdf,
             unused);

    ClosureReflection reflection_data;
    reflection_data.N = N;
    reflection_data.roughness = specular_roughness;
    reflection_data.color = weight * specular_brdf;
    /* `weight` is already applied in `color`. */
    reflection_data.weight = 1.0f;
    closure_eval(reflection_data);
  }

  ClosureDiffuse diffuse_data;
  diffuse_data.weight = weight;
  diffuse_data.color = base_color.rgb * base_weight;
  diffuse_data.N = N;

  closure_eval(diffuse_data);

  result = Closure(0);
}
