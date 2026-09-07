/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_material_open_pbr_util.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

float openpbr_specular_ior(const Specular specular, const Coat coat)
{
  float specular_ior = specular.ior;
  if (coat.weight > 0.0f) {
    /* Fixing the ratio to compensate for total internal reflections (TIR).
     * OpenPBR Surface specification v1.1.1, Eq. (74) */
    const float tir_fixed_ratio = (coat.ior > specular_ior) ? (coat.ior / specular_ior) :
                                                              (specular_ior / coat.ior);

    /* TODO(OpenPBR): we assume that the ambient ior is 1.0. */
    const float ambient_ior = 1.0f;
    /* OpenPBR Surface specification v1.1.1, Eq. (60) */
    specular_ior = mix(specular_ior / ambient_ior, tir_fixed_ratio, coat.weight);
  }

  return openpbr_modulate_ior(specular.weight, specular_ior);
}

float3 openpbr_eval_translucent(float3 weight,
                                const Specular specular,
                                const Transmission transmission,
                                const float3 N,
                                const float NV,
                                const bool thin_walled,
                                ClosureReflection &reflection_data)
{
#ifdef MAT_REFRACTION
  if (transmission.weight == 0.0f) {
    return weight;
  }

  const bool multiggx = true;
  const float2 bsdf = bsdf_lut(NV, specular.roughness, specular.ior, multiggx);
  float3 R, T;
  if (thin_walled) {
    /* Adjust transmission tint based on relative path length. */
    const float3 C = slab_transmittance_at_angle(transmission.tint, NV, specular.ior);

    /* Account for internal reflections, t' = ct1t2 + ct1(r2c)^2t2 + ct1(r2c)^4t2 + ... */
    T = safe_divide(C * square(bsdf.y), (1.0f - square(bsdf.x * C)));
    /* r' = r1 + ct1r2ct2 + ct1(r2c)^3t2 + ... */
    R = bsdf.x * (specular.tint + T * C);
  }
  else {
    /* An approximation of double reflection events, assuming the object is a slab. */
    R = bsdf.x * (specular.tint + square(bsdf.y * transmission.tint));
    T = transmission.tint * bsdf.y;
  }

  weight = openpbr_eval_translucent(
      weight, transmission.weight, R, T, N, specular, thin_walled, reflection_data);
#endif

  return weight;
}

float3 openpbr_eval_gloss(const float3 weight,
                          const Specular specular,
                          const float3 N,
                          const float NV,
                          ClosureReflection &reflection_data)
{
  if (specular.ior == 1.0f) {
    return weight;
  }

  const bool multiggx = true;
  const float reflectance = bsdf_lut(NV, specular.roughness, specular.ior, multiggx).x;

  reflection_data.N = N;
  reflection_data.roughness = specular.roughness;
  reflection_data.color += weight * reflectance * specular.tint;
  closure_eval(reflection_data);

  /* Attenuate lower layers */
  return weight * max(1.0f - reflectance, 0.0f);
}

[[node]]
void node_bsdf_open_pbr(const float float_weight,                         // #0
                        float base_weight,                                // #1
                        float4 base_color,                                // #2
                        float base_metalness,                             // #3
                        float base_diffuse_roughness,                     // #4
                        const float specular_weight,                      // #5
                        const float4 specular_color,                      // #6
                        const float specular_roughness,                   // #7
                        const float specular_roughness_anisotropy,        // #8
                        const float specular_ior,                         // #9
                        const float transmission_weight,                  // #10
                        const float4 transmission_color,                  // #11
                        const float transmission_depth,                   // #12
                        const float4 transmission_scatter,                // #13
                        const float transmission_scatter_anisotropy,      // #14
                        const float transmission_dispersion_scale,        // #15
                        const float transmission_dispersion_abbe_number,  // #16
                        const float subsurface_weight,                    // #17
                        const float4 subsurface_color,                    // #18
                        const float subsurface_radius,                    // #19
                        const float4 subsurface_radius_scale,             // #20
                        const float subsurface_scatter_anisotropy,        // #21
                        const float coat_weight,                          // #22
                        const float4 coat_color,                          // #23
                        const float coat_roughness,                       // #24
                        const float coat_roughness_anisotropy,            // #25
                        const float coat_ior,                             // #26
                        const float coat_darkening,                       // #27
                        const float fuzz_weight,                          // #28
                        const float4 fuzz_color,                          // #29
                        const float fuzz_roughness,                       // #30
                        float emission_luminance,                         // #31
                        float4 emission_color,                            // #32
                        const float thin_film_weight,                     // #33
                        const float thin_film_thickness,                  // #34
                        const float thin_film_ior,                        // #35
                        float geometry_opacity,                           // #36
                        const float geometry_thin_walled,                 // #37
                        const float3 geometry_normal,                     // #38
                        const float3 geometry_tangent,                    // #39
                        const float3 geometry_coat_normal,                // #40
                        const float3 geometry_coat_tangent,               // #41
                        Closure &result)
{
  /* Clamp parameters. OpenPBR Surface Specification, Section 5. */
  /* Base. */
  base_weight = saturate(base_weight);
  base_color = saturate(base_color);
  base_metalness = saturate(base_metalness);
  base_diffuse_roughness = saturate(base_diffuse_roughness);

  /* Specular. */
  Specular specular;
  specular.tint = saturate(specular_color.rgb);
  specular.roughness = saturate(specular_roughness);
  specular.ior = max(specular_ior, 0.0f);
  specular.weight = max(specular_weight, 0.0f);
  /* Not used by EEVEE */
  /* specular.anisotropy = saturate(specular_roughness_anisotropy); */

  /* Transmission. */
  Transmission transmission;
  /* To avoid numerical problems when a color channel is 0 we clamp it to a minimum of 1e-5 */
  transmission.tint = clamp(transmission_color.rgb, float3(1e-5f), float3(1.0f));
  transmission.weight = saturate(transmission_weight);
  /* Not used by EEVEE
   * transmission.depth = max(transmission_depth, 0.0f);
   * transmission.scatter = saturate(transmission_scatter);
   * transmission.anisotropy = clamp(transmission_scatter_anisotropy, -1.0f, 1.0f);
   * transmission.dispersion_scale = saturate(transmission_dispersion_scale);
   * transmission.dispersion_abbe_number = max(transmission_dispersion_abbe_number, 0.0f); */

  /* Subsurface. */
  Subsurface subsurface;
  subsurface.tint = saturate(subsurface_color.rgb);
  subsurface.anisotropy = clamp(subsurface_scatter_anisotropy, -1.0f, 1.0f);
  subsurface.radius = max(subsurface_radius, 0.0f) * saturate(subsurface_radius_scale.rgb);
  /* EEVEE only has Burley for subsurface, scale to approximately match the look of random walk. */
  subsurface.radius *= 4.0f * M_PI;
  subsurface.weight = saturate(subsurface_weight);

  /* Coat. */
  Coat coat;
  coat.tint = saturate(coat_color.rgb);
  coat.roughness = saturate(coat_roughness);
  coat.N = normalize_fallback(geometry_coat_normal, g_data.N);
  coat.ior = max(coat_ior, 0.0f);
  coat.weight = saturate(coat_weight);
  /* Not used by EEVEE.
   * coat.anisotropy = saturate(coat_roughness_anisotropy);
   * coat.T = geometry_coat_tangent; */
  /* TODO(OpenPBR): implement proper darkening. */
  /* coat.darkening = saturate(coat_darkening); */

  /* Fuzz. */
  Fuzz fuzz;
  fuzz.tint = saturate(fuzz_color.rgb);
  fuzz.roughness = saturate(fuzz_roughness);
  fuzz.weight = saturate(fuzz_weight);

  /* Emission */
  emission_luminance = max(emission_luminance, 0.0f);
  emission_color = max(emission_color, float4(0.0f));

  /* TODO(OpenPBR): implement thin film. */
  /* ThinFilm thin_film;
   * thin_film.weight = saturate(thin_film_weight);
   * thin_film.thickness = max(thin_film_thickness, 0.0f) * 1000.0f;
   * thin_film.ior = max(thin_film_ior, 0.0f); */

  /* Geometry. */
  geometry_opacity = saturate(geometry_opacity);
  const float3 N = normalize_fallback(geometry_normal, g_data.N);
  [[maybe_unused]] float3 T = geometry_tangent;
  const bool thin_walled = (geometry_thin_walled != 0.0f);

  /* Adjust parameters. */
  const float3 weighted_base_color = base_weight * base_color.rgb;
  specular.ior = openpbr_specular_ior(specular, coat);
  const float3 V = coordinate_incoming(g_data.P);
  const float NV = dot(N, V);
  const bool multiggx = true;

  float3 weight = float3(float_weight);
  ClosureDiffuse diffuse_data;
  ClosureReflection reflection_data;

  weight = openpbr_eval_transparency(weight, geometry_opacity);
  weight = openpbr_eval_fuzz(weight, coat, fuzz, N, V, diffuse_data);
  weight = openpbr_eval_coat(weight, coat, V);
  weight = openpbr_eval_emission(weight, emission_color.rgb, emission_luminance);
  weight = openpbr_eval_metal(
      weight, weighted_base_color, specular, base_metalness, NV, multiggx, reflection_data);
  weight = openpbr_eval_translucent(
      weight, specular, transmission, N, NV, thin_walled, reflection_data);
  weight = openpbr_eval_gloss(weight, specular, N, NV, reflection_data);
  weight = openpbr_eval_subsurface(weight, subsurface, thin_walled, N, diffuse_data);
  openpbr_eval_diffuse(weight, weighted_base_color, N, diffuse_data);

  result = Closure(0);
}
