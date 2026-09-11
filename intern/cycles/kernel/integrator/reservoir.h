// reservoir.h

#pragma once

#include "kernel/types.h"
#include "kernel/light/common.h" // light_reference helpers

CCL_NAMESPACE_BEGIN

// TODO(Tri): Clean up mess of commit ReSTIR: Fix zero-radius spot lights in ReSTIR DI

struct Reservoir {
  LightSample light;
  LightSampleReference reference;

  BsdfEval radiance;
  float luminance;
  float total_weight = 0.0f;

  // TODO(Tri): Support restir_temporal_frame_count
  /* Number of samples this reservoir represents. A freshly generated
   * candidate this render sample starts at 1; temporal combination grows
   * it (capped by restir_temporal_sample_count) as history accumulates
   * across samples. Spatial-only combination currently ignores this and
   * treats every neighbor as M == 1; see mis_weight_pairwise() call sites. */
  int M = 1;

  int num_light_samples;
  int num_bsdf_samples = 1;
  int max_samples;
  float3 rgb_to_y;

  int direct_sampling_method;
};

ccl_device_inline void reservoir_clear(ccl_private Reservoir *reservoir)
{
  reservoir->light = {};
  reservoir->reference = light_reference_empty();
  reservoir->radiance = {};
  reservoir->luminance = 0.0f;
  reservoir->total_weight = 0.0f;
  reservoir->M = 1;
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
  // if (reservoir->total_weight <= 0.0f) { // !!!: Debug only!
  //   ccl_debug_printf("reservoir_is_empty() returned false!!!; reservoir->total_weight=%f\n", (double)reservoir->total_weight);
  // }

  return !(reservoir->total_weight > 0.0f);
  // return reservoir->total_weight == 0.0f;
}

ccl_device_inline bool reservoir_has_valid_reference(ccl_private const Reservoir *reservoir)
{
  return light_reference_is_valid(&reservoir->reference);
}

ccl_device_inline float reservoir_compute_luminance(
    ccl_private const Reservoir *reservoir,
    const ccl_private BsdfEval &sample_radiance)
{
  return dot(spectrum_to_rgb(sample_radiance.sum), reservoir->rgb_to_y);
}

ccl_device_inline bool reservoir_finalize(ccl_private Reservoir *reservoir, // !!!
                                          const bool require_valid_reference)
{
  // if (reservoir_is_empty(reservoir) || // !!!: Suspicious
  //     (require_valid_reference && !reservoir_has_valid_reference(reservoir)) ||
  //     !(isfinite(reservoir->luminance) && reservoir->luminance > 0.0f))
  // {
  //   reservoir_clear(reservoir);
  //   return false;
  // }
  if (reservoir_is_empty(reservoir)) {
    // ccl_debug_printf("reservoir_finalize() returned false! reservoir_is_empty=true\n");
    // reservoir_clear(reservoir);
    return false;
  }

  ccl_debug_printf("reservoir_finalize(); total_weight=%f, luminance=%f\n", (double)reservoir->total_weight, (double)reservoir->luminance);
  /* Apply unbiased contribution weight. */
  reservoir->total_weight /= reservoir->luminance; // !!!

  // if (!(isfinite(reservoir->total_weight) && reservoir->total_weight > 0.0f)) { // !!!: Suspicious
  //   reservoir_clear(reservoir);
  //   return false;
  // }

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

ccl_device_inline void reservoir_add_sample( // !!!
    ccl_private Reservoir *reservoir,
    const ccl_private LightSample &ls,
    const ccl_private LightSampleReference &candidate_reference,
    const ccl_private BsdfEval &candidate_radiance,
    const float mis_weight,
    const float rand,
    const bool require_valid_reference)
{
  const float candidate_luminance = reservoir_compute_luminance(reservoir, candidate_radiance);
  const float weight = candidate_luminance * mis_weight;

// ccl_debug_printf(
//     "\n=== RESERVOIR ADD SAMPLE ===\n"
//     "candidate: type=%d prim=%d object=%d flags=%u param=(%f,%f)\n"
//     "candidate_luminance=%f mis_weight=%f weight=%f rand=%f\n"
//     "BEFORE: total_weight=%f selected_prim=%d selected_object=%d\n"
//     "============================\n\n",
//     candidate_reference.type,
//     candidate_reference.prim,
//     candidate_reference.object,
//     candidate_reference.flags,
//     (double)candidate_reference.param.x,
//     (double)candidate_reference.param.y,
//     (double)candidate_luminance,
//     (double)mis_weight,
//     (double)weight,
//     (double)rand,
//     (double)reservoir->total_weight,
//     reservoir->reference.prim,
//     reservoir->reference.object);

  // if ((require_valid_reference && !light_reference_is_valid(&candidate_reference)) || // !!!: zero-radius; suspicious
  //     !(isfinite(candidate_luminance) && candidate_luminance > 0.0f) ||
  //     !(isfinite(weight) && weight > 0.0f))
  // {
  //   return;
  // }
  if (!(weight > 0.0f)) {
    ccl_debug_printf(
        "=== RESERVOIR ADD REJECTED === "
        "valid_ref=%d luminance=%f weight=%f\n",
        (int)light_reference_is_valid(&candidate_reference),
        (double)candidate_luminance,
        (double)weight);
    /* Should be theoretically captured by the following condition, but we can not trust floating
      * point precision. */
    return;
  }

  reservoir->total_weight += weight;

  // if (!(isfinite(reservoir->total_weight) && reservoir->total_weight > 0.0f)) { // !!!: zero-radius; suspicious
  //   reservoir_clear(reservoir);
  //   return;
  // }

  // Reservoir replacement
  if (rand * reservoir->total_weight <= weight) {
    reservoir->light = ls;
    reservoir->reference = candidate_reference;
    reservoir->radiance = candidate_radiance;
    reservoir->luminance = candidate_luminance;

    // ccl_debug_printf("\nreservoir_add_sample() res replacement! total_weight=%f\n", (double)reservoir->total_weight);
  }
}

ccl_device_inline void reservoir_add_light_sample(
    ccl_private Reservoir *reservoir,
    const ccl_private LightSample &ls,
    const ccl_private LightSampleReference &candidate_reference,
    const ccl_private BsdfEval &candidate_radiance,
    const float bsdf_pdf,
    const float rand,
    const bool require_valid_reference)
{
  // if (!(isfinite(ls.pdf) && ls.pdf > 0.0f)) { // !!!: zero-radius; suspicious
  //   return;
  // }

/*
The RIS path weights samples via reservoir_mis_weight_nee() inside reservoir_add_light_sample(), 
normalized through reservoir_finalize()'s unbiased-contribution-weight division. 
The legacy path uses light_sample_mis_weight_nee() directly against ls.eval_fac. 
These are different estimators of the same underlying integral — 
patching the reference alone would still shade every non-ReSTIR bounce through the wrong one.
*/
  const float mis_weight = reservoir_mis_weight_nee(reservoir, ls.pdf, bsdf_pdf);

  float light_pdf_in_area = ls.pdf;

  // TODO(weizhen): Convert pdf to area measure when returning the pdf instead of here.
  float jacobian = 1.0f;
  /* Sun is already sampled in solid angle. */
  if (ls.type != LIGHT_SUN) {
    jacobian = ls.jacobian_solid_angle_to_area(); // !!!
  }

// ccl_debug_printf(
//     "\n=== ADD LIGHT SAMPLE ===\n"
//     "type=%d prim=%d object=%d\n"
//     "ls.pdf=%f\n"
//     "bsdf_pdf=%f\n"
//     "jacobian=%f\n"
//     "========================\n",
//     ls.type,
//     ls.prim,
//     ls.object,
//     (double)ls.pdf,
//     (double)bsdf_pdf,
//     (double)jacobian);

  if (jacobian > 0.0f) {
    light_pdf_in_area *= jacobian;
    // ccl_debug_printf("reservoir_add_light_sample() updated light_pdf_in_area=%f\n", (double)light_pdf_in_area);
  }

  // if (!(isfinite(light_pdf_in_area) && light_pdf_in_area > 0.0f)) { // !!!: zero-radius; suspicious
  //   return;
  // }
  
// ccl_debug_printf(
//     "CANDIDATE RADIANCE: "
//     "type=%d pdf=%f eval_fac=%f "
//     "sum radiance=(%f,%f,%f) "
//     "jacobian=%f\n",
//     ls.type,
//     (double)ls.pdf,
//     (double)ls.eval_fac,
//     (double)candidate_radiance.sum.x,
//     (double)candidate_radiance.sum.y,
//     (double)candidate_radiance.sum.z,
//     (double)jacobian);
  // ccl_debug_printf("reservoir_add_light_sample() updated (mis_weight / light_pdf_in_area)=%f\n", (double)(mis_weight / light_pdf_in_area));

  reservoir_add_sample(reservoir,
                       ls,
                       candidate_reference,
                       candidate_radiance,
                       mis_weight / light_pdf_in_area,
                       rand,
                       require_valid_reference);
}

ccl_device_inline void reservoir_add_bsdf_sample(
    ccl_private Reservoir *reservoir,
    const ccl_private LightSample &ls,
    const ccl_private LightSampleReference &candidate_reference,
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
  if (reservoir_is_empty(&other) || !reservoir_has_valid_reference(&other)) { // !!!: Suspicious
    return;
  }
  // if (reservoir_is_empty(&other)) {
  //   return;
  // }

  reservoir_add_sample(
      reservoir, other.light, other.reference, other.radiance, other.total_weight, rand, require_valid_reference);
}

// TODO: Cleaner to inspect reservoir.reference?
// TODO(weizhen): Maybe it should be determined when the sample is drawn, not afterwards
// !!!: Better against res.reference anyway, cause tis the persistent identity & survives film unpacking before light's been rebuilt.
ccl_device_inline bool sample_copy_direction(KernelGlobals kg, // !!!: lamp
                                             ccl_private const Reservoir &reservoir)
{
  if (reservoir_is_empty(&reservoir) || !reservoir_has_valid_reference(&reservoir)) { // !!!: Suspicious
    return false;
  }
  // if (reservoir_is_empty(&reservoir)) {
  //   return false;
  // }

  // if (reservoir.reference.object != OBJECT_NONE) { // !!!: Wrong?
  //   return false;
  // }

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

  // TODO: Spot and sun as well?

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

// !!!: Deserialize the persistent reference, not write identity and parameters into LightSample
// !!!: Unpacking must produce a fully valid LightSampleReference
ccl_device_inline bool restir_unpack_reservoir(KernelGlobals kg, // !!!: lamp
                                               ccl_private Reservoir *reservoir,
                                               const ccl_global float *buffer)
{
  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

  // prevents stale light, radiance, or reference metadata from surviving when reusing a stack variable
  // simply guarantees that anything you don't deserialize cannot remain stale.
  reservoir_clear(reservoir);


// ccl_debug_printf(
//     "ZERO RESERVOIR BEFORE WRITE: "
//     "total_weight=%f luminance=%f "
//     "flags=%u type=%d prim=%d object=%d\n",
//     (double)reservoir->total_weight,
//     (double)reservoir->luminance,
//     reservoir->reference.flags,
//     reservoir->reference.type,
//     reservoir->reference.prim, // If -1 here, the bug is reservoir selection/finalization/addition before film writing.
//     reservoir->reference.object);

  int i = 0;

  // TODO: this works for diffuse surfaces. For specular, probably `sd->wi` is needed instead.
  // TODO: compress the data.

  reservoir->reference.type = (LightType)(int)buffer[i++];
  reservoir->reference.flags = (uint32_t)buffer[i++];

  reservoir->reference.object = (int)buffer[i++];
  reservoir->reference.prim = (int)buffer[i++]; // For analytic lights: object = OBJECT_NONE; prim = KernelLight index
  reservoir->reference.param.x = buffer[i++];
  reservoir->reference.param.y = buffer[i++];

  reservoir->total_weight = buffer[i++];

  reservoir->luminance = buffer[i++];

  reservoir->M = (int)buffer[i]; // New

  // ccl_debug_printf("restir_unpack_reservoir() entered; object=%f, prim=%f, total_weight=%f\n\n\n",
  //   (double)reservoir->reference.object, (double)reservoir->reference.prim, (double)reservoir->total_weight);
// ccl_debug_printf( // Rejected candidates
//     "ZERO RESERVOIR AFTER READ: "
//     "total_weight=%f luminance=%f "
//     "flags=%u type=%d prim=%d object=%d\n"
//     "===================================\n",
//     (double)reservoir->total_weight,
//     (double)reservoir->luminance,
//     reservoir->reference.flags,
//     reservoir->reference.type,
//     reservoir->reference.prim,
//     reservoir->reference.object);

  return !reservoir_is_empty(reservoir); // TODO: Uncomment :)


// !!!: DEBUG ONLY
  /* No selected sample exists in an empty reservoir.
   * Do not attempt to restore/reconstruct its light reference.
   */
  if (reservoir_is_empty(reservoir)) {
    ccl_debug_printf("restir_unpack_reservoir() returns false on empty reservoir!\n\n");
    return false;
  }

ccl_debug_printf( // Accepted candidates
    "\n=== UNPACKED REFERENCE; Accepted candidates ===\n"
    "total_weight=%f\n"
    "luminance=%f\n"
    "type=%d\n"
    "flags=%u\n"
    "object=%d\n"
    "prim=%d\n"
    "param=(%f,%f)\n"
    "==========================\n",
    (double)reservoir->total_weight,
    (double)reservoir->luminance,
    reservoir->reference.type,
    reservoir->reference.flags,
    reservoir->reference.object,
    reservoir->reference.prim,
    (double)reservoir->reference.param.x,
    (double)reservoir->reference.param.y);

  return true;

  // TODO: Compress later... flags and ...
  // return restir_light_reference_restore_metadata(kg, &reservoir->reference);
}


// MARK: - TemporalReprojection

// TODO(Tri): Move elsewhere...
/* Minimal per-pixel record kept purely for the reprojection test!
 * NOT the full reservoir, and NOT reconstructed via re-intersection like
 * SpatialReservoir::sd is. Written every sample, ping-ponged like the
 * reservoir buffer. */
struct ReprojectionSurface {
  float3 N;       // Shading normal
  float depth;    // Position or linear depth
  float2 motion;  // raster-space displacement toward the previous frame
  bool is_valid;
};

ccl_device_inline ReprojectionSurface reprojection_surface_unpack(
    const ccl_global float *buffer)
{
  ReprojectionSurface s;
  int i = 0;
  s.N.x = buffer[i++];
  s.N.y = buffer[i++];
  s.N.z = buffer[i++];
  s.depth = buffer[i++];
  s.motion.x = buffer[i++];
  s.motion.y = buffer[i++];
  s.is_valid = buffer[i] > 0.0f;
  return s;
}

/* Motion-vector back-projection + reprojection-validity test, the temporal
 * analog of SpatialResampling. 
 * Falcor's TemporalResampling plays the same role via:
 * motionVectors + prevSurfaceData + normalThreshold/depthThreshold. */
struct TemporalReprojection {
  ccl_global const KernelWorkTile *tile;

  TemporalReprojection(ccl_global const KernelWorkTile *ccl_restrict tile)
  {
    this->tile = tile;
  }

  uint get_render_pixel_index(const int x, const int y) const
  {
    return (uint)tile->offset + x + y * tile->stride;
  }

  /* Falcor's isValidPixel(), against this tile's bounds rather than the
   * whole frame; matches how SpatialResampling::get_neighbor() already
   * clamps against tile->min_x/max_x/min_y/max_y. */
  bool is_valid_pixel(const int x, const int y) const
  {
    return x >= tile->min_x && x <= tile->max_x && y >= tile->min_y && y <= tile->max_y;
  }

  /* !!!(Tri):
   * Back-project (x, y) using a screen-space motion vector that points
   * current -> previous frame, in pixels. Sub-pixel jitter (Falcor adds
   * sampleNext2D(sg)) is deliberately omitted here since without it we'd
   * be biasing which of 4 neighboring previous pixels we land on. */
  uint3 get_previous_pixel(const int x, const int y, const float2 motion) const
  {
    const int px = (int)floorf((float)x + motion.x + 0.5f);
    const int py = (int)floorf((float)y + motion.y + 0.5f);

    if (!is_valid_pixel(px, py)) {
      return make_uint3(px, py, (uint)-1);
    }

    return make_uint3(px, py, get_render_pixel_index(px, py));
  }

  /* Reprojection-miss test. Unlike SpatialResampling::is_valid_neighbor()
   * (same object + same light type), the reprojected pixel is legitimately
   * a *different* primitive on a continuously-moving surface, so this
   * compares geometry directly rather than identity. */
  static bool is_valid_reprojection(const ccl_private ReprojectionSurface *curr,
                                    const ccl_private ReprojectionSurface *prev,
                                    const float normal_threshold,
                                    const float depth_threshold)
  {
    if (!prev->is_valid) {
      /* Disocclusion, off-screen last frame, or history never written
       * (first sample of the render). */
      return false;
    }

    if (dot(curr->N, prev->N) < normal_threshold) {
      return false;
    }

    if (fabsf(curr->depth - prev->depth) > depth_threshold * fmaxf(curr->depth, 1e-4f)) {
      return false;
    }

    return true;
  }
};



// MARK: --- GlobalReservoirs ---
// No Reference adaptation is required

/* TODO(Tri): GlobalReservoir/PT temporal, needs the same M treatment, but is entangled 
   with the still-unresolved reconnection-vertex problem explained in `shade_background.h`.
 * Can't correctly re-evaluate a GI candidate's target function at a new shading context 
 * without sd actually being reconstructable. Temporal PT reuse should wait...
 */
struct GlobalReservoir {
  // File intern/cycles/util/types_rgbe.h
  /* TODO(weizhen): Compress with rgbe. http://www.graphics.cornell.edu/online/formats/rgbe
   * https://www.graphics.cornell.edu/~bjw/rgbe.html
   * https://github.com/box/hdrCompressor/blob/master/src/rgbe/rgbe.c */
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
// ccl_debug_printf("====== global_reservoir_add_sample() ======\n");

  const float weight = global_reservoir_compute_luminance(reservoir, radiance);
  if (!(weight > 0.0f)) { // !!!:
ccl_debug_printf("global_reservoir_add_sample() early returned false, 1!\n");
    /* Should be theoretically captured by the following condition, but we can not trust floating
     * point precision. */
    return false;
  }

ccl_debug_printf("global_reservoir_add_sample() ADD: radiance=(%f,%f,%f) weight=%f total_before=%f rand=%f\n",
       (double)radiance.x, (double)radiance.y, (double)radiance.z,
       (double)weight, (double)reservoir->total_weight, (double)rand);

  reservoir->total_weight += weight;

  if (rand * reservoir->total_weight > weight) {
ccl_debug_printf("global_reservoir_add_sample() early returned false, 2!\n");
    return false;
  }

  reservoir->radiance = radiance;


// !!!: just enough to establish whether the path is entering it.
ccl_debug_printf("global_reservoir_add_sample() TOTAL=%f path_flag=%d\n\n\n",
       (double)reservoir->total_weight, reservoir->path_flag);

  return true;
}

ccl_device_inline bool global_reservoir_is_empty(
    ccl_private const GlobalReservoir *reservoir)
{
  return reservoir->total_weight == 0.0f;
  // return !(reservoir->total_weight > 0.0f);
}

ccl_device_inline bool global_reservoir_finalize(ccl_private GlobalReservoir *reservoir)
{
// ccl_debug_printf("====== global_reservoir_finalize() ENTER ======\n");

  if (global_reservoir_is_empty(reservoir)) {
ccl_debug_printf("global_reservoir_finalize() early returned; reservoir is empty\n");
    return false;
  }

  /* Apply unbiased contribution weight. */
  reservoir->total_weight /= global_reservoir_compute_luminance(reservoir, reservoir->radiance);
  reservoir->radiance *= reservoir->total_weight;

ccl_debug_printf("global_reservoir_finalize(): total=%f radiance=(%f, %f, %f)\n",
       (double)reservoir->total_weight, // Would be 1.0
       (double)reservoir->radiance.x, (double)reservoir->radiance.y, (double)reservoir->radiance.z);

  return true;
}

ccl_device_inline void global_reservoir_add_reservoir(
    ccl_private GlobalReservoir *reservoir,
    const ccl_private GlobalReservoir &other,
    const float rand)
{
  if (global_reservoir_is_empty(&other)) {
    ccl_debug_printf("global_reservoir_add_reservoir() early returned! other res is empty!\n\n");
    return;
  }

  if (global_reservoir_add_sample(reservoir, other.radiance, rand)) {
    reservoir->path_flag = other.path_flag;
    reservoir->rcv_index = other.rcv_index;
  }
}

// TODO: Compare against restir_unpack_reservoir() for BackGround
ccl_device_inline bool restir_unpack_reservoir_pt(KernelGlobals kg,
                                                  ccl_private GlobalReservoir *reservoir,
                                                  ccl_global float *buffer)
{
// ccl_debug_printf("====== restir_unpack_reservoir_pt() ENTER ======\n");

  PROFILING_INIT(kg, PROFILING_RESTIR_RESERVOIR_PASSES);

  reservoir->total_weight = kernel_read_pass_float(buffer);
  reservoir->path_flag = (uint32_t)kernel_read_pass_float(buffer + 1);
  reservoir->rcv_index = (int)kernel_read_pass_float(buffer + 2);
  reservoir->radiance = kernel_read_pass_float3(buffer + 3);

// !!!: If this isn't logged, The reservoir is being written but never consumed.
ccl_debug_printf("restir_unpack_reservoir_pt(): weight=%f flag=%u rcv=%d rad=(%f,%f,%f)\n\n",
       (double)reservoir->total_weight,
       reservoir->path_flag,
       reservoir->rcv_index,
       (double)reservoir->radiance.x, (double)reservoir->radiance.y, (double)reservoir->radiance.z);

  return !global_reservoir_is_empty(reservoir);
}

CCL_NAMESPACE_END