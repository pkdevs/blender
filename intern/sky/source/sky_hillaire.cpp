/* SPDX-FileCopyrightText: 2022 Fernando García Liñán
 * SPDX-FileCopyrightText: 2011-2026 Blender Authors
 *
 * SPDX-License-Identifier: MIT */

/** \file
 * \ingroup intern_sky_modal
 */

/*
 * "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" by Sébastien Hillaire
 * (EGSR 2020).
 *
 * The atmosphere, the spectral rendering technique and the ray marching are those of the Multiple
 * Scattering model, which is a converted version of the ShaderToy written by Fernando García
 * Liñán and is documented in sky_multiple_scattering.cpp. This model departs from it in one
 * place: multiple scattering is integrated into a LUT rather than approximated by an analytic
 * fit, which frees it from that fit's daylight tuning and lets it hold up at low sun angles and
 * under atmospheres far from Earth's average.
 *
 * Around that core it adds what a night sky needs: the airglow of the upper atmosphere, and the
 * eye's own loss of color at the levels a night is seen at.
 *
 * The code is deliberately kept separate from sky_multiple_scattering.cpp rather than sharing an
 * implementation with it, so that the two models are free to diverge and so that changes here
 * cannot disturb a shipping model.
 */

#include <algorithm>
#include <vector>

#include "sky_math.h"

#include "sky_hillaire.h"

using std::min;

/* Earth's atmosphere parameters. */
/* Ground reflectance. */
static const float4 GROUND_ALBEDO = make_float4(0.3f, 0.3f, 0.3f, 0.3f);
static const float PHASE_ISOTROPIC = M_1_4PI_F;
static const float RAYLEIGH_PHASE_SCALE = (3.0f / 16.0f) * M_1_PI_F;
/* Aerosols anisotropy. This model takes it from the node, so the constant is only a stand-in for
 * the paths below that ask for nothing but transmittance, which is pure extinction and never
 * reaches the phase function. */
static const float G = 0.8f;
/* Earth radius (km). */
static const float EARTH_RADIUS = 6371.0f;
/* Atmosphere thickness (km). */
static const float ATMOSPHERE_THICKNESS = 100.0f;
static const float ATMOSPHERE_RADIUS = EARTH_RADIUS + ATMOSPHERE_THICKNESS;
/* Ray marching steps. Higher steps means increased accuracy but worse performance. */
static const int TRANSMITTANCE_STEPS = 64;
static const int IN_SCATTERING_STEPS = 64;
static const int MULTISCATTERING_STEPS = 32;

/* LUTs. */
static const int TRANSMITTANCE_RES_X = 256;
static const int TRANSMITTANCE_RES_Y = 64;
static const int MULTISCATTERING_RES = 32;
/* Number of directions sampled around a point when integrating multiple scattering, given as the
 * square root of the total count so that the samples can be stratified in two dimensions. */
static const int MULTISCATTERING_SQRT_SAMPLES = 8;

/* Spectral data sampled at 630, 560, 490, 430 nm for urban area. */
static const float4 SUN_SPECTRAL_IRRADIANCE = make_float4(1.679f, 1.828f, 1.986f, 1.307f);
static const float4 MOLECULAR_SCATTERING_COEFFICIENT_BASE = make_float4(
    6.605e-3f, 1.067e-2f, 1.842e-2f, 3.156e-2f);
static const float4 OZONE_ABSORPTION_CROSS_SECTION = make_float4(
    3.472e-25f, 3.914e-25f, 1.349e-25f, 11.03e-27f);
/* Average ozone dobson of monthly mean values. */
static const float OZONE_MEAN_DOBSON = 334.5f;
static const float4 AEROSOL_ABSORPTION_CROSS_SECTION = make_float4(
    2.8722e-24f, 4.6168e-24f, 7.9706e-24f, 1.3578e-23f);
static const float4 AEROSOL_SCATTERING_CROSS_SECTION = make_float4(
    1.5908e-22f, 1.7711e-22f, 2.0942e-22f, 2.4033e-22f);
/* Airglow is emission from the upper atmosphere rather than scattered sunlight, so it is given
 * as a radiance in the same four bands. The 557.7 nm oxygen line dominates and lands almost
 * exactly on the 560 nm band, with the 630 nm line and a weak continuum under it. These are the
 * true line strengths, and on their own they render a vivid green - which is what a long exposure
 * records. Turning that into what a person standing outside would actually see is the night
 * adaptation's job, not this constant's.
 *
 * The four weights carry only the shape. The scale in front of them sets the level, and is not a
 * measured radiance: written as a bare spectral shape the emission would land in the same units
 * as SUN_SPECTRAL_IRRADIANCE and come out some five decades too bright. It is tuned instead
 * against the levels this model actually renders at, so that a moonless night reads at an
 * ordinary camera exposure rather than at a physically honest one nothing could see. */
static const float4 AIRGLOW_SPECTRAL_RADIANCE = 4.0e-6f * make_float4(0.35f, 1.0f, 0.16f, 0.10f);
/* Height of the emitting layer (km). */
static const float AIRGLOW_LAYER_ALTITUDE = 90.0f;

static const float AEROSOL_BASE_DENSITY = 1.3681e20f;
static const float AEROSOL_BACKGROUND_DENSITY = 2e6f;
static const float AEROSOL_HEIGHT_SCALE = 0.73f;
/* Spectral to XYZ space conversion matrix. */
static const float3 SPECTRAL_XYZ[4] = {
    make_float3(53.386917738564668023f, 22.981337506691024754f, 0.0f),
    make_float3(43.904844466369358263f, 71.347795700053393866f, 0.102506867965741307f),
    make_float3(1.6137278251608962005f, 18.422960591455485011f, 31.742921188390805758f),
    make_float3(20.762668673810577145f, 2.3614213523314368527f, 110.48009643252140334f),
};

inline float molecular_phase_function(const float cos_theta)
{
  return RAYLEIGH_PHASE_SCALE * (1.0f + sqr(cos_theta));
}

inline float aerosol_phase_function(const float cos_theta, const float g)
{
  const float sqr_g = sqr(g);
  float den = 1.0f + sqr_g + 2.0f * g * cos_theta;
  return M_1_4PI_F * (1.0f - sqr_g) / (den * sqrtf(den));
}

inline float4 get_molecular_scattering_coefficient(const float h)
{
  return MOLECULAR_SCATTERING_COEFFICIENT_BASE * expf(-0.07771971f * powf(h, 1.16364243f));
}

inline float4 get_molecular_absorption_coefficient(const float h)
{
  const float log_h = logf(fmaxf(h, 1e-4f));
  float density = 3.78547397e20f * expf(-sqr(log_h - 3.22261f) * 5.55555555f - log_h);
  return OZONE_ABSORPTION_CROSS_SECTION * OZONE_MEAN_DOBSON * density;
}

inline float get_aerosol_density(const float h)
{
  float division = AEROSOL_BACKGROUND_DENSITY / AEROSOL_BASE_DENSITY;
  return AEROSOL_BASE_DENSITY * (expf(-h / AEROSOL_HEIGHT_SCALE) + division);
}

inline float3 spectral_to_xyz(const float4 L)
{
  float3 xyz = make_float3(0.0f, 0.0f, 0.0f);
  for (int i = 0; i < 4; i++) {
    xyz += SPECTRAL_XYZ[i] * L[i];
  }
  return xyz;
}

/* Tint the light lighting the atmosphere. Scattering is linear in that light, so tinting the
 * result equals tinting the source. Tinting is a per-channel multiply in the render's RGB space,
 * but the sky is computed in XYZ, where channel scaling means something else - so the caller
 * passes the equivalent transform already written in XYZ, keeping color management out of this
 * file. Null leaves the light alone. */
inline float3 apply_illuminant(const float3 xyz, const float *m)
{
  if (m == nullptr) {
    return xyz;
  }
  return make_float3(m[0] * xyz.x + m[1] * xyz.y + m[2] * xyz.z,
                     m[3] * xyz.x + m[4] * xyz.y + m[5] * xyz.z,
                     m[6] * xyz.x + m[7] * xyz.y + m[8] * xyz.z);
}

/* Same, for an emission saturated enough to leave the render's gamut. The airglow is very nearly
 * a single wavelength once the air has taken the rest away, and no set of RGB primaries can
 * reach it, so its blue channel comes out negative. Carrying that negative through would leave
 * the shading backends to deal with it, and they do not agree: Cycles' SVM kernel clamps its
 * final color while its OSL kernel does not. Cutting it here, once, at the only place the
 * emission is added, keeps every backend looking at the same sky.
 *
 * to_rgb converts XYZ into the render's RGB, and tinted_to_xyz carries it back with the night
 * adaptation's tint already folded in. Both row major, both null to leave the color alone. */
inline float3 apply_emission(const float3 xyz, const float *to_rgb, const float *tinted_to_xyz)
{
  if (to_rgb == nullptr || tinted_to_xyz == nullptr) {
    return xyz;
  }
  const float3 rgb = make_float3(
      fmaxf(to_rgb[0] * xyz.x + to_rgb[1] * xyz.y + to_rgb[2] * xyz.z, 0.0f),
      fmaxf(to_rgb[3] * xyz.x + to_rgb[4] * xyz.y + to_rgb[5] * xyz.z, 0.0f),
      fmaxf(to_rgb[6] * xyz.x + to_rgb[7] * xyz.y + to_rgb[8] * xyz.z, 0.0f));
  return make_float3(
      tinted_to_xyz[0] * rgb.x + tinted_to_xyz[1] * rgb.y + tinted_to_xyz[2] * rgb.z,
      tinted_to_xyz[3] * rgb.x + tinted_to_xyz[4] * rgb.y + tinted_to_xyz[5] * rgb.z,
      tinted_to_xyz[6] * rgb.x + tinted_to_xyz[7] * rgb.y + tinted_to_xyz[8] * rgb.z);
}

/* Precomputed data. */
class SkyHillaire {
 public:
  SkyHillaire(const float air_density,
              const float aerosol_density,
              const float ozone_density,
              const float aerosol_anisotropy)
      : air_density(air_density),
        aerosol_density(aerosol_density),
        ozone_density(ozone_density),
        aerosol_anisotropy(aerosol_anisotropy)
  {
  }

  /* Compute atmosphere's transmittance from the given altitude to the sun. */
  float4 get_transmittance(const float cos_theta, const float normalized_altitude) const
  {
    const float3 sun_dir = sun_direction(cos_theta);
    const float distance_to_earth_center = mix(
        EARTH_RADIUS, ATMOSPHERE_RADIUS, normalized_altitude);
    const float3 ray_origin = make_float3(0.0f, 0.0f, distance_to_earth_center);
    const float t_d = ray_sphere_intersection(ray_origin, sun_dir, ATMOSPHERE_RADIUS);
    const float t_step = t_d / TRANSMITTANCE_STEPS;

    float4 result = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int step = 0; step < TRANSMITTANCE_STEPS; step++) {
      const float t = (step + 0.5f) * t_step;
      const float3 x_t = ray_origin + sun_dir * t;
      const float altitude = fmaxf(x_t.length() - EARTH_RADIUS, 0.0f);
      float4 aerosol_absorption, aerosol_scattering, molecular_absorption, molecular_scattering;
      get_atmosphere_collision_coefficients(altitude,
                                            aerosol_absorption,
                                            aerosol_scattering,
                                            molecular_absorption,
                                            molecular_scattering);
      const float4 extinction = aerosol_absorption + aerosol_scattering + molecular_absorption +
                                molecular_scattering;
      result += extinction * t_step;
    }

    return exp(-result);
  }

  /* Compute in-scattered radiance for the given ray. */
  float4 get_inscattering(const float3 sun_dir,
                          const float3 ray_origin,
                          const float3 ray_dir,
                          const float t_d) const
  {
    const float cos_theta = dot(-ray_dir, sun_dir);
    const float molecular_phase = molecular_phase_function(cos_theta);
    const float aerosol_phase = aerosol_phase_function(cos_theta, aerosol_anisotropy);
    const float dt = t_d / IN_SCATTERING_STEPS;
    float4 L_inscattering = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 transmittance = make_float4(1.0f, 1.0f, 1.0f, 1.0f);
    for (int i = 0; i < IN_SCATTERING_STEPS; i++) {
      const float t = (i + 0.5f) * dt;
      const float3 x_t = ray_origin + ray_dir * t;
      const float distance_to_earth_center = x_t.length();
      const float3 zenith_dir = x_t / distance_to_earth_center;
      const float altitude = fmaxf(distance_to_earth_center - EARTH_RADIUS, 0.0f);
      const float normalized_altitude = altitude / ATMOSPHERE_THICKNESS;
      const float sample_cos_theta = dot(zenith_dir, sun_dir);
      float4 aerosol_absorption, aerosol_scattering, molecular_absorption, molecular_scattering;
      get_atmosphere_collision_coefficients(altitude,
                                            aerosol_absorption,
                                            aerosol_scattering,
                                            molecular_absorption,
                                            molecular_scattering);
      const float4 extinction = aerosol_absorption + aerosol_scattering + molecular_absorption +
                                molecular_scattering;
      const float4 transmittance_to_sun = lookup_transmittance(sample_cos_theta,
                                                               normalized_altitude);
      const float4 ms = lookup_multiscattering(
          sample_cos_theta, normalized_altitude, distance_to_earth_center);
      const float4 S = SUN_SPECTRAL_IRRADIANCE *
                       (molecular_scattering * (molecular_phase * transmittance_to_sun + ms) +
                        aerosol_scattering * (aerosol_phase * transmittance_to_sun + ms));
      const float4 step_transmittance = exp(-dt * extinction);
      /* Energy-conserving analytical integration "Physically Based Sky, Atmosphere and Cloud
       * Rendering in Frostbite" by Sébastien Hillaire. */
      const float4 cut_ext = max(extinction, 1e-7f);
      const float4 S_int = (S - S * step_transmittance) / cut_ext;
      L_inscattering = L_inscattering + transmittance * S_int;
      transmittance *= step_transmittance;
    }

    return L_inscattering;
  }

  /* Compute the light reaching the given point after two or more scattering events.
   *
   * Multiply-scattered light is assumed to be isotropic and locally uniform, so every scattering
   * order passes on the same fraction of its energy to the next one and the orders form a
   * geometric series. Only the second order and that fraction have to be integrated; all the
   * remaining orders are then summed in closed form. See "A Scalable and Production Ready Sky and
   * Atmosphere Rendering Technique" by Sébastien Hillaire (EGSR 2020). */
  float4 get_multiscattering(const float cos_theta, const float normalized_altitude) const
  {
    const float3 sun_dir = sun_direction(cos_theta);
    /* Keep the point strictly above the surface, matching the altitude clamp applied in
     * SKY_hillaire_precompute_texture(). Exactly on the surface the ray/sphere
     * intersection becomes degenerate and rays pointing down would pass through the planet
     * instead of being blocked by it. */
    const float distance_to_earth_center = fmaxf(
        mix(EARTH_RADIUS, ATMOSPHERE_RADIUS, normalized_altitude), EARTH_RADIUS + 0.001f);
    const float3 ray_origin = make_float3(0.0f, 0.0f, distance_to_earth_center);

    float4 second_order = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 transfer = make_float4(0.0f, 0.0f, 0.0f, 0.0f);

    /* Stratified uniform sampling of the sphere of directions around the point. */
    for (int i = 0; i < MULTISCATTERING_SQRT_SAMPLES; i++) {
      for (int j = 0; j < MULTISCATTERING_SQRT_SAMPLES; j++) {
        const float u = (i + 0.5f) / MULTISCATTERING_SQRT_SAMPLES;
        const float v = (j + 0.5f) / MULTISCATTERING_SQRT_SAMPLES;
        const float azimuth = M_2PI_F * u;
        /* Sample the cosine rather than the angle, so that the directions are distributed
         * uniformly in solid angle instead of clustering around the poles. */
        const float cos_zenith = 1.0f - 2.0f * v;
        const float sin_zenith = safe_sqrtf(1.0f - sqr(cos_zenith));
        const float3 ray_dir = make_float3(
            sin_zenith * cosf(azimuth), sin_zenith * sinf(azimuth), cos_zenith);

        integrate_multiscattering_ray(sun_dir, ray_origin, ray_dir, second_order, transfer);
      }
    }

    /* Both quantities are averages of the radiance arriving over the sphere, so the uniformly
     * distributed samples are simply averaged. */
    const float inv_samples = 1.0f / (MULTISCATTERING_SQRT_SAMPLES * MULTISCATTERING_SQRT_SAMPLES);
    second_order = second_order * inv_samples;
    transfer = transfer * inv_samples;

    /* Sum of the geometric series formed by the third and all further scattering orders. */
    const float4 one = make_float4(1.0f, 1.0f, 1.0f, 1.0f);
    return second_order / max(one - transfer, 1e-7f);
  }

  /* Precompute the transmittance LUT. Must be called before get_inscattering(). */
  void precompute_lut()
  {
    SKY_parallel_for(0, TRANSMITTANCE_RES_Y, 4, [&](const size_t begin, const size_t end) {
      for (int y = begin; y < end; y++) {
        for (int x = 0; x < TRANSMITTANCE_RES_X; x++) {
          const float2 uv = make_float2(x / float(TRANSMITTANCE_RES_X - 1),
                                        y / float(TRANSMITTANCE_RES_Y - 1));
          transmittance_lut[y][x] = get_transmittance(uv.x * 2.0f - 1.0f, uv.y);
        }
      }
    });
  }

  /* Precompute the multiple scattering LUT. Must be called after precompute_lut(), which it
   * samples to find how much sunlight reaches each point of the atmosphere. */
  void precompute_multiscattering_lut()
  {
    SKY_parallel_for(0, MULTISCATTERING_RES, 4, [&](const size_t begin, const size_t end) {
      for (int y = begin; y < end; y++) {
        for (int x = 0; x < MULTISCATTERING_RES; x++) {
          const float2 uv = make_float2(x / float(MULTISCATTERING_RES - 1),
                                        y / float(MULTISCATTERING_RES - 1));
          multiscattering_lut[y][x] = get_multiscattering(uv.x * 2.0f - 1.0f, uv.y);
        }
      }
    });
  }

 protected:
  float4 transmittance_lut[TRANSMITTANCE_RES_Y][TRANSMITTANCE_RES_X];
  float4 multiscattering_lut[MULTISCATTERING_RES][MULTISCATTERING_RES];
  float air_density;
  float aerosol_density;
  float ozone_density;
  /* Asymmetry of the aerosol phase function. Positive scatters forward, which tightens the glow
   * around the Sun; zero scatters evenly in every direction. */
  float aerosol_anisotropy;

  /* Compute absorption/scattering coefficients at the given altitude. */
  void get_atmosphere_collision_coefficients(const float altitude,
                                             float4 &aerosol_absorption,
                                             float4 &aerosol_scattering,
                                             float4 &molecular_absorption,
                                             float4 &molecular_scattering) const
  {
    const float local_aerosol_density = get_aerosol_density(altitude) * aerosol_density;
    aerosol_absorption = AEROSOL_ABSORPTION_CROSS_SECTION * local_aerosol_density;
    aerosol_scattering = AEROSOL_SCATTERING_CROSS_SECTION * local_aerosol_density;
    molecular_absorption = get_molecular_absorption_coefficient(altitude) * ozone_density;
    molecular_scattering = get_molecular_scattering_coefficient(altitude) * air_density;
  }

  /* March a single direction away from the point being precomputed, accumulating the second
   * order of scattering and the fraction of light that each order passes on to the next.
   *
   * Rays that reach the ground stop there. The light the ground reflects back into the
   * atmosphere is accounted for separately, in lookup_multiscattering(). */
  void integrate_multiscattering_ray(const float3 sun_dir,
                                     const float3 ray_origin,
                                     const float3 ray_dir,
                                     float4 &r_second_order,
                                     float4 &r_transfer) const
  {
    const float ground_dist = ray_sphere_intersection(ray_origin, ray_dir, EARTH_RADIUS);
    const float atmos_dist = ray_sphere_intersection(ray_origin, ray_dir, ATMOSPHERE_RADIUS);
    /* If there is no ground collision the ray leaves through the outer atmosphere, otherwise it
     * is blocked by the ground. */
    const float t_d = (ground_dist < 0.0f) ? atmos_dist : ground_dist;
    if (t_d <= 0.0f) {
      return;
    }
    const float dt = t_d / MULTISCATTERING_STEPS;

    float4 transmittance = make_float4(1.0f, 1.0f, 1.0f, 1.0f);
    for (int step = 0; step < MULTISCATTERING_STEPS; step++) {
      const float t = (step + 0.5f) * dt;
      const float3 x_t = ray_origin + ray_dir * t;
      const float distance_to_earth_center = x_t.length();
      const float3 zenith_dir = x_t / distance_to_earth_center;
      const float altitude = fmaxf(distance_to_earth_center - EARTH_RADIUS, 0.0f);
      const float normalized_altitude = altitude / ATMOSPHERE_THICKNESS;
      const float sample_cos_theta = dot(zenith_dir, sun_dir);
      float4 aerosol_absorption, aerosol_scattering, molecular_absorption, molecular_scattering;
      get_atmosphere_collision_coefficients(altitude,
                                            aerosol_absorption,
                                            aerosol_scattering,
                                            molecular_absorption,
                                            molecular_scattering);
      const float4 extinction = aerosol_absorption + aerosol_scattering + molecular_absorption +
                                molecular_scattering;
      const float4 scattering = aerosol_scattering + molecular_scattering;
      const float4 step_transmittance = exp(-dt * extinction);
      /* Energy-conserving analytical integration, as in get_inscattering(). */
      const float4 cut_ext = max(extinction, 1e-7f);

      /* Sunlight that is scattered exactly once on its way to the point. */
      const float4 S = scattering * PHASE_ISOTROPIC *
                       lookup_transmittance(sample_cos_theta, normalized_altitude);
      r_second_order += transmittance * (S - S * step_transmittance) / cut_ext;

      /* Fraction of an isotropic field of unit radiance that is scattered toward the point.
       * This is what each scattering order hands on to the next one. No phase function appears
       * here: the incoming field is the same from every direction, and the phase function
       * integrates to one over the sphere. */
      const float4 F = scattering;
      r_transfer += transmittance * (F - F * step_transmittance) / cut_ext;

      transmittance *= step_transmittance;
    }
  }

  float4 lookup_multiscattering(float cos_theta, float normalized_height, float d) const
  {
    /* Solid angle subtended by the planet from a point at d distance from the planet center. */
    const float omega = M_2PI_F * (1.0f - safe_sqrtf(1.0f - sqr(EARTH_RADIUS / d)));
    const float4 T_to_ground = lookup_transmittance_at_ground(cos_theta);
    /* We can split the path into Ground <-> Sample <-> Sun.
     * The LUT gives us both T(Sample,Sun) and T(Ground,Sun) = T(Ground,Sample)*T(Sample,Sun),
     * so we can easily compute T(Ground,Sample) from those two. */
    const float4 T_ground_to_sample = lookup_transmittance_to_sun(0.0f) /
                                      lookup_transmittance_to_sun(normalized_height);
    /* 2nd order scattering from the ground. */
    const float4 L_ground = PHASE_ISOTROPIC * omega * (GROUND_ALBEDO * M_1_PI_F) * T_to_ground *
                            T_ground_to_sample * cos_theta;
    /* Earth's multiple scattering coming from other points in the atmosphere. */
    const float4 L_ms = lookup_atmospheric_multiscattering(cos_theta, normalized_height);
    return L_ms + L_ground;
  }

  /* Look up the atmosphere's multiple scattering from the precomputed LUT. */
  float4 lookup_atmospheric_multiscattering(const float cos_theta,
                                            const float normalized_altitude) const
  {
    const float u = saturate(cos_theta * 0.5f + 0.5f);
    const float v = saturate(normalized_altitude);
    const float x = float(MULTISCATTERING_RES - 1) * u;
    const float y = float(MULTISCATTERING_RES - 1) * v;
    const int x1 = int(x);
    const int y1 = int(y);
    const int x2 = min(x1 + 1, MULTISCATTERING_RES - 1);
    const int y2 = min(y1 + 1, MULTISCATTERING_RES - 1);
    const float fx = x - x1;
    const float fy = y - y1;
    const float4 bottom = mix(multiscattering_lut[y1][x1], multiscattering_lut[y1][x2], fx);
    const float4 top = mix(multiscattering_lut[y2][x1], multiscattering_lut[y2][x2], fx);
    return mix(bottom, top, fy);
  }

  /* Look up a transmittance from the precomputed LUT. */
  float4 lookup_transmittance(const float cos_theta, const float normalized_altitude) const
  {
    const float u = saturate(cos_theta * 0.5f + 0.5f);
    const float v = saturate(normalized_altitude);
    const float x = float(TRANSMITTANCE_RES_X - 1) * u;
    const float y = float(TRANSMITTANCE_RES_Y - 1) * v;
    const int x1 = int(x);
    const int y1 = int(y);
    const int x2 = min(x1 + 1, TRANSMITTANCE_RES_X - 1);
    const int y2 = min(y1 + 1, TRANSMITTANCE_RES_Y - 1);
    const float fx = x - x1;
    const float fy = y - y1;
    const float4 bottom = mix(transmittance_lut[y1][x1], transmittance_lut[y1][x2], fx);
    const float4 top = mix(transmittance_lut[y2][x1], transmittance_lut[y2][x2], fx);
    return mix(bottom, top, fy);
  }

  /* Specialized versions of lookup_transmittance that skip one interpolation. */
  float4 lookup_transmittance_at_ground(const float cos_theta) const
  {
    const float u = saturate(cos_theta * 0.5f + 0.5f);
    const float x = float(TRANSMITTANCE_RES_X - 1) * u;
    const int x1 = int(x);
    const int x2 = min(x1 + 1, TRANSMITTANCE_RES_X - 1);
    const int y = 0;
    const float fx = x - x1;
    return mix(transmittance_lut[y][x1], transmittance_lut[y][x2], fx);
  }

  float4 lookup_transmittance_to_sun(const float normalized_altitude) const
  {
    const float v = saturate(normalized_altitude);
    const float y = float(TRANSMITTANCE_RES_Y - 1) * v;
    const int x = TRANSMITTANCE_RES_X - 1;
    const int y1 = int(y);
    const int y2 = min(y1 + 1, TRANSMITTANCE_RES_Y - 1);
    const float fy = y - y1;
    return mix(transmittance_lut[y1][x], transmittance_lut[y2][x], fy);
  }
};

void SKY_hillaire_precompute_texture(float *pixels,
                                     int stride,
                                     int width,
                                     int height,
                                     float sun_elevation,
                                     float altitude,
                                     float air_density,
                                     float aerosol_density,
                                     float ozone_density,
                                     float aerosol_anisotropy,
                                     const float sun_color_transform[9])
{
  SkyHillaire sms(air_density, aerosol_density, ozone_density, aerosol_anisotropy);
  sms.precompute_lut();
  sms.precompute_multiscattering_lut();

  /* Clamp altitude to avoid numerical issues. */
  altitude = clamp(altitude, 1.0f, 99999.0f) / 1000.0f;
  const int half_width = width / 2;
  const float sun_zenith_cos_angle = cosf(M_PI_2_F - sun_elevation);
  const float3 sun_dir = sun_direction(sun_zenith_cos_angle);
  const int rows_per_task = std::max(1024 / width, 1);

  SKY_parallel_for(0, height, rows_per_task, [&](const size_t begin, const size_t end) {
    for (int y = begin; y < end; y++) {
      float *pixel_row = pixels + (y * width * stride);
      for (int x = 0; x < half_width; x++) {
        float2 uv = make_float2((x + 0.5f) / width, (y + 0.5f) / height);

        const float azimuth = M_2PI_F * uv.x;
        /* Apply a non-linear transformation to the elevation to dedicate more texels to the
         * horizon, where having more detail matters. */
        const float l = uv.y * 2.0f - 1.0f;
        /* [-pi/2, pi/2]. */
        const float elev = copysignf(sqr(l), l) * M_PI_2_F;
        const float3 ray_dir = make_float3(
            cosf(elev) * cosf(azimuth), cosf(elev) * sinf(azimuth), sinf(elev));
        const float3 ray_origin = make_float3(0.0f, 0.0f, EARTH_RADIUS + altitude);
        const float atmos_dist = ray_sphere_intersection(ray_origin, ray_dir, ATMOSPHERE_RADIUS);
        const float ground_dist = ray_sphere_intersection(ray_origin, ray_dir, EARTH_RADIUS);
        /* If no ground collision then use the distance to the outer atmosphere, else we have a
         * collision with the ground so we use the distance to it. */
        const float t_d = (ground_dist < 0.0f) ? atmos_dist : ground_dist;
        const float4 L = sms.get_inscattering(sun_dir, ray_origin, ray_dir, t_d);
        const float3 sky = apply_illuminant(spectral_to_xyz(L), sun_color_transform);

        /* Store pixels. */
        const int pos_x = x * stride;
        pixel_row[pos_x] = sky.x;
        pixel_row[pos_x + 1] = sky.y;
        pixel_row[pos_x + 2] = sky.z;
        /* Mirror pixels. */
        const int mirror_x = (width - x - 1) * stride;
        pixel_row[mirror_x] = sky.x;
        pixel_row[mirror_x + 1] = sky.y;
        pixel_row[mirror_x + 2] = sky.z;
      }
    }
  });
}

/* Oxygen and hydroxyl high in the atmosphere glow faintly on their own, with no sunlight involved
 * at all. It is what keeps a genuinely moonless night from being black, and it is brightest some
 * way above the horizon: the line of sight runs further through the layer as it flattens, until
 * the air underneath swallows more than the longer path gains. */
void SKY_airglow_color(float r_xyz[3])
{
  const float3 xyz = spectral_to_xyz(AIRGLOW_SPECTRAL_RADIANCE);
  r_xyz[0] = xyz.x;
  r_xyz[1] = xyz.y;
  r_xyz[2] = xyz.z;
}

void SKY_hillaire_add_airglow(float *pixels,
                              int stride,
                              int width,
                              int height,
                              float altitude,
                              float air_density,
                              float aerosol_density,
                              float ozone_density,
                              float scale,
                              const float color_to_rgb[9],
                              const float tinted_color_to_xyz[9])
{
  if (scale <= 0.0f) {
    return;
  }

  SkyHillaire sms(air_density, aerosol_density, ozone_density, G);
  sms.precompute_lut();

  /* Altitude arrives in meters, as everywhere else in this library, but the geometry here is in
   * kilometers. An observer above the layer would see it below rather than around them, which
   * this does not model, so keep them underneath it. */
  const float altitude_km = clamp(altitude, 1.0f, 99999.0f) / 1000.0f;
  const float observer_altitude = fminf(altitude_km, AIRGLOW_LAYER_ALTITUDE - 1.0f);
  const float observer_radius = EARTH_RADIUS + observer_altitude;
  const float radius_ratio = observer_radius / (EARTH_RADIUS + AIRGLOW_LAYER_ALTITUDE);
  const float normalized_altitude = fmaxf(observer_altitude, 0.0f) / ATMOSPHERE_THICKNESS;

  SKY_parallel_for(
      0, height, std::max(1024 / width, 1), [&](const size_t begin, const size_t end) {
        for (size_t y = begin; y < end; y++) {
          /* Same non-linear elevation axis the sky itself is baked on. */
          const float l = ((y + 0.5f) / height) * 2.0f - 1.0f;
          const float elevation = copysignf(sqr(l), l) * M_PI_2_F;
          const float cos_elevation = cosf(elevation);
          const float sin_elevation = sinf(elevation);

          /* Below the horizon the ground stands in the way of the layer. */
          const float3 ray_origin = make_float3(0.0f, 0.0f, observer_radius);
          const float3 ray_dir = make_float3(cos_elevation, 0.0f, sin_elevation);
          if (ray_sphere_intersection(ray_origin, ray_dir, EARTH_RADIUS) >= 0.0f) {
            continue;
          }

          /* Van Rhijn's function: how much further the line of sight runs through a thin
           * shell than it would straight up. About six times as far at the horizon. */
          const float denominator = 1.0f - sqr(radius_ratio * cos_elevation);
          const float path_gain = 1.0f / sqrtf(fmaxf(denominator, 1.0e-6f));
          /* The layer sits just under the top of the atmosphere, so the transmittance out to space
           * is a good stand-in for the trip down from it, and there is almost no air above it to
           * matter. */
          const float4 transmittance = sms.get_transmittance(sin_elevation, normalized_altitude);
          const float3 xyz = apply_emission(
              spectral_to_xyz(AIRGLOW_SPECTRAL_RADIANCE * (path_gain * scale) * transmittance),
              color_to_rgb,
              tinted_color_to_xyz);

          float *row = pixels + y * size_t(width) * size_t(stride);
          for (int x = 0; x < width; x++) {
            row[x * stride] += xyz.x;
            row[x * stride + 1] += xyz.y;
            row[x * stride + 2] += xyz.z;
          }
        }
      });
}

/* Rec.709 luminance. This works on the render's RGB rather than on the spectral values the sky
 * models deal in, because saturation is a property of the displayed color. */
static float rgb_luminance(const float c[3])
{
  return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/* Where the hue ends up once the rods have taken over completely. Renormalized below, so only
 * its direction matters. */
static const float SCOTOPIC_TINT[3] = {0.78f, 0.97f, 1.45f};

void SKY_night_adaptation(const float color[3],
                          const float luminance,
                          const float strength,
                          float r_color[3])
{
  const float hue_luminance = rgb_luminance(color);
  if (strength <= 0.0f || hue_luminance <= 0.0f) {
    r_color[0] = color[0];
    r_color[1] = color[1];
    r_color[2] = color[2];
    return;
  }

  /* Below a few candela the cones stop keeping up and the rods, which carry no color at all,
   * take over. This fall-off was fit by matching images across display brightnesses in
   * "Simulating and compensating changes in appearance between day and night vision" by Robert
   * Wanat and Rafal K. Mantiuk (SIGGRAPH 2014). s runs from 1, where color is seen in full, to
   * 0, where none of it is left. */
  float s = luminance / (luminance + SKY_SCOTOPIC_HALF_LUMINANCE);
  s = 1.0f - clamp(strength, 0.0f, 1.0f) * (1.0f - s);
  s = clamp(s, 0.0f, 1.0f);

  /* Their saturation correction: divide out the brightness, pull the remaining color toward
   * neutral, put the brightness back. */
  float adapted[3];
  for (int i = 0; i < 3; i++) {
    adapted[i] = powf(fmaxf(color[i], 0.0f) / hue_luminance, s) * hue_luminance;
  }

  /* Rods and cones share the pathways to the visual cortex, so as the rod signal grows it mixes
   * into every cone channel and slides the hue toward blue. This is the Purkinje shift, and it
   * is why a night that is almost colorless still does not read as plain gray. */
  for (int i = 0; i < 3; i++) {
    adapted[i] *= mix(1.0f, SCOTOPIC_TINT[i], 1.0f - s);
  }

  /* Only the hue was supposed to move. Hand back the brightness that came in. */
  const float adapted_luminance = rgb_luminance(adapted);
  const float rescale = (adapted_luminance > 0.0f) ? hue_luminance / adapted_luminance : 1.0f;
  for (int i = 0; i < 3; i++) {
    r_color[i] = adapted[i] * rescale;
  }
}

void SKY_hillaire_precompute_sun(float sun_elevation,
                                 float angular_diameter,
                                 float altitude,
                                 float air_density,
                                 float aerosol_density,
                                 float ozone_density,
                                 float r_pixel_bottom[3],
                                 float r_pixel_top[3])
{
  /* The Sun disc is lit directly, so multiple scattering plays no part in its radiance. */
  const SkyHillaire sms(air_density, aerosol_density, ozone_density, G);

  /* Clamp altitude to avoid numerical issues. */
  altitude = clamp(altitude, 1.0f, 99999.0f) / 1000.0f;
  const float half_angular = angular_diameter / 2.0f;
  const float solid_angle = M_2PI_F * (1.0f - cosf(half_angular));
  const float normalized_altitude = altitude / ATMOSPHERE_THICKNESS;

  /* Compute 2 pixels for Sun disc: one is the lowest point of the disc, one is the highest. */
  auto get_sun_xyz = [&](const float elevation) {
    const float sun_zenith_cos_angle = cosf(M_PI_2_F - elevation);
    const float4 transmittance_to_sun = sms.get_transmittance(sun_zenith_cos_angle,
                                                              normalized_altitude);
    const float4 spectrum = SUN_SPECTRAL_IRRADIANCE * transmittance_to_sun / solid_angle;
    return spectral_to_xyz(spectrum);
  };
  const float3 bottom = get_sun_xyz(sun_elevation - half_angular);
  const float3 top = get_sun_xyz(sun_elevation + half_angular);

  /* Store pixels */
  r_pixel_bottom[0] = bottom.x;
  r_pixel_bottom[1] = bottom.y;
  r_pixel_bottom[2] = bottom.z;
  r_pixel_top[0] = top.x;
  r_pixel_top[1] = top.y;
  r_pixel_top[2] = top.z;
}
