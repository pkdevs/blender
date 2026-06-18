/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/closure/alloc.h"
#include "kernel/closure/bsdf.h"
#include "kernel/closure/bsdf_util.h"
#include "kernel/closure/bssrdf.h"
#include "kernel/closure/emissive.h"
#include "kernel/closure/volume.h"

#include "kernel/geom/curve.h"
#include "kernel/geom/object.h"
#include "kernel/geom/primitive.h"

#include "kernel/svm/math_util.h"
#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"

#include "kernel/util/colorspace.h"
#include "util/defines.h"

#define OPENPBR_SPEC_COMPLIANT

CCL_NAMESPACE_BEGIN

/* TODO(OpenPBR): all these helper functions should probably go to a separate header file? */
/* Analytical approximation of the average directional albedo of the Fresnel factor [d'Eon2021]*/
// avoiding recursion to make it work for oneAPI
ccl_device float hemispherical_albedo_(const float eta)
{
  const float eta_sqr = eta * eta;
  const float nominator = (10893.0f * eta) - 1438.2f;
  const float denominator = (-774.4f * eta_sqr) + (10212.0f * eta) + 1.0f;
  return logf(nominator / denominator);
}

ccl_device float hemispherical_albedo(const float eta)
{
  if (eta > 1.0f) {
    return hemispherical_albedo_(eta);
  }
  else {
    return 1.0f - eta * eta * (1.0f - hemispherical_albedo_(1.0f / eta));
  }
}

/* Closure Nodes */

ccl_device_inline int svm_node_closure_bsdf_skip(int offset, const uint type)
{
  switch (type) {
    case CLOSURE_BSDF_PRINCIPLED_ID:
      offset += sizeof(SVMNodePrincipledBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_HAIR_CHIANG_ID:
    case CLOSURE_BSDF_HAIR_HUANG_ID:
      offset += sizeof(SVMNodePrincipledHairBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_PHYSICAL_CONDUCTOR:
    case CLOSURE_BSDF_F82_CONDUCTOR:
      offset += sizeof(SVMNodeMetallicBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSDF_OREN_NAYAR_ID:
    case CLOSURE_BSDF_BURLEY_ID:
      offset += sizeof(SVMNodeDiffuseBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_SHEEN_ID:
    case CLOSURE_BSDF_ASHIKHMIN_VELVET_ID:
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSDF_TRANSPARENT_ID:
      offset += sizeof(SVMNodeSimpleBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_RAY_PORTAL_ID:
      offset += sizeof(SVMNodeRayPortalBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_ID:
    case CLOSURE_BSDF_MICROFACET_BECKMANN_ID:
    case CLOSURE_BSDF_ASHIKHMIN_SHIRLEY_ID:
    case CLOSURE_BSDF_MICROFACET_MULTI_GGX_ID:
      offset += sizeof(SVMNodeGlossyBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
    case CLOSURE_BSDF_MICROFACET_BECKMANN_REFRACTION_ID:
      offset += sizeof(SVMNodeRefractionBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_GLASS_ID:
    case CLOSURE_BSDF_MICROFACET_BECKMANN_GLASS_ID:
    case CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID:
      offset += sizeof(SVMNodeGlassBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_GLOSSY_TOON_ID:
    case CLOSURE_BSDF_DIFFUSE_TOON_ID:
      offset += sizeof(SVMNodeToonBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_HAIR_REFLECTION_ID:
    case CLOSURE_BSDF_HAIR_TRANSMISSION_ID:
      offset += sizeof(SVMNodeHairBsdfData) / sizeof(uint);
      break;
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSSRDF_RANDOM_WALK_ID:
    case CLOSURE_BSSRDF_RANDOM_WALK_SKIN_ID:
      offset += sizeof(SVMNodeBssrdfData) / sizeof(uint);
      break;
    case CLOSURE_BSDF_OPEN_PBR_ID:
      offset += sizeof(SVMNodeOpenPBRBsdfData) / sizeof(uint);
      break;
    default:
      offset += sizeof(SVMNodeSimpleBsdfData) / sizeof(uint);
      break;
  }
  return offset;
}

/* Compute emission attenuated by coat and sheen for Principled BSDF, and return the weight of the
 * layers after emission. */
ccl_device_inline Spectrum
principled_bsdf_emission(KernelGlobals kg,
                         ccl_private ShaderData *sd,
                         ccl_private float *ccl_restrict stack,
                         const ccl_global SVMNodePrincipledBsdfData &data,
                         ccl_private float3 *N,
                         const PathRayVisibility ray_visibility,
                         const uint32_t path_flag,
                         const float mix_weight)
{
  /* We're ignoring closure_weight here since it's always 1 for the Principled BSDF, so there's no
   * point in setting it. */
  Spectrum weight = make_spectrum(mix_weight);

  *N = stack_load_float3_default(stack, data.normal_offset, sd->N);
  *N = safe_normalize_fallback(*N, sd->N);

  /* Before any actual shader components, apply transparency. */
  const float alpha = saturatef(stack_load(stack, data.alpha));
  if (alpha < 1.0f) {
    bsdf_transparent_setup(sd, weight * (1.0f - alpha), path_flag);
    weight *= alpha;
  }

  /* First layer: Sheen */
  const float coat_weight = fmaxf(stack_load(stack, data.coat_weight), 0.0f);
  const float sheen_weight = fmaxf(stack_load(stack, data.sheen_weight), 0.0f);

  if (sheen_weight > CLOSURE_WEIGHT_CUTOFF) {
    const float3 sheen_tint = max(stack_load(stack, data.sheen_tint), zero_float3());
    const float sheen_roughness = saturatef(stack_load(stack, data.sheen_roughness));
    const float3 coat_normal = safe_normalize_fallback(
        stack_load_float3_default(stack, data.coat_normal_offset, *N), sd->N);
    const float3 sheen_N = safe_normalize(mix(*N, coat_normal, saturatef(coat_weight)));

    const Spectrum closure_weight = sheen_weight * rgb_to_spectrum(sheen_tint) * weight;
    const Spectrum albedo = bsdf_sheen_setup(kg, sd, closure_weight, sheen_N, sheen_roughness);

    /* Attenuate lower layers */
    weight = closure_layering_weight(albedo, weight);
  }

  /* Second layer: Coat */
  if (coat_weight > CLOSURE_WEIGHT_CUTOFF) {
    Coat coat;
    coat.tint = rgb_to_spectrum(max(stack_load(stack, data.coat_tint), zero_float3()));
    coat.ior = fmaxf(stack_load(stack, data.coat_ior), 1.0f);
    coat.N = safe_normalize_fallback(stack_load_float3_default(stack, data.coat_normal_offset, *N),
                                     sd->N);
    coat.roughness = saturatef(stack_load(stack, data.coat_roughness));
    coat.weight = coat_weight * weight;

    const Spectrum albedo = bsdf_coat_setup(kg, sd, ray_visibility, coat);

    weight = closure_layering_weight(albedo, weight);
  }

  /* Emission (attenuated by sheen and coat) */
  const Spectrum emission = rgb_to_spectrum(stack_load(stack, data.emission_color)) *
                            stack_load(stack, data.emission_strength);
  if (!is_zero(emission)) {
    emission_setup(sd, emission * weight);
  }

  return weight;
}

/* Modulate the IOR so that the reflectivity at normal incidence is scaled by `weight`.
 * OpenPBR Surface specification v1.1.1, Eq. (25,26).  */
ccl_device_inline float openpbr_modulate_ior(const float ior, const float weight)
{
  if (weight == 1.0f) {
    return ior;
  }

  const float modulated_ior = ior_from_F0(weight * F0_from_ior(ior));
  return (ior < 1.0f) ? 1.0f / modulated_ior : modulated_ior;
}

ccl_device_inline float3 openpbr_coat_darkening(const ccl_global SVMNodeOpenPBRBsdfData &data,
                                                ccl_private float *ccl_restrict stack,
                                                const float3 base_color,
                                                const float base_metalness,
                                                const float specular_weight,
                                                const float3 subsurface_color,
                                                const float subsurface_weight,
                                                const float coat_weight,
                                                const float coat_ior)
{
  // TODO: Need to add darkening
  const float coat_darkening = saturatef(stack_load(stack, data.coat_darkening));
  if (!(coat_darkening < 1.0f)) {
    return one_float3();
  }
  /*
            //// Missing in MaterialX
            const float Fs; //TODO
            // metal roughness
            const float r_m = specular_roughness;
            // dielectric_base roughness
            const float r_d = lerp(1.0f, specular_roughness, specular_weight * Fs);
            // effective base roughness
            const float r_b = lerp(r_d, r_m, base_metalness);   // Missing in MaterialX
            ////////////////////
  */
  if (!(coat_weight > 0.0f)) {
    return one_float3();
  }

  const float relative_coat_ior = coat_ior;
  const float relative_coat_ior_sqr = relative_coat_ior * relative_coat_ior;
  // heispherical albedo of the coat layer
  const float E_F = hemispherical_albedo(relative_coat_ior);
  // Base albedo at normal-incedent
  //// According to MaterialX
  const float3 Edielctric = mix(
      base_color, subsurface_color, subsurface_weight);  // missing transmissive
  const float3 Emetal = base_color * specular_weight;
  const float3 Ebase = mix(Edielctric, Emetal, base_metalness);
  //////
  const float3 E_b = Ebase;  // need a better albedo approximation
  // Internal diffuse reflection coefficient
  const float K_r = 1.0f - (1.0f - E_F) / relative_coat_ior_sqr;  // check: Spec, Adobbe, MaterialX
  const float K_s = hemispherical_albedo(
      relative_coat_ior);  // check: Adobe,  missing in MaterialX
  const float K = K_r;     // MaterialX
  // const float K = lerp(K_s, K_r, r_b); // check: Spec and Adobe, missing in MaterialX

  const float3 base_darkening_factor = (1.0f - K) /
                                       (1.0f - E_b * K);  // check: Spec, MaterialX, Adobe
  return mix(one_float3(),
             base_darkening_factor,
             coat_weight * coat_darkening);  // check: Spec,  MaterialX, Adobe
}

/* Anisotropic mapping, OpenPBR Surface specification v1.1.1, Eq. (20). */
ccl_device_inline float2 openpbr_anisotropic_roughness(ccl_private float *ccl_restrict stack,
                                                       const float roughness,
                                                       const float anisotropy,
                                                       const SVMStackOffset tangent_offset,
                                                       ccl_private float3 *tangent)
{
  float2 alpha = make_float2(sqr(roughness));
  *tangent = zero_float3();

  if (anisotropy > 0.0f && stack_valid(tangent_offset)) {
    *tangent = stack_load_float3(stack, tangent_offset);

    alpha.x *= sqrt(2.0f / (1.0f + sqr(1.0f - anisotropy)));
    alpha.y = (1.0f - anisotropy) * alpha.x;
    alpha.x = max(alpha.x, 1e-6f);
    alpha.y = max(alpha.y, 1e-6f);
  }

  return alpha;
}

ccl_device_inline float2 openpbr_specular_roughness(const ccl_global SVMNodeOpenPBRBsdfData &data,
                                                    ccl_private float *ccl_restrict stack,
                                                    ccl_private float3 *tangent,
                                                    const float coat_weight,
                                                    const float coat_roughness)
{
  float specular_roughness = saturatef(stack_load(stack, data.specular_roughness));

  if ((coat_weight > CLOSURE_WEIGHT_CUTOFF) && (coat_roughness > 0.0f)) {
    /* Coat roughening, OpenPBR Surface specification v1.1.1, Eq. (73). */
    const float min_coated_roughness = min(
        1.0f, powf(sqr(sqr(specular_roughness)) + 2.0f * sqr(sqr(coat_roughness)), 0.25f));
    specular_roughness = mix(specular_roughness, min_coated_roughness, coat_weight);
  }

  const float anisotropy = saturatef(stack_load(stack, data.specular_roughness_anisotropy));
  const SVMStackOffset offset = data.geometry_tangent_offset;
  return openpbr_anisotropic_roughness(stack, specular_roughness, anisotropy, offset, tangent);
}

ccl_device_inline float openpbr_specular_ior(const ccl_global SVMNodeOpenPBRBsdfData &data,
                                             ccl_private float *ccl_restrict stack,
                                             const float coat_weight,
                                             const float coat_ior,
                                             const bool backfacing)
{
  float specular_ior = stack_load(stack, data.specular_ior);
  /* Flipping the IOR in case we are inside the object. */
  specular_ior = backfacing ? 1.0f / specular_ior : specular_ior;

  if (coat_weight > CLOSURE_WEIGHT_CUTOFF) {
    /* Fixing the ration to compensate for total internal reflections (TIR),
     * OpenPBR Surface specification v1.1.1, Eq. (74) */
    const float tir_fixed_ratio = (coat_ior > specular_ior) ? (coat_ior / specular_ior) :
                                                              (specular_ior / coat_ior);

    /* TODO(OpenPBR): we assume that the ambient ior is 1.0. */
    const float ambient_ior = 1.0f;
    /* OpenPBR Surface specification v1.1.1, Eq. (60) */
    specular_ior = mix(specular_ior / ambient_ior, tir_fixed_ratio, coat_weight);
  }

  return specular_ior;
}

/* Compute emission attenuated by coat and sheen for OpenPBR BSDF, and return the weight of the
 * layers after emission. */
ccl_device_inline Spectrum openpbr_bsdf_emission(KernelGlobals kg,
                                                 ccl_private ShaderData *sd,
                                                 ccl_private float *ccl_restrict stack,
                                                 const ccl_global SVMNodeOpenPBRBsdfData &data,
                                                 ccl_private float3 *N,
                                                 const bool reflective_caustics,
                                                 const uint32_t path_flag,
                                                 const float mix_weight)
{
  Spectrum weight = make_spectrum(mix_weight);

  *N = stack_load_float3_default(stack, data.geometry_normal_offset, sd->N);
  *N = safe_normalize_fallback(*N, sd->N);

  /* Before any actual shader components, apply transparency. */
  const float geometry_opacity = saturatef(stack_load(stack, data.geometry_opacity));
  if (geometry_opacity < 1.0f) {
    bsdf_transparent_setup(sd, weight * (1.0f - geometry_opacity), path_flag);
    weight *= geometry_opacity;
  }

  const float coat_weight = saturatef(stack_load(stack, data.coat_weight));
  const float3 geometry_coat_normal = safe_normalize_fallback(
      stack_load_float3_default(stack, data.geometry_coat_normal_offset, *N), sd->N);

  /* First layer: Fuzz */
  const float fuzz_weight = saturatef(stack_load(stack, data.fuzz_weight));
  if (fuzz_weight > CLOSURE_WEIGHT_CUTOFF) {
    const float3 fuzz_color = saturate(stack_load(stack, data.fuzz_color));
    SheenBsdf sheen;
    ccl_private SheenBsdf *bsdf = bsdf_alloc_maybe_emission(
        sd, &sheen, path_flag, fuzz_weight * rgb_to_spectrum(fuzz_color) * weight);

    if (bsdf) {
      bsdf->N = safe_normalize(mix(*N, geometry_coat_normal, saturatef(coat_weight)));
      bsdf->roughness = saturatef(stack_load(stack, data.fuzz_roughness));

      /* setup bsdf */
      // TODO: need to validate if we use the correct sheen/fuzz model as descibed in the
      // OpenPBR v 1.1 spec.
      const int fuzz_flag = bsdf_sheen_setup(kg, sd, bsdf);

      if (fuzz_flag) {
        sd->flag |= fuzz_flag;

        /* Attenuate lower layers */
        const Spectrum albedo = bsdf_albedo(
            kg, sd, (ccl_private ShaderClosure *)bsdf, true, false);
        weight = closure_layering_weight(albedo, weight);
      }
    }
  }

  /* Second layer: Coat */
  if (coat_weight > CLOSURE_WEIGHT_CUTOFF) {
    const float3 valid_coat_normal = maybe_ensure_valid_specular_reflection(sd,
                                                                            geometry_coat_normal);
    float ior = stack_load(stack, data.coat_ior);
    /* FIXME(OpenPBR): simply invert ior is incorrect, probably should do the same as thin film?
     * Didn't find in the spec how to address backface. Principled BSDF uses original coat IOR
     * without modification. */
    ior = (sd->flag & SD_BACKFACING) ? 1.0f / ior : ior;
    if (reflective_caustics) {
      MicrofacetBsdf coat;
      ccl_private MicrofacetBsdf *bsdf = bsdf_alloc_maybe_emission(
          sd, &coat, path_flag, coat_weight * weight);

      if (bsdf) {
        float3 coat_tangent = zero_float3();
        const float roughness = saturatef(stack_load(stack, data.coat_roughness));
        const float anisotropy = saturatef(stack_load(stack, data.coat_roughness_anisotropy));
        const SVMStackOffset coat_tangent_offset = data.geometry_coat_tangent_offset;
        const float2 coat_alpha = openpbr_anisotropic_roughness(
            stack, roughness, anisotropy, coat_tangent_offset, &coat_tangent);

        bsdf->N = valid_coat_normal;
        bsdf->ior = ior;
        bsdf->T = coat_tangent;
        bsdf->alpha_x = coat_alpha.x;
        bsdf->alpha_y = coat_alpha.y;

        /* setup bsdf */
        sd->flag |= bsdf_microfacet_ggx_setup(bsdf);
        bsdf_microfacet_setup_fresnel_dielectric(kg, bsdf, sd->wi);

        /* Attenuate lower layers */
        const Spectrum albedo = bsdf_albedo(
            kg, sd, (ccl_private ShaderClosure *)bsdf, true, false);
        weight = closure_layering_weight(albedo, weight);
      }
    }

    /* View-dependent absorption, OpenPBR Surface specification v1.1.1, Sec. 3.4.2. */
    const float3 coat_color = saturate(stack_load(stack, data.coat_color));
    if (!isequal(coat_color, one_float3())) {
      /* Note: the correct approximation relies on the cosines of the incoming and
        outgoing directions. But we only have acces to the outgoing direction. We therefore
        assume that the refracted cosine of both directionas are the same. The same
        approaximation is done in Adobes' implementation and in our PrincipledBSDF.*/

      const float cosNI = dot(sd->wi, valid_coat_normal);
      /* Refract incoming direction into coat material.*/
      const float cosNT = sqrtf(1.0f - sqr(1.0f / ior) * (1 - sqr(cosNI)));
      const float optical_depth = 1.0f / cosNT;
      /* Note: Since we assume that the incoming and outgoing cosine are the same the sqrt
        of the coat_color cancels out.*/
      weight *= mix(
          one_spectrum(), power(rgb_to_spectrum(coat_color), optical_depth), coat_weight);
    }
  }

  /* Emission (attenuated by sheen and coat) */
  const float emission_luminance = stack_load(stack, data.emission_luminance);
  const float3 emission_color = saturate(stack_load(stack, data.emission_color));
  const float3 emission = emission_color * emission_luminance;
  if (!is_zero(emission)) {
    emission_setup(sd, rgb_to_spectrum(emission) * weight);
  }

  return weight;
}

template<uint64_t node_feature_mask, ShaderType shader_type>
#ifndef __KERNEL_ONEAPI__
ccl_device_noinline
#else
ccl_device
#endif
    int
    svm_node_closure_bsdf(KernelGlobals kg,
                          ccl_private ShaderData *sd,
                          ccl_private float *ccl_restrict stack,
                          Spectrum closure_weight,
                          const ccl_global SVMNodeClosureBsdf &ccl_restrict node,
                          const PathRayVisibility ray_visibility,
                          const uint32_t path_flag,
                          int offset)
{
  ClosureType type = node.closure_type;

  const float mix_weight = stack_load_float_default(stack, node.mix_weight_offset, 1.0f);

  /* Only compute BSDF for surfaces, transparent variable is shared with volume extinction. */
  if constexpr (shader_type != SHADER_TYPE_SURFACE) {
    return svm_node_closure_bsdf_skip(offset, type);
  }

  if (mix_weight == 0.0f) {
    return svm_node_closure_bsdf_skip(offset, type);
  }

  IF_KERNEL_NODES_FEATURE(BSDF) {}
  else IF_KERNEL_NODES_FEATURE(EMISSION) {
    if (type != CLOSURE_BSDF_PRINCIPLED_ID && type != CLOSURE_BSDF_OPEN_PBR_ID) {
      /* Only principled and OpenPBR BSDF can have emission. */
      return svm_node_closure_bsdf_skip(offset, type);
    }

    switch (type) {
      case CLOSURE_BSDF_PRINCIPLED_ID: {
        const ccl_global SVMNodePrincipledBsdfData &data = svm_node_get<SVMNodePrincipledBsdfData>(
            kg, &offset);

        float3 N;
        principled_bsdf_emission(kg, sd, stack, data, &N, ray_visibility, path_flag, mix_weight);
        break;
      }
      case CLOSURE_BSDF_OPEN_PBR_ID: {
        const ccl_global SVMNodeOpenPBRBsdfData &data = svm_node_get<SVMNodeOpenPBRBsdfData>(
            kg, &offset);

        float3 N;
        openpbr_bsdf_emission(kg, sd, stack, data, &N, ray_visibility, path_flag, mix_weight);
        break;
      }
      default:
        kernel_assert(false);
        break;
    }

    return offset;
  }
  else {
    return svm_node_closure_bsdf_skip(offset, type);
  }

  const Spectrum black = zero_spectrum();
  const Spectrum white = one_spectrum();
  switch (type) {
    case CLOSURE_BSDF_PRINCIPLED_ID: {
      const ccl_global SVMNodePrincipledBsdfData &data = svm_node_get<SVMNodePrincipledBsdfData>(
          kg, &offset);

      float3 N;
      Spectrum weight = principled_bsdf_emission(
          kg, sd, stack, data, &N, ray_visibility, path_flag, mix_weight);

      const Spectrum base_color = rgb_to_spectrum(
          max(stack_load(stack, data.base_color), zero_float3()));
      const Spectrum clamped_base_color = min(base_color, white);
      const float ior = fmaxf(stack_load(stack, data.ior), 1e-5f);
      const float roughness = saturatef(stack_load(stack, data.roughness));
      const float3 valid_reflection_N = maybe_ensure_valid_specular_reflection(sd, N);
      const float anisotropic = saturatef(stack_load(stack, data.anisotropic));

      const ClosureType distribution = data.distribution;
      const Spectrum specular_tint = rgb_to_spectrum(
          max(stack_load(stack, data.specular_tint), zero_float3()));
      const float thinfilm_thickness = stack_load(stack, data.thin_film_thickness);
      const float thinfilm_ior = (thinfilm_thickness > THINFILM_THICKNESS_CUTOFF) ?
                                     fmaxf(stack_load(stack, data.thin_film_ior), 1e-5f) :
                                     0.0f;

      float alpha_x = sqr(roughness);
      float alpha_y = sqr(roughness);
      float3 T = zero_float3();
      if (anisotropic > 0.0f && stack_valid(data.tangent_offset)) {
        T = stack_load_float3(stack, data.tangent_offset);
        const float aspect = sqrtf(1.0f - anisotropic * 0.9f);
        alpha_x /= aspect;
        alpha_y *= aspect;
        const float anisotropic_rotation = stack_load(stack, data.anisotropic_rotation);
        if (anisotropic_rotation != 0.0f) {
          T = rotate_around_axis(T, N, anisotropic_rotation * M_2PI_F);
        }
      }

#ifdef __CAUSTICS_TRICKS__
      const bool reflective_caustics = (kernel_data.integrator.caustics_reflective ||
                                        (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE) == 0);
      const bool refractive_caustics = (kernel_data.integrator.caustics_refractive ||
                                        (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE) == 0);
#else
      const bool reflective_caustics = true;
      const bool refractive_caustics = true;
#endif

      /* Metallic component */
      const float metallic = saturatef(stack_load(stack, data.metallic));
      if (metallic > CLOSURE_WEIGHT_CUTOFF) {
        if (reflective_caustics) {
          ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
              sd, sizeof(MicrofacetBsdf), metallic * weight);
          ccl_private FresnelF82Tint *fresnel =
              (bsdf != nullptr) ?
                  (ccl_private FresnelF82Tint *)closure_alloc_extra(sd, sizeof(FresnelF82Tint)) :
                  nullptr;

          if (bsdf && fresnel) {
            bsdf->N = valid_reflection_N;
            bsdf->ior = 1.0f;
            bsdf->T = T;
            bsdf->alpha_x = alpha_x;
            bsdf->alpha_y = alpha_y;

            fresnel->f0 = clamped_base_color;
            const Spectrum f82 = min(specular_tint, white);

            fresnel->thin_film.thickness = thinfilm_thickness;
            fresnel->thin_film.ior = thinfilm_ior;

            /* setup bsdf */
            sd->runtime_flag |= bsdf_microfacet_ggx_setup(bsdf);
            const bool is_multiggx = (distribution == CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID);
            bsdf_microfacet_setup_fresnel_f82_tint(kg, bsdf, sd->wi, fresnel, f82, is_multiggx);
          }
        }
        /* Attenuate other components */
        weight *= (1.0f - metallic);
      }

      const bool thin_wall = stack_load(stack, data.thin_wall);

      /* Transmission component */
      const float transmission_weight = saturatef(stack_load(stack, data.transmission_weight));
      if (transmission_weight > CLOSURE_WEIGHT_CUTOFF) {
        if (reflective_caustics || refractive_caustics) {
          FresnelThinFilm thinfilm = {thinfilm_thickness, thinfilm_ior};
          if (thin_wall) {
            bsdf_thin_glass_setup(kg,
                                  sd,
                                  reflective_caustics,
                                  refractive_caustics,
                                  {specular_tint, clamped_base_color},
                                  transmission_weight * weight,
                                  valid_reflection_N,
                                  sqr(roughness),
                                  ior,
                                  thinfilm,
                                  ray_visibility,
                                  path_flag);
          }
          else {
            ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
                sd, sizeof(MicrofacetBsdf), transmission_weight * weight);
            ccl_private FresnelGeneralizedSchlick *fresnel =
                (bsdf != nullptr) ? (ccl_private FresnelGeneralizedSchlick *)closure_alloc_extra(
                                        sd, sizeof(FresnelGeneralizedSchlick)) :
                                    nullptr;

            if (bsdf && fresnel) {
              const bool backfacing = (sd->runtime_flag & SR_BACKFACING);
              bsdf->N = valid_reflection_N;
              bsdf->T = T;
              bsdf->alpha_x = alpha_x;
              bsdf->alpha_y = alpha_y;

              const float dispersion_scale = saturatef(
                  stack_load(stack, data.transmission_dispersion_scale));
              const float abbe_number = fmaxf(
                  stack_load(stack, data.transmission_dispersion_abbe_number), 0.0f);
              const float inv_abbe = safe_divide(dispersion_scale, abbe_number);
              bsdf->ior = backfacing ? 1.0f / ior : ior;
              bsdf->ior = bsdf_glass_ior(sd, bsdf->ior, inv_abbe);

              if (backfacing) {
                adjust_thin_film_ior_at_backface(thinfilm.ior, bsdf->ior);
              }

              *fresnel = generalized_schlick_setup(ior,
                                                   reflective_caustics,
                                                   refractive_caustics,
                                                   specular_tint,
                                                   sqrt(clamped_base_color),
                                                   thinfilm);

              /* setup bsdf */
              sd->runtime_flag |= bsdf_microfacet_ggx_glass_setup(bsdf);
              const bool is_multiggx = (distribution ==
                                        CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID);
              bsdf_microfacet_setup_fresnel_generalized_schlick(
                  kg, bsdf, sd->wi, fresnel, is_multiggx);
            }
          }
        }
        /* Attenuate other components */
        weight *= (1.0f - transmission_weight);
      }

      /* Apply IOR adjustment */
      const float specular_ior_level = max(stack_load(stack, data.specular_ior_level), 0.0f);
      float eta = ior;
      float f0 = F0_from_ior(eta);
      if (specular_ior_level != 0.5f) {
        f0 *= 2.0f * specular_ior_level;
        eta = ior_from_F0(f0);
        if (ior < 1.0f) {
          eta = 1.0f / eta;
        }
      }

      /* Specular component */
      if (reflective_caustics && (eta != 1.0f || thinfilm_thickness > 0.1f)) {
        ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
            sd, sizeof(MicrofacetBsdf), weight);
        ccl_private FresnelGeneralizedSchlick *fresnel =
            (bsdf != nullptr) ? (ccl_private FresnelGeneralizedSchlick *)closure_alloc_extra(
                                    sd, sizeof(FresnelGeneralizedSchlick)) :
                                nullptr;

        if (bsdf && fresnel) {
          bsdf->N = valid_reflection_N;
          bsdf->ior = eta;
          bsdf->T = T;
          bsdf->alpha_x = alpha_x;
          bsdf->alpha_y = alpha_y;

          fresnel->f0 = f0 * specular_tint;
          fresnel->f90 = white;
          fresnel->exponent = -eta;
          fresnel->tint = {white, black};
          fresnel->thin_film = {thinfilm_thickness, thinfilm_ior};

          /* setup bsdf */
          sd->runtime_flag |= bsdf_microfacet_ggx_setup(bsdf);
          const bool is_multiggx = (distribution == CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID);
          bsdf_microfacet_setup_fresnel_generalized_schlick(
              kg, bsdf, sd->wi, fresnel, is_multiggx);

          /* Attenuate lower layers */
          const Spectrum albedo = closure_layer_albedo(kg, sd, (ccl_private ShaderClosure *)bsdf);
          weight = closure_layering_weight(albedo, weight);
        }
      }

      /* Diffuse/Subsurface component */
#ifdef __SUBSURFACE__
      const float subsurface_weight = saturatef(stack_load(stack, data.subsurface_weight));
      if (subsurface_weight > CLOSURE_WEIGHT_CUTOFF) {
        const float anisotropy = stack_load(stack, data.subsurface_anisotropy);
        const Spectrum closure_weight = clamped_base_color * subsurface_weight * weight;
        if (thin_wall) {
          const float diffuse_roughness = saturatef(stack_load(stack, data.diffuse_roughness));
          bsdf_thin_subsurface_setup(
              sd, N, closure_weight, anisotropy, diffuse_roughness, clamped_base_color);
        }
        else {
          const ClosureType subsurface_method = data.subsurface_method;
          ccl_private Bssrdf *bssrdf = bssrdf_alloc(sd, closure_weight);
          if (bssrdf) {
            const float3 subsurface_radius = stack_load(stack, data.subsurface_radius);
            const float subsurface_scale = stack_load(stack, data.subsurface_scale);

            bssrdf->radius = rgb_to_spectrum(
                max(subsurface_radius * subsurface_scale, zero_float3()));
            bssrdf->albedo = clamped_base_color;
            bssrdf->N = maybe_ensure_valid_specular_reflection(sd, N);
            bssrdf->alpha = sqr(roughness);
            /* IOR is clamped to [1.01..3.8] inside bssrdf_setup */
            bssrdf->ior = eta;
            /* Anisotropy is clamped to a valid range inside bssrdf_setup. */
            bssrdf->anisotropy = anisotropy;
            if (subsurface_method == CLOSURE_BSSRDF_RANDOM_WALK_SKIN_ID) {
              bssrdf->ior = stack_load(stack, data.subsurface_ior);
            }

            /* setup bsdf */
            sd->runtime_flag |= bssrdf_setup(sd, bssrdf, path_flag, subsurface_method);
          }
        }
      }
#else
      const float subsurface_weight = 0.0f;
#endif

      const float diffuse_roughness = saturatef(stack_load(stack, data.diffuse_roughness));
      const Spectrum diffuse_weight = base_color * (1.0f - subsurface_weight) * weight;
      if (diffuse_roughness_is_almost_zero(diffuse_roughness)) {
        bsdf_diffuse_setup(sd, N, diffuse_weight);
      }
      else {
        bsdf_oren_nayar_setup(sd, N, diffuse_weight, diffuse_roughness, base_color);
      }

      break;
    }
    case CLOSURE_BSDF_OPEN_PBR_ID: {
      const ccl_global SVMNodeOpenPBRBsdfData &data = svm_node_get<SVMNodeOpenPBRBsdfData>(
          kg, &offset);

#ifdef __CAUSTICS_TRICKS__
      const bool reflective_caustics = (kernel_data.integrator.caustics_reflective ||
                                        (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE) == 0);
#else
      const bool reflective_caustics = true;
#endif

      float3 N;
      Spectrum weight = openpbr_bsdf_emission(
          kg, sd, stack, data, &N, reflective_caustics, path_flag, mix_weight);

      const float base_weight = saturatef(stack_load(stack, data.base_weight));
      const float3 base_color = max(stack_load(stack, data.base_color), zero_float3());
      const float base_metalness = saturatef(stack_load(stack, data.base_metalness));
      const float base_diffuse_roughness = stack_load(stack, data.base_diffuse_roughness);

      const float specular_weight = saturatef(stack_load(stack, data.specular_weight));
      const float3 specular_color = saturate(stack_load(stack, data.specular_color));

#ifdef __SUBSURFACE__
      const float subsurface_weight = saturatef(stack_load(stack, data.subsurface_weight));
      const float3 subsurface_color = saturate(stack_load(stack, data.subsurface_color));
#else
      const float subsurface_weight = 0.f;
#endif

      const bool is_multiggx = true;

      const float thin_film_weight = saturatef(stack_load(stack, data.thin_film_weight));
      float thin_film_thickness = stack_load(stack, data.thin_film_thickness);
      // TODO (OpenPBR): disabling thin-film for now until we have implemented it for all Fresnel
      // modes and find a way to integrate the thin_film_weight
      thin_film_thickness *= 0.f;
      thin_film_thickness *= 1000.0f;
      const float thin_film_ior = stack_load(stack, data.thin_film_ior);

      const float coat_weight = saturatef(stack_load(stack, data.coat_weight));
      const float coat_roughness = saturatef(stack_load(stack, data.coat_roughness));
      float coat_ior = stack_load(stack, data.coat_ior);
      const bool thin_wall = stack_load(stack, data.geometry_thin_walled);
      const bool backfacing = (sd->flag & SD_BACKFACING) && !thin_wall;
      /* FIXME(OpenPBR): simply invert ior is incorrect, probably should do the same as thin film?
       * Didn't find in the spec how to address backface. Principled BSDF uses original coat IOR
       * without modification. */
      coat_ior = backfacing ? 1.0f / coat_ior : coat_ior;

      weight *= openpbr_coat_darkening(data,
                                       stack,
                                       base_color,
                                       base_metalness,
                                       specular_weight,
                                       subsurface_color,
                                       subsurface_weight,
                                       coat_weight,
                                       coat_ior);

      float3 geometry_tangent;
      const float2 specular_alpha = openpbr_specular_roughness(
          data, stack, &geometry_tangent, coat_weight, coat_roughness);
      const float specular_ior = openpbr_specular_ior(
          data, stack, coat_weight, coat_ior, backfacing);
      const float modulated_specular_ior = openpbr_modulate_ior(specular_ior, specular_weight);

      const float3 valid_reflection_N = maybe_ensure_valid_specular_reflection(sd, N);

      /* Metallic component */
      if (base_metalness > CLOSURE_WEIGHT_CUTOFF) {
        if (reflective_caustics) {
          /* In OpenPBR v1.1 Eq. 32 the Fresnel_82 term is multiplied with the specular_weight.
           * We can achieve the same effect by mutl*/
          ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
              sd, sizeof(MicrofacetBsdf), specular_weight * base_metalness * weight);
          ccl_private FresnelF82Tint *fresnel =
              (bsdf != nullptr) ?
                  (ccl_private FresnelF82Tint *)closure_alloc_extra(sd, sizeof(FresnelF82Tint)) :
                  nullptr;

          if (bsdf && fresnel) {
            bsdf->N = valid_reflection_N;
            bsdf->ior = 1.0f;
            bsdf->T = geometry_tangent;
            bsdf->alpha_x = specular_alpha.x;
            bsdf->alpha_y = specular_alpha.y;

            fresnel->f0 = rgb_to_spectrum(base_color) * base_weight;
#ifdef OPENPBR_SPEC_COMPLIANT
            const Spectrum f82 = min(specular_color, one_spectrum());
#else
            const Spectrum f82 = min(specular_color * specular_weight, one_spectrum());
#endif
            fresnel->thin_film.thickness = thin_film_thickness;
            /* TODO(OpenPBR): check if thin film_ior needs to be adjusted at backface. */
            fresnel->thin_film.ior = thin_film_ior;

            /* setup bsdf */
            sd->flag |= bsdf_microfacet_ggx_setup(bsdf);
            bsdf_microfacet_setup_fresnel_f82_tint(kg, bsdf, sd->wi, fresnel, f82, is_multiggx);
          }
        }

        /* Attenuate other components */
        weight *= (1.0f - base_metalness);
      }

      const float transmission_weight = saturatef(stack_load(stack, data.transmission_weight));

#ifdef __CAUSTICS_TRICKS__
      const bool refractive_caustics = (kernel_data.integrator.caustics_refractive ||
                                        (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE) == 0);
#else
      const bool refractive_caustics = true;
#endif

#ifdef OPENPBR_SPEC_COMPLIANT  // OpenPBR v1.1 spec version (glossy diffuse layer)
      /* Translucent Component*/
      /* FIXME(OpenPBR): should be reflective || refractive. Also fix specular_ior == 1. */
      if (transmission_weight > CLOSURE_WEIGHT_CUTOFF &&
          (refractive_caustics && (specular_ior != 1.0f /* || thinfilm_thickness > 0.1f*/)))
      {
        const float3 transmission_color = saturate(stack_load(stack, data.transmission_color));
        FresnelThinFilm thinfilm = {thin_film_thickness, thin_film_ior};
        if (thin_wall) {
          Spectrum reflectance, transmittance;
          bsdf_thin_glass_setup(kg,
                                sd,
                                reflective_caustics,
                                refractive_caustics,
                                specular_color,
                                transmission_color,
                                transmission_weight * weight,
                                valid_reflection_N,
                                len(specular_alpha) * M_SQRT1_2F,
                                modulated_specular_ior,
                                thinfilm,
                                &reflectance,
                                &transmittance,
                                ray_visibility,
                                path_flag);
        }
        else {
          ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
              sd, sizeof(MicrofacetBsdf), weight * transmission_weight);
          if (bsdf) {
            bsdf->N = valid_reflection_N;
            bsdf->ior = modulated_specular_ior;
            bsdf->T = geometry_tangent;
            bsdf->alpha_x = specular_alpha.x;
            bsdf->alpha_y = specular_alpha.y;
            if (backfacing) {
              /* TODO(OpenPBR): do we need to modulate thin film IOR too? */
              adjust_thin_film_ior_at_backface(thinfilm.ior, modulated_specular_ior);
            }

            const float transmission_depth = max(stack_load(stack, data.transmission_depth), 0.0f);

            if (transmission_depth == 0.0f) {
              ccl_private FresnelDielectricTint *fresnel = (ccl_private FresnelDielectricTint *)
                  closure_alloc_extra(sd, sizeof(FresnelDielectricTint));

              if (fresnel) {
                fresnel->reflection_tint = specular_color;
                fresnel->transmission_tint = transmission_color;
                fresnel->thin_film = thinfilm;

                /* setup bsdf */
                sd->flag |= bsdf_microfacet_ggx_glass_setup(bsdf);
                bsdf_microfacet_setup_fresnel_dielectric_tint(
                    kg, bsdf, sd->wi, fresnel, is_multiggx);
              }
            }
            else {
              ccl_private FresnelDielectricVolumetric *fresnel =
                  (ccl_private FresnelDielectricVolumetric *)closure_alloc_extra(
                      sd, sizeof(FresnelDielectricVolumetric));

              if (fresnel) {
                const Spectrum transmission_scatter = rgb_to_spectrum(
                    saturate(stack_load(stack, data.transmission_scatter)));
                fresnel->reflection_tint = specular_color;
                fresnel->thin_film = thinfilm;
                fresnel->anisotropy = clamp(
                    stack_load(stack, data.transmission_scatter_anisotropy), -1.0f, 1.0f);
                sd->flag |= bsdf_volumetric_setup(kg,
                                                  bsdf,
                                                  sd->wi,
                                                  fresnel,
                                                  transmission_scatter,
                                                  transmission_color,
                                                  transmission_depth);
              }
            }
          }
        }
        /* Attenuate other components */
        weight *= (1.0f - transmission_weight);
      }

      /* Specular Component */
      if (specular_weight > CLOSURE_WEIGHT_CUTOFF &&
          (reflective_caustics &&
           (modulated_specular_ior != 1.0f /* || thinfilm_thickness > 0.1f*/)))
      {
        ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
            sd, sizeof(MicrofacetBsdf), weight);
        ccl_private FresnelDielectricTint *fresnel =
            (bsdf != nullptr) ? (ccl_private FresnelDielectricTint *)closure_alloc_extra(
                                    sd, sizeof(FresnelDielectricTint)) :
                                nullptr;

        if (bsdf && fresnel) {
          bsdf->N = valid_reflection_N;
          bsdf->ior = modulated_specular_ior;
          bsdf->T = geometry_tangent;
          bsdf->alpha_x = specular_alpha.x;
          bsdf->alpha_y = specular_alpha.y;

          fresnel->reflection_tint = specular_color;
          fresnel->transmission_tint = zero_spectrum();
          fresnel->thin_film.thickness = thin_film_thickness;
          fresnel->thin_film.ior = thin_film_ior;

          /* setup bsdf */
          sd->flag |= bsdf_microfacet_ggx_setup(bsdf);
          bsdf_microfacet_setup_fresnel_dielectric_tint(kg, bsdf, sd->wi, fresnel, is_multiggx);

          /* Attenuate lower layers */
          const Spectrum albedo = bsdf_albedo(
              kg, sd, (ccl_private ShaderClosure *)bsdf, true, false);
          weight = closure_layering_weight(albedo, weight);
        }
      }
#else  // MaterialX OSL version (one layer to ruin them all)

      /* Specular Component */
      if (specular_weight > CLOSURE_WEIGHT_CUTOFF &&
          (reflective_caustics &&
           (modulated_specular_ior != 1.0f /* || thinfilm_thickness > 0.1f*/)))
      {
        ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
            sd, sizeof(MicrofacetBsdf), weight);
        ccl_private FresnelDielectricTint *fresnel =
            (bsdf != nullptr) ? (ccl_private FresnelDielectricTint *)closure_alloc_extra(
                                    sd, sizeof(FresnelDielectricTint)) :
                                nullptr;

        if (bsdf && fresnel) {
          bsdf->N = valid_reflection_N;
          bsdf->ior = modulated_specular_ior;
          bsdf->T = geometry_tangent;
          bsdf->alpha_x = specular_alpha.x;
          bsdf->alpha_y = specular_alpha.y;

          fresnel->reflection_tint = specular_color;
          fresnel->transmission_tint = zero_spectrum();
          fresnel->thin_film.thickness = thin_film_thickness;
          fresnel->thin_film.ior = thin_film_ior;

          /* setup bsdf */
          sd->flag |= bsdf_microfacet_ggx_setup(bsdf);
          bsdf_microfacet_setup_fresnel_dielectric_tint(kg, bsdf, sd, fresnel, is_multiggx);

          /* Attenuate lower layers */
          const Spectrum albedo = bsdf_albedo(
              kg, sd, (ccl_private ShaderClosure *)bsdf, true, false);
          weight = closure_layering_weight(albedo, weight);
        }
      }

      /* Translucent Component*/
      if (transmission_weight > CLOSURE_WEIGHT_CUTOFF &&
          (refractive_caustics && (specular_ior != 1.0f /* || thinfilm_thickness > 0.1f*/)))
      {
        ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
            sd, sizeof(MicrofacetBsdf), weight * transmission_weight);
        ccl_private FresnelDielectricTint *fresnel =
            (bsdf != nullptr) ? (ccl_private FresnelDielectricTint *)closure_alloc_extra(
                                    sd, sizeof(FresnelDielectricTint)) :
                                nullptr;

        if (bsdf && fresnel) {
          bsdf->N = valid_reflection_N;
          // Note the spec in this section says specular_ior but they mean the modulated version
          // which is used for all slabs of the dielectric base.
          bsdf->ior = modulated_specular_ior;
          bsdf->T = geometry_tangent;
          bsdf->alpha_x = specular_alpha.x;
          bsdf->alpha_y = specular_alpha.y;

          fresnel->reflection_tint = zero_float3();
          fresnel->transmission_tint = transmission_color;
          fresnel->thin_film.thickness = thin_film_thickness;
          fresnel->thin_film.ior = thin_film_ior;
          if (backfacing) {
            adjust_thin_film_ior_at_backface(fresnel->thin_film.ior, modulated_specular_ior);
          }

          /* setup bsdf */
          sd->flag |= bsdf_microfacet_ggx_glass_setup(bsdf);
          bsdf_microfacet_setup_fresnel_dielectric_tint(kg, bsdf, sd, fresnel, is_multiggx);

          /* Attenuate lower layers */
          // const Spectrum albedo = bsdf_albedo(
          //     kg, sd, (ccl_private ShaderClosure *)bsdf, true, false);
          weight = weight * (1.0f - transmission_weight);
        }
      }

#endif
#ifdef __SUBSURFACE__
      /* Subsurface Scattering Component */
      if (subsurface_weight > CLOSURE_WEIGHT_CUTOFF) {
        const float sss_anisotropy = clamp(
            stack_load(stack, data.subsurface_scatter_anisotropy), -1.0f, 1.0f);
        const Spectrum closure_weight = subsurface_weight * subsurface_color * weight;
        if (thin_wall) {
          bsdf_thin_subsurface_setup(
              sd, N, closure_weight, sss_anisotropy, base_diffuse_roughness, subsurface_color);
        }
        else {
          const float subsurface_radius = stack_load(stack, data.subsurface_radius);
          const float3 subsurface_radius_scale = saturate(
              stack_load(stack, data.subsurface_radius_scale));

          const ClosureType subsurface_method = CLOSURE_BSSRDF_RANDOM_WALK_ID;
          ccl_private Bssrdf *bssrdf = bssrdf_alloc(sd, closure_weight);
          if (bssrdf) {
            bssrdf->radius = subsurface_radius_scale * subsurface_radius;
            bssrdf->albedo = subsurface_color;
            bssrdf->N = maybe_ensure_valid_specular_reflection(sd, N);
            // To match with the OSL code path
            // TODO (OpenPBR): double check the OSL/MaterialX spec for the BSSRDF closure
            bssrdf->alpha = 1.0f;  // To match with the OSL code path
            bssrdf->ior = 1.4f;    // To match with the OSL code path
            // bssrdf->alpha = specular_alpha.x;
            // bssrdf->ior = modulated_specular_ior;
            bssrdf->anisotropy = sss_anisotropy;

            /* setup bsdf */
            sd->flag |= bssrdf_setup(sd, bssrdf, path_flag, subsurface_method);
          }
        }
      }
#endif
      if (base_weight > CLOSURE_WEIGHT_CUTOFF) {
        /* Diffuse Component*/
        const Spectrum diffuse_weight = (base_color * base_weight) * (1.0f - subsurface_weight) *
                                        weight;
        if (diffuse_roughness_is_almost_zero(base_diffuse_roughness)) {
          bsdf_diffuse_setup(sd, N, diffuse_weight);
        }
        else {
          bsdf_oren_nayar_setup(
              sd, N, diffuse_weight, base_diffuse_roughness, (base_color * base_weight));
        }
      }
      break;
    }
    case CLOSURE_BSDF_DIFFUSE_ID: {
      const ccl_global SVMNodeDiffuseBsdfData &bsdf_data = svm_node_get<SVMNodeDiffuseBsdfData>(
          kg, &offset);
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      const float roughness = stack_load(stack, bsdf_data.roughness);
      if (diffuse_roughness_is_almost_zero(roughness)) {
        bsdf_diffuse_setup(sd, N, weight);
      }
      else {
        const Spectrum color = saturate(rgb_to_spectrum(stack_load(stack, bsdf_data.color)));
        bsdf_oren_nayar_setup(sd, N, weight, roughness, color);
      }
      break;
    }
    case CLOSURE_BSDF_TRANSLUCENT_ID: {
      const ccl_global SVMNodeSimpleBsdfData &bsdf_data = svm_node_get<SVMNodeSimpleBsdfData>(
          kg, &offset);
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      /* FIXME(weizhen): `maybe_ensure_valid_specular_reflection` should only be applied to glossy
       * closures, applying to translucent closure seems to be a mistake. */
      bsdf_translucent_setup(sd, maybe_ensure_valid_specular_reflection(sd, N), weight);
      break;
    }
    case CLOSURE_BSDF_TRANSPARENT_ID: {
      svm_node_get<SVMNodeSimpleBsdfData>(kg, &offset);
      const Spectrum weight = closure_weight * mix_weight;
      bsdf_transparent_setup(sd, weight, path_flag);
      break;
    }
    case CLOSURE_BSDF_PHYSICAL_CONDUCTOR:
    case CLOSURE_BSDF_F82_CONDUCTOR: {
      const ccl_global SVMNodeMetallicBsdfData &cdata = svm_node_get<SVMNodeMetallicBsdfData>(
          kg, &offset);

#ifdef __CAUSTICS_TRICKS__
      if (!kernel_data.integrator.caustics_reflective &&
          (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE))
      {
        break;
      }
#endif
      ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
          sd, sizeof(MicrofacetBsdf), rgb_to_spectrum(make_float3(mix_weight)));

      if (bsdf != nullptr) {
        float3 N = stack_load_float3_default(stack, cdata.normal_offset, sd->N);
        N = safe_normalize_fallback(N, sd->N);
        const float3 valid_reflection_N = maybe_ensure_valid_specular_reflection(sd, N);
        const float anisotropy = saturatef(stack_load(stack, cdata.anisotropy));
        const float roughness = saturatef(stack_load(stack, cdata.roughness));
        bsdf->alpha_x = sqr(roughness);
        bsdf->alpha_y = sqr(roughness);
        if (anisotropy > 0.0f && stack_valid(cdata.tangent_offset)) {
          bsdf->T = stack_load_float3(stack, cdata.tangent_offset);
          const float aspect = sqrtf(1.0f - anisotropy * 0.9f);
          bsdf->alpha_x /= aspect;
          bsdf->alpha_y *= aspect;
          const float anisotropic_rotation = stack_load(stack, cdata.rotation);
          if (anisotropic_rotation != 0.0f) {
            bsdf->T = rotate_around_axis(bsdf->T, N, anisotropic_rotation * M_2PI_F);
          }
        }
        else {
          bsdf->T = zero_float3();
        }

        bsdf->N = valid_reflection_N;
        bsdf->ior = 1.0f;

        const float thin_film_thickness = fmaxf(stack_load(stack, cdata.thin_film_thickness),
                                                1e-5f);
        const float thin_film_ior = fmaxf(stack_load(stack, cdata.thin_film_ior), 1e-5f);

        const ClosureType distribution = cdata.distribution;
        /* Setup BSDF */
        if (distribution == CLOSURE_BSDF_MICROFACET_BECKMANN_ID) {
          sd->runtime_flag |= bsdf_microfacet_beckmann_setup(bsdf);
        }
        else {
          sd->runtime_flag |= bsdf_microfacet_ggx_setup(bsdf);
        }

        const bool is_multiggx = (distribution == CLOSURE_BSDF_MICROFACET_MULTI_GGX_ID);

        if (type == CLOSURE_BSDF_PHYSICAL_CONDUCTOR) {
          ccl_private FresnelConductor *fresnel = (ccl_private FresnelConductor *)
              closure_alloc_extra(sd, sizeof(FresnelConductor));

          if (!fresnel) {
            break;
          }

          fresnel->thin_film.thickness = thin_film_thickness;
          fresnel->thin_film.ior = thin_film_ior;

          const float3 n = max(stack_load(stack, cdata.base_ior), zero_float3());
          const float3 k = max(stack_load(stack, cdata.edge_tint_k), zero_float3());

          fresnel->ior = {rgb_to_spectrum(n), rgb_to_spectrum(k)};
          bsdf_microfacet_setup_fresnel_conductor(kg, bsdf, sd->wi, fresnel, is_multiggx);
        }
        else {
          ccl_private FresnelF82Tint *fresnel = (ccl_private FresnelF82Tint *)closure_alloc_extra(
              sd, sizeof(FresnelF82Tint));

          if (!fresnel) {
            break;
          }

          fresnel->thin_film.thickness = thin_film_thickness;
          fresnel->thin_film.ior = thin_film_ior;

          const float3 color = saturate(stack_load(stack, cdata.base_ior));
          const float3 tint = saturate(stack_load(stack, cdata.edge_tint_k));

          fresnel->f0 = rgb_to_spectrum(color);
          const Spectrum f82 = rgb_to_spectrum(tint);
          bsdf_microfacet_setup_fresnel_f82_tint(kg, bsdf, sd->wi, fresnel, f82, is_multiggx);
        }
      }
      break;
    }
    case CLOSURE_BSDF_RAY_PORTAL_ID: {
      const ccl_global SVMNodeRayPortalBsdfData &bsdf_data =
          svm_node_get<SVMNodeRayPortalBsdfData>(kg, &offset);
      const Spectrum weight = closure_weight * mix_weight;
      const float3 position = stack_load_float3_default(stack, bsdf_data.position_offset, sd->P);
      const float3 direction = stack_load(stack, bsdf_data.direction);
      bsdf_ray_portal_setup(sd, weight, position, direction);
      break;
    }
    case CLOSURE_BSDF_MICROFACET_GGX_ID:
    case CLOSURE_BSDF_MICROFACET_BECKMANN_ID:
    case CLOSURE_BSDF_ASHIKHMIN_SHIRLEY_ID:
    case CLOSURE_BSDF_MICROFACET_MULTI_GGX_ID: {
      const ccl_global SVMNodeGlossyBsdfData &bsdf_data = svm_node_get<SVMNodeGlossyBsdfData>(
          kg, &offset);

#ifdef __CAUSTICS_TRICKS__
      if (!kernel_data.integrator.caustics_reflective &&
          (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE))
      {
        break;
      }
#endif
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
          sd, sizeof(MicrofacetBsdf), weight);

      if (!bsdf) {
        break;
      }

      const float roughness = sqr(saturatef(stack_load(stack, bsdf_data.roughness)));

      bsdf->N = maybe_ensure_valid_specular_reflection(sd, N);
      bsdf->ior = 1.0f;

      /* compute roughness */
      const float anisotropy = clamp(stack_load(stack, bsdf_data.anisotropy), -0.99f, 0.99f);
      if (!stack_valid(bsdf_data.tangent_offset) || fabsf(anisotropy) <= 1e-4f) {
        /* Isotropic case. */
        bsdf->T = zero_float3();
        bsdf->alpha_x = roughness;
        bsdf->alpha_y = roughness;
      }
      else {
        bsdf->T = stack_load_float3(stack, bsdf_data.tangent_offset);

        /* Rotate tangent. */
        const float rotation = stack_load(stack, bsdf_data.rotation);
        if (rotation != 0.0f) {
          bsdf->T = rotate_around_axis(bsdf->T, bsdf->N, rotation * M_2PI_F);
        }

        if (anisotropy < 0.0f) {
          bsdf->alpha_x = roughness / (1.0f + anisotropy);
          bsdf->alpha_y = roughness * (1.0f + anisotropy);
        }
        else {
          bsdf->alpha_x = roughness * (1.0f - anisotropy);
          bsdf->alpha_y = roughness / (1.0f - anisotropy);
        }
      }

      /* setup bsdf */
      if (type == CLOSURE_BSDF_MICROFACET_BECKMANN_ID) {
        sd->runtime_flag |= bsdf_microfacet_beckmann_setup(bsdf);
      }
      else if (type == CLOSURE_BSDF_ASHIKHMIN_SHIRLEY_ID) {
        sd->runtime_flag |= bsdf_ashikhmin_shirley_setup(bsdf);
      }
      else {
        sd->runtime_flag |= bsdf_microfacet_ggx_setup(bsdf);
        if (type == CLOSURE_BSDF_MICROFACET_MULTI_GGX_ID) {
          const Spectrum color = max(rgb_to_spectrum(stack_load(stack, bsdf_data.color)), black);
          bsdf_microfacet_setup_fresnel_constant(kg, bsdf, sd->wi, color);
        }
      }

      break;
    }
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
    case CLOSURE_BSDF_MICROFACET_BECKMANN_REFRACTION_ID: {
      const ccl_global SVMNodeRefractionBsdfData &bsdf_data =
          svm_node_get<SVMNodeRefractionBsdfData>(kg, &offset);

#ifdef __CAUSTICS_TRICKS__
      if (!kernel_data.integrator.caustics_refractive &&
          (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE))
      {
        break;
      }
#endif
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
          sd, sizeof(MicrofacetBsdf), weight);

      if (bsdf) {
        bsdf->N = maybe_ensure_valid_specular_reflection(sd, N);
        bsdf->T = zero_float3();

        float eta = fmaxf(stack_load(stack, bsdf_data.ior), 1e-5f);
        eta = (sd->runtime_flag & SR_BACKFACING) ? 1.0f / eta : eta;

        /* setup bsdf */
        const float roughness = sqr(stack_load(stack, bsdf_data.roughness));
        bsdf->alpha_x = roughness;
        bsdf->alpha_y = roughness;
        bsdf->ior = eta;

        if (type == CLOSURE_BSDF_MICROFACET_BECKMANN_REFRACTION_ID) {
          sd->runtime_flag |= bsdf_microfacet_beckmann_refraction_setup(bsdf);
        }
        else {
          sd->runtime_flag |= bsdf_microfacet_ggx_refraction_setup(bsdf);
        }
      }

      break;
    }
    case CLOSURE_BSDF_MICROFACET_GGX_GLASS_ID:
    case CLOSURE_BSDF_MICROFACET_BECKMANN_GLASS_ID:
    case CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID: {
      const ccl_global SVMNodeGlassBsdfData &bsdf_data = svm_node_get<SVMNodeGlassBsdfData>(
          kg, &offset);

#ifdef __CAUSTICS_TRICKS__
      const bool reflective_caustics = (kernel_data.integrator.caustics_reflective ||
                                        (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE) == 0);
      const bool refractive_caustics = (kernel_data.integrator.caustics_refractive ||
                                        (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE) == 0);
      if (!(reflective_caustics || refractive_caustics)) {
        break;
      }
#else
      const bool reflective_caustics = true;
      const bool refractive_caustics = true;
#endif

      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const float thinfilm_thickness = stack_load(stack, bsdf_data.thin_film_thickness);
      const float thinfilm_ior = fmaxf(stack_load(stack, bsdf_data.thin_film_ior), 1e-5f);

      ccl_private MicrofacetBsdf *bsdf = (ccl_private MicrofacetBsdf *)bsdf_alloc(
          sd, sizeof(MicrofacetBsdf), make_spectrum(mix_weight));
      ccl_private FresnelGeneralizedSchlick *fresnel =
          (bsdf != nullptr) ? (ccl_private FresnelGeneralizedSchlick *)closure_alloc_extra(
                                  sd, sizeof(FresnelGeneralizedSchlick)) :
                              nullptr;

      if (bsdf && fresnel) {
        bsdf->N = maybe_ensure_valid_specular_reflection(sd, N);
        const float anisotropy = clamp(stack_load(stack, bsdf_data.anisotropy), -0.99f, 0.99f);
        const float roughness = sqr(saturatef(stack_load(stack, bsdf_data.roughness)));
        if (!stack_valid(bsdf_data.tangent_offset) || fabsf(anisotropy) <= 1e-4f) {
          /* Isotropic case. */
          bsdf->T = zero_float3();
          bsdf->alpha_x = bsdf->alpha_y = roughness;
        }
        else {
          bsdf->T = stack_load_float3(stack, bsdf_data.tangent_offset);

          /* Rotate tangent. */
          const float rotation = stack_load(stack, bsdf_data.rotation);
          if (rotation != 0.0f) {
            bsdf->T = rotate_around_axis(bsdf->T, bsdf->N, rotation * M_2PI_F);
          }

          if (anisotropy < 0.0f) {
            bsdf->alpha_x = roughness / (1.0f + anisotropy);
            bsdf->alpha_y = roughness * (1.0f + anisotropy);
          }
          else {
            bsdf->alpha_x = roughness * (1.0f - anisotropy);
            bsdf->alpha_y = roughness / (1.0f - anisotropy);
          }
        }

        const float ior = fmaxf(stack_load(stack, bsdf_data.ior), 1e-5f);
        bsdf->ior = (sd->runtime_flag & SR_BACKFACING) ? 1.0f / ior : ior;

        fresnel->f0 = make_float3(F0_from_ior(ior));
        fresnel->f90 = white;
        fresnel->exponent = -ior;
        const Spectrum color = max(rgb_to_spectrum(stack_load(stack, bsdf_data.color)), black);
        fresnel->tint = {float(reflective_caustics) * color, float(refractive_caustics) * color};
        fresnel->thin_film.thickness = thinfilm_thickness;
        fresnel->thin_film.ior = (sd->runtime_flag & SR_BACKFACING) ? thinfilm_ior / ior :
                                                                      thinfilm_ior;
        /* setup bsdf */
        if (type == CLOSURE_BSDF_MICROFACET_BECKMANN_GLASS_ID) {
          sd->runtime_flag |= bsdf_microfacet_beckmann_glass_setup(bsdf);
        }
        else {
          sd->runtime_flag |= bsdf_microfacet_ggx_glass_setup(bsdf);
        }
        const bool is_multiggx = (type == CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID);
        bsdf_microfacet_setup_fresnel_generalized_schlick(kg, bsdf, sd->wi, fresnel, is_multiggx);
      }
      break;
    }
    case CLOSURE_BSDF_ASHIKHMIN_VELVET_ID: {
      const ccl_global SVMNodeSimpleBsdfData &bsdf_data = svm_node_get<SVMNodeSimpleBsdfData>(
          kg, &offset);
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      ccl_private VelvetBsdf *bsdf = (ccl_private VelvetBsdf *)bsdf_alloc(
          sd, sizeof(VelvetBsdf), weight);

      if (bsdf) {
        bsdf->N = N;

        bsdf->sigma = saturatef(stack_load(stack, bsdf_data.param1));
        sd->runtime_flag |= bsdf_ashikhmin_velvet_setup(bsdf);
      }
      break;
    }
    case CLOSURE_BSDF_SHEEN_ID: {
      const ccl_global SVMNodeSimpleBsdfData &bsdf_data = svm_node_get<SVMNodeSimpleBsdfData>(
          kg, &offset);
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      const float roughness = saturatef(stack_load(stack, bsdf_data.param1));
      bsdf_sheen_setup(kg, sd, weight, N, roughness);
      break;
    }
    case CLOSURE_BSDF_GLOSSY_TOON_ID:
    case CLOSURE_BSDF_DIFFUSE_TOON_ID: {
      const ccl_global SVMNodeToonBsdfData &bsdf_data = svm_node_get<SVMNodeToonBsdfData>(kg,
                                                                                          &offset);

#ifdef __CAUSTICS_TRICKS__
      if (type == CLOSURE_BSDF_GLOSSY_TOON_ID && !kernel_data.integrator.caustics_reflective &&
          (ray_visibility & PATH_RAY_VISIBILITY_DIFFUSE))
      {
        break;
      }
#endif
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      ccl_private ToonBsdf *bsdf = (ccl_private ToonBsdf *)bsdf_alloc(
          sd, sizeof(ToonBsdf), weight);

      if (bsdf) {
        bsdf->N = N;
        bsdf->size = stack_load(stack, bsdf_data.size);
        bsdf->smooth = stack_load(stack, bsdf_data.smooth);

        if (type == CLOSURE_BSDF_DIFFUSE_TOON_ID) {
          sd->runtime_flag |= bsdf_diffuse_toon_setup(bsdf);
        }
        else {
          sd->runtime_flag |= bsdf_glossy_toon_setup(bsdf);
        }
      }
      break;
    }
#ifdef __HAIR__
#  ifdef __PRINCIPLED_HAIR__
    case CLOSURE_BSDF_HAIR_CHIANG_ID:
    case CLOSURE_BSDF_HAIR_HUANG_ID: {
      const ccl_global SVMNodePrincipledHairBsdfData &hdata =
          svm_node_get<SVMNodePrincipledHairBsdfData>(kg, &offset);

      const Spectrum weight = closure_weight * mix_weight;

      const float alpha = stack_load(stack, hdata.offset);
      const float ior = stack_load(stack, hdata.ior);

      const AttributeDescriptor attr_descr_random = find_attribute(kg, sd, hdata.attr_random);
      float random = 0.0f;
      if (is_attribute_found(attr_descr_random)) {
        random = primitive_surface_attribute<float>(kg, sd, attr_descr_random);
      }
      else {
        random = stack_load(stack, hdata.random);
      }

      /* Random factors range: [-randomization/2, +randomization/2]. */
      const float random_roughness = stack_load(stack, hdata.random_roughness);
      const float factor_random_roughness = 1.0f + 2.0f * (random - 0.5f) * random_roughness;
      const float roughness = stack_load(stack, hdata.roughness) * factor_random_roughness;
      const float radial_roughness = (type == CLOSURE_BSDF_HAIR_CHIANG_ID) ?
                                         stack_load(stack, hdata.radial_roughness) *
                                             factor_random_roughness :
                                         roughness;

      Spectrum sigma;
      switch (hdata.parametrization) {
        case NODE_PRINCIPLED_HAIR_DIRECT_ABSORPTION: {
          const float3 absorption_coefficient = stack_load(stack, hdata.absorption_coefficient);
          sigma = rgb_to_spectrum(absorption_coefficient);
          break;
        }
        case NODE_PRINCIPLED_HAIR_PIGMENT_CONCENTRATION: {
          float melanin = stack_load(stack, hdata.melanin);
          const float melanin_redness = stack_load(stack, hdata.melanin_redness);

          /* Randomize melanin. */
          float random_color = stack_load(stack, hdata.random_color);
          random_color = clamp(random_color, 0.0f, 1.0f);
          const float factor_random_color = 1.0f + 2.0f * (random - 0.5f) * random_color;
          melanin *= factor_random_color;

          /* Map melanin 0..inf from more perceptually linear 0..1. */
          melanin = -logf(fmaxf(1.0f - melanin, 0.0001f));

          /* Benedikt Bitterli's melanin ratio remapping. */
          const float eumelanin = melanin * (1.0f - melanin_redness);
          const float pheomelanin = melanin * melanin_redness;
          const Spectrum melanin_sigma = bsdf_principled_hair_sigma_from_concentration(
              eumelanin, pheomelanin);

          /* Optional tint. */
          const float3 tint = stack_load(stack, hdata.tint);
          const Spectrum tint_sigma = bsdf_principled_hair_sigma_from_reflectance(
              rgb_to_spectrum(tint), radial_roughness);

          sigma = melanin_sigma + tint_sigma;
          break;
        }
        case NODE_PRINCIPLED_HAIR_REFLECTANCE: {
          const float3 color = stack_load(stack, hdata.color);
          sigma = bsdf_principled_hair_sigma_from_reflectance(rgb_to_spectrum(color),
                                                              radial_roughness);
          break;
        }
        default: {
          /* Fallback to brownish hair, same as defaults for melanin. */
          kernel_assert(!"Invalid Hair parametrization!");
          sigma = bsdf_principled_hair_sigma_from_concentration(0.0f, 0.8054375f);
          break;
        }
      }

      if (type == CLOSURE_BSDF_HAIR_CHIANG_ID) {
        ccl_private ChiangHairBSDF *bsdf = (ccl_private ChiangHairBSDF *)bsdf_alloc(
            sd, sizeof(ChiangHairBSDF), weight);
        if (bsdf) {
          /* Remap Coat value to [0, 100]% of Roughness. */
          const float coat = stack_load(stack, hdata.coat);
          const float m0_roughness = 1.0f - clamp(coat, 0.0f, 1.0f);

          bsdf->v = roughness;
          bsdf->s = radial_roughness;
          bsdf->m0_roughness = m0_roughness;
          bsdf->alpha = alpha;
          bsdf->eta = ior;
          bsdf->sigma = sigma;

          sd->runtime_flag |= bsdf_hair_chiang_setup(sd, bsdf);
        }
      }
      else {
        kernel_assert(type == CLOSURE_BSDF_HAIR_HUANG_ID);
        const float R = stack_load(stack, hdata.R);
        const float TT = stack_load(stack, hdata.TT);
        const float TRT = stack_load(stack, hdata.TRT);
        if (R <= 0.0f && TT <= 0.0f && TRT <= 0.0f) {
          break;
        }

        ccl_private HuangHairBSDF *bsdf = (ccl_private HuangHairBSDF *)bsdf_alloc(
            sd, sizeof(HuangHairBSDF), weight);
        if (bsdf) {
          ccl_private HuangHairExtra *extra = (ccl_private HuangHairExtra *)closure_alloc_extra(
              sd, sizeof(HuangHairExtra));

          if (!extra) {
            break;
          }

          bsdf->extra = extra;
          bsdf->extra->R = fmaxf(0.0f, R);
          bsdf->extra->TT = fmaxf(0.0f, TT);
          bsdf->extra->TRT = fmaxf(0.0f, TRT);

          bsdf->extra->pixel_coverage = 1.0f;

          /* For camera ray, check if the hair covers more than one pixel, in which case a
           * nearfield model is needed to prevent ribbon-like appearance. */
          if ((ray_visibility & PATH_RAY_VISIBILITY_CAMERA) && (sd->type & PRIMITIVE_CURVE)) {
            /* Interpolate radius between curve keys. */
            const KernelCurve kcurve = kernel_data_fetch(curves, sd->prim);
            const int k0 = kcurve.first_key + PRIMITIVE_UNPACK_SEGMENT(sd->type);
            const int k1 = k0 + 1;
            const int position_offset = kernel_data_fetch(objects, sd->object).position_offset;
            const float radius = mix(kernel_data_fetch(curve_keys, position_offset + k0).w,
                                     kernel_data_fetch(curve_keys, position_offset + k1).w,
                                     sd->u);

            bsdf->extra->pixel_coverage = 0.5f * sd->dP / radius;
          }

          bsdf->aspect_ratio = stack_load(stack, hdata.aspect_ratio);
          if (bsdf->aspect_ratio != 1.0f) {
            /* Align ellipse major axis with the curve normal direction. */
            const AttributeDescriptor attr_descr_normal = find_attribute(
                kg, sd, hdata.attr_normal);
            bsdf->N = curve_attribute<float3>(kg, sd, attr_descr_normal);
          }

          bsdf->roughness = roughness;
          bsdf->tilt = alpha;
          bsdf->eta = ior;
          bsdf->sigma = sigma;

          sd->runtime_flag |= bsdf_hair_huang_setup(sd, bsdf, path_flag);
        }
      }
      break;
    }
#  endif /* __PRINCIPLED_HAIR__ */
    case CLOSURE_BSDF_HAIR_REFLECTION_ID:
    case CLOSURE_BSDF_HAIR_TRANSMISSION_ID: {
      const ccl_global SVMNodeHairBsdfData &bsdf_data = svm_node_get<SVMNodeHairBsdfData>(kg,
                                                                                          &offset);

      const Spectrum weight = closure_weight * mix_weight;

      ccl_private HairBsdf *bsdf = (ccl_private HairBsdf *)bsdf_alloc(
          sd, sizeof(HairBsdf), weight);

      if (bsdf) {
        bsdf->N = maybe_ensure_valid_specular_reflection(sd, sd->N);
        bsdf->roughness1 = stack_load(stack, bsdf_data.roughness1);
        bsdf->roughness2 = stack_load(stack, bsdf_data.roughness2);
        bsdf->offset = -stack_load(stack, bsdf_data.offset);

        if (stack_valid(bsdf_data.tangent_offset)) {
          bsdf->T = normalize(stack_load_float3(stack, bsdf_data.tangent_offset));
        }
        else if (!(sd->type & PRIMITIVE_CURVE)) {
          bsdf->T = normalize(sd->dPdv);
          bsdf->offset = 0.0f;
        }
        else {
          bsdf->T = normalize(sd->dPdu);
        }

        if (type == CLOSURE_BSDF_HAIR_REFLECTION_ID) {
          sd->runtime_flag |= bsdf_hair_reflection_setup(bsdf);
        }
        else {
          sd->runtime_flag |= bsdf_hair_transmission_setup(bsdf);
        }
      }

      break;
    }
#endif /* __HAIR__ */

#ifdef __SUBSURFACE__
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSSRDF_RANDOM_WALK_ID:
    case CLOSURE_BSSRDF_RANDOM_WALK_LEGACY_ID:
    case CLOSURE_BSSRDF_RANDOM_WALK_SKIN_ID: {
      const ccl_global SVMNodeBssrdfData &bsdf_data = svm_node_get<SVMNodeBssrdfData>(kg, &offset);
      float3 N = stack_load_float3_default(stack, bsdf_data.normal_offset, sd->N);
      N = safe_normalize_fallback(N, sd->N);

      const Spectrum weight = closure_weight * mix_weight;
      ccl_private Bssrdf *bssrdf = bssrdf_alloc(sd, weight);

      if (bssrdf) {
        const float scale = stack_load(stack, bsdf_data.scale);
        bssrdf->radius = max(rgb_to_spectrum(stack_load(stack, bsdf_data.radius) * scale), black);
        bssrdf->albedo = closure_weight;
        bssrdf->N = maybe_ensure_valid_specular_reflection(sd, N);
        bssrdf->ior = stack_load(stack, bsdf_data.ior);
        bssrdf->alpha = saturatef(stack_load(stack, bsdf_data.roughness));
        bssrdf->anisotropy = stack_load(stack, bsdf_data.anisotropy);

        sd->runtime_flag |= bssrdf_setup(sd, bssrdf, path_flag, type);
      }

      break;
    }
#endif
    default:
      /* Unknown closure type, skip the minimum data payload. */
      svm_node_get<SVMNodeSimpleBsdfData>(kg, &offset);
      break;
  }

  return offset;
}

ccl_device_inline void svm_alloc_closure_volume_scatter(ccl_private ShaderData *sd,
                                                        ccl_private float *stack,
                                                        Spectrum weight,
                                                        const uint type,
                                                        const SVMInputFloat param1,
                                                        const SVMInputFloat param_extra)
{
  switch (type) {
    case CLOSURE_VOLUME_HENYEY_GREENSTEIN_ID: {
      ccl_private HenyeyGreensteinVolume *volume = (ccl_private HenyeyGreensteinVolume *)
          bsdf_alloc(sd, sizeof(HenyeyGreensteinVolume), weight);
      if (volume) {
        volume->g = stack_load(stack, param1);
        sd->runtime_flag |= volume_henyey_greenstein_setup(volume);
      }
    } break;
    case CLOSURE_VOLUME_FOURNIER_FORAND_ID: {
      ccl_private FournierForandVolume *volume = (ccl_private FournierForandVolume *)bsdf_alloc(
          sd, sizeof(FournierForandVolume), weight);
      if (volume) {
        const float IOR = stack_load(stack, param1);
        const float B = stack_load(stack, param_extra);
        sd->runtime_flag |= volume_fournier_forand_setup(volume, B, IOR);
      }
    } break;
    case CLOSURE_VOLUME_RAYLEIGH_ID: {
      ccl_private RayleighVolume *volume = (ccl_private RayleighVolume *)bsdf_alloc(
          sd, sizeof(RayleighVolume), weight);
      if (volume) {
        sd->runtime_flag |= volume_rayleigh_setup(volume);
      }
      break;
    }
    case CLOSURE_VOLUME_DRAINE_ID: {
      ccl_private DraineVolume *volume = (ccl_private DraineVolume *)bsdf_alloc(
          sd, sizeof(DraineVolume), weight);
      if (volume) {
        volume->g = stack_load(stack, param1);
        volume->alpha = stack_load(stack, param_extra);
        sd->runtime_flag |= volume_draine_setup(volume);
      }
    } break;
    case CLOSURE_VOLUME_MIE_ID: {
      const float d = stack_load(stack, param1);
      float g_HG;
      float g_D;
      float alpha;
      float mixture;
      phase_mie_fitted_parameters(d, &g_HG, &g_D, &alpha, &mixture);
      ccl_private HenyeyGreensteinVolume *hg = (ccl_private HenyeyGreensteinVolume *)bsdf_alloc(
          sd, sizeof(HenyeyGreensteinVolume), weight * (1.0f - mixture));
      if (hg) {
        hg->g = g_HG;
        sd->runtime_flag |= volume_henyey_greenstein_setup(hg);
      }
      ccl_private DraineVolume *draine = (ccl_private DraineVolume *)bsdf_alloc(
          sd, sizeof(DraineVolume), weight * mixture);
      if (draine) {
        draine->g = g_D;
        draine->alpha = alpha;
        sd->runtime_flag |= volume_draine_setup(draine);
      }
    } break;
    default: {
      kernel_assert(0);
      break;
    }
  }
}

template<ShaderType shader_type>
ccl_device_noinline void svm_node_closure_volume(
    KernelGlobals kg,
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    Spectrum closure_weight,
    const ccl_global SVMNodeClosureVolume &ccl_restrict node)
{
#ifdef __VOLUME__
  /* Only sum extinction for volumes, variable is shared with surface transparency. */
  if (shader_type != SHADER_TYPE_VOLUME) {
    return;
  }

  const float mix_weight = stack_load_float_default(stack, node.mix_weight_offset, 1.0f);
  if (mix_weight == 0.0f) {
    return;
  }

  float density = stack_load(stack, node.density);
  density = mix_weight * fmaxf(density, 0.0f) * object_volume_density(kg, sd->object);

  /* Compute scattering coefficient. */
  Spectrum weight = closure_weight;

  if (node.closure_type == CLOSURE_VOLUME_ABSORPTION_ID) {
    weight = one_spectrum() - weight;
  }

  weight *= density;

  /* Add closure for volume scattering. */
  if (CLOSURE_IS_VOLUME_SCATTER(node.closure_type)) {
    svm_alloc_closure_volume_scatter(
        sd, stack, weight, node.closure_type, node.param1, node.param_extra);
  }

  /* Sum total extinction weight. */
  volume_extinction_setup(sd, weight);
#endif
}

template<ShaderType shader_type>
ccl_device_noinline void svm_node_volume_coefficients(
    KernelGlobals kg,
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    Spectrum scatter_coeffs,
    const ccl_global SVMNodeVolumeCoefficients &ccl_restrict node,
    const PathRayVisibility path_visibility,
    const uint32_t path_flag)
{
#ifdef __VOLUME__
  /* Only sum extinction for volumes, variable is shared with surface transparency. */
  if (shader_type != SHADER_TYPE_VOLUME) {
    return;
  }

  const float mix_weight = stack_load_float_default(stack, node.mix_weight_offset, 1.0f);
  if (mix_weight == 0.0f) {
    return;
  }

  /* Compute scattering coefficient. */
  const float weight = mix_weight * object_volume_density(kg, sd->object);

  /* Add closure for volume scattering. */
  if (!is_zero(scatter_coeffs) && CLOSURE_IS_VOLUME_SCATTER(node.closure_type)) {
    svm_alloc_closure_volume_scatter(
        sd, stack, weight * scatter_coeffs, node.closure_type, node.param1, node.param_extra);
  }

  const float3 absorption_coeffs = stack_load(stack, node.absorption_coeffs);
  volume_extinction_setup(sd, weight * (scatter_coeffs + absorption_coeffs));

  /* Compute emission. */
  if ((path_visibility & PATH_RAY_VISIBILITY_SHADOW) || (path_flag & PATH_RAY_EXTINCTION)) {
    /* Don't need emission for shadows and extinction. */
    return;
  }

  const float3 emission_coeffs = stack_load(stack, node.emission_coeffs);
  if (is_zero(emission_coeffs)) {
    return;
  }
  emission_setup(sd, weight * emission_coeffs);

#endif
}

template<ShaderType shader_type>
ccl_device_noinline void svm_node_principled_volume(
    KernelGlobals kg,
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    const Spectrum closure_weight,
    const ccl_global SVMNodePrincipledVolume &ccl_restrict node,
    const PathRayVisibility path_visibility,
    const uint32_t path_flag)
{
#ifdef __VOLUME__
  /* Only sum extinction for volumes, variable is shared with surface transparency. */
  if (shader_type != SHADER_TYPE_VOLUME) {
    return;
  }

  const float mix_weight = stack_load_float_default(stack, node.mix_weight_offset, 1.0f);

  if (mix_weight == 0.0f) {
    return;
  }

  /* Compute density. */
  const float weight = mix_weight * object_volume_density(kg, sd->object);
  float primitive_density = 1.0f;
  float density = stack_load(stack, node.density);
  density = weight * fmaxf(density, 0.0f);

  if (density > 0.0f) {
    /* Density and color attribute lookup if available. */
    const AttributeDescriptor attr_density = find_attribute(kg, sd, node.attr_density);
    if (is_attribute_found(attr_density)) {
      primitive_density = primitive_volume_attribute<float>(kg, sd, attr_density, true);
      density = fmaxf(density * primitive_density, 0.0f);
    }
  }

  if (density > 0.0f) {
    /* Compute scattering color. */
    Spectrum color = closure_weight;

    const AttributeDescriptor attr_color = find_attribute(kg, sd, node.attr_color);
    if (is_attribute_found(attr_color)) {
      color *= rgb_to_spectrum(primitive_volume_attribute<float3>(kg, sd, attr_color, true));
    }

    /* Add closure for volume scattering. */
    ccl_private HenyeyGreensteinVolume *volume = (ccl_private HenyeyGreensteinVolume *)bsdf_alloc(
        sd, sizeof(HenyeyGreensteinVolume), color * density);
    if (volume) {
      const float anisotropy = stack_load(stack, node.anisotropy);
      volume->g = anisotropy;
      sd->runtime_flag |= volume_henyey_greenstein_setup(volume);
    }

    /* Add extinction weight. */
    const float3 absorption_color = max(sqrt(stack_load(stack, node.absorption_color)),
                                        zero_float3());

    const Spectrum zero = zero_spectrum();
    const Spectrum one = one_spectrum();
    const Spectrum absorption = max(one - color, zero) *
                                max(one - rgb_to_spectrum(absorption_color), zero);
    volume_extinction_setup(sd, (color + absorption) * density);
  }

  /* Compute emission. */
  if ((path_visibility & PATH_RAY_VISIBILITY_SHADOW) || (path_flag & PATH_RAY_EXTINCTION)) {
    /* Don't need emission for shadows and extinction. */
    return;
  }

  const float emission = stack_load(stack, node.emission);
  const float blackbody = stack_load(stack, node.blackbody);

  if (emission > 0.0f) {
    const float3 emission_color = stack_load(stack, node.emission_color);
    emission_setup(sd, rgb_to_spectrum(emission * emission_color * weight));
  }

  if (blackbody > 0.0f) {
    float T = stack_load(stack, node.temperature);

    /* Add flame temperature from attribute if available. */
    const AttributeDescriptor attr_temperature = find_attribute(kg, sd, node.attr_temperature);
    if (is_attribute_found(attr_temperature)) {
      const float temperature = primitive_volume_attribute<float>(kg, sd, attr_temperature, true);
      T *= fmaxf(temperature, 0.0f);
    }

    T = fmaxf(T, 0.0f);

    /* Stefan-Boltzmann law. */
    const float T4 = sqr(sqr(T));
    const float sigma = 5.670373e-8f * 1e-6f / M_PI_F;
    const float intensity = sigma * mix(1.0f, T4, blackbody);

    if (intensity > 0.0f) {
      const float3 blackbody_tint = stack_load(stack, node.blackbody_tint);
      const float3 bb = blackbody_tint * intensity *
                        rec709_to_rgb(kg, svm_math_blackbody_color_rec709(T));
      emission_setup(sd, rgb_to_spectrum(bb * weight));
    }
  }
#endif
}

ccl_device_noinline void svm_node_closure_emission(
    KernelGlobals kg,
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    Spectrum closure_weight,
    const ccl_global SVMNodeClosureEmission &ccl_restrict node,
    const PathRayVisibility path_visibility,
    const uint32_t path_flag)
{
  Spectrum weight = closure_weight;

  if (stack_valid(node.mix_weight_offset)) {
    const float mix_weight = stack_load_float(stack, node.mix_weight_offset);

    if (mix_weight == 0.0f) {
      return;
    }

    weight *= mix_weight;
  }

  if (sd->runtime_flag & SR_IS_VOLUME_SHADER_EVAL) {
    if ((path_visibility & PATH_RAY_VISIBILITY_SHADOW) || (path_flag & PATH_RAY_EXTINCTION)) {
      /* Don't need emission for shadows and extinction. */
      return;
    }
    weight *= object_volume_density(kg, sd->object);
  }

  emission_setup(sd, weight);
}

ccl_device_noinline void svm_node_closure_background(
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    Spectrum closure_weight,
    const ccl_global SVMNodeClosureBackground &ccl_restrict node)
{
  Spectrum weight = closure_weight;

  if (stack_valid(node.mix_weight_offset)) {
    const float mix_weight = stack_load_float(stack, node.mix_weight_offset);

    if (mix_weight == 0.0f) {
      return;
    }

    weight *= mix_weight;
  }

  background_setup(sd, weight);
}

ccl_device_noinline void svm_node_closure_holdout(
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    Spectrum closure_weight,
    const ccl_global SVMNodeClosureHoldout &ccl_restrict node)
{
  if (stack_valid(node.mix_weight_offset)) {
    const float mix_weight = stack_load_float(stack, node.mix_weight_offset);

    if (mix_weight == 0.0f) {
      return;
    }

    closure_alloc(sd, sizeof(ShaderClosure), CLOSURE_HOLDOUT_ID, closure_weight * mix_weight);
  }
  else {
    closure_alloc(sd, sizeof(ShaderClosure), CLOSURE_HOLDOUT_ID, closure_weight);
  }

  sd->runtime_flag |= SR_HOLDOUT;
}

/* Closure Nodes */

ccl_device void svm_node_closure_set_weight(ccl_private Spectrum *closure_weight,
                                            const ccl_global SVMNodeClosureSetWeight &ccl_restrict
                                                node)
{
  *closure_weight = rgb_to_spectrum(node.rgb);
}

ccl_device void svm_node_closure_weight(ccl_private float *ccl_restrict stack,
                                        ccl_private Spectrum *closure_weight,
                                        const ccl_global SVMNodeClosureWeight &ccl_restrict node)
{
  *closure_weight = rgb_to_spectrum(stack_load_float3(stack, node.weight_offset));
}

ccl_device void svm_node_emission_weight(ccl_private float *ccl_restrict stack,
                                         ccl_private Spectrum *closure_weight,
                                         const ccl_global SVMNodeEmissionWeight &ccl_restrict node)
{
  const float strength = stack_load(stack, node.strength);
  *closure_weight = rgb_to_spectrum(stack_load(stack, node.color)) * strength;
}

ccl_device_noinline void svm_node_mix_closure(
    ccl_private float *ccl_restrict stack, const ccl_global SVMNodeMixClosure &ccl_restrict node)
{
  /* fetch weight from blend input, previous mix closures,
   * and write to stack to be used by closure nodes later */
  float weight = stack_load(stack, node.fac);
  weight = saturatef(weight);

  const float in_weight = stack_load_float_default(stack, node.in_weight_offset, 1.0f);

  if (stack_valid(node.weight1_offset)) {
    stack_store_float(stack, node.weight1_offset, in_weight * (1.0f - weight));
  }
  if (stack_valid(node.weight2_offset)) {
    stack_store_float(stack, node.weight2_offset, in_weight * weight);
  }
}

/* (Bump) normal */

ccl_device void svm_node_set_normal(ccl_private ShaderData *sd,
                                    ccl_private float *ccl_restrict stack,
                                    const ccl_global SVMNodeClosureSetNormal &ccl_restrict node)
{
  const float3 normal = stack_load_float3(stack, node.direction_offset);
  sd->N = normal;
  stack_store_float3(stack, node.normal_offset, normal);
}

CCL_NAMESPACE_END
