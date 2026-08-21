/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// sample_reference.h
// persistent light sample reference

#pragma once

#include "kernel/light/common.h"

#include "kernel/light/area.h"
#include "kernel/light/background.h"
#include "kernel/light/point.h"
#include "kernel/light/spot.h"
#include "kernel/light/sun.h"
// #include "kernel/light/triangle.h" // !!!
#include "kernel/sample/lcg.h"
#include "kernel/types.h"

#include "kernel/types.h"
#include "util/math_float2.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline float2 restir_direction_encode(const float3 direction)
{
  return map_to_sphere(direction);
}

ccl_device_inline float3 restir_direction_decode(const float2 uv)
{
  return map_to_sphere(uv); // !!! 
}

ccl_device_inline float2 restir_area_position_encode(const ccl_global KernelLight *klight,
                                                     const float3 light_P)
{
  return area_light_uv(klight, light_P);
}

ccl_device_inline float3 restir_area_position_decode(const ccl_global KernelLight *klight,
                                                     const float2 uv)
{
  const float v = uv.x - 0.5f;
  const float u = -uv.y - v;

  return klight->co +
         u * klight->area.len_u * klight->area.axis_u +
         v * klight->area.len_v * klight->area.axis_v;
}

ccl_device_inline float2 restir_spot_position_encode(KernelGlobals kg,
                                                     const ccl_global KernelLight *klight,
                                                     const float3 light_P)
{
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const float3 local_P = transform_point(&itfm, light_P);

  const float radius = klight->spot.radius;

  if (radius < 0.0f) { // !!!: Ensure correctness!
    return zero_float2();
  }
  // TODO: Handle disks? radius == 0

  return make_float2(local_P.x / radius, local_P.y / radius);
}

// !!!: The problem is that reference_from_sample() currently feeds them an `ls->P` that came from a different, target-dependent disk.
ccl_device_inline float3 restir_spot_position_decode(
    KernelGlobals kg,
    const ccl_global KernelLight *klight,
    const float2 param)
{
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const Transform tfm = transform_inverse(itfm);

  const float radius = klight->spot.radius;

  const float3 local_P = make_float3(param.x * radius, param.y * radius, 0.0f); // !!!: OverSimplified; not matching Cylce's

// #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
const float3 transform_origin = transform_point(&tfm, zero_float3());

printf("TRANSFORM ORIGIN = %f %f %f\n",
       (double)transform_origin.x,
       (double)transform_origin.y,
       (double)transform_origin.z);

printf("LIGHT CO         = %f %f %f\n",
       (double)klight->co.x,
       (double)klight->co.y,
       (double)klight->co.z);

printf("CENTER ERROR      = %g\n",
       (double)len(transform_origin - klight->co));
// #endif

  return transform_point(&tfm, local_P);
}

// MARK: -
// TODO: Move elsewhere?

// Should be added to ReSTIRReservoir eventually?
ccl_device_inline ReSTIRLightSampleReference restir_light_reference_empty()
{
  ReSTIRLightSampleReference reference{};
  reference.object = OBJECT_NONE;
  reference.prim = PRIM_NONE;
  return reference;
}

ccl_device_inline bool restir_light_reference_is_valid(
    const ccl_private ReSTIRLightSampleReference *reference)
{
  printf("restir_light_reference_is_valid(); returned ((reference->flags & RESTIR_LIGHT_REFERENCE_VALID) != 0) = %u, reference->flags=%u\n",
         ((reference->flags & RESTIR_LIGHT_REFERENCE_VALID) != 0), reference->flags);
  return (reference->flags & RESTIR_LIGHT_REFERENCE_VALID) != 0;
}

// MARK: - Producers

ccl_device_inline bool restir_point_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  *reference = restir_light_reference_empty();

  if (ls->type != LIGHT_POINT || ls->prim < 0) {
    // targets must be reconstructed from LightSample
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
  if ((LightType)klight->type != LIGHT_POINT) {
    return false;
  }

  reference->type = LIGHT_POINT;
  reference->object = ls->object;
  reference->prim = ls->prim;

  if (klight->spot.is_sphere) {
    const Transform itfm = lamp_get_inverse_transform(kg, klight);
    const float3 local_Ng = normalize(transform_direction(&itfm, ls->Ng));

    reference->param = restir_direction_encode(local_Ng);
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                       RESTIR_LIGHT_REFERENCE_HAS_PARAM;
    return true;
  }
/**
At the target point, reconstruct:

ls->P = light_P;
ls->Ng = world_Ng;
ls->D = safe_normalize_len(ls->P - target_P, &ls->t);
 */

  // !!!: Similar to Disk-Spot
  // !!!: Wrong!
  if (klight->spot.radius == 0.0f) {
    reference->param = zero_float2(); // !!!: Wrong!?
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID;
    return true;
  }
/**
At the target:

ls->P = klight->co;
ls->D = safe_normalize_len(ls->P - target_P, &ls->t);
ls->Ng = -ls->D;
ls->eval_fac = klight->spot.eval_fac;
 */

  /* A finite non-sphere point light is a target-facing sampling disk. */
  *reference = restir_light_reference_empty();
  return false;
}

ccl_device_inline bool restir_sun_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  *reference = restir_light_reference_empty();

  if (ls->type != LIGHT_SUN || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
  if ((LightType)klight->type != LIGHT_SUN) {
    return false;
  }

  reference->type = LIGHT_SUN;
  reference->object = ls->object;
  reference->prim = ls->prim;

  // store light-local cone direction
  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const float3 local_Ng = normalize(transform_direction(&itfm, ls->Ng));

  reference->param = restir_direction_encode(local_Ng);
  // reference->param = restir_area_position_encode(klight, ls->P); // !!!: DEBUG ONLY?
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM;

  return true;
}


/** TODO: Investigate whether light_normal_uv_from_position() can be used
 * as a common producer-side *canonicalization* path for emitter-local references.
 * It currently provides the exact area-light UV we need, but other light types
 * still require explicit per-type encode/decode validation.
 */
ccl_device_inline bool restir_area_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  *reference = restir_light_reference_empty();

  if (ls->type != LIGHT_AREA || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
  if ((LightType)klight->type != LIGHT_AREA) {
    return false;
  }

  reference->type = LIGHT_AREA;
  reference->object = ls->object;
  reference->prim = ls->prim;
// param stores the same barycentric-style emitter coordinates `float2(v + 0.5f, -u - v)`.
  reference->param = area_light_uv(klight, ls->P);
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
                     RESTIR_LIGHT_REFERENCE_HAS_PARAM; // !!!

  return true;
}

// ccl_device_inline bool restir_spot_light_reference_from_sample(
//     KernelGlobals kg,
//     const ccl_private LightSample *ls,
//     ccl_private ReSTIRLightSampleReference *reference)
// {
//   *reference = restir_light_reference_empty();

//   if (ls->type != LIGHT_SPOT || ls->prim < 0) {
//     return false;
//   }

//   const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
//   if ((LightType)klight->type != LIGHT_SPOT) {
//     return false;
//   }

//   reference->type = LIGHT_SPOT;
//   reference->object = ls->object;
//   reference->prim = ls->prim;

//   /*
//    * Store the direction toward the sampled spot light in the
//    * spot's canonical projected parameterization.
//    */
//   const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);

//   reference->param = spot_light_uv(
//       local_ray, klight->spot.half_cot_half_spot_angle);

//   reference->flags = RESTIR_LIGHT_REFERENCE_VALID |
//                      RESTIR_LIGHT_REFERENCE_HAS_PARAM;

//   return true;
// }

// TODO(Tri): Establish exactly what plane Cycles considers the finite-radius spot emitter to occupy.
// ccl_device_inline bool restir_spot_light_reference_from_sample(
//     KernelGlobals kg,
//     const ccl_private LightSample *ls,
//     ccl_private ReSTIRLightSampleReference *reference)
// {
//   *reference = restir_light_reference_empty();

//   if (ls->type != LIGHT_SPOT || ls->prim < 0) {
//     return false;
//   }

//   const ccl_global KernelLight *klight =
//       &kernel_data_fetch(lights, ls->prim);

//   if ((LightType)klight->type != LIGHT_SPOT) {
//     return false;
//   }

//   /* A zero-radius spot has no finite emitter position to persist. */
//   if (klight->spot.radius <= 0.0f) {
//     return false;
//   }

//   reference->type = LIGHT_SPOT;
//   reference->object = ls->object;
//   reference->prim = ls->prim;

//   /* Persist the sampled emitter position in the spot's local XY plane. */
//   reference->param = restir_spot_position_encode(kg, klight, ls->P);

//   // reference->flags = RESTIR_LIGHT_REFERENCE_VALID | // !!!: crashes on CPU !!!
//   //                    RESTIR_LIGHT_REFERENCE_HAS_PARAM;

//   return true;
// }

/* !!!: If ran against the current spot_light_sample(), the disk_light_sample(lightN, rand)
//  * produces points in the target-dependent plane. Their transformed local z will generally be non-zero.*/
// ccl_device_inline bool restir_spot_light_reference_from_sample(
//     KernelGlobals kg,
//     const ccl_private LightSample *ls,
//     ccl_private ReSTIRLightSampleReference *reference)
// {
//   *reference = restir_light_reference_empty();

// printf(">>> ENTERED RESTIR SPOT REFERENCE: ls->P = %f %f %f\n",
//        (double)ls->P.x,
//        (double)ls->P.y,
//        (double)ls->P.z);


//   if (ls->type != LIGHT_SPOT || ls->prim < 0) {
//     return false;
//   }

//   const ccl_global KernelLight *klight =
//       &kernel_data_fetch(lights, ls->prim);

//   if ((LightType)klight->type != LIGHT_SPOT) {
//     return false;
//   }

//   const float radius = klight->spot.radius;

//   // /* A zero-radius spot has no finite emitter position. */ // !!!: Wrong
//   // if (!(radius > 0.0f)) {
//   //   return false;
//   // }


// const Transform ref_itfm = lamp_get_inverse_transform(kg, klight);

// const float3 ref_local_P = transform_point(&ref_itfm, ls->P); // !!!: transforming an intersection-derived point, not the canonical sampled disk point. see spot_light_sample_from_intersection

// printf("\n=== SPOT REFERENCE INPUT ===\n");

// printf("ls->P world      = %f %f %f\n",
//        (double)ls->P.x,
//        (double)ls->P.y,
//        (double)ls->P.z);

// printf("ls->P local      = %f %f %f\n",
//        (double)ref_local_P.x,
//        (double)ref_local_P.y,
//        (double)ref_local_P.z);

// printf("local Z          = %g\n",
//        (double)ref_local_P.z);

// printf("radius           = %f\n",
//        (double)klight->spot.radius);

// printf("XY radius sq     = %f\n",
//        (double)(sqr(ref_local_P.x) +
//                 sqr(ref_local_P.y)));

// printf("================================\n");


//   /*
//    * The non-spherical spot uses a canonical disk in spot-local XY:
//    *
//    *     x² + y² <= radius²
//    *     z = 0
//    *
//    * Persist those coordinates normalized by radius.
//    */
//   const float2 param = restir_spot_position_encode(kg, klight, ls->P); // !!!

//   /*
//    * Validate that the sampled point really lies on the canonical disk.
//    *is_sphere
//    * This is particularly useful while debugging because the existing
//    * Cycles sampler currently uses a target-dependent disk.
//    */
//   const float param_len_sq = dot(param, param);

//   if (!isfinite(param.x) ||
//       !isfinite(param.y) ||
//       param_len_sq > 1.0f + 1e-5f)
//   {
//     return false;
//   }


// // printf("--- RESTIR SPOT REF CREATE: **prim**=%d object=%d type=%d ---\n",
// //        ls->prim, ls->object, ls->type);

//   reference->type = LIGHT_SPOT;
//   reference->object = ls->object;
//   reference->prim = ls->prim;
//   reference->param = param;

// // printf("REF CREATE: ref=%p prim=%d object=%d type=%d flags=%u param=(%f,%f)\n\n\n",
// //        (void *)reference,
// //        reference->prim,
// //        reference->object,
// //        reference->type,
// //        reference->flags,
// //        (double)reference->param.x,
// //        (double)reference->param.y);


//   // >>> DEBUG ONLY
// // #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
// const Transform itfm = lamp_get_inverse_transform(kg, klight);
// const float3 local_P = transform_point(&itfm, ls->P);

// printf("RESTIR SPOT REF:\n");
// printf("  world P = %f %f %f\n", (double)ls->P.x, (double)ls->P.y, (double)ls->P.z);
// printf("  local P = %f %f %f\n", (double)local_P.x, (double)local_P.y, (double)local_P.z);
// printf("  radius = %f\n", (double)radius);
// printf("  param = %f %f\n", (double)param.x, (double)param.y);
// printf("  norm xy : %f %f\n",
//        (double)local_P.x / (double)klight->spot.radius,
//        (double)local_P.y / (double)klight->spot.radius);

// printf("  canonical-plane error = %g\n", (double)fabsf(local_P.z));
// if (fabsf(local_P.z) > 1e-5f) {
//   printf("RESTIR SPOT: SAMPLE IS NOT ON CANONICAL SPOT PLANE!\n");
// }
// // #endif
//   // <<<

//   reference->flags = RESTIR_LIGHT_REFERENCE_VALID | // !!!: crashes on CPU !!!; fixed in `restir_light_reference_restore_metadata()`
//                      RESTIR_LIGHT_REFERENCE_HAS_PARAM;

//   return true;
// }

ccl_device_inline bool restir_spot_light_reference_from_sample(
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  *reference = restir_light_reference_empty();

  if (ls->type != LIGHT_SPOT || ls->prim < 0) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);

  if ((LightType)klight->type != LIGHT_SPOT) {
    return false;
  }

  if (klight->spot.radius < 0.0f) {
    printf("\nrestir_spot_light_reference_from_sample(); returned false; (klight->spot.radius < 0.0f)=%u\n",
           (klight->spot.radius < 0.0f));
    return false;
  }

  if (klight->spot.is_sphere) { // spherical Spot
  // if (klight->spot.radius > 0.0f) {
    /*
    * The sphere's persistent identity is its surface normal.
    *
    * ls->Ng is already the geometric normal of the sampled sphere
    * point in world space.
    */
    const float3 world_Ng = normalize(ls->Ng);

    if (!isfinite(world_Ng.x) ||
        !isfinite(world_Ng.y) ||
        !isfinite(world_Ng.z))
    {
      return false;
    }

    /*
    * Convert the sphere normal into the emitter's local space.
    *
    * This is the canonical parameterization of a spherical Spot.
    */
    const Transform itfm = lamp_get_inverse_transform(kg, klight);

    const float3 local_Ng = normalize(transform_direction(&itfm, world_Ng));

    if (!isfinite(local_Ng.x) ||
        !isfinite(local_Ng.y) ||
        !isfinite(local_Ng.z))
    {
      return false;
    }

    const float2 param = restir_direction_encode(local_Ng);

    if (!isfinite(param.x) || !isfinite(param.y)) {
      return false;
    }

    /*
    * Persistent light identity.
    */
    reference->type = LIGHT_SPOT;
    reference->object = ls->object;
    reference->prim = ls->prim;
    reference->param = param;

    reference->flags =
        RESTIR_LIGHT_REFERENCE_VALID |
        RESTIR_LIGHT_REFERENCE_HAS_PARAM;

  // #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
    printf("\n=== RESTIR SPHERICAL SPOT REFERENCE ===\n");
    printf("prim             = %d\n", reference->prim);
    printf("object           = %d\n", reference->object);
    printf("world P          = %f %f %f\n",
          (double)ls->P.x,
          (double)ls->P.y,
          (double)ls->P.z);
    printf("world Ng         = %f %f %f\n",
          (double)world_Ng.x,
          (double)world_Ng.y,
          (double)world_Ng.z);
    printf("local Ng         = %f %f %f\n",
          (double)local_Ng.x,
          (double)local_Ng.y,
          (double)local_Ng.z);
    printf("param            = %f %f\n",
          (double)param.x,
          (double)param.y);
    printf("radius           = %f\n",
          (double)klight->spot.radius);
    printf("========================================\n");
  // #endif

    return true;
  }
  else { // zero-radius point Spot: no 2D canonical disk parameterization
    // printf("\n=== RESTIR ZERO-RADIUS SPOT REFERENCE ENTERED!!! ===\n");

    /*
     * Non-spherical (zero-radius) Spot:
     *
     * The non-spherical Spot has zero radius, so its emitter
     * geometry degenerates to a single point at klight->co.
     *
     * There is therefore no 2D disk parameterization:
     *
     *     local P = (0, 0, 0)
     *
     * and no meaningful (x / r, y / r) coordinate because r == 0.
     *
     * The persistent identity is simply the Spot emitter itself,
     * with no additional geometric parameter.
     */

    reference->type = LIGHT_SPOT;
    reference->object = ls->object; // ???: Wrong here? // Returns 4 for spot
    // reference->object = OBJECT_NONE;
    reference->prim = ls->prim;
    reference->flags = RESTIR_LIGHT_REFERENCE_VALID;

    printf("\n=== RESTIR ZERO-RADIUS SPOT REFERENCE ===\n");

    printf("prim             = %d\n", reference->prim);
    printf("object           = %d\n", reference->object);
    printf("world P          = %f %f %f\n",
          (double)ls->P.x,
          (double)ls->P.y,
          (double)ls->P.z);
    printf("emitter co       = %f %f %f\n",
          (double)klight->co.x,
          (double)klight->co.y,
          (double)klight->co.z);
    printf("radius           = %f\n",
          (double)klight->spot.radius);
    printf("========================================\n");

    return true;
  }
}








// TODO: Handle all analytic lights
/* Dispatcher
 * Fallback Conversion API: Available for paths where only an LS exists, such as ordinary NEE samples.
 */
ccl_device_inline bool restir_light_reference_from_sample( // !!!
    KernelGlobals kg,
    const ccl_private LightSample *ls,
    ccl_private ReSTIRLightSampleReference *reference)
{
  switch (ls->type) {
    case LIGHT_POINT:
      return restir_point_light_reference_from_sample(kg, ls, reference);

    case LIGHT_SUN:
      return restir_sun_light_reference_from_sample(kg, ls, reference);

    case LIGHT_BACKGROUND:
      break;

    case LIGHT_AREA:
      return restir_area_light_reference_from_sample(kg, ls, reference);

    case LIGHT_SPOT: // !!!
// printf(">>> CALLING RESTIR SPOT REFERENCE: ls->P = %f %f %f, ls->prim=%f, ls->object=%f\n\n",
//        (double)ls->P.x, (double)ls->P.y, (double)ls->P.z,
//        (double)ls->prim, (double)ls->object);
      return restir_spot_light_reference_from_sample(kg, ls, reference);

    // !!!: Error!
    // const bool result = restir_spot_light_reference_from_sample(kg, ls, reference);
    // printf("AFTER SPOT REFERENCE: result=%u flags=%u type=%d prim=%d object=%d param=(%f,%f)\n",
    //       result,
    //       reference->flags,
    //       reference->type,
    //       reference->prim,
    //       reference->object,
    //       (double)reference->param.x,
    //       (double)reference->param.y);
    // !!!: AFTER SPOT REFERENCE: result=1 flags=1 type=4 prim=0 object=4 param=(0.000000,0.000000)
    // return result;

    // case LIGHT_TRIANGLE:
    //   return false; // TODO

    default:
      *reference = restir_light_reference_empty();
      return false;
  }
}

// !!!: Temporarily rejects geometric emitters !!!
/* validates, reconstructs metadata, performs kernel fetches. */
ccl_device_inline bool restir_light_reference_restore_metadata(
    KernelGlobals kg,
    ccl_private ReSTIRLightSampleReference *reference)
{
  // reference->flags = 0; // !!!: Wrong? the flags are part of the serialized reference state.

  if (reference->prim < 0) {
    printf("RESTIR RESTORE: INVALID prim=%d\n", reference->prim);
    *reference = restir_light_reference_empty();
    return false;
  }

  printf("restir_light_reference_restore_metadata(): fetching light from reference->prim=%f\n", (double)reference->prim);
  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim); // !!!: Sometime crashes!

  reference->type = (LightType)klight->type;
  reference->flags = RESTIR_LIGHT_REFERENCE_VALID;

  if (reference->type == LIGHT_SUN ||
      reference->type == LIGHT_AREA ||
      (reference->type == LIGHT_SPOT && klight->spot.is_sphere) ||
      (reference->type == LIGHT_POINT && klight->spot.is_sphere))
  {
    reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
  }

printf(
    "\n=== RESTORED REFERENCE ===\n"
    "type=%d prim=%d object=%d flags=%u param=(%f,%f)\n",
    reference->type,
    reference->prim,
    reference->object,
    reference->flags,
    (double)reference->param.x,
    (double)reference->param.y);

  // TODO:
  switch (reference->type) {
    case LIGHT_POINT:
    case LIGHT_SUN:
    case LIGHT_AREA:
      return true;
    case LIGHT_SPOT:
      // if (!klight->spot.is_sphere && klight->spot.radius > 0.0f) {
      //     reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
      // }
      return true;
    default:
      *reference = restir_light_reference_empty();
      return false;
  }

  // switch (reference->type) {
  //   case LIGHT_SUN:
  //   case LIGHT_AREA:
  //     reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
  //     break;

  //   case LIGHT_SPOT:
  //     if (klight->spot.is_sphere) {
  //       reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
  //     }
  //     break;

  //   case LIGHT_POINT:
  //     if (klight->spot.is_sphere) {
  //       reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
  //     }
  //     break;

  //   default:
  //     break;
  // }
}



// MARK: - Consumption

/*
To Check:
Confirm the forward transform helper name corresponding to lamp_get_inverse_transform().
Confirm the invalid emitter_id constant.
Confirm whether packed_float3 accepts direct assignment/conversion as shown.
Confirm whether zero-radius point lights are expected to use the area-to-solid-angle expression exactly as current sampling does.
shader_flags is currently unused by reconstruction because the selected sphere point is already fixed. Keep it only if the generic API will need it, or remove it from the point-specific helper.
path_flag is needed by sphere_light_pdf().
*/
ccl_device_inline bool restir_point_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags, // !!!: Unused
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  if (!restir_light_reference_is_valid(reference) ||
      reference->type != LIGHT_POINT || reference->prim < 0)
  {
    return false;
  }

  const ccl_global KernelLight *klight =
      &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_POINT) {
    return false;
  }

/** TODO: For relocated sphere-light reuse, verify that ls->P is the
 * first valid intersection from target P along ls->D. A persisted point on the
 * far side of the sphere may not be a valid sample from the new target.
 * Use point_light_intersect() with a scale-aware tolerance and reject on mismatch.
 */
  ls->type = LIGHT_POINT;
  ls->shader = klight->shader_id;
  ls->object = klight->object_id;
  ls->prim = reference->prim;
  ls->group = object_lightgroup(kg, ls->object);
  ls->eval_fac = klight->spot.eval_fac;

  /*
   * These are not available from the current reference contract. Callers must
   * restore them from emitter-selection state before using the sample for MIS.
   */
  ls->pdf_selection = 0.0f;
  ls->emitter_id = EMITTER_NONE; // !!!: Use the actual Cycles invalid constant.

  const float r_sq = sqr(klight->spot.radius);

  if (klight->spot.is_sphere) {
    if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
      return false;
    }

    const float3 local_Ng = restir_direction_decode(reference->param);
    // const Transform tfm = lamp_get_transform(kg, klight); // lamp_get_transform deprecated
    const Transform itfm = lamp_get_inverse_transform(kg, klight);
    const Transform tfm = transform_inverse(itfm);

    ls->Ng = normalize(transform_direction(&tfm, local_Ng));
    ls->P = klight->co + float3(ls->Ng) * klight->spot.radius;
    ls->D = safe_normalize_len(ls->P - P, &ls->t);

    const float d_sq = len_squared(P - klight->co);
    ls->pdf = sphere_light_pdf(d_sq, r_sq, N, ls->D, path_flag); // !!!: Deliberately passed N
    return isfinite(ls->pdf) && ls->pdf > 0.0f && ls->t >= 0.0f;
  }

  if (klight->spot.radius != 0.0f ||
      (reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM))
  {
    return false;
  }

  ls->P = klight->co;
  ls->D = safe_normalize_len(ls->P - P, &ls->t);
  ls->Ng = -ls->D;

  /*
   * Match point_light_sample() rather than labeling this as a generic
   * measure-one delta PDF.
   */
  const float3 lightN = safe_normalize(P - klight->co);
  // TODO: Check for zero-radius
  ls->pdf = light_pdf_area_to_solid_angle(lightN, -ls->D, ls->t);

  return isfinite(ls->pdf) && ls->pdf > 0.0f && ls->t >= 0.0f;
}

ccl_device_inline bool restir_sun_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  // Unused vars
  (void)P;
  (void)N;
  (void)shader_flags;
  (void)path_flag;

  if (!restir_light_reference_is_valid(reference) ||
      reference->type != LIGHT_SUN ||
      reference->prim < 0)
  {
    return false;
  }

  if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
    return false;
  }

  const ccl_global KernelLight *klight =
      &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_SUN) {
    return false;
  }

  const float3 local_Ng = restir_direction_decode(reference->param);

  const Transform itfm = lamp_get_inverse_transform(kg, klight);
  const Transform tfm = transform_inverse(itfm);

  const float3 Ng = normalize(transform_direction(&tfm, local_Ng));
  const float3 D = -Ng;

  // /*
  //  * The persisted direction should still lie inside the current sun cone.
  //  * This matters for relocated/reused samples and for animated or changed lights.
  //  */
  // if (klight->sun.angle == 0.0f) {
  //   return false;
  // }

  // if (vector_angle(-klight->co, D) > klight->sun.angle) {
  //   return false;
  // }

  // Allow zero-angle and reconstruct the delta sun direction:
  if (klight->sun.angle != 0.0f &&
      vector_angle(-klight->co, D) > klight->sun.angle)
  {
    return false;
  }


  ls->type = LIGHT_SUN;
  ls->shader = klight->shader_id;
  ls->object = klight->object_id;
  ls->prim = reference->prim;
  ls->group = object_lightgroup(kg, ls->object);

  ls->Ng = Ng;
  ls->P = Ng;
  ls->D = D;
  ls->t = FLT_MAX;

  ls->pdf = klight->sun.pdf; // !!!: og sample gets pdf from sample_uniform_cone, should match klight->sun.pdf
  ls->eval_fac = klight->sun.eval_fac;

  ls->pdf_selection = 0.0f;
  ls->emitter_id = EMITTER_NONE; /* Replace if Cycles has a more specific invalid value. */

  return isfinite(ls->pdf) && ls->pdf > 0.0f;
}

/**
 * reconstructs exactly the two pieces that matter:
 * 1. Chosen point on emitter geometry
 * 2. Current target-dependent evaluation
 *
 */
ccl_device_inline bool restir_area_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  (void)N;
  (void)shader_flags;
  (void)path_flag;

  if (!restir_light_reference_is_valid(reference) ||
      reference->type != LIGHT_AREA ||
      reference->prim < 0)
  {
    return false;
  }

  if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_AREA) {
    return false;
  }

  const float2 uv = reference->param;

  /* Invert area_light_uv():
   * uv.x = v + 0.5
   * uv.y = -u - v
   */
  const float v = uv.x - 0.5f;
  const float u = -uv.y - v;

  /**
  * Ellipse Validity: area_light_uv() clamps coordinates to [-0.5, 0.5], 
  * but for ellipse lights not every point in the rectangle is valid. 
  */
  if (area_light_is_ellipse(&klight->area)) {
    const float ellipse_u = (2.0f * u);
    const float ellipse_v = (2.0f * v);
    if (sqr(ellipse_u) + sqr(ellipse_v) > 1.0f + 1e-5f) {
      *ls = {};
      return false;
    }
  }


  /* Reconstruct emitter point from normalized in-plane coordinates. */
  // const float3 light_P = klight->co +
  //                        u * klight->area.len_u * klight->area.axis_u +
  //                        v * klight->area.len_v * klight->area.axis_v;
  const float3 light_P = restir_area_position_decode(klight, uv);

  ls->type = LIGHT_AREA;
  ls->shader = klight->shader_id;
  ls->object = klight->object_id;
  ls->prim = reference->prim;
  ls->group = object_lightgroup(kg, ls->object);

  ls->P = light_P;
  ls->Ng = klight->area.dir;
  ls->D = safe_normalize_len(ls->P - P, &ls->t);

  ls->pdf_selection = 0.0f;
  ls->emitter_id = EMITTER_NONE;

  /* Match area_light_sample(): one-sided surface light rejection. */
  if (dot(ls->P - P, ls->Ng) > 0.0f) {
    *ls = {};
    return false;
  }

// instead of hand-rebuilding pdf & eval_fac, use the existing eval (area_light_eval()) after reconstructing ls->P, ls->D, ls->t, and ls->Ng.
  float3 eval_light_P = ls->P;
  if (!area_light_eval<false>(klight, P, &eval_light_P, ls, zero_float2(), false)) {
    *ls = {};
    return false;
  }

  return isfinite(ls->pdf) && ls->pdf > 0.0f &&
         isfinite(ls->eval_fac) && ls->eval_fac >= 0.0f &&
         ls->t >= 0.0f;
}

// ccl_device_inline bool restir_spot_light_sample_from_reference(
//     KernelGlobals kg,
//     const ccl_private ReSTIRLightSampleReference *reference,
//     const float3 P,
//     const float3 N,
//     const int shader_flags,
//     const uint32_t path_flag,
//     ccl_private LightSample *ls)
// {
//   *ls = {};

//   (void)N;
//   (void)shader_flags;

//   if (!restir_light_reference_is_valid(reference) ||
//       reference->type != LIGHT_SPOT ||
//       reference->prim < 0)
//   {
//     return false;
//   }

//   if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
//     return false;
//   }

//   const ccl_global KernelLight *klight =
//       &kernel_data_fetch(lights, reference->prim);

//   if ((LightType)klight->type != LIGHT_SPOT) {
//     return false;
//   }

//   /*
//    * !!!: Invert spot_light_uv().
//    *
//    * spot_light_uv() gives:
//    *   u = ray.y * factor + 0.5
//    *   v = -(ray.x + ray.y) * factor
//    *
//    * where:
//    *   factor = half_cot_half_spot_angle / ray.z
//    *
//    * Since the parameterization is homogeneous in the ray direction,
//    * reconstruct a direction from (u, v).
//    */
//   const float u = reference->param.x;
//   const float v = reference->param.y;

//   const float x = -(u + v - 0.5f);
//   const float y = u - 0.5f;
//   const float z = klight->spot.half_cot_half_spot_angle;

//   const float3 local_ray = safe_normalize(make_float3(x, y, z));

//   /*
//    * spot_light_to_local() transforms a world-space direction into
//    * spot-local space and flips Z. Therefore invert that transform
//    * to recover the world-space direction toward the light.
//    */
//   const Transform itfm = lamp_get_inverse_transform(kg, klight);
//   const Transform tfm = transform_inverse(itfm);

//   float3 local_ray_unflipped = local_ray;
//   local_ray_unflipped.z = -local_ray_unflipped.z;

//   const float3 ray_to_light =
//       normalize(transform_direction(&tfm, local_ray_unflipped));

//   const float3 D = ray_to_light;

//   ls->type = LIGHT_SPOT;
//   ls->shader = klight->shader_id;
//   ls->object = klight->object_id;
//   ls->prim = reference->prim;
//   ls->group = object_lightgroup(kg, ls->object);

//   ls->D = D;

//   /*
//    * Reconstruct the emitter geometry / sample point.
//    *
//    * This differs between spherical and non-spherical spot lights.
//    */
//   const float r_sq = sqr(klight->spot.radius);

//   if (klight->spot.is_sphere) {
//     ls->t = FLT_MAX;

//     if (!ray_sphere_intersect(P,
//                               D,
//                               0.0f,
//                               FLT_MAX,
//                               klight->co,
//                               klight->spot.radius,
//                               &ls->P,
//                               &ls->t))
//     {
//       return false;
//     }

//     ls->Ng = normalize(ls->P - klight->co);
//   }
//   else {
//     /*
//      * A non-spherical spot is effectively a point / disk emitter.
//      * The reference identifies the direction, so reconstruct the
//      * emitter point from the disk parameterization.
//      *
//      * The current reference parameterization does not preserve the
//      * disk position itself, so for a finite radius disk this is not
//      * sufficient to uniquely reconstruct the original sample.
//      */
//     ls->P = klight->co;
//     // ls->t = safe_length(ls->P - P); // ???: safe_normalize_len?
//     ls->t = len(ls->P - P);
//     ls->Ng = -D;

//     if (r_sq > 0.0f) {
//       /*
//        * Cannot recover the original disk sample from spot UV alone.
//        * Treating it as the center would produce a different sample.
//        */
//     }
//   }

//   /*
//    * Recompute PDF and attenuation exactly as the native spot sampler
//    * does for the reconstructed sample.
//    */
//   const float d_sq = len_squared(P - klight->co);

//   if (klight->spot.is_sphere) {
//     ls->pdf = spot_light_pdf(
//         &klight->spot, d_sq, r_sq, N, D, path_flag);
//   }
//   else {
//     if (ls->t != FLT_MAX) {
//       const float3 lightN = normalize(ls->P - klight->co);
//       const float invarea = (r_sq > 0.0f) ? 1.0f / (r_sq * M_PI_F) : 1.0f;

//       ls->pdf = invarea * light_pdf_area_to_solid_angle(lightN, -D, ls->t);
//     }
//     else {
//       ls->pdf = 0.0f;
//     }
//   }

//   ls->eval_fac = klight->spot.eval_fac;

//   const float3 local_ray_for_attenuation = spot_light_to_local(kg, klight, -D);

//   if (!klight->spot.is_sphere || d_sq > r_sq) {
//     ls->eval_fac *= spot_light_attenuation(
//         &klight->spot, local_ray_for_attenuation);
//   }

//   ls->pdf_selection = 0.0f;
//   ls->emitter_id = EMITTER_NONE;

//   return isfinite(ls->pdf) && ls->pdf > 0.0f &&
//          isfinite(ls->eval_fac) && ls->eval_fac > 0.0f;
// }

// ccl_device_inline bool restir_spot_light_sample_from_reference(
//     KernelGlobals kg,
//     const ccl_private ReSTIRLightSampleReference *reference,
//     const float3 P,
//     const float3 N,
//     const int shader_flags,
//     const uint32_t path_flag,
//     ccl_private LightSample *ls)
// {
//   *ls = {};

//   if (!restir_light_reference_is_valid(reference) ||
//       reference->type != LIGHT_SPOT ||
//       reference->prim < 0)
//   {
//     return false;
//   }

//   if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
//     return false;
//   }

//   const ccl_global KernelLight *klight =
//       &kernel_data_fetch(lights, reference->prim);

//   if ((LightType)klight->type != LIGHT_SPOT) {
//     return false;
//   }

//   const float radius = klight->spot.radius;

//   if (radius <= 0.0f) {
//     return false;
//   }

//   /*
//    * Reconstruct the persistent emitter point.
//    */
//   const float3 light_P =
//       restir_spot_position_decode(kg, klight, reference->param);

//   /*
//    * Reject references outside the disk.
//    */
//   if (dot(reference->param, reference->param) > 1.0f + 1e-5f) {
//     return false;
//   }

//   /*
//    * Construct the target-dependent part of LightSample.
//    */
//   ls->type = LIGHT_SPOT;
//   ls->shader = klight->shader_id;
//   ls->object = klight->object_id;
//   ls->prim = reference->prim;
//   ls->group = object_lightgroup(kg, ls->object);

//   ls->P = light_P;
//   ls->D = safe_normalize_len(ls->P - P, &ls->t);

//   /*
//    * The disk is represented by the spot's local XY plane.
//    */
//   const Transform itfm = lamp_get_inverse_transform(kg, klight);
//   const float3 local_Ng =
//       make_float3(0.0f, 0.0f, -1.0f);

//   const Transform tfm = transform_inverse(itfm);
//   ls->Ng = normalize(transform_direction(&tfm, local_Ng));

//   /*
//    * Spot attenuation is evaluated from the reconstructed
//    * target-dependent direction.
//    */
//   const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);

//   ls->eval_fac = klight->spot.eval_fac;
//   ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);

//   if (ls->eval_fac == 0.0f) {
//     return false;
//   }

//   /* Reconstruct the same area-to-solid-angle PDF used by the
//    * non-spherical spot-light sampling path. */
//   const float r_sq = sqr(radius);
//   const float3 lightN = normalize(P - klight->co); // !!!: depends on the current shading point.

//   const float invarea = 1.0f / (r_sq * M_PI_F);

//   ls->pdf = invarea * light_pdf_area_to_solid_angle(lightN, -ls->D, ls->t);

//   ls->pdf_selection = 0.0f;
//   ls->emitter_id = EMITTER_NONE;

//   return isfinite(ls->pdf) &&
//          ls->pdf > 0.0f &&
//          isfinite(ls->eval_fac) &&
//          ls->eval_fac >= 0.0f &&
//          isfinite(ls->t) &&
//          ls->t >= 0.0f;
// }

// ccl_device_inline bool restir_spot_light_sample_from_reference(
//     KernelGlobals kg,
//     const ccl_private ReSTIRLightSampleReference *reference,
//     const float3 P,
//     const float3 N,
//     const int shader_flags,
//     const uint32_t path_flag,
//     ccl_private LightSample *ls)
// {
//   printf("=== restir_spot_light_sample_from_reference() ENTERED ===\n");

//   *ls = {};

//   (void)N;
//   (void)shader_flags;
//   (void)path_flag;

//   if (!restir_light_reference_is_valid(reference) ||
//       reference->type != LIGHT_SPOT ||
//       reference->prim < 0)
//   {
//     printf("restir_spot_light_sample_from_reference(); returned FALSE; !restir_light_reference_is_valid(refe)=%u\n",
//            !restir_light_reference_is_valid(reference));
//     return false;
//   }

//   if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) { // !!!: Always returning here: reference->flags=1
//     printf("restir_spot_light_sample_from_reference(); returned FALSE; reference->flags=%u\n"
//            "!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)=%u\n",
//            reference->flags, !(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM));
//     return false;
//   }

//   const ccl_global KernelLight *klight =
//       &kernel_data_fetch(lights, reference->prim);

//   if ((LightType)klight->type != LIGHT_SPOT) {
//     printf("restir_spot_light_sample_from_reference(); returned FALSE; (LightType)klight->type=%u\n",
//            (LightType)klight->type);
//     return false;
//   }

//   /*
//    * We only have a finite emitter position for the non-spherical
//    * finite-radius spot representation.
//    */
//   // if (klight->spot.is_sphere || !(klight->spot.radius > 0.0f)) // !!!: Always returning here!
//   // {
//   //   printf("= restir_spot_light_sample_from_reference() returned false!; spot.is_sphere! =\n");
//   //   return false;
//   // }
//   if (!(klight->spot.radius > 0.0f))
//   {
//     printf("=== restir_spot_light_sample_from_reference() returned false; klight->spot.radius=%f ===\n",
//            (double)klight->spot.radius);
//     return false;
//   }

//   const float2 param = reference->param;

//   /*
//    * Persistent emitter coordinates must lie inside the unit disk.
//    */
//   const float param_len_sq = dot(param, param);

//   if (!isfinite(param.x) ||
//       !isfinite(param.y) ||
//       param_len_sq > 1.0f + 1e-5f)
//   {
//   printf("= restir_spot_light_sample_from_reference() returned false; param_len_sq=%f\n", (double)param_len_sq);

//     return false;
//   }

//   /*
//    * Reconstruct the exact emitter point encoded by the reference.
//    */
//   ls->P = restir_spot_position_decode(kg, klight, param);

//   /*
//    * Target-dependent quantities.
//    */
//   ls->D = safe_normalize_len(ls->P - P, &ls->t);

//   /*
//    * The canonical spot emitter plane has local normal +Z or -Z
//    * depending on the convention used for the light's emitting side.
//    *
//    * spot_light_to_local() flips local Z so that the spot direction
//    * is represented as -Z. For the non-spherical spot, Cycles'
//    * existing sample uses Ng = -ls->D, i.e. the disk is treated
//    * as facing the shading point.
//    *
//    * Therefore do not invent a fixed Ng here unless we have decided
//    * to change that measure as well.
//    */
//   // !!!: inherited from the old model, where the disk itself was effectively being treated as a target-oriented construction.
//   // ls->Ng = -ls->D; // !!!: disk is effectively being used as an oriented sampling domain rather than a conventional fixed surface emitter.
//   // TODO: Change spot light sampling model spot_light_sample()? ls->Ng = fixed_disk_normal; light_pdf_area_to_solid_angle(ls->Ng, -ls->D, ls->t);

//   const Transform itfm = lamp_get_inverse_transform(kg, klight);

//   const float3 disk_N = normalize(transform_direction(
//       &itfm,
//       make_float3(0.0f, 0.0f, -1.0f)));

//   ls->Ng = disk_N;

// printf("--- RESTIR SPOT REF RECONSTRUCT sample_from_reference(): **prim**=%d object=%d type=%d flags=%u ---\n\n",
//        reference->prim, reference->object, reference->type, reference->flags);

//   ls->type = LIGHT_SPOT;
//   ls->shader = klight->shader_id;
//   ls->object = klight->object_id;
//   ls->prim = reference->prim;
//   ls->group = object_lightgroup(kg, ls->object);

//   ls->pdf_selection = 0.0f;
//   ls->emitter_id = EMITTER_NONE;

//   /*
//    * Spot attenuation depends on the target-dependent direction.
//    */
//   ls->eval_fac = klight->spot.eval_fac;

//   const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);

//   ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);

//   printf("= restir_spot_light_sample_from_reference() local_ray=(%f, %f, %f)\n", (double)local_ray.x, (double)local_ray.y, (double)local_ray.z);

//   if (ls->eval_fac == 0.0f) {
//     return false;
//   }

//   /*
//    * The sampling measure is uniform area over the canonical disk,
//    * followed by conversion from area measure to solid angle.
//    */
//   const float r_sq = sqr(klight->spot.radius);

//   printf("= restir_spot_light_sample_from_reference() r_sq=%f\n", (double)r_sq);

//   if (!(r_sq > 0.0f)) {
//     return false;
//   }

//   /*
//    * For the canonical disk, its geometric normal is fixed.
//    */
//   // const Transform itfm = lamp_get_inverse_transform(kg, klight);

//   // const float3 disk_N = normalize(transform_direction(
//   //     &itfm,
//   //     make_float3(0.0f, 0.0f, -1.0f)));

//   /*
//    * light_pdf_area_to_solid_angle() expects:
//    *
//    *   Ng = emitter normal
//    *   I  = direction from emitter toward shading point
//    */
//   const float invarea = 1.0f / (r_sq * M_PI_F);

//   printf("= restir_spot_light_sample_from_reference() invarea=%f\n", (double)invarea);

//   ls->pdf =
//       invarea *
//       light_pdf_area_to_solid_angle(disk_N, -ls->D, ls->t);

//   return isfinite(ls->pdf) &&
//          ls->pdf > 0.0f &&
//          isfinite(ls->eval_fac) &&
//          ls->eval_fac >= 0.0f &&
//          isfinite(ls->t) &&
//          ls->t >= 0.0f;
// }

ccl_device_inline bool restir_spot_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
  *ls = {};

  (void)N;
  (void)shader_flags;

  if (!restir_light_reference_is_valid(reference) ||
      reference->type != LIGHT_SPOT || reference->prim < 0)
  {
    printf("\nrestir_spot_light_sample_from_reference(); returned false; !restir_light_reference_is_valid(reference)=%u\n",
           !restir_light_reference_is_valid(reference));
    return false;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);

  if ((LightType)klight->type != LIGHT_SPOT) {
    return false;
  }

  printf("\n=== restir_spot_light_sample_from_reference() ENTERED!!! is_sphere=%u ===\n",
         klight->spot.is_sphere);

  if (klight->spot.radius < 0.0f) {
    printf("\nrestir_spot_light_sample_from_reference(); returned false; (klight->spot.radius < 0.0f)=%u\n",
           (klight->spot.radius < 0.0f));
    return false;
  }

  /* Zero-radius Spot (degenerate point) reference intentionally doesn't have a parameter.*/
  // if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
  //   printf("\nrestir_spot_light_sample_from_reference(); returned false; !(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)=%u\n",
  //          !(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM));
  //   return false;
  // }

  if (klight->spot.is_sphere) {
  // if (klight->spot.radius > 0.0f) {
  if (!(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)) {
    printf("\nrestir_spot_light_sample_from_reference(); returned false; !(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM)=%u\n",
           !(reference->flags & RESTIR_LIGHT_REFERENCE_HAS_PARAM));
    return false;
  }

    /* Decode the canonical sphere-surface normal in emitter-local space. */
    const float3 local_Ng =
        restir_direction_decode(reference->param);

    if (!isfinite(local_Ng.x) ||
        !isfinite(local_Ng.y) ||
        !isfinite(local_Ng.z))
    {
      return false;
    }

    /* Transform the canonical normal into world space. */
    const Transform itfm = lamp_get_inverse_transform(kg, klight);

    const Transform tfm = transform_inverse(itfm);

    const float3 world_Ng = normalize(transform_direction(&tfm, local_Ng));

    if (!isfinite(world_Ng.x) ||
        !isfinite(world_Ng.y) ||
        !isfinite(world_Ng.z))
    {
      return false;
    }

    /*
    * Reconstruct the exact point on the spherical emitter.
    */
    ls->P = klight->co + world_Ng * klight->spot.radius;

    ls->Ng = world_Ng;

    /*
    * Target-dependent direction and distance.
    */
    ls->D = safe_normalize_len(ls->P - P, &ls->t);

    if (!isfinite(ls->t) || ls->t < 0.0f) {
      return false;
    }

    /*
    * Restore the ordinary LightSample metadata.
    */
    ls->type = LIGHT_SPOT;
    ls->shader = klight->shader_id;
    ls->object = klight->object_id;
    ls->prim = reference->prim;
    ls->group = object_lightgroup(kg, ls->object);

    ls->pdf_selection = 0.0f;
    ls->emitter_id = EMITTER_NONE;

    /*
    * Spot attenuation is target-dependent and therefore must be
    * recomputed after reconstruction.
    */
    ls->eval_fac = klight->spot.eval_fac;

    const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);

    ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);

    if (!isfinite(ls->eval_fac)) {
      return false;
    }

    if (ls->eval_fac == 0.0f) {
      return false;
    }

    /*
    * Recompute the spherical-light sampling PDF for the new target.
    *
    * This is target-dependent and therefore must NOT be stored in
    * the persistent reference.
    */
    const float r_sq = sqr(klight->spot.radius);

    const float d_sq = len_squared(P - klight->co);

    ls->pdf = sphere_light_pdf(d_sq, r_sq, N, ls->D, path_flag);

  #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
    printf("\n=== RESTIR SPHERICAL SPOT RECONSTRUCTION ===\n");
    printf("prim             = %d\n", reference->prim);
    printf("param            = %f %f\n",
          (double)reference->param.x,
          (double)reference->param.y);
    printf("local Ng         = %f %f %f\n",
          (double)local_Ng.x,
          (double)local_Ng.y,
          (double)local_Ng.z);
    printf("world Ng         = %f %f %f\n",
          (double)world_Ng.x,
          (double)world_Ng.y,
          (double)world_Ng.z);
    printf("reconstructed P  = %f %f %f\n",
          (double)ls->P.x,
          (double)ls->P.y,
          (double)ls->P.z);
    printf("target P         = %f %f %f\n",
          (double)P.x,
          (double)P.y,
          (double)P.z);
    printf("D                = %f %f %f\n",
          (double)ls->D.x,
          (double)ls->D.y,
          (double)ls->D.z);
    printf("t                = %f\n",
          (double)ls->t);
    printf("eval_fac         = %f\n",
          (double)ls->eval_fac);
    printf("pdf              = %f\n",
          (double)ls->pdf);
    printf("============================================\n");
  #endif

    return isfinite(ls->pdf) &&
          ls->pdf > 0.0f &&
          isfinite(ls->eval_fac) &&
          ls->eval_fac >= 0.0f;
  }
  else {
    printf("\n=== RESTIR ZERO-RADIUS SPOT RECONSTRUCTION ENTERED!!! ===\n");

    /*
     * Non-spherical (zero-radius) Spot:
     *
     * The emitter degenerates to a single point at klight->co.
     *
     * There is no persistent geometric parameter to decode.
     *
     * Therefore:
     *
     *     ls->P = klight->co
     *
     * and all remaining quantities are reconstructed relative
     * to the current target P.
     */

    ls->P = klight->co;

    /* Target-dependent direction and distance. */
    ls->D = safe_normalize_len(ls->P - P, &ls->t);

    if (!isfinite(ls->t) || ls->t < 0.0f) {
      return false;
    }

    ls->Ng = -ls->D; // based on sampler

    /* Restore the metadata. */
    ls->type = LIGHT_SPOT;
    ls->shader = klight->shader_id;
    ls->object = klight->object_id;
    ls->prim = reference->prim;
    ls->group = object_lightgroup(kg, ls->object);

    ls->pdf_selection = 0.0f;
    ls->emitter_id = EMITTER_NONE;

    /*
     * Attenuation is evaluated against the new target (target-dependent).
     * Exactly analogous to the sphere reconstruction.
     */
    ls->eval_fac = klight->spot.eval_fac;

    const float3 local_ray = spot_light_to_local(kg, klight, -ls->D);

    ls->eval_fac *= spot_light_attenuation(&klight->spot, local_ray);

    if (!isfinite(ls->eval_fac)) {
      return false;
    }

    if (ls->eval_fac == 0.0f) {
      return false;
    }

    /*
     * Match the zero-radius non-spherical Spot sampling PDF.
     *
     * spot_light_sample() uses:
     *
     *     invarea = 1
     *
     * when radius == 0.
     */
    const float3 lightN = safe_normalize(P - klight->co);

    ls->pdf = light_pdf_area_to_solid_angle(lightN, -ls->D, ls->t);

    printf("\n=== RESTIR ZERO-RADIUS SPOT RECONSTRUCTION ===\n");

    printf("prim             = %d\n", reference->prim);

    printf("world P          = %f %f %f\n",
          (double)ls->P.x,
          (double)ls->P.y,
          (double)ls->P.z);

    printf("target P         = %f %f %f\n",
          (double)P.x,
          (double)P.y,
          (double)P.z);

    printf("D                = %f %f %f\n",
          (double)ls->D.x,
          (double)ls->D.y,
          (double)ls->D.z);

    printf("Ng               = %f %f %f\n",
          (double)ls->Ng.x,
          (double)ls->Ng.y,
          (double)ls->Ng.z);

    printf("t                = %f\n",
          (double)ls->t);

    printf("eval_fac         = %f\n",
          (double)ls->eval_fac);

    printf("pdf              = %f\n",
          (double)ls->pdf);

    printf("============================================\n\n\n\n");

    return isfinite(ls->pdf) &&
          ls->pdf > 0.0f &&
          isfinite(ls->eval_fac) &&
          ls->eval_fac >= 0.0f;
  }
}




// Dispatcher; restir_light_sample_reconstruct()
// Basically a replacement for `light_sample_from_uv()`
ccl_device_inline bool restir_light_sample_from_reference(
    KernelGlobals kg,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag,
    ccl_private LightSample *ls)
{
printf("RECONSTRUCT DISPATCH: flags=%u type=%d prim=%d object=%d param=(%f,%f)\n",
       reference->flags,
       reference->type,
       reference->prim,
       reference->object,
       (double)reference->param.x,
       (double)reference->param.y);

  if (!restir_light_reference_is_valid(reference)) {
    *ls = {};
    return false;
  }

  // TODO(Tri): Rest of the light types
  switch (reference->type) {
    case LIGHT_POINT:
      return restir_point_light_sample_from_reference(
          kg, reference, P, N, shader_flags, path_flag, ls);

    case LIGHT_SUN:
      return restir_sun_light_sample_from_reference(
          kg, reference, P, N, shader_flags, path_flag, ls);

    case LIGHT_BACKGROUND:
      break;

    case LIGHT_AREA:
      return restir_area_light_sample_from_reference(
          kg, reference, P, N, shader_flags, path_flag, ls);

    case LIGHT_SPOT:
      printf("=== restir_light_sample_from_reference(); restir_spot_light_sample_from_reference() called ===\n\n");
      return restir_spot_light_sample_from_reference(
          kg, reference, P, N, shader_flags, path_flag,ls);

    case LIGHT_TRIANGLE:
      return false; // TODO

    default:
      *ls = {};
      return false;
  }
}


// MARK: --- Debug ---

ccl_device_inline bool restir_debug_compare_float3(const float3 a,
                                                   const float3 b,
                                                   const float eps)
{
  return len(a - b) <= eps;
}

ccl_device_inline bool restir_debug_compare_float(const float a,
                                                  const float b,
                                                  const float rel_eps)
{
  if (a == FLT_MAX || b == FLT_MAX) {
    return a == b;
  }

  const float scale = fmaxf(fmaxf(fabsf(a), fabsf(b)), 1.0f);
  return fabsf(a - b) <= rel_eps * scale;
}


ccl_device_inline void restir_debug_validate_light_reference_roundtrip_from_reference(
    KernelGlobals kg,
    const ccl_private LightSample *original,
    const ccl_private ReSTIRLightSampleReference *reference,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag)
{
  LightSample reconstructed;

  if (!restir_light_reference_is_valid(reference)) {
    return;
  }

  const bool decoded = restir_light_sample_from_reference(
      kg, reference, P, N, shader_flags, path_flag, &reconstructed);

  if (!decoded) {
#if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
    printf("ReSTIR roundtrip decode failed: type=%d prim=%d object=%d\n",
           (int)original->type, original->prim, original->object);
#endif
    return;
  }

  const float pos_eps = 1e-4f;
  const float dir_eps = 1e-4f;
  const float rel_eps = 1e-4f;

  bool ok = true;
  ok &= (original->type == reconstructed.type);
  ok &= (original->prim == reconstructed.prim);
  ok &= (original->object == reconstructed.object);
  ok &= (original->shader == reconstructed.shader);
  ok &= (original->group == reconstructed.group);

  ok &= restir_debug_compare_float3(original->P, reconstructed.P, pos_eps);
  ok &= restir_debug_compare_float3(original->Ng, reconstructed.Ng, dir_eps);
  ok &= restir_debug_compare_float3(original->D, reconstructed.D, dir_eps);
  ok &= restir_debug_compare_float(original->t, reconstructed.t, rel_eps);
  ok &= restir_debug_compare_float(original->pdf, reconstructed.pdf, rel_eps);
  ok &= restir_debug_compare_float(original->eval_fac, reconstructed.eval_fac, rel_eps);

  if (!ok) {
#if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
    printf("ReSTIR roundtrip mismatch\n");

//     printf("  original: type=%d prim=%d object=%d pdf=%f t=%f\n",
//            (int)original->type, original->prim, original->object,
//            (double)original->pdf, (double)original->t);
//     printf("  recon   : type=%d prim=%d object=%d pdf=%f t=%f\n",
//            (int)reconstructed.type, reconstructed.prim, reconstructed.object,
//            (double)reconstructed.pdf, (double)reconstructed.t);
// #else
//     assert(false);
#endif
  }
}

ccl_device_inline void restir_debug_validate_light_reference_roundtrip(
    KernelGlobals kg,
    const ccl_private LightSample *original,
    const float3 P,
    const float3 N,
    const int shader_flags,
    const uint32_t path_flag)
{
  ReSTIRLightSampleReference reference = restir_light_reference_empty();

  const bool encoded = restir_light_reference_from_sample(kg, original, &reference);
  if (!encoded) {
    return;
  }

  restir_debug_validate_light_reference_roundtrip_from_reference(
      kg, original, &reference, P, N, shader_flags, path_flag);
}

CCL_NAMESPACE_END