/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Hair BSDF.
 *
 * EEVEE approximates the hair BSDF with a single isotropic GGX specular lobe around the
 * cylinder normal (which sweeps around the strand and produces the highlight streak).
 * The node tree pass folds the Principled Hair BSDF inputs (tint, roughness, offset) into
 * the closure, so all the deferred/light paths can reuse the isotropic GGX helpers.
 */

#include "eevee_bxdf_microfacet.bsl.hh"
#include "eevee_bxdf_types.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

/* Effective isotropic roughness of the hair lobe. */
float bxdf_hair_roughness_get(const ClosureHair &cl)
{
  return max(cl.roughness.x, cl.roughness.y);
}

BsdfEval bxdf_hair_eval(const ClosureHair &cl, float3 N, float3 L, float3 V, bool do_clamp)
{
  float roughness_sq = square(bxdf_hair_roughness_get(cl));
  return bxdf_ggx_eval_reflection(N, L, V, roughness_sq, do_clamp);
}

BsdfSample bxdf_hair_sample(float3 rand, float3 Vt, const ClosureHair &cl, bool do_clamp)
{
  /* `Vt` is the view direction in the tangent frame of `cl.N` (see ray generation). */
  return bxdf_ggx_sample_reflection(rand, Vt, square(bxdf_hair_roughness_get(cl)), do_clamp);
}

LightProbeRay bxdf_hair_lightprobe(const ClosureHair &cl, float3 N, float3 V)
{
  LightProbeRay probe;
  probe.perceptual_roughness = bxdf_hair_roughness_get(cl);
  probe.dominant_direction = bxdf_ggx_dominant_direction_reflection(
      N, V, probe.perceptual_roughness);
  return probe;
}

ClosureLight bxdf_hair_light([[resource_table]] const UtilityTexture &util_tx,
                             const ClosureHair &cl,
                             float3 N,
                             float3 V)
{
  ClosureReflection reflection;
  reflection.color = cl.color;
  reflection.N = N;
  reflection.roughness = bxdf_hair_roughness_get(cl);
  return bxdf_ggx_light_reflection(util_tx, reflection, V);
}
