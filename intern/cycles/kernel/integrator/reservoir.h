// reservoir.h

#pragma once

#include "kernel/types.h"

#include "kernel/light/sample_reference.h"

CCL_NAMESPACE_BEGIN

struct Reservoir {
  LightSample light;
  ReSTIRLightSampleReference reference;

  BsdfEval radiance;
  float luminance;
  float total_weight = 0.0f;

  int num_light_samples;
  int num_bsdf_samples = 1;
  int max_samples;
  float3 rgb_to_y;

  int direct_sampling_method;
};

ccl_device_inline void reservoir_clear(ccl_private Reservoir *reservoir)
{
  reservoir->light = {};
  reservoir->reference = restir_light_reference_empty();
  reservoir->radiance = {};
  reservoir->luminance = 0.0f;
  reservoir->total_weight = 0.0f;
}

ccl_device_inline void reservoir_init(KernelGlobals kg,
                                      ccl_private Reservoir *reservoir)
{
  reservoir_clear(reservoir);

  reservoir->num_light_samples = 0;
  reservoir->num_bsdf_samples = 1;
  reservoir->max_samples = 0;
  reservoir->rgb_to_y = float4_to_float3(kernel_data.film.rgb_to_y); // make_float3()
  reservoir->direct_sampling_method = kernel_data.integrator.direct_light_sampling_type;
}

ccl_device_inline void reservoir_init(KernelGlobals kg,
                                      ccl_private Reservoir *reservoir,
                                      const bool use_ris)
{
  reservoir_clear(reservoir);

  reservoir->num_light_samples = use_ris ? kernel_data.integrator.restir_light_samples : 1;
  reservoir->num_bsdf_samples = 1;
  reservoir->max_samples = reservoir->num_light_samples + reservoir->num_bsdf_samples;
  reservoir->rgb_to_y = float4_to_float3(kernel_data.film.rgb_to_y); // make_float3()
  reservoir->direct_sampling_method = kernel_data.integrator.direct_light_sampling_type;
}

ccl_device_inline bool reservoir_is_empty(ccl_private const Reservoir *reservoir)
{
  return !(reservoir->total_weight > 0.0f);
}

ccl_device_inline bool reservoir_has_valid_reference(ccl_private const Reservoir *reservoir)
{
  return restir_light_reference_is_valid(&reservoir->reference);
}

ccl_device_inline float reservoir_compute_luminance(
    ccl_private const Reservoir *reservoir,
    const ccl_private BsdfEval &sample_radiance)
{
  return dot(spectrum_to_rgb(sample_radiance.sum), reservoir->rgb_to_y);
}

ccl_device_inline bool reservoir_finalize(ccl_private Reservoir *reservoir,
                                          const bool require_valid_reference)
{
  if (reservoir_is_empty(reservoir) ||
      (require_valid_reference && !reservoir_has_valid_reference(reservoir)) ||
      !(isfinite(reservoir->luminance) && reservoir->luminance > 0.0f))
  {
    reservoir_clear(reservoir);
    return false;
  }

// #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
  // printf(
  // "reservoir_finalize():\n"
  // "num_bsdf_samples=%u\n"
  // "num_light_samples=%u\n"
  // "luminance=%g\n"
  // "total_weight(before)=%g\n",
  // reservoir->num_bsdf_samples,
  // reservoir->num_light_samples,
  // (double)reservoir->luminance,
  // (double)reservoir->total_weight);


// printf(
// "\n================ RESERVOIR FINALIZE ================\n"
// "before normalization\n"
// "total_weight = %g\n"
// "luminance    = %g\n"
// "num_light    = %d\n"
// "num_bsdf     = %d\n"
// "reference.valid = %d\n"
// "reference.type = %d\n"
// "reference.object = %d\n"
// "reference.prim = %d\n"
// "reference.param = (%f,%f)\n"
// "selected light:\n"
// "  type=%d object=%d prim=%d shader=%d\n"
// "selected radiance=(%f,%f,%f)\n",
// (double)reservoir->total_weight,
// (double)reservoir->luminance,
// reservoir->num_light_samples,
// reservoir->num_bsdf_samples,
// (int)reservoir_has_valid_reference(reservoir),
// (int)reservoir->reference.type,
// reservoir->reference.object,
// reservoir->reference.prim,
// (double)reservoir->reference.param.x,
// (double)reservoir->reference.param.y,
// (int)reservoir->light.type,
// reservoir->light.object,
// reservoir->light.prim,
// reservoir->light.shader,
// (double)reservoir->radiance.sum.x,
// (double)reservoir->radiance.sum.y,
// (double)reservoir->radiance.sum.z);
// #endif

  /* Apply unbiased contribution weight. */
  reservoir->total_weight /= reservoir->luminance; // !!!

#if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
  // printf("reservoir_finalize(): \ntotal_weight(after)=%f\n", (double)reservoir->total_weight);

  // printf(
  // "after normalization\n"
  // "reservoir weight=%g\n",
  // (double)reservoir->total_weight);
#endif

  if (!(isfinite(reservoir->total_weight) && reservoir->total_weight > 0.0f)) {
    reservoir_clear(reservoir);
    printf("reservoir_finalize(): checks failed! returned false!!! !!!");
    return false;
  }

#if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
  // printf(
  // "after checks...\n"
  // "reservoir weight=%g\n"
  // "====================================================\n\n",
  // (double)reservoir->total_weight);
#endif

  return true;
}

ccl_device_inline float reservoir_power_heuristic(const int num_a,
                                                  const float pdf_a,
                                                  const int num_b,
                                                  const float pdf_b)
{
  const float weighted_a = float(num_a) * sqr(pdf_a);
  const float weighted_b = float(num_b) * sqr(pdf_b);
  const float denominator = weighted_a + weighted_b;

  return (denominator > 0.0f) ? sqr(pdf_a) / denominator : 0.0f;
}

ccl_device_inline float reservoir_mis_weight_nee(ccl_private const Reservoir *reservoir,
                                                 float light_pdf,
                                                 float bsdf_pdf)
{
#ifdef WITH_CYCLES_DEBUG
  if (reservoir->direct_sampling_method == DIRECT_LIGHT_SAMPLING_FORWARD) {
    light_pdf *= (bsdf_pdf == 0.0f);
  }
  else if (reservoir->direct_sampling_method == DIRECT_LIGHT_SAMPLING_NEE) {
    bsdf_pdf = 0.0f;
  }
#endif

  return reservoir_power_heuristic(
      reservoir->num_light_samples, light_pdf, reservoir->num_bsdf_samples, bsdf_pdf);
}

ccl_device_inline float reservoir_mis_weight_forward(ccl_private const Reservoir *reservoir,
                                                     float bsdf_pdf,
                                                     float light_pdf)
{
#ifdef WITH_CYCLES_DEBUG
  if (reservoir->direct_sampling_method == DIRECT_LIGHT_SAMPLING_FORWARD) {
    light_pdf = 0.0f;
  }
  else if (reservoir->direct_sampling_method == DIRECT_LIGHT_SAMPLING_NEE) {
    bsdf_pdf *= (light_pdf == 0.0f);
  }
#endif

  return reservoir_power_heuristic(
      reservoir->num_bsdf_samples, bsdf_pdf, reservoir->num_light_samples, light_pdf);
}

ccl_device_inline void reservoir_add_sample(
    ccl_private Reservoir *reservoir,
    const ccl_private LightSample &ls,
    const ccl_private ReSTIRLightSampleReference &candidate_reference,
    const ccl_private BsdfEval &candidate_radiance,
    const float mis_weight,
    const float rand,
    const bool require_valid_reference)
{
  const float candidate_luminance = reservoir_compute_luminance(reservoir, candidate_radiance);
  const float weight = candidate_luminance * mis_weight;

  if ((require_valid_reference && !restir_light_reference_is_valid(&candidate_reference)) ||
      !(isfinite(candidate_luminance) && candidate_luminance > 0.0f) ||
      !(isfinite(weight) && weight > 0.0f))
  {
    return;
  }
  reservoir->total_weight += weight;

  if (!(isfinite(reservoir->total_weight) && reservoir->total_weight > 0.0f)) {
    reservoir_clear(reservoir);
    return;
  }

  // Reservoir replacement
  if (rand * reservoir->total_weight <= weight) {
    reservoir->light = ls;
    reservoir->reference = candidate_reference;
    reservoir->radiance = candidate_radiance;
    reservoir->luminance = candidate_luminance;
  }
}

ccl_device_inline void reservoir_add_light_sample(
    ccl_private Reservoir *reservoir,
    const ccl_private LightSample &ls,
    const ccl_private ReSTIRLightSampleReference &candidate_reference,
    const ccl_private BsdfEval &candidate_radiance,
    const float bsdf_pdf,
    const float rand,
    const bool require_valid_reference)
{
  if (!(isfinite(ls.pdf) && ls.pdf > 0.0f)) {
    return;
  }

  const float mis_weight = reservoir_mis_weight_nee(reservoir, ls.pdf, bsdf_pdf);

  float light_pdf_in_area = ls.pdf;

  // TODO(weizhen): Convert pdf to area measure when returning the pdf instead of here.
  const float jacobian = ls.jacobian_solid_angle_to_area();

  if (jacobian > 0.0f) {
    light_pdf_in_area *= jacobian;
  }

  if (!(isfinite(light_pdf_in_area) && light_pdf_in_area > 0.0f)) {
    return;
  }

  reservoir_add_sample(reservoir,
                       ls,
                       candidate_reference,
                       candidate_radiance,
                       mis_weight / light_pdf_in_area,
                       rand,
                       require_valid_reference);

// #if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
//   printf("reservoir_add_sample():\n" // New Debug
//   "candidate_reference param=(%f, %f)\n"
//   "candidate_radiance sum= (%f, %f, %f)\n"
//   "mis_weight=%f\n"
//   "light_pdf_in_area=%f\n"
//   "reservoir->total_weight=%g\n",
//   (double)candidate_reference.param.x, (double)candidate_reference.param.y,
//   (double)candidate_radiance.sum.x, (double)candidate_radiance.sum.y, (double)candidate_radiance.sum.z,
//   (double)mis_weight,
//   (double)light_pdf_in_area,
//   (double)reservoir->total_weight);
// #endif
}

ccl_device_inline void reservoir_add_bsdf_sample(
    ccl_private Reservoir *reservoir,
    const ccl_private LightSample &ls,
    const ccl_private ReSTIRLightSampleReference &candidate_reference,
    const ccl_private BsdfEval &candidate_radiance,
    const float bsdf_pdf,
    const float rand,
    const bool require_valid_reference)
{
  if (!(isfinite(bsdf_pdf) && bsdf_pdf > 0.0f)) {
    return;
  }

  const float mis_weight = reservoir_mis_weight_forward(reservoir, bsdf_pdf, ls.pdf);

  float bsdf_pdf_in_area = bsdf_pdf;

  // TODO(weizhen): Convert pdf to area measure when returning the pdf instead of here.
  const float jacobian = ls.jacobian_solid_angle_to_area();

  if (jacobian > 0.0f) {
    bsdf_pdf_in_area *= jacobian;
  }

  if (!(isfinite(bsdf_pdf_in_area) && bsdf_pdf_in_area > 0.0f)) {
    return;
  }

  reservoir_add_sample(reservoir,
                       ls,
                       candidate_reference,
                       candidate_radiance,
                       mis_weight / bsdf_pdf_in_area,
                       rand,
                       require_valid_reference);
}

/**
* The selected light sample and radiance must be reconstructed and
* reevaluated for the current target before combining reservoirs. 
*/
ccl_device_inline void reservoir_add_reservoir(ccl_private Reservoir *reservoir,
                                               const ccl_private Reservoir &other,
                                               const float rand,
                                               const bool require_valid_reference)
{
  // !!!: DI only
  const bool a = reservoir_is_empty(&other);
  const bool b = !reservoir_has_valid_reference(&other);
  // if (reservoir_is_empty(&other) || !reservoir_has_valid_reference(&other)) {
  if (a || b) {
#if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
    printf("!!! !!! reservoir_add_reservoir() early returned! !!! !!!\n"
      "reservoir_is_empty():%u, !reservoir_has_valid_reference(): %d\n", 
      a, b);
#endif
    return;
  }

#if !defined(__KERNEL_GPU__) && defined(RESTIR_DI_DEBUG)
printf(
"\n----- reservoir_add_reservoir -----\n"
"other.total_weight=%g\n"
"other.radiance=(%f,%f,%f)\n"
"other.luminance=%g\n"
"reference.valid=%d\n"
"reference.type=%d\n"
"reference.object=%d\n"
"reference.prim=%d\n",
(double)other.total_weight,
(double)other.radiance.sum.x,
(double)other.radiance.sum.y,
(double)other.radiance.sum.z,
(double)other.luminance,
(int)reservoir_has_valid_reference(&other),
(int)other.reference.type,
other.reference.object,
other.reference.prim);
#endif
/*If this already shows `radiance=(0,0,0)`
then we know the corruption happened during film packing/unpacking.*/

  reservoir_add_sample(
      reservoir, other.light, other.reference, other.radiance, other.total_weight, rand, require_valid_reference);
}

// TODO: Cleaner to inspect reservoir.reference?
// TODO(weizhen): Maybe it should be determined when the sample is drawn, not afterwards
// ccl_device_inline bool sample_copy_direction(KernelGlobals kg,
//                                              ccl_private const Reservoir &reservoir)
// {
//   if (reservoir.is_empty()) {
//     return false;
//   }
//
//   if (reservoir.light.type == LIGHT_AREA) {
//     const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reservoir.light.prim); // light.lamp
//     if (klight->area.tan_half_spread == 0.0f) {
//       return true;
//     }
//   }
//
//   if (reservoir.light.type == LIGHT_POINT) {
//     const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reservoir.light.prim); // light.lamp
//     if (!klight->spot.is_sphere) {
//       return true;
//     }
//   }
//
//   return false;
// }
// !!!: Better against res.reference anyway, cause tis the persistent identity & survives film unpacking before light's been rebuilt.
ccl_device_inline bool sample_copy_direction(KernelGlobals kg, // !!!: lamp
                                             ccl_private const Reservoir &reservoir)
{
  if (reservoir_is_empty(&reservoir) || !reservoir_has_valid_reference(&reservoir)) {
    return false;
  }

  if (reservoir.reference.object != OBJECT_NONE) {
    return false;
  }

  if (reservoir.reference.type == LIGHT_AREA) {
    const ccl_global KernelLight *klight = &kernel_data_fetch(
        lights, reservoir.reference.prim);
    if (klight->area.tan_half_spread == 0.0f) {
      return true;
    }
  }

  if (reservoir.reference.type == LIGHT_POINT) {
    const ccl_global KernelLight *klight = &kernel_data_fetch(
        lights, reservoir.reference.prim);
    if (!klight->spot.is_sphere) {
      return true;
    }
  }

  return false;
}



// MARK: - SpatialReservoir

struct SpatialReservoir {
  Reservoir reservoir;
  // TODO(weizhen): maybe the neighbors should share the same flag.
  uint32_t path_flag;
  ShaderData sd;
  uint3 pixel_index;
};

ccl_device_inline void spatial_reservoir_init(KernelGlobals kg,
                                              ccl_private SpatialReservoir *spatial_reservoir,
                                              const bool require_valid_reference)
{
  reservoir_init(kg, &spatial_reservoir->reservoir);
  spatial_reservoir->path_flag = 0;
  spatial_reservoir->sd = {};
  spatial_reservoir->pixel_index = make_uint3(-1, -1, -1);
}

ccl_device_inline bool spatial_reservoir_is_empty(
    ccl_private const SpatialReservoir *spatial_reservoir)
{
  return reservoir_is_empty(&spatial_reservoir->reservoir);
}

ccl_device_inline void spatial_reservoir_add_reservoir(
    ccl_private SpatialReservoir *spatial_reservoir,
    const ccl_private Reservoir &other,
    const float rand,
    const bool require_valid_reference)
{
  reservoir_add_reservoir(&spatial_reservoir->reservoir, other, rand, require_valid_reference);
}

// TODO: Remove
ccl_device_inline void spatial_reservoir_add_reservoir(
    ccl_private SpatialReservoir *spatial_reservoir,
    const ccl_private SpatialReservoir &other,
    const float rand,
    const bool require_valid_reference)
{
  spatial_reservoir_add_reservoir(spatial_reservoir, other.reservoir, rand, require_valid_reference);
}

// TODO: Remove?
// ccl_device_inline bool restir_unpack_light_reference(
//     KernelGlobals kg,
//     ccl_global const float *ptr,
//     ccl_private ReSTIRLightSampleReference *reference)
// {
//   *reference = restir_light_reference_empty();

//   reference->object = (int)ptr[0];
//   reference->prim = (int)ptr[1];
//   reference->param = make_float2(ptr[2], ptr[3]);

//   if (reference->prim < 0) {
//     return false;
//   }

//   const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reference->prim);

//   reference->type = (LightType)klight->type;
//   reference->flags = RESTIR_LIGHT_REFERENCE_VALID;

//   switch (reference->type) {
//     case LIGHT_POINT:
//       if (klight->spot.is_sphere) { // !!!: USE Proper distinguishment for spherical & delta point lights
//         reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
//       }
//       break;

//     case LIGHT_SUN:
//     case LIGHT_AREA:
//       reference->flags |= RESTIR_LIGHT_REFERENCE_HAS_PARAM;
//       break;

//     default:
//       *reference = restir_light_reference_empty();
//       return false;
//   }

//   return true;
// }

// ccl_device_inline void restir_unpack_reservoir(
//     KernelGlobals kg,
//     ccl_private ReSTIRDIReservoir *reservoir,
//     ccl_global const float *buffer)
// {
//   PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

//   reservoir->reference = restir_light_reference_empty();

//   const bool valid_reference = restir_unpack_light_reference(kg, buffer, &reservoir->reference);

//   reservoir->total_weight = buffer[4];
//   reservoir->luminance = buffer[5];

//   // if (!valid_reference || !(reservoir->total_weight > 0.0f)) {
//   if (!(isfinite(reservoir->total_weight) && reservoir->total_weight > 0.0f &&
//       isfinite(reservoir->luminance) && reservoir->luminance >= 0.0f)) {
//     reservoir->reference = restir_light_reference_empty();
//     reservoir->total_weight = 0.0f;
//     reservoir->luminance = 0.0f;
//   }
// }


// !!!: Deserialize the persistent reference, not write identity and parameters into LightSample
// !!!: Unpacking must produce a fully valid ReSTIRLightSampleReference
ccl_device_inline bool restir_unpack_reservoir(KernelGlobals kg, // !!!: lamp
                                               ccl_private Reservoir *reservoir,
                                               const ccl_global float *buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

  // prevents stale light, radiance, or reference metadata from surviving when reusing a stack variable
  reservoir_clear(reservoir);

  int i = 0;
  // TODO: this works for diffuse surfaces. For specular, probably `sd->wi` is needed instead.
  // TODO: compress the data.
  reservoir->reference.object = (int)buffer[i++];
  reservoir->reference.prim = (int)buffer[i++]; // For analytic lights: object = OBJECT_NONE; prim = KernelLight index
  reservoir->reference.param.x = buffer[i++];
  reservoir->reference.param.y = buffer[i++];

  reservoir->total_weight = buffer[i++];

  reservoir->luminance = buffer[i];

  if (reservoir_is_empty(reservoir)) {
    return false;
  }

  // Reconstruct reference.type, RESTIR_LIGHT_REFERENCE_VALID, RESTIR_LIGHT_REFERENCE_HAS_PARAM
  // return restir_light_reference_restore_metadata(kg, &reservoir->reference); // !!!: Make sure it properly reconstructs type
  const bool ok = restir_light_reference_restore_metadata(kg, &reservoir->reference);

  printf("restir_unpack_reservoir() returned=%d; prim=%d object=%d type=%d total_weight=%g\n",
        ok,
        reservoir->reference.prim,
        reservoir->reference.object,
        reservoir->reference.type,
        (double)reservoir->total_weight);
  kernel_assert(!ok || (reservoir->reference.prim >= 0 &&
                reservoir->reference.prim < kernel_data.integrator.num_lights));

  return ok;
}




// MARK: --- GlobalReservoirs ---
// No Reference adaptation is required

struct GlobalReservoir {
  // TODO(weizhen): Compress with rgbe. http://www.graphics.cornell.edu/online/formats/rgbe
  Spectrum radiance;
  float total_weight = 0.0f;
  ShaderData sd;
  uint32_t path_flag;

  /* Information about the reconnection vertex (rcv). */
  int rcv_index;

  float3 rgb_to_y;
};

ccl_device_inline void global_reservoir_init(KernelGlobals kg,
                                             ccl_private GlobalReservoir *reservoir)
{
  reservoir->radiance = zero_spectrum();
  reservoir->total_weight = 0.0f;
  reservoir->sd = {};
  reservoir->path_flag = 0;
  reservoir->rcv_index = -1;
  reservoir->rgb_to_y = float4_to_float3(kernel_data.film.rgb_to_y); // make_float3()
}

ccl_device_inline float global_reservoir_compute_luminance(
    ccl_private const GlobalReservoir *reservoir,
    const ccl_private Spectrum &radiance)
{
  return dot(spectrum_to_rgb(radiance), reservoir->rgb_to_y);
}

ccl_device_inline bool global_reservoir_add_sample(ccl_private GlobalReservoir *reservoir,
                                                   const ccl_private Spectrum &radiance,
                                                   const float rand)
{
  const float weight = global_reservoir_compute_luminance(reservoir, radiance);

  if (!(weight > 0.0f)) {
    /* Should be theoretically captured by the following condition, but we can not trust floating
     * point precision. */
    return false;
  }

  reservoir->total_weight += weight;

  if (rand * reservoir->total_weight > weight) {
    return false;
  }

  reservoir->radiance = radiance;

  return true;
}

ccl_device_inline bool global_reservoir_is_empty(
    ccl_private const GlobalReservoir *reservoir)
{
  return reservoir->total_weight == 0.0f;
}

ccl_device_inline bool global_reservoir_finalize(ccl_private GlobalReservoir *reservoir)
{
  if (global_reservoir_is_empty(reservoir)) {
    return false;
  }
  /* Apply unbiased contribution weight. */
  reservoir->total_weight /= global_reservoir_compute_luminance(reservoir, reservoir->radiance);
  reservoir->radiance *= reservoir->total_weight;
  return true;
}

ccl_device_inline void global_reservoir_add_reservoir(
    ccl_private GlobalReservoir *reservoir,
    const ccl_private GlobalReservoir &other,
    const float rand)
{
  if (global_reservoir_is_empty(&other)) {
    return;
  }

  if (global_reservoir_add_sample(reservoir, other.radiance, rand)) {
    reservoir->path_flag = other.path_flag;
    reservoir->rcv_index = other.rcv_index;
  }
}

ccl_device_inline bool restir_unpack_reservoir_pt(KernelGlobals kg,
                                                  ccl_private GlobalReservoir *reservoir,
                                                  ccl_global float *buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

  reservoir->total_weight = kernel_read_pass_float(buffer);
  reservoir->path_flag = (uint32_t)kernel_read_pass_float(buffer + 1);
  reservoir->rcv_index = (int)kernel_read_pass_float(buffer + 2);
  reservoir->radiance = kernel_read_pass_float3(buffer + 3);

  return !global_reservoir_is_empty(reservoir);
}

CCL_NAMESPACE_END