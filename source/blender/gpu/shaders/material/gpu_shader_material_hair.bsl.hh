/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_common_hash.bsl.hh"
#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"

/* Principled Hair BSDF models. Must match eNodePrincipledHair_Model. */
#define HAIR_PRINCIPLED_MODEL_CHIANG 0.0f
#define HAIR_PRINCIPLED_MODEL_HUANG 1.0f

/* Principled Hair BSDF parametrizations. Must match eNodePrincipledHair_Param. */
#define HAIR_PRINCIPLED_PARAMETRIZATION_REFLECTANCE 0.0f
#define HAIR_PRINCIPLED_PARAMETRIZATION_PIGMENT 1.0f
#define HAIR_PRINCIPLED_PARAMETRIZATION_ABSORPTION 2.0f

/* Port of `bsdf_principled_hair_albedo_roughness_scale`
 * (intern/cycles/kernel/closure/bsdf_util.h). */
float hair_principled_albedo_roughness_scale(float azimuthal_roughness)
{
  float x = azimuthal_roughness;
  return (((((0.245f * x) + 5.574f) * x - 10.73f) * x + 2.532f) * x - 0.215f) * x + 5.969f;
}

/* Port of `bsdf_principled_hair_sigma_from_reflectance` (RGB instead of spectrum). */
float3 hair_principled_sigma_from_reflectance(float3 color, float azimuthal_roughness)
{
  float3 sigma = log(max(color, float3(0.0f))) /
                 hair_principled_albedo_roughness_scale(azimuthal_roughness);
  return sigma * sigma;
}

/* Port of `bsdf_principled_hair_sigma_from_concentration` (RGB instead of spectrum). */
float3 hair_principled_sigma_from_concentration(float eumelanin, float pheomelanin)
{
  const float3 eumelanin_color = float3(0.506f, 0.841f, 1.653f);
  const float3 pheomelanin_color = float3(0.343f, 0.733f, 1.924f);
  return eumelanin * eumelanin_color + pheomelanin * pheomelanin_color;
}
[[node]]
void node_bsdf_hair(float4 color,
                    float offset,
                    float roughness_u,
                    float roughness_v,
                    float3 T,
                    float weight,
                    ShadingData &sd,
                    Closure &result)
{
  color = max(color, float4(0.0f));

  if (sd.is_strand) {
    ClosureHair hair_data;
    hair_data.color = color.rgb * weight;
    hair_data.T = (length(T) > 0.0f) ? safe_normalize(T) : sd.curve_B;
    hair_data.offset = offset;
    hair_data.roughness = float2(saturate(roughness_u), saturate(roughness_v));
    result = closure_eval(sd, hair_data);
  }
  else {
    /* Hair is not physically meaningful on non-strand geometry. Fall back to a colored surface. */
    ClosureDiffuse hair_data;
    hair_data.color = color.rgb * weight;
    hair_data.N = sd.N;
    result = closure_eval(sd, hair_data);
  }
}

[[node]]
void node_bsdf_hair_principled(float4 color,
                               float melanin,
                               float melanin_redness,
                               float4 tint,
                               float3 absorption_coefficient,
                               float /*aspect_ratio*/,
                               float roughness,
                               float radial_roughness,
                               float coat,
                               float /*ior*/,
                               float offset,
                               float random_color,
                               float random_roughness,
                               float random,
                               float weight,
                               float /*R*/,
                               float /*TT*/,
                               float /*TRT*/,
                               const float model,
                               const float parametrization,
                               ShadingData &sd,
                               Closure &result)
{
  weight = max(weight, 0.0f);

  if (sd.is_strand) {
    /* Per-strand randomization, see node_principled_hair_bsdf in Cycles (`svm/closure.h`). */
    float random_eff = (random > 0.0f) ? random : wang_hash_noise(uint(sd.hair_strand_id));
    float factor_random_roughness = 1.0f + 2.0f * (random_eff - 0.5f) * random_roughness;
    float factor_random_color = 1.0f + 2.0f * (random_eff - 0.5f) * saturate(random_color);

    float roughness_eff = saturate(roughness * max(factor_random_roughness, 0.0f));
    float radial_roughness_eff = (model == HAIR_PRINCIPLED_MODEL_CHIANG) ?
                                     saturate(radial_roughness *
                                              max(factor_random_roughness, 0.0f)) :
                                     roughness_eff;

    /* Coat reduces the roughness of the first light bounce. */
    if (model == HAIR_PRINCIPLED_MODEL_CHIANG) {
      float m0_roughness = 1.0f - saturate(coat);
      roughness_eff = saturate(roughness_eff * m0_roughness);
      radial_roughness_eff = saturate(radial_roughness_eff * m0_roughness);
    }

    /* Absorption coefficient following the node parametrization. */
    float3 sigma;
    if (parametrization == HAIR_PRINCIPLED_PARAMETRIZATION_PIGMENT) {
      float melanin_eff = -log(max(1.0f - melanin * factor_random_color, 0.0001f));
      float eumelanin = melanin_eff * (1.0f - melanin_redness);
      float pheomelanin = melanin_eff * melanin_redness;
      sigma = hair_principled_sigma_from_concentration(eumelanin, pheomelanin) +
              hair_principled_sigma_from_reflectance(tint.rgb, radial_roughness_eff);
    }
    else if (parametrization == HAIR_PRINCIPLED_PARAMETRIZATION_ABSORPTION) {
      sigma = max(absorption_coefficient, float3(0.0f));
    }
    else {
      /* REFLECTANCE (default). */
      sigma = hair_principled_sigma_from_reflectance(color.rgb, radial_roughness_eff);
    }

    /* Fold the multi-lobe pigment absorption into a single tint.
     * The path length is dimensionless (see the TT lobe path `2 * cos_gamma / cos_theta` in the
     * Cycles Chiang BSDF). A path of 2 approximates the combined R/TT/TRT energy. */
    float3 hair_color = exp(-sigma * 2.0f);

    /* `aspect_ratio`, `ior`, and the per-lobe modulators (R/TT/TRT) are out of scope for the
     * single-lobe approximation (see the EEVEE hair BSDF design document). */

    ClosureHair hair_data;
    hair_data.color = hair_color * weight;
    hair_data.T = sd.curve_B;
    hair_data.offset = offset;
    hair_data.roughness = float2(max(roughness_eff, radial_roughness_eff),
                                 max(roughness_eff, radial_roughness_eff));
    result = closure_eval(sd, hair_data);
  }
  else {
    /* Hair is not physically meaningful on non-strand geometry. Fall back to a colored surface. */
    ClosureDiffuse hair_data;
    hair_data.color = color.rgb * weight;
    hair_data.N = sd.N;
    result = closure_eval(sd, hair_data);
  }
}
