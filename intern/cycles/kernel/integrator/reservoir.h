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

  // MARK: - Methods

  Reservoir() = default;

  // Not Needed
//   Reservoir(const ccl_private Reservoir &other)
//   {
//     light = other.light;
//     reference = other.reference;
//     radiance = other.radiance;
//     luminance = other.luminance;
//     total_weight = other.total_weight;

//     num_light_samples = other.num_light_samples;
//     num_bsdf_samples = other.num_bsdf_samples;
//     max_samples = other.max_samples;
//     rgb_to_y = other.rgb_to_y;

//     direct_sampling_method = other.direct_sampling_method;
//   }

  Reservoir(KernelGlobals kg)
  {
    clear();
    num_light_samples = 0;
    num_bsdf_samples = 1;
    max_samples = 0;
    rgb_to_y = make_float3(kernel_data.film.rgb_to_y);
    direct_sampling_method = kernel_data.integrator.direct_light_sampling_type;
  }

  Reservoir(KernelGlobals kg, const bool use_ris)
  {
    clear();

    num_light_samples = use_ris ? kernel_data.integrator.restir_light_samples : 1;
    num_bsdf_samples = 1;
    max_samples = num_light_samples + num_bsdf_samples;
    rgb_to_y = float4_to_float3(kernel_data.film.rgb_to_y); // make_float3()
    direct_sampling_method = kernel_data.integrator.direct_light_sampling_type;
  }

  ccl_device_inline void clear()
  {
    light = {};
    reference = restir_light_reference_empty();
    radiance = {};
    luminance = 0.0f;
    total_weight = 0.0f;
  }

  ccl_device_inline bool is_empty() const
  {
    return !(total_weight > 0.0f);
  }

  ccl_device_inline bool has_valid_reference() const
  {
    return restir_light_reference_is_valid(&reference);
  }

  ccl_device_inline float compute_luminance(
      const ccl_private BsdfEval &sample_radiance) const
  {
    return dot(spectrum_to_rgb(sample_radiance.sum), rgb_to_y);
  }

  ccl_device_inline bool finalize()
  {
    if (is_empty() || !has_valid_reference() ||
        !(isfinite(luminance) && luminance > 0.0f))
    {
      clear();
      return false;
    }

    /* Apply unbiased contribution weight. */
    total_weight /= luminance; // !!!

    if (!(isfinite(total_weight) && total_weight > 0.0f)) {
      clear();
      return false;
    }

    return true;
  }

  ccl_device_inline void add_sample(
      const ccl_private LightSample &ls,
      const ccl_private ReSTIRLightSampleReference &candidate_reference,
      const ccl_private BsdfEval &candidate_radiance,
      const float mis_weight,
      const float rand)
  {
    const float candidate_luminance = compute_luminance(candidate_radiance);
    const float weight = candidate_luminance * mis_weight;

    if (!restir_light_reference_is_valid(&candidate_reference) ||
        !(isfinite(candidate_luminance) && candidate_luminance > 0.0f) ||
        !(isfinite(weight) && weight > 0.0f))
    {
      return;
    }

    total_weight += weight;

    if (!(isfinite(total_weight) && total_weight > 0.0f)) {
      clear();
      return;
    }

    // Reservoir replacement
    if (rand * total_weight <= weight) {
      light = ls;
      reference = candidate_reference;
      radiance = candidate_radiance;
      luminance = candidate_luminance;
    }
  }

  ccl_device_inline float power_heuristic(
      const int num_a,
      const float pdf_a,
      const int num_b,
      const float pdf_b) const
  {
    const float weighted_a = float(num_a) * sqr(pdf_a);
    const float weighted_b = float(num_b) * sqr(pdf_b);
    const float denominator = weighted_a + weighted_b;

    return (denominator > 0.0f) ? sqr(pdf_a) / denominator : 0.0f;
  }

  ccl_device_inline float mis_weight_nee(
      float light_pdf,
      float bsdf_pdf) const
  {
#ifdef WITH_CYCLES_DEBUG
    if (direct_sampling_method == DIRECT_LIGHT_SAMPLING_FORWARD) {
      light_pdf *= (bsdf_pdf == 0.0f);
    }
    else if (direct_sampling_method == DIRECT_LIGHT_SAMPLING_NEE) {
      bsdf_pdf = 0.0f;
    }
#endif

    return power_heuristic(
        num_light_samples, light_pdf, num_bsdf_samples, bsdf_pdf);
  }

  ccl_device_inline float mis_weight_forward(
      float bsdf_pdf,
      float light_pdf) const
  {
#ifdef WITH_CYCLES_DEBUG
    if (direct_sampling_method == DIRECT_LIGHT_SAMPLING_FORWARD) {
      light_pdf = 0.0f;
    }
    else if (direct_sampling_method == DIRECT_LIGHT_SAMPLING_NEE) {
      bsdf_pdf *= (light_pdf == 0.0f);
    }
#endif

    return power_heuristic(
        num_bsdf_samples, bsdf_pdf, num_light_samples, light_pdf);
  }

  ccl_device_inline void add_light_sample(
      const ccl_private LightSample &ls,
      const ccl_private ReSTIRLightSampleReference &candidate_reference,
      const ccl_private BsdfEval &candidate_radiance,
      const float bsdf_pdf,
      const float rand)
  {
    if (!(isfinite(ls.pdf) && ls.pdf > 0.0f)) {
      return;
    }

    const float mis_weight = mis_weight_nee(ls.pdf, bsdf_pdf);

    float light_pdf_in_area = ls.pdf;

    // TODO(weizhen): Convert pdf to area measure when returning the pdf instead of here.
    const float jacobian = ls.jacobian_solid_angle_to_area();

    if (jacobian > 0.0f) {
      light_pdf_in_area *= jacobian;
    }

    if (!(isfinite(light_pdf_in_area) && light_pdf_in_area > 0.0f)) {
      return;
    }

    add_sample(ls,
               candidate_reference,
               candidate_radiance,
               mis_weight / light_pdf_in_area,
               rand);
  }

  ccl_device_inline void add_bsdf_sample(
      const ccl_private LightSample &ls,
      const ccl_private ReSTIRLightSampleReference &candidate_reference,
      const ccl_private BsdfEval &candidate_radiance,
      const float bsdf_pdf,
      const float rand)
  {
    if (!(isfinite(bsdf_pdf) && bsdf_pdf > 0.0f)) {
      return;
    }

    const float mis_weight = mis_weight_forward(bsdf_pdf, ls.pdf);

    float bsdf_pdf_in_area = bsdf_pdf;

    // TODO(weizhen): Convert pdf to area measure when returning the pdf instead of here.
    const float jacobian = ls.jacobian_solid_angle_to_area();

    if (jacobian > 0.0f) {
      bsdf_pdf_in_area *= jacobian;
    }

    if (!(isfinite(bsdf_pdf_in_area) && bsdf_pdf_in_area > 0.0f)) {
      return;
    }

    add_sample(ls,
               candidate_reference,
               candidate_radiance,
               mis_weight / bsdf_pdf_in_area,
               rand);
  }
};

// TODO: Cleaner to inspect reservoir.reference?
// TODO(weizhen): Maybe it should be determined when the sample is drawn, not afterwards
// ccl_device_inline bool sample_copy_direction(KernelGlobals kg,
//                                              ccl_private const Reservoir &reservoir)
// {
//   if (reservoir.is_empty()) {
//     return false;
//   }

//   if (reservoir.light.type == LIGHT_AREA) {
//     const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reservoir.light.prim); // light.lamp
//     if (klight->area.tan_half_spread == 0.0f) {
//       return true;
//     }
//   }

//   if (reservoir.light.type == LIGHT_POINT) {
//     const ccl_global KernelLight *klight = &kernel_data_fetch(lights, reservoir.light.prim); // light.lamp
//     if (!klight->spot.is_sphere) {
//       return true;
//     }
//   }

//   return false;
// }
// !!!: Better against res.reference anyway, cause tis the persistent identity & survives film unpacking before light's been rebuilt.
ccl_device_inline bool sample_copy_direction(KernelGlobals kg, // !!!: lamp
                                             ccl_private const Reservoir &reservoir)
{
  if (reservoir.is_empty() || !reservoir.has_valid_reference()) {
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

/**
 * The selected light sample and radiance must be reconstructed and
 * reevaluated for the current target before combining reservoirs. 
 */
ccl_device_inline void Reservoir::add_reservoir(const ccl_private Reservoir &other, const float rand)
{
  // !!!: DI only
  if (other.is_empty() || !other.has_valid_reference()) {
    return;
  }

  add_sample(other.light, other.reference, other.radiance, other.total_weight, rand);
}


// MARK: - SpatialReservoir

struct SpatialReservoir {
  Reservoir reservoir;
  // TODO(weizhen): maybe the neighbors should share the same flag.
  uint32_t path_flag;
  ShaderData sd;
  uint3 pixel_index;

  SpatialReservoir() = default;

  SpatialReservoir(KernelGlobals kg) : reservoir(kg)
  {
    pixel_index = make_uint3(-1, -1, -1);
  }

  SpatialReservoir(const ccl_private SpatialReservoir &other)
  {
    reservoir = other.reservoir;
    path_flag = other.path_flag;
    sd = other.sd;
    pixel_index = other.pixel_index;
  }

  bool is_empty() const
  {
    return reservoir.is_empty();
  }

  void add_reservoir(const ccl_private SpatialReservoir &other, const float rand)
  {
    reservoir.add_reservoir(other.reservoir, rand);
  }
};

// !!!: Deserialize the persistent reference, not write identity and parameters into LightSample
// !!!: Unpacking must produce a fully valid ReSTIRLightSampleReference
ccl_device_inline bool restir_unpack_reservoir(KernelGlobals kg, // !!!: lamp
                                               ccl_private Reservoir *reservoir,
                                               const ccl_global float *buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

  // prevents stale light, radiance, or reference metadata from surviving when reusing a stack variable
  reservoir->clear();

  int i = 0;
  // TODO: this works for diffuse surfaces. For specular, probably `sd->wi` is needed instead.
  // TODO: compress the data.
  reservoir->reference.object = (int)buffer[i++];
  reservoir->reference.prim = (int)buffer[i++]; // For analytic lights: object = OBJECT_NONE; prim = KernelLight index
  reservoir->reference.param.x = buffer[i++];
  reservoir->reference.param.y = buffer[i++];

  reservoir->total_weight = buffer[i++];

  reservoir->luminance = buffer[i];

//   return !reservoir->is_empty();

  // TODO:
  if (reservoir->is_empty()) {
    return false;
  }

  // Reconstruct reference.type, RESTIR_LIGHT_REFERENCE_VALID, RESTIR_LIGHT_REFERENCE_HAS_PARAM
  return restir_light_reference_restore_metadata(kg, &reservoir->reference);
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

  GlobalReservoir() = default;

  GlobalReservoir(KernelGlobals kg)
  {
    rgb_to_y = float4_to_float3(kernel_data.film.rgb_to_y); // make_float3()
  }

  float compute_luminance(const ccl_private Spectrum &radiance) const
  {
    return dot(spectrum_to_rgb(radiance), rgb_to_y);
  }

  bool add_sample(const ccl_private Spectrum &radiance, const float rand)
  {
    const float weight = compute_luminance(radiance);

    if (!(weight > 0.0f)) {
      /* Should be theoretically captured by the following condition, but we can not trust floating
       * point precision. */
      return false;
    }

    total_weight += weight;

    if (rand * total_weight > weight) {
      return false;
    }

    this->radiance = radiance;

    return true;
  }

  bool is_empty() const
  {
    return total_weight == 0.0f;
  }

  bool finalize()
  {
    if (is_empty()) {
      return false;
    }
    /* Apply unbiased contribution weight. */
    total_weight /= compute_luminance(radiance);
    radiance *= total_weight;
    return true;
  }

  void add_reservoir(GlobalReservoir &other, const float rand)
  {
    if (other.is_empty()) {
      return;
    }

    if (add_sample(other.radiance, rand)) {
      path_flag = other.path_flag;
      rcv_index = other.rcv_index;
    }
  }
};

ccl_device_inline bool restir_unpack_reservoir_pt(KernelGlobals kg,
                                                  ccl_private GlobalReservoir *reservoir,
                                                  ccl_global float *buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

  reservoir->total_weight = kernel_read_pass_float(buffer);
  reservoir->path_flag = (uint32_t)kernel_read_pass_float(buffer + 1);
  reservoir->rcv_index = (int)kernel_read_pass_float(buffer + 2);
  reservoir->radiance = kernel_read_pass_float3(buffer + 3);

  return !reservoir->is_empty();
}


CCL_NAMESPACE_END