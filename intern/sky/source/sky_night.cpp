/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_sky_modal
 */

/*
 * The night sky, as far as it can be worked out without an atmosphere: where a body sits, how far
 * one disc covers another, how the Moon is lit and what an eclipse takes away, and the frame the
 * stars are fixed to.
 *
 * None of it depends on a sky model, so it is shared by every one of them rather than being tied
 * to any. It is shared between Cycles and Blender for the same reason: pure geometry with no
 * engine-specific dependency, kept in one place rather than duplicated per shading backend,
 * unlike the render-time kernel code, which genuinely does need one implementation per shading
 * language.
 */

#include "sky_math.h"

#include "sky_night.h"

/* Share of the Sun's light a full Moon returns, lifted about five hundred times above the true
 * figure so that a moonlit night reads at an ordinary camera exposure. The stars are lifted by
 * the same factor, so what they are worth against each other is unchanged. */
static const float MOONLIGHT_FULL_SHARE = 1.25e-3f;
/* Fitted so that a half Moon lands near a tenth of a full one, which is what is measured. */
static const float MOONLIGHT_PHASE_FALLOFF = 1.6f;

/* Unit vector toward a body at the given elevation and rotation, in the frame the shaders use.
 * The rotation is negated on the way in, since it turns the sky rather than the body. */
static void body_direction(const float elevation, const float rotation, float r_direction[3])
{
  const float theta = M_PI_2_F - elevation;
  const float phi = -rotation;
  const float3 dir = make_float3(sinf(theta) * sinf(phi), sinf(theta) * cosf(phi), cosf(theta));
  r_direction[0] = dir.x;
  r_direction[1] = dir.y;
  r_direction[2] = dir.z;
}

void SKY_celestial_frame(const float latitude,
                         const float longitude,
                         const float rotation,
                         float r_matrix[9])
{
  /* The pole sits due north at an altitude equal to the latitude, and the sky turns about it.
   * Taking the first basis vector along the east-west axis keeps this well defined right up to
   * the poles, where a cross product against the zenith would collapse. */
  const float cos_lat = cosf(latitude);
  const float sin_lat = sinf(latitude);
  const float angle = longitude + rotation;
  const float cos_a = cosf(angle);
  const float sin_a = sinf(angle);

  r_matrix[0] = cos_a;
  r_matrix[1] = sin_a * sin_lat;
  r_matrix[2] = -sin_a * cos_lat;

  r_matrix[3] = -sin_a;
  r_matrix[4] = cos_a * sin_lat;
  r_matrix[5] = -cos_a * cos_lat;

  r_matrix[6] = 0.0f;
  r_matrix[7] = cos_lat;
  r_matrix[8] = sin_lat;
}

/* Fraction of one disc's area hidden behind another, from the angle between their centers and
 * their two diameters. The overlap of two circles, so it is one when the covering disc swallows
 * the other whole and zero once they no longer touch. */
static float disc_covered_fraction(const float separation,
                                   const float covered_diameter,
                                   const float covering_diameter)
{
  const float r = covered_diameter * 0.5f;
  const float R = covering_diameter * 0.5f;
  if (r <= 0.0f || R <= 0.0f || separation >= r + R) {
    return 0.0f;
  }
  if (separation <= R - r) {
    /* The covering disc swallows the other one whole. */
    return 1.0f;
  }
  if (separation <= r - R) {
    /* The covering disc sits entirely inside, so it hides its own area and no more. */
    return (R / r) * (R / r);
  }
  /* Two lenses meeting. Areas of the two circular segments either side of the chord. */
  const float d = separation;
  const float a = clamp((d * d + r * r - R * R) / (2.0f * d * r), -1.0f, 1.0f);
  const float b = clamp((d * d + R * R - r * r) / (2.0f * d * R), -1.0f, 1.0f);
  const float area = r * r * (acosf(a) - a * sqrtf(fmax(1.0f - a * a, 0.0f))) +
                     R * R * (acosf(b) - b * sqrtf(fmax(1.0f - b * b, 0.0f)));
  return clamp(area / (M_PI_F * r * r), 0.0f, 1.0f);
}

void SKY_eclipse(const bool use_moon,
                 const float sun_size,
                 const float moon_size,
                 const float sun_elevation,
                 const float sun_rotation,
                 const float moon_elevation,
                 const float moon_rotation,
                 float r_sun_light[3],
                 float r_moon_light[3])
{
  if (!use_moon) {
    return;
  }
  float sun_dir_arr[3], moon_dir_arr[3];
  body_direction(sun_elevation, sun_rotation, sun_dir_arr);
  body_direction(moon_elevation, moon_rotation, moon_dir_arr);
  const float3 sun_dir = make_float3(sun_dir_arr[0], sun_dir_arr[1], sun_dir_arr[2]);
  const float3 moon_dir = make_float3(moon_dir_arr[0], moon_dir_arr[1], moon_dir_arr[2]);
  float3 sun_light = make_float3(r_sun_light[0], r_sun_light[1], r_sun_light[2]);
  float3 moon_light = make_float3(r_moon_light[0], r_moon_light[1], r_moon_light[2]);

  /* Solar eclipse: the sky loses the same fraction of light as the Sun loses of its face. What's
   * left of the disc keeps full brightness - it's cut to a crescent by occlusion in the kernels,
   * not dimmed here. */
  const float separation = acosf(clamp(dot(sun_dir, moon_dir), -1.0f, 1.0f));
  sun_light = sun_light * (1.0f - disc_covered_fraction(separation, sun_size, moon_size));

  /* Lunar eclipse: Earth's shadow falls opposite the Sun and is far wider than the Moon at that
   * distance. Light that still arrives is bent through the whole ring of Earth's atmosphere,
   * losing its blue and leaving a dim copper instead of black. */
  const float antisolar = acosf(clamp(dot(-sun_dir, moon_dir), -1.0f, 1.0f));
  const float shadowed = disc_covered_fraction(antisolar, moon_size, 1.4f * M_PI_F / 180.0f);
  const float3 copper = make_float3(0.020f, 0.0056f, 0.0020f);
  moon_light = moon_light * (1.0f - shadowed) + moon_light * copper * shadowed;

  r_sun_light[0] = sun_light.x;
  r_sun_light[1] = sun_light.y;
  r_sun_light[2] = sun_light.z;
  r_moon_light[0] = moon_light.x;
  r_moon_light[1] = moon_light.y;
  r_moon_light[2] = moon_light.z;
}

void SKY_moon_illumination(const bool manual_phase,
                           const float moon_phase,
                           const float moon_elevation,
                           const float moon_rotation,
                           const float sun_elevation,
                           const float sun_rotation,
                           float r_direction[3],
                           float *r_phase_angle)
{
  float moon_dir_arr[3];
  body_direction(moon_elevation, moon_rotation, moon_dir_arr);
  const float3 moon_dir = make_float3(moon_dir_arr[0], moon_dir_arr[1], moon_dir_arr[2]);

  float3 direction;
  float phase_angle;
  if (manual_phase) {
    /* The lit fraction of a disc is (1 + cos(phase angle)) / 2, so this makes the phase mean
     * exactly what it says: 0 is new, 0.5 is half lit, 1 is full. */
    phase_angle = acosf(clamp(2.0f * moon_phase - 1.0f, -1.0f, 1.0f));
    float3 east = cross(moon_dir, make_float3(0.0f, 0.0f, 1.0f));
    /* A Moon at the zenith has no east to speak of, so any direction across it will do. */
    east = (len(east) > 1e-6f) ? east / len(east) : make_float3(1.0f, 0.0f, 0.0f);
    const float3 dir = -moon_dir * cosf(phase_angle) + east * sinf(phase_angle);
    direction = dir / len(dir);
  }
  else {
    float sun_dir_arr[3];
    body_direction(sun_elevation, sun_rotation, sun_dir_arr);
    direction = make_float3(sun_dir_arr[0], sun_dir_arr[1], sun_dir_arr[2]);
    phase_angle = acosf(clamp(dot(direction, -moon_dir), -1.0f, 1.0f));
  }

  r_direction[0] = direction.x;
  r_direction[1] = direction.y;
  r_direction[2] = direction.z;
  *r_phase_angle = phase_angle;
}

float SKY_moonlight_scale(const bool use_moon,
                          const float phase_angle,
                          const float moon_intensity,
                          const float night_exposure)
{
  if (!use_moon) {
    return -1.0f;
  }
  return MOONLIGHT_FULL_SHARE * expf(-MOONLIGHT_PHASE_FALLOFF * phase_angle) * moon_intensity *
         exp2f(night_exposure);
}
