/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 * SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/image.h"

#include "kernel/svm/node_types.h"
#include "kernel/svm/types.h"
#include "kernel/svm/util.h"

#include "kernel/util/colorspace.h"
#include "kernel/util/differential.h"

#include "util/color.h"
#include "util/defines.h"

CCL_NAMESPACE_BEGIN

/* Sky texture */

ccl_device float sky_angle_between(const float thetav,
                                   const float phiv,
                                   const float theta,
                                   const float phi)
{
  const float cospsi = sinf(thetav) * sinf(theta) * cosf(phi - phiv) + cosf(thetav) * cosf(theta);
  return safe_acosf(cospsi);
}

/*
 * "A Practical Analytic Model for Daylight"
 * A. J. Preetham, Peter Shirley, Brian Smits
 */
ccl_device float sky_perez_function(const ccl_private float *lam,
                                    const float theta,
                                    const float gamma)
{
  const float ctheta = cosf(theta);
  const float cgamma = cosf(gamma);

  return (1.0f + lam[0] * expf(lam[1] / ctheta)) *
         (1.0f + lam[2] * expf(lam[3] * gamma) + lam[4] * cgamma * cgamma);
}

ccl_device float3 sky_radiance_preetham(KernelGlobals kg,
                                        const float3 dir,
                                        const float sunphi,
                                        const float suntheta,
                                        const float radiance_x,
                                        const float radiance_y,
                                        const float radiance_z,
                                        ccl_private float *config_x,
                                        ccl_private float *config_y,
                                        ccl_private float *config_z)
{
  /* convert vector to spherical coordinates */
  const float2 spherical = direction_to_spherical(dir);
  float theta = spherical.x;
  const float phi = -spherical.y + M_PI_2_F;

  /* angle between sun direction and dir */
  const float gamma = sky_angle_between(theta, phi, suntheta, sunphi);

  /* clamp theta to horizon */
  theta = min(theta, M_PI_2_F - 0.001f);

  /* compute xyY color space values */
  const float x = radiance_y * sky_perez_function(config_y, theta, gamma);
  const float y = radiance_z * sky_perez_function(config_z, theta, gamma);
  const float Y = radiance_x * sky_perez_function(config_x, theta, gamma);

  /* convert to RGB */
  const float3 xyz = xyY_to_xyz(x, y, Y);
  return xyz_to_rgb_clamped(kg, xyz);
}

/*
 * "An Analytic Model for Full Spectral Sky-Dome Radiance"
 * Lukas Hosek, Alexander Wilkie
 */
ccl_device float sky_radiance_internal(const ccl_private float *configuration,
                                       const float theta,
                                       const float gamma)
{
  const float ctheta = cosf(theta);
  const float cgamma = cosf(gamma);

  const float expM = expf(configuration[4] * gamma);
  const float rayM = cgamma * cgamma;
  const float mieM = (1.0f + rayM) / powf((1.0f + configuration[8] * configuration[8] -
                                           2.0f * configuration[8] * cgamma),
                                          1.5f);
  const float zenith = sqrtf(ctheta);

  return (1.0f + configuration[0] * expf(configuration[1] / (ctheta + 0.01f))) *
         (configuration[2] + configuration[3] * expM + configuration[5] * rayM +
          configuration[6] * mieM + configuration[7] * zenith);
}

ccl_device float3 sky_radiance_hosek(KernelGlobals kg,
                                     const float3 dir,
                                     const float sunphi,
                                     const float suntheta,
                                     const float radiance_x,
                                     const float radiance_y,
                                     const float radiance_z,
                                     ccl_private float *config_x,
                                     ccl_private float *config_y,
                                     ccl_private float *config_z)
{
  /* convert vector to spherical coordinates */
  const float2 spherical = direction_to_spherical(dir);
  float theta = spherical.x;
  const float phi = -spherical.y + M_PI_2_F;

  /* angle between sun direction and dir */
  const float gamma = sky_angle_between(theta, phi, suntheta, sunphi);

  /* clamp theta to horizon */
  theta = min(theta, M_PI_2_F - 0.001f);

  /* compute xyz color space values */
  const float x = sky_radiance_internal(config_x, theta, gamma) * radiance_x;
  const float y = sky_radiance_internal(config_y, theta, gamma) * radiance_y;
  const float z = sky_radiance_internal(config_z, theta, gamma) * radiance_z;

  /* convert to RGB and adjust strength */
  return xyz_to_rgb_clamped(kg, make_float3(x, y, z)) * (M_2PI_F / 683);
}

/* Nishita improved sky model */
ccl_device float3 geographical_to_direction(const float lat, const float lon)
{
  return spherical_to_direction(lat - M_PI_2_F, lon - M_PI_2_F);
}

/* Stars.
 *
 * Cube-face grid: the sky is divided into cells on the six faces of a cube, one star per cell.
 * Cube-face coordinates avoid trigonometry between the view direction and the hash, so the star
 * field is bit-identical on every device. Each star is kept inside its own cell, so a lookup only
 * ever needs that cell and face seams cost nothing. */
#define SKY_STAR_GRID 256.0f
/* Surface brightness of the band where it is thickest, lifted above the physical level by the
 * same reasoning as the stars and moonlight. */
#define SKY_MILKY_WAY_RADIANCE 2.4e-3f
/* Surface brightness of the zodiacal light, scaled so that near the Sun it stands with the Milky
 * Way's band, which is what it does at a dark site, and the gegenschein opposite lands about a
 * tenth of that. Lifted above the physical level by the same reasoning as the stars and the
 * moonlight. */
#define SKY_ZODIACAL_RADIANCE 2.0e-4f
/* Lattice size of the band's dust and cloud structure, in cycles across the sky. Six puts the
 * coarse cell near ten degrees and the fine one near five, against a band ten to thirty wide. */
#define SKY_NEBULA_SCALE 6.0f

ccl_device float3 sky_star_direction(const int face, const float2 uv)
{
  /* Faces pair up: even is the positive side of an axis, odd the negative. */
  const float side = (face & 1) ? -1.0f : 1.0f;
  if (face < 2) {
    return normalize(make_float3(side, uv.x, uv.y));
  }
  if (face < 4) {
    return normalize(make_float3(uv.x, side, uv.y));
  }
  return normalize(make_float3(uv.x, uv.y, side));
}

/* Where a direction falls in the galaxy, given in celestial coordinates. Returns the sine of
 * galactic latitude (zero on the band, +/-1 at the galactic poles) and how far the direction
 * leans toward the galactic center in Sagittarius, which is much the brightest part of the
 * band. The two constants are the north galactic pole and the galactic center, written out in
 * equatorial coordinates. */
ccl_device float2 sky_galactic_coords(const float3 cel)
{
  const float3 pole = make_float3(-0.8678f, -0.1970f, 0.4560f);
  const float3 center = make_float3(-0.0549f, -0.8736f, -0.4841f);
  return make_float2(dot(cel, pole), dot(cel, center));
}

/* How much the band thickens the star field at this direction.
 *
 * The strength saturates rather than rising without limit. Left linear, turning the band up also
 * multiplies the share of cells holding a star, and past a few units every cell on the band has
 * one - so the band stops reading as a glow with stars in it and becomes a field of dots. The
 * crowding factor is separate from the band's brightness for the same reason. */
ccl_device float sky_milky_way_crowding(const float3 cel,
                                        const float milky_way,
                                        const float crowding)
{
  if (milky_way <= 0.0f || crowding <= 0.0f) {
    return 1.0f;
  }
  const float2 g = sky_galactic_coords(cel);
  /* Width of the crowded zone in sine latitude, about that of the band's own core. */
  const float spread = g.x / 0.30f;
  /* Toward the anticenter the field thins to roughly a third of its value at the center. */
  const float along = 0.35f + 0.65f * smoothstep(-0.6f, 0.9f, g.y);
  /* Saturating, so that however far the band is turned up the factor stops at two and a half. */
  const float strength = milky_way / (1.0f + 0.4f * milky_way);
  /* At the default crowding this peaks near three times the field off the band. */
  return 1.0f + 7.0f * crowding * strength * expf(-spread * spread) * along;
}

/* Value noise on the same integer hash all three backends share, so the field is identical
 * everywhere. Nothing but multiply, add and floor stands between the direction and the integer
 * lattice, which is the rule the star field is built on; and unlike the stars, this field is
 * continuous across a cell boundary, so a last-bit disagreement about which cell a ray is in
 * changes the result by nothing at all.
 *
 * The hash returns three independent numbers, so one lattice yields three uncorrelated fields for
 * the price of one. That is what makes dust, star clouds and emission affordable together. */
ccl_device float3 sky_value_noise(const float3 p)
{
  const float3 cell = floor(p);
  const float3 f = p - cell;
  /* Smoothstep fade, so the lattice does not show as creases. */
  const float3 w = f * f * (make_float3(3.0f, 3.0f, 3.0f) - 2.0f * f);
  const int3 i = make_int3(int(cell.x), int(cell.y), int(cell.z));

  const float3 c000 = hash_int3_to_float3(i + make_int3(0, 0, 0));
  const float3 c100 = hash_int3_to_float3(i + make_int3(1, 0, 0));
  const float3 c010 = hash_int3_to_float3(i + make_int3(0, 1, 0));
  const float3 c110 = hash_int3_to_float3(i + make_int3(1, 1, 0));
  const float3 c001 = hash_int3_to_float3(i + make_int3(0, 0, 1));
  const float3 c101 = hash_int3_to_float3(i + make_int3(1, 0, 1));
  const float3 c011 = hash_int3_to_float3(i + make_int3(0, 1, 1));
  const float3 c111 = hash_int3_to_float3(i + make_int3(1, 1, 1));

  const float3 x00 = mix(c000, c100, w.x);
  const float3 x10 = mix(c010, c110, w.x);
  const float3 x01 = mix(c001, c101, w.x);
  const float3 x11 = mix(c011, c111, w.x);
  return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z);
}

/* Dust, star clouds and emission along the band, from one lattice.
 *
 * What gives the Milky Way its shape in any photograph is dust taking light away, not stars piling
 * up: the Great Rift, the Coalsack and the Pipe are foreground clouds subtracting from the star
 * clouds behind them. So the dust term is thresholded rather than smooth - most of the band is
 * left alone and what remains reads as discrete clouds with edges, instead of an even haze.
 *
 * Returned as (extinction, cloud gain, emission) and used by the glow, the star brightness and the
 * star count together, so a dark cloud loses its glow AND its stars while a bright knot gains
 * both. Fed only into the glow it would look painted on.
 *
 * Two lattices, added by hand rather than looped as a fractal sum. The first cell spans about ten
 * degrees against a band ten to thirty wide, which is the scale the dust reads at; the second, at
 * twice the frequency and half the weight, breaks up the plain blobs the first leaves on its own.
 * A third would land near a pixel and only average the two that show back toward flat. */
ccl_device float3 sky_nebulosity(const float3 cel, const float structure, const float nebulae)
{
  const float3 p = cel * SKY_NEBULA_SCALE;
  const float3 n = (sky_value_noise(p) * 2.0f + sky_value_noise(p * 2.0f)) * (1.0f / 3.0f);
  /* The noise sits about a half. Cutting in just below that leaves roughly two thirds of the band
   * carrying some dust and only the upper tail carrying much, which is what gives the clouds edges
   * instead of an even haze. The scale sets how deep the thickest run - about one and a half
   * optical depths at full structure, a fifth of the light on average. */
  const float tau = structure * fmaxf(n.x - 0.45f, 0.0f) * 3.2f;
  /* Star clouds, taken either side of that same half so the band's mean brightness is unchanged
   * and only its variation grows: about a quarter either way at full structure. */
  const float clouds = 1.0f + 1.8f * structure * (n.y - 0.5f);
  /* Cut far higher and squared on top, so emission lands on a few percent of the band as isolated
   * knots rather than becoming a third layer of haze. */
  const float emission = nebulae * sqr(fmaxf(n.z - 0.75f, 0.0f)) * 6.0f;
  return make_float3(expf(-tau), fmaxf(clouds, 0.0f), emission);
}

/* Unresolved starlight along the band, plus the dust of the Great Rift cutting a dark lane
 * through the middle of it. */
ccl_device float3 sky_milky_way_glow(const float3 cel,
                                     const float milky_way,
                                     const float3 tint,
                                     const float3 nebulosity)
{
  if (milky_way <= 0.0f) {
    return zero_float3();
  }
  const float2 g = sky_galactic_coords(cel);
  const float sin_b = g.x;
  const float toward_center = g.y;
  /* Toward the anticenter the band falls to about a third of its brightness at the center. */
  const float along = 0.35f + 0.65f * smoothstep(-0.6f, 0.9f, toward_center);
  /* The band is thin toward the anticenter and swells toward the galactic center, where the line
   * of sight runs the length of the disc and through the bulge. A single width everywhere is most
   * of what makes it read as a smear rather than a galaxy. */
  const float width = mix(0.09f, 0.26f, smoothstep(-0.6f, 0.9f, toward_center));
  /* The bulge is a second, far wider mound at about half the core's brightness, raised only in
   * the last thirty degrees or so around the galactic center. */
  const float band = expf(-(sin_b / width) * (sin_b / width)) +
                     0.55f * expf(-(sin_b / 0.34f) * (sin_b / 0.34f)) *
                         smoothstep(0.72f, 0.99f, toward_center);
  /* The Great Rift: a narrow lane of foreground dust sitting just south of the plane, taking away
   * about half the light where it is thickest and fading out toward the anticenter, where the
   * real lane ends. */
  const float rift_offset = (sin_b + 0.035f) / 0.045f;
  const float rift = 1.0f - 0.55f * expf(-rift_offset * rift_offset) *
                                smoothstep(-0.2f, 0.8f, toward_center);
  /* The band's own color arrives already adapted, so it carries whatever the eye would still find
   * in it at this brightness, not its full photographic warmth. Dust takes light away and star
   * clouds add it; the emission patches sit on top, tinted warm, since a hydrogen region is red
   * where the band around it is not. */
  const float3 glow = tint * (SKY_MILKY_WAY_RADIANCE * milky_way * along * band * rift *
                              nebulosity.x * nebulosity.y);
  const float3 emission = make_float3(1.0f, 0.35f, 0.40f) *
                          (SKY_MILKY_WAY_RADIANCE * milky_way * band * nebulosity.z);
  return glow + emission;
}

/* One star per cell, or none. Density decides how many cells hold a visible star, and brightness
 * how strongly the survivors shine: letting one number do both gives either a bare sky or an even
 * speckle, neither of which reads as stars. A star smaller than a pixel is widened to the ray's
 * own footprint and dimmed to match, so that it keeps its light instead of flickering as the
 * camera moves. */
ccl_device float3 sky_stars(const float3 cel,
                            const float density,
                            const float brightness,
                            const float variation,
                            const float seed,
                            const float footprint,
                            const float3 color_cool,
                            const float3 color_hot,
                            const float milky_way,
                            const float milky_way_crowding,
                            const float3 nebulosity)
{
  const float3 a = fabs(cel);
  int face;
  float2 uv;
  if (a.x >= a.y && a.x >= a.z) {
    face = (cel.x > 0.0f) ? 0 : 1;
    uv = make_float2(cel.y, cel.z) / a.x;
  }
  else if (a.y >= a.z) {
    face = (cel.y > 0.0f) ? 2 : 3;
    uv = make_float2(cel.x, cel.z) / a.y;
  }
  else {
    face = (cel.z > 0.0f) ? 4 : 5;
    uv = make_float2(cel.x, cel.y) / a.z;
  }

  const float2 grid_pos = (uv * 0.5f + make_float2(0.5f, 0.5f)) * SKY_STAR_GRID;
  const int cx = int(clamp(floorf(grid_pos.x), 0.0f, SKY_STAR_GRID - 1.0f));
  const int cy = int(clamp(floorf(grid_pos.y), 0.0f, SKY_STAR_GRID - 1.0f));
  /* One integer identifying the cell's face, with the seed folded in. The node bounds the seed so
   * that this multiply stays inside a signed 32-bit integer in every backend. */
  const int base = face + int(seed) * 6;

  /* Same hash used by all three shading backends, in signed integers for OSL compatibility, so
   * the star field matches everywhere. Two calls cover the four values needed here. */
  const float3 h0 = hash_int3_to_float3(make_int3(cx, cy, base));
  const float3 h1 = hash_int3_to_float3(make_int3(cx, cy, base + 97));
  const float pick = h0.x;

  /* Keep the star well inside its own cell. */
  const float2 star_grid = make_float2(float(cx), float(cy)) + make_float2(0.5f, 0.5f) +
                           (make_float2(h0.y, h0.z) - make_float2(0.5f, 0.5f)) * 0.6f;
  const float2 star_uv = (star_grid / SKY_STAR_GRID) * 2.0f - make_float2(1.0f, 1.0f);
  const float3 star_dir = sky_star_direction(face, star_uv);

  /* Only a small share of cells get a star. Count and brightness are chosen separately: one
   * number driving both would give either an empty sky or uniform speckle, not the mix of a few
   * bright stars over many faint ones a real sky has.
   *
   * The band is a crowd of stars as much as it is a glow, so it thickens the field here rather
   * than only adding light on top of it. Cells are equal area on the cube face but not on the
   * sphere: one near a face center covers about five times the sky one near a corner does, and
   * without the cube of the largest component undoing exactly that stretch the field clumps into
   * eight knots at the corners of the cube.
   *
   * Both of those are asked at the star's own direction, never at the ray's. They vary
   * continuously across a cell, so asking at the ray would let the same star exist for one sample
   * and not the next, and would put a transcendental between the hash and the decision, which is
   * what the field being identical on every device rests on not doing.
   *
   * The dust is the exception: it is taken at the ray, since the glow needs it there and asking
   * again at the star would double the cost of the whole field. Its lattice spans about ten
   * degrees, so across one pixel it moves by a fraction of a percent - far below the gap between
   * the hash values either side of the decision, save for a cell sitting right on it. */
  const float3 star_abs = fabs(star_dir);
  const float cell_max = fmaxf(star_abs.x, fmaxf(star_abs.y, star_abs.z));
  /* The same clouds that shape the glow shape the field: a dark cloud loses its stars as well as
   * its light, and a bright knot gains both. */
  const float crowding = sky_milky_way_crowding(star_dir, milky_way, milky_way_crowding) *
                         nebulosity.x * nebulosity.y;
  /* Two percent of cells at the default density. Over the whole grid that is near eight thousand
   * stars, about what the eye reaches on the darkest night. */
  const float share = clamp(0.02f * density * crowding, 0.0f, 1.0f) *
                      (cell_max * cell_max * cell_max);
  if (share <= 0.0f || pick < 1.0f - share) {
    return zero_float3();
  }
  /* Real star counts rise steeply toward the faint end, so bias survivors toward dim. A cube puts
   * the median star at an eighth of full brightness and leaves the bright few genuinely rare. */
  const float flux = powf((pick - (1.0f - share)) / share, 3.0f);
  if (flux < 1.0e-5f) {
    return zero_float3();
  }

  /* A star is far smaller than a pixel. Draw it at least pixel-sized and dim it to compensate, or
   * it would flicker in and out as the camera moves and the sample point drifts on and off it.
   *
   * The size does not follow brightness. A bright star looks bigger in a photograph because of the
   * camera - the core saturates and spills into its neighbors, and the lens spreads a fringe
   * around it - not because the star is bigger; every star is an unresolved point. That belongs to
   * the compositor's Glare, which produces it from brightness for free. Growing the disc here
   * would also push it past its own cell, and since a lookup only ever consults the cell the ray
   * falls in, the part that crossed the boundary would simply be cut off square. */
  const float radius = fmaxf(footprint * 0.7f, 1.0e-5f);
  const float dist = len(cel - star_dir);
  const float shape = 1.0f - smoothstep(0.0f, radius, dist);
  if (shape <= 0.0f) {
    return zero_float3();
  }

  /* Color comes from a temperature, the way the Sun's and the Moon's do. The two ends of the
   * ramp are blackbody colors worked out on the CPU, so all three backends agree even though
   * each has its own blackbody routine; the middle is the Sun's own temperature, which is white
   * by construction. The hash is spread evenly in reciprocal temperature, where the Planckian
   * locus is nearly straight, so interpolating along it stays close to a real blackbody. */
  const float warmth = h1.x;
  const float3 tint = (warmth < 0.5f) ? mix(color_cool, one_float3(), warmth * 2.0f) :
                                        mix(one_float3(), color_hot, (warmth - 0.5f) * 2.0f);
  /* Only the bright ones show their color. Sight is nearly achromatic at these levels, which the
   * night adaptation already applies to the sky as a whole; this is the same effect per star,
   * and without it a high variation reads as confetti. */
  const float3 color = mix(one_float3(), tint, clamp(variation, 0.0f, 1.0f) * sqrtf(flux));

  /* Only partly dimmed by the dust: about half the stars the eye can pick out are in front of
   * the band rather than behind it. */
  const float extinction = mix(1.0f, nebulosity.x, 0.6f);
  /* Radiance divides by disc area, so spreading a star over more pixels doesn't change the total
   * light it delivers. Boosted well above the physical level - a star's true brightness is some
   * five decades under a daytime sky, so a night scene would otherwise open black - by the same
   * factor as moonlight, so the two keep their relative brightness. */
  const float radiance = 3.0e-6f * flux * brightness * extinction / (radius * radius);
  return color * radiance * shape;
}

/* Direction of a body in the sky, in the frame used by direction_to_spherical(). */
ccl_device float3 sky_body_direction(const float elevation, const float rotation)
{
  const float theta = M_PI_2_F - elevation;
  const float phi = -rotation;
  return make_float3(sinf(theta) * sinf(phi), sinf(theta) * cosf(phi), cosf(theta));
}

/* True if the ray hits the disc of a body occluding the stars. Negative diameter means off. */
ccl_device bool sky_behind_body(const float3 view,
                                const float elevation,
                                const float rotation,
                                const float angular_diameter)
{
  if (angular_diameter <= 0.0f) {
    return false;
  }
  return dot(view, sky_body_direction(elevation, rotation)) > cosf(angular_diameter * 0.5f);
}

/* Hand-placed maria (the Moon's dark plains), in selenographic coordinates. Each entry is a unit
 * vector in the Moon's own frame (x toward the observer, y east, z north) plus an angular radius
 * in radians.
 *
 * The near-side maria are what makes the Moon recognizable, more than any crater, so a handful of
 * overlapping circles is enough - no texture needed. */
#define SKY_MARIA_COUNT 40
ccl_static_constexpr float sky_maria[SKY_MARIA_COUNT][4] = {
    {0.8062f, -0.2312f, 0.5446f, 0.3310f},  /* Mare Imbrium */
    {0.5826f, -0.6943f, 0.4226f, 0.2591f},  /* Oceanus Procellarum */
    {0.4924f, -0.8529f, 0.1736f, 0.2706f},  /* Oceanus Procellarum */
    {0.5736f, -0.8192f, 0.0000f, 0.2303f},  /* Oceanus Procellarum */
    {0.7002f, -0.7002f, -0.1392f, 0.2015f}, /* Oceanus Procellarum */
    {0.8444f, 0.2581f, 0.4695f, 0.2015f},   /* Mare Serenitatis */
    {0.8488f, 0.5100f, 0.1392f, 0.2504f},   /* Mare Tranquillitatis */
    {0.5821f, 0.8011f, -0.1392f, 0.2533f},  /* Mare Fecunditatis */
    {0.8008f, 0.5401f, -0.2588f, 0.0979f},  /* Mare Nectaris */
    {0.4925f, 0.8197f, 0.2924f, 0.1595f},   /* Mare Crisium */
    {0.7100f, -0.5749f, -0.4067f, 0.1220f}, /* Mare Humorum */
    {0.8928f, -0.2730f, -0.3584f, 0.2055f}, /* Mare Nubium */
    {0.9730f, 0.0510f, 0.2250f, 0.0702f},   /* Mare Vaporum */
    {0.5591f, 0.0098f, 0.8290f, 0.1382f},   /* Mare Frigoris */
    {0.4341f, -0.3039f, 0.8480f, 0.1151f},  /* Mare Frigoris */
    {0.5043f, 0.3531f, 0.7880f, 0.1094f},   /* Mare Frigoris */
    {0.9065f, -0.3848f, -0.1736f, 0.1007f}, /* Mare Cognitum */
    {0.6123f, -0.3752f, 0.6959f, 0.0679f},  /* Sinus Iridum */
    {0.8507f, -0.5091f, 0.1305f, 0.1474f},  /* Mare Insularum */
    {0.9704f, -0.1502f, 0.1891f, 0.0835f},  /* Sinus Aestuum */
    {0.9987f, 0.0296f, 0.0419f, 0.0823f},   /* Sinus Medii */
    {0.8738f, -0.4549f, -0.1719f, 0.0823f}, /* Palus Epidemiarum */
    {0.3287f, -0.4873f, 0.8090f, 0.1163f},  /* Sinus Roris */
    {0.6879f, 0.3844f, 0.6157f, 0.1203f},   /* Lacus Somniorum */
    {0.3503f, 0.8542f, 0.3843f, 0.0864f},   /* Mare Anguis */
    {0.3655f, 0.9232f, 0.1184f, 0.0697f},   /* Mare Undarum */
    {0.4210f, 0.9069f, 0.0192f, 0.0800f},   /* Mare Spumans */
    {0.0436f, 0.9988f, 0.0227f, 0.1077f},   /* Mare Smythii */
    {0.0662f, 0.9709f, 0.2300f, 0.1036f},   /* Mare Marginis */
    {0.3680f, -0.9249f, -0.0958f, 0.0495f}, /* Grimaldi */
    {0.2651f, -0.9625f, -0.0576f, 0.0443f}, /* Riccioli */
    {0.6130f, -0.1004f, 0.7837f, 0.0294f},  /* Plato */
    {0.8665f, -0.0606f, 0.4955f, 0.0230f},  /* Archimedes */
    {0.9866f, -0.0310f, -0.1599f, 0.0443f}, /* Ptolemaeus */
    {0.9716f, -0.0475f, -0.2317f, 0.0317f}, /* Alphonsus */
    {0.9522f, 0.2623f, 0.1564f, 0.0259f},   /* Julius Caesar */
    {0.3232f, 0.4977f, 0.8049f, 0.0357f},   /* Endymion */
    {0.7696f, 0.4552f, 0.4478f, 0.0173f},   /* Le Monnier */
    {0.6229f, -0.7450f, -0.2385f, 0.0132f}, /* Billy */
    {0.8780f, 0.4359f, -0.1977f, 0.0288f},  /* Theophilus */
};

/* How dark each plain is. Rows sharing a mare share a tone, so the four Procellarum circles and
 * the three of Frigoris do not show seams against themselves, and the ordering is the one the eye
 * gets from a photograph - Grimaldi, Plato and Billy nearly black, Crisium and Iridum noticeably
 * lighter than Imbrium - rather than measured values.
 *
 * The range is compressed against the measured albedos, which run 0.046 to 0.060 for the maria
 * against about 0.13 for the highlands: at the true spread the disc renders as paper and ink. The
 * mean is what matters downstream, and it is preserved. */
ccl_static_constexpr float sky_maria_tone[SKY_MARIA_COUNT] = {
    0.0795f, /* Mare Imbrium */
    0.0811f, /* Oceanus Procellarum */
    0.0811f, /* Oceanus Procellarum */
    0.0811f, /* Oceanus Procellarum */
    0.0811f, /* Oceanus Procellarum */
    0.0875f, /* Mare Serenitatis */
    0.0827f, /* Mare Tranquillitatis */
    0.0859f, /* Mare Fecunditatis */
    0.0843f, /* Mare Nectaris */
    0.0891f, /* Mare Crisium */
    0.0811f, /* Mare Humorum */
    0.0827f, /* Mare Nubium */
    0.0859f, /* Mare Vaporum */
    0.0875f, /* Mare Frigoris */
    0.0875f, /* Mare Frigoris */
    0.0875f, /* Mare Frigoris */
    0.0843f, /* Mare Cognitum */
    0.0907f, /* Sinus Iridum */
    0.0859f, /* Mare Insularum */
    0.0875f, /* Sinus Aestuum */
    0.0891f, /* Sinus Medii */
    0.0907f, /* Palus Epidemiarum */
    0.0891f, /* Sinus Roris */
    0.0859f, /* Lacus Somniorum */
    0.0907f, /* Mare Anguis */
    0.0891f, /* Mare Undarum */
    0.0891f, /* Mare Spumans */
    0.0859f, /* Mare Smythii */
    0.0875f, /* Mare Marginis */
    0.0763f, /* Grimaldi */
    0.0795f, /* Riccioli */
    0.0763f, /* Plato */
    0.0907f, /* Archimedes */
    0.0923f, /* Ptolemaeus */
    0.0907f, /* Alphonsus */
    0.0907f, /* Julius Caesar */
    0.0827f, /* Endymion */
    0.0859f, /* Le Monnier */
    0.0779f, /* Billy */
    0.0923f, /* Theophilus */
};

/* Young craters whose ejecta still lies bright on the surface, as a centre in the same
 * selenographic frame as the maria, and the angular reach of the ray system in radians. Reach is
 * the published extent of each system over a 1737 km radius. */
#define SKY_RAY_COUNT 8
ccl_static_constexpr float sky_ray_craters[SKY_RAY_COUNT][4] = {
    {0.7134f, -0.1438f, -0.6858f, 1.05f}, /* Tycho */
    {0.9259f, -0.3388f, 0.1668f, 0.46f},  /* Copernicus */
    {0.7801f, -0.6095f, 0.1409f, 0.30f},  /* Kepler */
    {0.6198f, -0.6740f, 0.4019f, 0.34f},  /* Aristarchus */
    {0.6577f, 0.7004f, 0.2773f, 0.28f},   /* Proclus */
    {0.2813f, -0.0501f, 0.9583f, 0.38f},  /* Anaxagoras */
    {0.4029f, -0.8151f, -0.4163f, 0.30f}, /* Byrgius A */
    {0.5279f, 0.6661f, -0.5270f, 0.30f},  /* Stevinus A */
};

/* How strongly each system shows, and how bright its own core is against the ray albedo. The
 * young craters are not interchangeable: Aristarchus is the brightest point on the Moon by a wide
 * margin, Proclus and Copernicus follow, and Anaxagoras is faint. */
ccl_static_constexpr float sky_ray_strength[SKY_RAY_COUNT] = {
    1.00f, 0.75f, 0.55f, 0.65f, 0.50f, 0.40f, 0.45f, 0.45f};
ccl_static_constexpr float sky_ray_bright[SKY_RAY_COUNT] = {
    1.15f, 1.10f, 1.05f, 1.55f, 1.30f, 0.95f, 1.00f, 1.05f};

/* Smooth minimum, for unioning the plains below. */
ccl_device float sky_smin(const float a, const float b, const float k)
{
  const float h = fmaxf(k - fabsf(a - b), 0.0f) / k;
  return fminf(a, b) - h * h * k * 0.25f;
}

/* Derivative of smoothstep, for the crater profile's slope. */
ccl_device float sky_dsmoothstep(const float a, const float b, const float x)
{
  const float t = clamp((x - a) / (b - a), 0.0f, 1.0f);
  return 6.0f * t * (1.0f - t) / (b - a);
}

/* Displace a direction on the Moon's surface by banded noise, so that a shoreline traced out of
 * circles does not read as circles. Amplitude falls with frequency, which is what lets it be
 * applied to every basin at once: the coarse octave carries a small crater bodily instead of
 * tearing it up, and only the fine octaves work the edges. The amplitude is in radians, directly
 * comparable to the radii in the table above. */
ccl_device float3 sky_moon_warp(const float3 body)
{
  float3 d = zero_float3();
  float amp = 0.05f;
  float freq = 6.0f;
  for (int i = 0; i < 3; i++) {
    d += amp * (sky_value_noise(body * freq) * 2.0f - one_float3());
    freq *= 2.17f;
    amp *= 0.5f;
  }
  return normalize(body + d);
}

/* Albedo variation across plains and highlands alike, or the face reads as two flat tones.
 *
 * A sum of smooth octaves is smooth however much amplitude it is given - turning it up only grows
 * the blotches. The Moon's albedo is not made that way: it is discrete events at every scale,
 * crater floors and ejecta aprons and basalt units with mapped boundaries, superposed. So each
 * octave is thresholded into sparse bright and sparse dark patches with hard edges at its own
 * scale, rather than summed raw, and the stack spans two decades instead of one.
 *
 * Six octaves is the ceiling worth paying for. They run 3, 7.2, 17.3, 41.5, 99.5, 239, and at a
 * five degree disc one body unit is the disc radius, so the last already has cells under a pixel.
 * A seventh is sub-pixel even for a Moon filling a twelve degree frame. */
ccl_device float sky_moon_patches(const float3 body)
{
  float3 q = body * 3.0f + make_float3(5.1f, 41.7f, -13.3f);
  float total = 0.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < 6; i++) {
    const float n = sky_value_noise(q).x;
    /* Thresholds sit close together so that most of each octave clears one of them and the
     * patches are dense; the amplitude is what keeps that from adding up to more contrast. */
    const float bright = smoothstep(0.475f, 0.585f, n);
    /* Written as a forward ramp subtracted from one, never as smoothstep(hi, lo, n). Reversed
     * edges are undefined in GLSL and Cycles resolves them to a step in the wrong direction, so
     * the reversed form silently disagrees between backends. */
    const float dark = 1.0f - smoothstep(0.415f, 0.525f, n);
    total += amp * (bright - 0.9f * dark);
    norm += amp;
    q *= 2.4f;
    amp *= 0.72f;
  }
  return total / norm;
}

/* Fine surface texture from the level sets of a noise field: a thin band either side of a level
 * crossing gives closed loops, and the region past it gives their interiors. The loops are not
 * circular - level sets of value noise are amoebas - so this reads as grain rather than as
 * craters, and the craters proper are the cellular field below. It is a zoom-only layer either
 * way; at the half degree the Moon really subtends it averages into a uniform lift. */
ccl_device float sky_moon_speckle(const float3 body)
{
  float3 q = body * 35.0f + make_float3(-3.9f, 27.1f, 61.4f);
  float total = 0.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < 3; i++) {
    const float n = sky_value_noise(q).x;
    const float rim = 1.0f - smoothstep(0.0f, 0.08f, fabsf(n - 0.52f));
    const float inside = smoothstep(0.52f, 0.87f, n);
    total += amp * (rim - 0.55f * inside);
    norm += amp;
    q *= 2.4f;
    amp *= 0.75f;
  }
  return total / norm;
}

/* Craterlets: one crater per lattice cell, jittered inside it by the cell's own hash, so they
 * come out circular by construction rather than as whatever shape a level set happens to take.
 * The hash returns three floats, which is exactly the jitter offset, and the radius is scrambled
 * out of those same three rather than costing a second lookup. Radii are drawn log-uniformly over
 * a nine-fold range, so small craters outnumber large ones and the field does not read as bubble
 * wrap; that range, not a second octave, is what buys the size variety.
 *
 * Twenty-seven cells have to be visited because the nearest centre can be in any neighbour. That
 * is 27 hashes, affordable only because none of it runs unless the ray hits the disc.
 *
 * Returned as (height gradient, albedo). The gradient is what lets the Sun shade them: crater
 * depth scales with crater radius, so the radius cancels out of dh/ds and one profile serves
 * every size with no lattice frequency in the answer. The constants put the inner walls near the
 * angle of repose, which is where real simple-crater walls sit. */
ccl_device float4 sky_moon_craters(const float3 body)
{
  const float3 q = body * 26.0f + make_float3(113.0f, -47.0f, 29.0f);
  const float3 base = floor(q);
  float best_x = 1.0e6f;
  float3 best_u = zero_float3();
  for (int dz = -1; dz <= 1; dz++) {
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        const float3 cell = base + make_float3(float(dx), float(dy), float(dz));
        const float3 h = hash_int3_to_float3(make_int3(int(cell.x), int(cell.y), int(cell.z)));
        const float3 delta = q - (cell + h);
        const float dist = len(delta);
        const float u = fractf(dot(h, make_float3(7.31f, 3.17f, 11.9f)));
        const float radius = 0.12f * powf(1.10f / 0.12f, u);
        const float x = dist / radius;
        if (x < best_x) {
          best_x = x;
          best_u = delta / fmaxf(dist, 1e-6f);
        }
      }
    }
  }
  const float x = best_x;
  const float rim = smoothstep(0.78f, 0.94f, x) * (1.0f - smoothstep(1.00f, 1.16f, x));
  const float bowl = 1.0f - smoothstep(0.55f, 0.86f, x);
  const float albedo = rim - 0.55f * bowl;

  const float dbowl = -sky_dsmoothstep(0.35f, 1.00f, x);
  const float drim = sky_dsmoothstep(0.78f, 0.94f, x) * (1.0f - smoothstep(1.00f, 1.16f, x)) -
                     smoothstep(0.78f, 0.94f, x) * sky_dsmoothstep(1.00f, 1.16f, x);
  const float dh_ds = -0.12f * dbowl + 0.06f * drim;

  float3 u = best_u - body * dot(best_u, body);
  u = u / fmaxf(len(u), 1e-6f);
  return make_float4(u.x * dh_ds, u.y * dh_ds, u.z * dh_ds, albedo);
}

/* Relief, felt only at the terminator.
 *
 * The terminator is the locus where the incidence cosine crosses zero, so adding a little signed
 * noise to that cosine alone shifts the locus sideways by about the amplitude in radians of arc
 * and does nothing anywhere else, where the cosine is of order one. No height field, no perturbed
 * normal, and no cost away from the one place relief shows on the real Moon.
 *
 * The amplitude is set against resolution rather than against lunar topography: it displaces the
 * terminator by roughly its own value in disc radii, which is under half a pixel at the half
 * degree the Moon really subtends and only becomes visible past a few degrees of sky. Real slopes
 * would be an order of magnitude larger and ragged at any size.
 *
 * Stretching the lattice along the Sun elongates each feature the way a low Sun draws out a
 * shadow - nothing is occluding anything here, but the dark side of a bump running away from the
 * light is the cue the eye reads as relief rather than as a noisy edge. The stretch relaxes toward
 * isotropic on the finer octaves; held constant it combs them into parallel fingers. */
ccl_device float sky_moon_relief(const float3 body, const float3 sun_body)
{
  float total = 0.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  float freq = 60.0f;
  float stretch = 2.2f;
  for (int i = 0; i < 4; i++) {
    const float3 p = body - sun_body * dot(body, sun_body) * (1.0f - 1.0f / stretch);
    total += amp * (sky_value_noise(p * freq + make_float3(-31.7f, 12.9f, 22.4f)).x - 0.5f);
    norm += amp;
    freq *= 2.11f;
    amp *= 0.5f;
    stretch = 1.0f + (stretch - 1.0f) * 0.35f;
  }
  return total / norm;
}

/* Ray systems, as (coverage, the albedo that coverage mixes toward).
 *
 * Two fields, each doing what its parameterisation is good at. A field in distance and azimuth
 * always converges on the crater, so on its own it can only read as a dot with a spider around
 * it; mixing a positional lattice into the same lookup buys a blob at the cost of the radial
 * structure everywhere else. So the continuous ejecta gets its own purely positional field,
 * confined to the inner part of the system, and the filaments are suppressed underneath it. The
 * blob's ragged edge is what hides their convergence, and no separate core is needed.
 *
 * The tangent direction carries azimuth with no polar seam and no trigonometry, and the three
 * hash channels give a filament its presence, its length and its tone from one lookup. Without
 * the length varying, every filament ends on a common circle and the system reads as a lens
 * flare; without brightness falling faster than coverage, a ray that widens with distance - as
 * any radial feature must - reads as a searchlight beam.
 *
 * The whole field is gated on phase because fresh ejecta has a stronger opposition effect than
 * mature regolith: a ray system does not merely stay visible at full, it gains on its
 * surroundings, and is swamped by relief contrast well before quarter. That gate is also what
 * lets the contrast at full be this high without the filaments reading as scratches elsewhere. */
ccl_device float2 sky_moon_rays(const float3 p, const float cos_phase)
{
  float cover = 0.0f;
  float target = 0.170f;
  for (int i = 0; i < SKY_RAY_COUNT; i++) {
    const float3 c = make_float3(
        sky_ray_craters[i][0], sky_ray_craters[i][1], sky_ray_craters[i][2]);
    const float reach = sky_ray_craters[i][3];
    const float cd = clamp(dot(p, c), -1.0f, 1.0f);
    const float d = safe_acosf(cd);
    /* Nothing of this system reaches here, and most points are in range of only one or two. */
    if (d > reach * 1.45f) {
      continue;
    }
    const float br = reach * 0.28f;
    const float bn = sky_value_noise(p * 30.0f + c * 7.0f).x;
    const float blob = 0.62f * smoothstep(0.26f, 0.58f, bn) *
                       (1.0f - smoothstep(br * 0.35f, br, d));

    float3 t = p - c * cd;
    t = t / fmaxf(len(t), 1e-6f);
    const float3 fine = sky_value_noise(t * 18.0f + c * d);
    const float tuft = sky_value_noise(t * 5.04f).x;
    const float v = fine.x * (0.45f + 0.55f * tuft);
    const float end = reach * (0.35f + 1.05f * fine.y);
    const float far = clamp(d / fmaxf(end, 1e-4f), 0.0f, 1.0f);
    /* Raising the threshold as a filament runs kills the weak ones first, so the system is dense
     * at the crater and sparse at its edge instead of dissolving all at once. */
    const float th = 0.34f + 0.54f * powf(far, 0.7f);
    float rays = 0.50f * smoothstep(th - 0.11f, th + 0.11f, v) * powf(1.0f - far, 1.8f);
    rays *= smoothstep(0.0f, br * 0.9f, d);

    const float s = sky_ray_strength[i] * fmaxf(blob, rays);
    /* The strongest system here sets the tone, not whichever comes last in the table. */
    if (s > cover) {
      cover = s;
      target = 0.170f * sky_ray_bright[i];
    }
  }
  cover *= 0.35f + 0.65f * powf(fmaxf(cos_phase, 0.0f), 0.6f);
  return make_float2(clamp(cover, 0.0f, 1.0f), target);
}

/* Fraction of incoming light reflected back along the view ray, or zero outside the disc.
 * Multiply by the light reaching the Moon to get its radiance. */
ccl_device float3 sky_moon_reflectance(const float3 view,
                                       const float3 moon_dir,
                                       const float3 sun_dir,
                                       const float3 pole,
                                       const float angular_diameter)
{
  const float half_angular = angular_diameter * 0.5f;
  const float cos_sep = dot(view, moon_dir);
  if (cos_sep < cosf(half_angular)) {
    return zero_float3();
  }

  /* Frame centered on the Moon; distance is great enough to treat the disc projection as
   * orthographic. Lunar east is the leading limb as the Moon crosses the sky - the right side of
   * the disc when north is up.
   *
   * North is the celestial pole, not the observer's zenith. The Moon's axis points near the pole,
   * so its face turns as it crosses the sky - by as much as a right angle between rising and
   * setting at middle latitudes. Anchored to the zenith instead, the maria would hold the same
   * orientation all night, which is the one thing a moonrise makes obvious. */
  const float3 east_axis = cross(moon_dir, pole);
  /* At the zenith or nadir there's no well-defined east; any perpendicular direction works.
   * Without this fallback the cross product is zero and the whole face comes out NaN. */
  const float3 east = (len(east_axis) > 1e-6f) ? normalize(east_axis) :
                                                 make_float3(1.0f, 0.0f, 0.0f);
  const float3 north = cross(east, moon_dir);

  const float3 offset = view - moon_dir * cos_sep;
  const float2 uv = make_float2(dot(offset, east), dot(offset, north)) / sinf(half_angular);
  const float r2 = dot(uv, uv);
  if (r2 > 1.0f) {
    return zero_float3();
  }

  /* Outward normal of the lunar surface, pointing back toward the observer at the center. */
  const float3 N = uv.x * east + uv.y * north - sqrtf(fmaxf(1.0f - r2, 0.0f)) * moon_dir;

  /* Darker plains over brighter highlands, in the Moon's own frame. */
  const float3 body = make_float3(dot(N, -moon_dir), dot(N, east), dot(N, north));
  const float3 p = sky_moon_warp(body);
  const float patches = sky_moon_patches(body);

  /* Union of the plains, as a smooth minimum over the signed distance to each. Summing soft
   * falloffs cannot merge them: a lone basin has to clear the threshold by itself, so a circle
   * alone already clears it, overlap only tells in a thin annulus, and the joins come out as
   * lens-shaped bulges rather than one plain. */
  float sd = safe_acosf(dot(p, make_float3(sky_maria[0][0], sky_maria[0][1], sky_maria[0][2]))) -
             sky_maria[0][3];
  float nearest_rim = sd;
  float plain_tone = sky_maria_tone[0];
  for (int i = 1; i < SKY_MARIA_COUNT; i++) {
    const float3 center = make_float3(sky_maria[i][0], sky_maria[i][1], sky_maria[i][2]);
    const float rim = safe_acosf(dot(p, center)) - sky_maria[i][3];
    sd = sky_smin(sd, rim, 0.045f);
    if (rim < nearest_rim) {
      nearest_rim = rim;
      plain_tone = sky_maria_tone[i];
    }
  }
  /* A wide shoreline, broken by the same patch field the highlands carry. Widening it alone only
   * blurs the boundary; breaking it dissolves the shore into flooded embayments and stranded
   * highland, so that half the disc stops reading as one flat dark shape. */
  const float maria = 1.0f - smoothstep(-0.050f, 0.050f, sd + patches * 0.10f);
  /* Maria are young surfaces with almost no craters on them. Holding the craterlets and the
   * speckle out of the plains is most of what makes the disc read as two terrains. */
  const float highland = 1.0f - 0.85f * maria;

  /* The phase angle, from the two directions already in hand: the angle at the Moon between the
   * Sun and the viewer. Constant across a half-degree disc, but it costs nothing to take it here
   * rather than thread another value through three backends. */
  const float cos_phase = clamp(-dot(moon_dir, sun_dir), -1.0f, 1.0f);
  const float phase_angle = safe_acosf(cos_phase);
  const float phase_deg = phase_angle * (180.0f / M_PI_F);

  /* Plains over highlands. The spread between them is compressed against the measured albedos -
   * maria average 0.046 to 0.060 and highlands about 0.13 - because at the true spread the disc
   * renders as paper and ink. What is given up there is spent on the patch field, which leaves
   * the mean where it was, and the mean is what SKY_moonlight_scale() is fitted against. */
  float albedo = mix(0.130f, plain_tone, maria);
  albedo *= 1.0f + patches * 0.45f * (1.0f - 0.45f * maria);
  albedo *= 1.0f + sky_moon_speckle(body) * 0.34f * (1.0f - 0.75f * maria);
  const float4 crater = sky_moon_craters(body);
  albedo *= 1.0f + crater.w * 0.18f * highland;
  /* Bright ejecta is one material lying on two grounds. Mixing toward its own albedo rather than
   * scaling the ground makes it loud over a plain and quiet over highland by itself, which is how
   * the real systems behave. */
  const float2 rays = sky_moon_rays(p, cos_phase);
  albedo = mix(albedo, rays.y, rays.x);

  /* Lommel-Seeliger against Lambert, weighted by phase - McEwen's lunar-Lambert function, with
   * the cubic USGS ISIS uses. At opposition it is pure Lommel-Seeliger, and a full Moon really is
   * a flat disc with no limb darkening at all: incidence equals emission everywhere, the ratio
   * below is 1/2 across the whole face, and a cosine term there is what makes a rendered Moon
   * look like a shaded ball instead. But that flatness is a property of full Moon and not of the
   * surface - by quarter the same regolith is very nearly Lambertian, and L has fallen to 0.19.
   *
   * The cubic is a fit over a limited range and turns negative past 104 degrees, so a thin
   * crescent needs the clamp or the shading inverts. */
  const float lunar_lambert = clamp(1.0f - 0.019f * phase_deg + 0.000242f * phase_deg * phase_deg -
                                        0.00000146f * phase_deg * phase_deg * phase_deg,
                                    0.0f,
                                    1.0f);

  const float3 sun_body = make_float3(
      dot(sun_dir, -moon_dir), dot(sun_dir, east), dot(sun_dir, north));
  const float3 crater_grad = make_float3(crater.x, crater.y, crater.z);
  /* Craterlets are shaded by their own slope against the Sun. The gradient lies in the tangent
   * plane, so this dot product picks up only the Sun's tangential component by itself: the
   * shading vanishes where the Sun is overhead and is strongest where it grazes. A full Moon
   * therefore has flat craters at the centre of the disc and shaded ones toward the limb, and a
   * quarter Moon has them shaded along the terminator, with no phase term written here. */
  const float shaded = dot(N, sun_dir) +
                       sky_moon_relief(body, sun_body) * 0.040f * (1.0f - 0.6f * maria) -
                       dot(crater_grad, sun_body) * highland * 0.22f;
  /* Earthshine needs the signed value - it lights the face the Sun does not - so the clamp for
   * the sunlit term is kept separate. */
  const float mu0 = fmaxf(shaded, 0.0f);
  const float mu = dot(N, -view);
  float lit = (mu > 0.0f) ? lunar_lambert * mu0 / fmaxf(mu0 + mu, 1e-9f) +
                                (1.0f - lunar_lambert) * mu0 * 0.5f :
                            0.0f;
  /* L redistributes brightness across the disc; it must not change how much the disc emits in
   * total, or the Moon would dim through the lunation while the glow it casts did not. This undoes
   * the shift, measured off the disc integral and exact at full, where everything downstream is
   * calibrated. */
  lit *= 1.0f + 8.7373e-3f * phase_deg - 1.8246e-4f * phase_deg * phase_deg +
         1.7312e-6f * phase_deg * phase_deg * phase_deg;

  /* Opposition surge. Within a few degrees of full, regolith brightens sharply beyond what
   * Lommel-Seeliger gives: the shadows between grains hide behind the grains themselves, and
   * backscatter adds to it. This is why a full Moon is startlingly brighter than a gibbous one
   * rather than proportionally so. Hapke's form, with the shadow-hiding amplitude and width he
   * fits for the Moon.
   *
   * The disc only. The glow the Moon puts into the sky is scaled by SKY_moonlight_scale(), whose
   * exponential fit against phase already absorbs the surge; applying it in both places would
   * count it twice. */
  const float surge = 1.0f + 0.6f / (1.0f + tanf(phase_angle * 0.5f) / 0.06f);

  /* Earthshine: sunlight reflected off Earth faintly lights the Moon's night side, tinted blue.
   * Brightest at new Moon, when the Earth seen from there is full, and gone at full Moon, when it
   * is new. That is what puts the old moon in the new moon's arms. */
  const float earth_phase = (1.0f - cos_phase) * 0.5f;
  /* Weighted to blue because Earth is, and small: under a tenth of the sunlit face at its best. */
  const float3 earthshine = albedo * make_float3(0.020f, 0.026f, 0.044f) * earth_phase *
                            fmaxf(-shaded, 0.0f);

  /* Fade the very edge to keep the silhouette from aliasing. */
  const float edge = 1.0f - smoothstep(0.98f, 1.0f, r2);
  const float sunlit = albedo * lit * surge;
  return (make_float3(sunlit, sunlit, sunlit) + earthshine) * edge;
}

ccl_device float3 sky_radiance_nishita(KernelGlobals kg,
                                       ccl_private ShaderData *sd,
                                       const float3 dir,
                                       const uint32_t path_flag,
                                       const PathRayVisibility path_visibility,
                                       const float3 pixel_bottom,
                                       const float3 pixel_top,
                                       const ccl_private float *sky_data,
                                       const ccl_private float *moon_data,
                                       const float3 moon_color,
                                       const float3 sun_color,
                                       const float3 moon_sun_dir,
                                       const float3 moon_illuminant,
                                       const ccl_private float *star_data,
                                       const float3 zenith_transmittance,
                                       const ccl_private float *celestial,
                                       const float3 star_color_cool,
                                       const float3 star_color_hot,
                                       const float milky_way,
                                       const float milky_way_crowding,
                                       const float milky_way_structure,
                                       const float nebulae,
                                       const float3 milky_way_tint,
                                       const float zodiacal_light,
                                       const float3 zodiacal_tint,
                                       const uint texture_id)
{
  /* Definitions */
  const float sun_elevation = sky_data[0];
  const float sun_rotation = sky_data[1];
  const float angular_diameter = sky_data[2];
  const float sun_intensity = sky_data[3];
  const float earth_intersection_angle = sky_data[4];
  const bool sun_disc = (angular_diameter >= 0.0f);
  float3 xyz = zero_float3();
  float3 sun_disc_xyz = zero_float3();
  /* Normalized once. The Vector socket carries whatever a Mapping node put on it, and every
   * angular test below compares a dot product against a cosine, which only means anything for a
   * unit vector: unnormalized, a scaled Vector makes the eclipse test true for the whole sky or
   * for none of it. */
  const float3 view = normalize(dir);
  const float2 direction = direction_to_spherical(dir);
  const float3 sun_dir = spherical_to_direction(sun_elevation - M_PI_2_F, sun_rotation - M_PI_2_F);
  const float sun_dir_angle = precise_angle(dir, sun_dir);
  const float half_angular = angular_diameter * 0.5f;
  const float dir_elevation = M_PI_2_F - direction.x;

  /* If the ray is inside the Sun disc, render it, otherwise render the sky.
   * Alternatively, ignore the Sun if we're evaluating the background texture. */
  /* The Moon occludes the Sun when it's nearer along this ray - an eclipse. What's left of the
   * Sun disc keeps full brightness; the sky dims by the covered fraction, computed from both
   * angular diameters before baking. */
  const bool eclipsed = sky_behind_body(view, moon_data[0], moon_data[1], moon_data[2]);
  if (sun_disc && !eclipsed && sun_dir_angle < half_angular &&
      dir_elevation > earth_intersection_angle &&
      !((path_flag & PATH_RAY_IMPORTANCE_BAKE) && kernel_data.background.use_sun_guiding))
  {
    /* Sun interpolation */
    const float y = ((dir_elevation - sun_elevation) / angular_diameter) + 0.5f;
    /* Limb darkening, coefficient is 0.6f */
    const float limb_darkening = (1.0f -
                                  0.6f * (1.0f - sqrtf(1.0f - sqr(sun_dir_angle / half_angular))));
    sun_disc_xyz = mix(pixel_bottom, pixel_top, y) * sun_intensity * limb_darkening;
  }

  /* Sky */
  const float x = fractf((-direction.y - M_PI_2_F + sun_rotation) * M_1_2PI_F);
  /* Undo the non-linear transformation from the sky LUT */
  const float y = copysignf(sqrtf(fabsf(dir_elevation) * M_2_PI_F), dir_elevation) * 0.5f + 0.5f;
  xyz += make_float3(kernel_image_interp(kg, sd, texture_id, dual2(make_float2(x, y))));

  /* Convert to RGB. The Sun's and the Moon's colors are already baked into the sky texture. The
   * disc is converted separately so that it can carry the Sun's tint on top of the atmospheric
   * reddening it has already picked up near the horizon - but the two are summed and clamped
   * together, as one total, rather than clamped individually first. Clamping drops negative
   * channels, which a saturated sky commonly has, and doing that to the sky and the disc
   * separately throws away a cancellation that a joint clamp would have kept, leaving the sum
   * brighter than it should be. */
  float3 rgb = xyz_to_rgb(kg, xyz) + xyz_to_rgb(kg, sun_disc_xyz) * sun_color;

  /* Negative angular diameter means the Moon is off. */
  const float moon_angular_diameter = moon_data[2];
  /* Visible to camera and glossy rays, not diffuse. The Moon's light already reaches the scene
   * through the moonlit sky around it; gathering a half-degree, far-brighter-than-its-surroundings
   * disc into diffuse light would just add noise. A Moon reflected in a lake is fine - smeared
   * into ambient light is not.
   *
   * The Sun's disc stays visible to every ray type: Cycles builds an importance map over the
   * background and can sample it properly. EEVEE can't, so its Sun disc is camera-only. */
  const uint visible_to = PATH_RAY_VISIBILITY_CAMERA | PATH_RAY_VISIBILITY_GLOSSY;
  if (moon_angular_diameter > 0.0f && (path_visibility & visible_to) &&
      dir_elevation > earth_intersection_angle)
  {
    const float3 moon_dir = sky_body_direction(moon_data[0], moon_data[1]);
    /* moon_sun_dir is the direction the Moon is lit from - the real Sun direction, or wherever a
     * manual phase implies. Precomputed on the CPU.
     *
     * Row three of the celestial frame is the pole in world coordinates. */
    const float3 pole = make_float3(celestial[6], celestial[7], celestial[8]);
    const float3 reflectance = sky_moon_reflectance(
        view, moon_dir, moon_sun_dir, pole, moon_angular_diameter);
    rgb += reflectance * xyz_to_rgb(kg, moon_illuminant) * moon_data[3] * moon_color;
  }

  /* Stars are never faded by hand - daylight is millions of times brighter, so it drowns them
   * out on its own. */
  if ((star_data[0] >= 0.0f || zodiacal_light > 0.0f) && (path_visibility & visible_to) &&
      dir_elevation > earth_intersection_angle)
  {
#ifdef __RAY_DIFFERENTIALS__
    /* Ray footprint at the sky. Camera rays carry their true spread; other rays are widened to
     * the importance map - exactly right for stars: sharp when seen directly, soft when
     * gathered. */
    const float footprint = sd->dP;
#else
    /* Never taken today - ray differentials are enabled for every device - but a zero footprint
     * would divide a star's radiance by a squared minimum radius and produce a firefly, so the
     * fallback is a plausible pixel rather than nothing. */
    const float footprint = 1.0e-3f;
#endif
    /* The Sun and Moon occlude stars behind their discs. */
    const bool hidden = sky_behind_body(view, moon_data[0], moon_data[1], moon_data[2]) ||
                        sky_behind_body(view, sun_elevation, sun_rotation, angular_diameter);
    /* Stars and the band are fixed to the celestial sphere, not to the world, so the view
     * direction is carried into that frame before either is looked up. The rotation is rigid, so
     * angles and the pixel footprint mean the same thing on both sides of it. */
    const float3 cel = make_float3(
        celestial[0] * view.x + celestial[1] * view.y + celestial[2] * view.z,
        celestial[3] * view.x + celestial[4] * view.y + celestial[5] * view.z,
        celestial[6] * view.x + celestial[7] * view.y + celestial[8] * view.z);
    /* Off the band there is nothing to shape, and the fractal is the most expensive thing in
     * this function, so it is only evaluated where it shows. */
    float3 nebulosity = make_float3(1.0f, 1.0f, 0.0f);
    if ((milky_way_structure > 0.0f || nebulae > 0.0f) && milky_way > 0.0f) {
      const float2 g = sky_galactic_coords(cel);
      if (fabsf(g.x) < 0.45f) {
        nebulosity = sky_nebulosity(cel, milky_way_structure, nebulae);
      }
    }
    float3 stars = zero_float3();
    if (star_data[0] >= 0.0f) {
      stars = hidden ? zero_float3() :
                       sky_stars(cel,
                                 star_data[0],
                                 star_data[1],
                                 star_data[2],
                                 star_data[3],
                                 footprint,
                                 star_color_cool,
                                 star_color_hot,
                                 milky_way,
                                 milky_way_crowding,
                                 nebulosity) +
                           sky_milky_way_glow(cel, milky_way, milky_way_tint, nebulosity);
    }

    /* Sunlight scattered by the dust of the solar system, spread along the ecliptic: a broad
     * wedge reaching up from where the Sun set, and a faint brightening directly opposite it.
     * At a dark site it stands with the Milky Way, and after twilight it is often the brightest
     * thing in the sky, so leaving it out is a visible omission rather than a small one.
     *
     * No date is modeled, so the ecliptic is taken as the plane through the Sun that leans as
     * close to the celestial pole as it can. The Sun lies on the ecliptic by definition, which
     * makes that exactly right whenever the sky's frame and the Sun agree, and sensible when
     * they do not. */
    if (zodiacal_light > 0.0f && !hidden) {
      const float3 pole = make_float3(celestial[6], celestial[7], celestial[8]);
      const float3 ecliptic = normalize(pole - dot(pole, sun_dir) * sun_dir);
      const float cos_elongation = dot(view, sun_dir);
      const float sin_latitude = dot(view, ecliptic);
      /* Brightest near the Sun and falling away steeply, held to a finite peak so that the band
       * does not blow up along the line of sight to the Sun itself. */
      const float wedge = 1.0f / (0.08f + (1.0f - cos_elongation));
      const float across = expf(-(sin_latitude / 0.30f) * (sin_latitude / 0.30f));
      /* The gegenschein: dust directly opposite the Sun is seen at full phase and brightens. */
      const float opposition = 1.0f + cos_elongation;
      const float gegenschein = 0.55f * expf(-(opposition / 0.09f) * (opposition / 0.09f));
      stars += zodiacal_tint *
               (SKY_ZODIACAL_RADIANCE * zodiacal_light * (wedge + gegenschein) * across);
    }

    /* A star near the horizon is seen through far more air than one overhead - the air mass,
     * approximately 1/sin(elevation) except at the very lowest angles. Transmittance falls off as
     * a power of air mass, so raising the zenith transmittance to this power gives the
     * transmittance along any other line of sight. */
    const float air_mass = clamp(1.0f / fmaxf(sinf(dir_elevation), 0.02f), 1.0f, 38.0f);
    const float3 transmittance = make_float3(powf(zenith_transmittance.x, air_mass),
                                             powf(zenith_transmittance.y, air_mass),
                                             powf(zenith_transmittance.z, air_mass));
    rgb += stars * transmittance;
  }

  /* Clamped here, after everything has been added, rather than around the sky and the Sun alone.
   * A saturated sky and a low, reddened Moon both fall outside the render's RGB and carry a
   * negative channel; clamping only part of the total lets the rest through, and clamping each
   * part on its own throws away cancellation that the sum would have kept. */
  return max(rgb, zero_float3());
}

ccl_device_noinline int svm_node_tex_sky(KernelGlobals kg,
                                         ccl_private ShaderData *sd,
                                         const PathRayVisibility path_visibility,
                                         const uint32_t path_flag,
                                         ccl_private float *ccl_restrict stack,
                                         const ccl_global SVMNodeTexSky &ccl_restrict node,
                                         int offset)
{
  /* Load data */
  const NodeSkyType sky_type = node.sky_type;

  const float3 dir = stack_load_float3(stack, node.dir_offset);
  float3 rgb;

  /* Preetham and Hosek share the same data */
  if (sky_type == NODE_SKY_PREETHAM || sky_type == NODE_SKY_HOSEK) {
    const ccl_global SVMNodeTexSkyPreethamData &preetham =
        *reinterpret_cast<const ccl_global SVMNodeTexSkyPreethamData *>(
            &kernel_data_fetch(svm_nodes, offset));
    offset += sizeof(SVMNodeTexSkyPreethamData) / sizeof(uint);

    /* Copy config arrays to private memory for GPU compatibility. */
    float config_x[9], config_y[9], config_z[9];
    for (int i = 0; i < 9; i++) {
      config_x[i] = preetham.config_x[i];
      config_y[i] = preetham.config_y[i];
      config_z[i] = preetham.config_z[i];
    }

    /* Compute Sky */
    if (sky_type == NODE_SKY_PREETHAM) {
      rgb = sky_radiance_preetham(kg,
                                  dir,
                                  preetham.phi,
                                  preetham.theta,
                                  preetham.radiance_x,
                                  preetham.radiance_y,
                                  preetham.radiance_z,
                                  config_x,
                                  config_y,
                                  config_z);
    }
    else {
      rgb = sky_radiance_hosek(kg,
                               dir,
                               preetham.phi,
                               preetham.theta,
                               preetham.radiance_x,
                               preetham.radiance_y,
                               preetham.radiance_z,
                               config_x,
                               config_y,
                               config_z);
    }
  }
  /* Nishita */
  else {
    const ccl_global SVMNodeTexSkyNishitaData &nishita =
        *reinterpret_cast<const ccl_global SVMNodeTexSkyNishitaData *>(
            &kernel_data_fetch(svm_nodes, offset));
    offset += sizeof(SVMNodeTexSkyNishitaData) / sizeof(uint);

    const float3 pixel_bottom = make_float3(
        nishita.pixel_bottom_x, nishita.pixel_bottom_y, nishita.pixel_bottom_z);
    const float3 pixel_top = make_float3(
        nishita.pixel_top_x, nishita.pixel_top_y, nishita.pixel_top_z);
    const float sky_data[5] = {nishita.sun_elevation,
                               nishita.sun_rotation,
                               nishita.angular_diameter,
                               nishita.sun_intensity,
                               nishita.earth_intersection_angle};

    const float moon_data[4] = {nishita.moon_elevation,
                                nishita.moon_rotation,
                                nishita.moon_angular_diameter,
                                nishita.moon_intensity};
    const float3 moon_color = make_float3(
        nishita.moon_color_x, nishita.moon_color_y, nishita.moon_color_z);
    const float3 sun_color = make_float3(
        nishita.sun_color_x, nishita.sun_color_y, nishita.sun_color_z);
    const float3 moon_sun_dir = make_float3(
        nishita.moon_sun_dir_x, nishita.moon_sun_dir_y, nishita.moon_sun_dir_z);
    const float3 moon_illuminant = make_float3(
        nishita.moon_illuminant_x, nishita.moon_illuminant_y, nishita.moon_illuminant_z);
    const float star_data[4] = {nishita.star_density,
                                nishita.star_brightness,
                                nishita.star_color_variation,
                                nishita.star_seed};
    const float3 zenith_transmittance = make_float3(nishita.zenith_transmittance_x,
                                                    nishita.zenith_transmittance_y,
                                                    nishita.zenith_transmittance_z);
    const float3 star_color_cool = make_float3(
        nishita.star_color_cool_x, nishita.star_color_cool_y, nishita.star_color_cool_z);
    const float3 star_color_hot = make_float3(
        nishita.star_color_hot_x, nishita.star_color_hot_y, nishita.star_color_hot_z);
    const float celestial[9] = {nishita.celestial_0,
                                nishita.celestial_1,
                                nishita.celestial_2,
                                nishita.celestial_3,
                                nishita.celestial_4,
                                nishita.celestial_5,
                                nishita.celestial_6,
                                nishita.celestial_7,
                                nishita.celestial_8};
    const float3 zodiacal_tint = make_float3(
        nishita.zodiacal_tint_x, nishita.zodiacal_tint_y, nishita.zodiacal_tint_z);
    const float3 milky_way_tint = make_float3(
        nishita.milky_way_tint_x, nishita.milky_way_tint_y, nishita.milky_way_tint_z);

    /* Compute Sky */
    rgb = sky_radiance_nishita(kg,
                               sd,
                               dir,
                               path_flag,
                               path_visibility,
                               pixel_bottom,
                               pixel_top,
                               sky_data,
                               moon_data,
                               moon_color,
                               sun_color,
                               moon_sun_dir,
                               moon_illuminant,
                               star_data,
                               zenith_transmittance,
                               celestial,
                               star_color_cool,
                               star_color_hot,
                               nishita.milky_way,
                               nishita.milky_way_crowding,
                               nishita.milky_way_structure,
                               nishita.nebulae,
                               milky_way_tint,
                               nishita.zodiacal_light,
                               zodiacal_tint,
                               nishita.texture_id);
  }

  stack_store_float3(stack, node.out_offset, rgb);
  return offset;
}

CCL_NAMESPACE_END
