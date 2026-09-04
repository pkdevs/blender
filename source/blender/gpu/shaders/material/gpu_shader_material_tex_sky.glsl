/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_hash.glsl"

float sky_angle_between(float thetav, float phiv, float theta, float phi)
{
  float cospsi = sin(thetav) * sin(theta) * cos(phi - phiv) + cos(thetav) * cos(theta);

  if (cospsi > 1.0f) {
    return 0.0f;
  }
  if (cospsi < -1.0f) {
    return M_PI;
  }

  return acos(cospsi);
}

float3 sky_spherical_coordinates(float3 dir)
{
  return float3(M_PI_2 - atan(dir.z, length(dir.xy)), atan(dir.x, dir.y), 0.0f);
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

float3 sky_star_direction(int face, float2 uv)
{
  /* Faces pair up: even is the positive side of an axis, odd the negative. */
  float side = ((face & 1) == 1) ? -1.0f : 1.0f;
  if (face < 2) {
    return normalize(float3(side, uv.x, uv.y));
  }
  if (face < 4) {
    return normalize(float3(uv.x, side, uv.y));
  }
  return normalize(float3(uv.x, uv.y, side));
}

/* Where a direction falls in the galaxy, given in celestial coordinates. Returns the sine of
 * galactic latitude (zero on the band, +/-1 at the galactic poles) and how far the direction
 * leans toward the galactic center in Sagittarius, which is much the brightest part of the
 * band. The two constants are the north galactic pole and the galactic center, written out in
 * equatorial coordinates. */
float2 sky_galactic_coords(float3 cel)
{
  float3 pole = float3(-0.8678f, -0.1970f, 0.4560f);
  float3 center = float3(-0.0549f, -0.8736f, -0.4841f);
  return float2(dot(cel, pole), dot(cel, center));
}

/* Unresolved starlight along the band, plus the dust of the Great Rift cutting a dark lane
 * through the middle of it. */
/* Value noise on the same integer hash all three backends share, so the field is identical
 * everywhere. Nothing but multiply, add and floor stands between the direction and the integer
 * lattice, and unlike the star field this one is continuous across a cell boundary, so a
 * last-bit disagreement about which cell a ray is in changes the result by nothing.
 *
 * The hash returns three independent numbers, so one lattice yields three uncorrelated fields for
 * the price of one. That is what makes dust, star clouds and emission affordable together. */
float3 sky_value_noise(float3 p)
{
  float3 cell = floor(p);
  float3 f = p - cell;
  float3 w = f * f * (float3(3.0f) - 2.0f * f);
  int3 i = int3(int(cell.x), int(cell.y), int(cell.z));

  float3 c000 = hash_int3_to_vec3(i + int3(0, 0, 0));
  float3 c100 = hash_int3_to_vec3(i + int3(1, 0, 0));
  float3 c010 = hash_int3_to_vec3(i + int3(0, 1, 0));
  float3 c110 = hash_int3_to_vec3(i + int3(1, 1, 0));
  float3 c001 = hash_int3_to_vec3(i + int3(0, 0, 1));
  float3 c101 = hash_int3_to_vec3(i + int3(1, 0, 1));
  float3 c011 = hash_int3_to_vec3(i + int3(0, 1, 1));
  float3 c111 = hash_int3_to_vec3(i + int3(1, 1, 1));

  float3 x00 = mix(c000, c100, w.x);
  float3 x10 = mix(c010, c110, w.x);
  float3 x01 = mix(c001, c101, w.x);
  float3 x11 = mix(c011, c111, w.x);
  return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z);
}

/* Dust, star clouds and emission along the band, from one lattice.
 *
 * What gives the Milky Way its shape in any photograph is dust taking light away, not stars piling
 * up: the Great Rift, the Coalsack and the Pipe are foreground clouds subtracting from the star
 * clouds behind them. The dust term is thresholded rather than smooth, so most of the band is left
 * alone and what remains reads as discrete clouds with edges instead of an even haze.
 *
 * Returned as (extinction, cloud gain, emission) and used by the glow, the star brightness and the
 * star count together, so a dark cloud loses its glow AND its stars. */
float3 sky_nebulosity(float3 cel, float structure, float nebulae)
{
  /* Two lattices, the second at twice the frequency and half the weight. One alone leaves plain
   * blobs on a ten-degree grid; a third would land near a pixel and only flatten the two. */
  float3 p = cel * SKY_NEBULA_SCALE;
  float3 n = (sky_value_noise(p) * 2.0f + sky_value_noise(p * 2.0f)) * (1.0f / 3.0f);
  float tau = structure * max(n.x - 0.45f, 0.0f) * 3.2f;
  float clouds = 1.0f + 1.8f * structure * (n.y - 0.5f);
  float emission = nebulae * pow(max(n.z - 0.75f, 0.0f), 2.0f) * 6.0f;
  return float3(exp(-tau), max(clouds, 0.0f), emission);
}

/* How much the band thickens the star field at this direction.
 *
 * The strength saturates rather than rising without limit. Left linear, turning the band up also
 * multiplies the share of cells holding a star, and past a few units every cell on the band has
 * one - so the band stops reading as a glow with stars in it and becomes a field of dots. The
 * crowding factor is separate from the band's brightness for the same reason. */
float sky_milky_way_crowding(float3 cel, float milky_way, float crowding)
{
  if (milky_way <= 0.0f || crowding <= 0.0f) {
    return 1.0f;
  }
  float2 g = sky_galactic_coords(cel);
  float spread = g.x / 0.30f;
  float along = 0.35f + 0.65f * smoothstep(-0.6f, 0.9f, g.y);
  float strength = milky_way / (1.0f + 0.4f * milky_way);
  return 1.0f + 7.0f * crowding * strength * exp(-spread * spread) * along;
}

float3 sky_milky_way_glow(float3 cel, float milky_way, float3 tint, float3 nebulosity)
{
  if (milky_way <= 0.0f) {
    return float3(0.0f);
  }
  float2 g = sky_galactic_coords(cel);
  float sin_b = g.x;
  float toward_center = g.y;
  float along = 0.35f + 0.65f * smoothstep(-0.6f, 0.9f, toward_center);
  /* The band is thin toward the anticenter and swells toward the galactic center, where the line
   * of sight runs the length of the disc and through the bulge. A single width everywhere is most
   * of what makes it read as a smear rather than a galaxy. */
  float width = mix(0.09f, 0.26f, smoothstep(-0.6f, 0.9f, toward_center));
  float band = exp(-(sin_b / width) * (sin_b / width)) +
               0.55f * exp(-(sin_b / 0.34f) * (sin_b / 0.34f)) *
                   smoothstep(0.72f, 0.99f, toward_center);
  float rift_offset = (sin_b + 0.035f) / 0.045f;
  float rift = 1.0f -
               0.55f * exp(-rift_offset * rift_offset) * smoothstep(-0.2f, 0.8f, toward_center);
  /* The band's own color arrives already adapted, so it carries whatever the eye would still find
   * in it at this brightness, not its full photographic warmth. Dust takes light away and star
   * clouds add it; the emission patches sit on top, tinted warm, since a hydrogen region is red
   * where the band around it is not. */
  float3 glow = tint * (SKY_MILKY_WAY_RADIANCE * milky_way * along * band * rift * nebulosity.x *
                        nebulosity.y);
  float3 emission = float3(1.0f, 0.35f, 0.40f) *
                    (SKY_MILKY_WAY_RADIANCE * milky_way * band * nebulosity.z);
  return glow + emission;
}

/* One star per cell, or none. Density decides how many cells hold a visible star, and brightness
 * how strongly the survivors shine: letting one number do both gives either a bare sky or an even
 * speckle, neither of which reads as stars. A star smaller than a pixel is widened to the ray's
 * own footprint and dimmed to match, so that it keeps its light instead of flickering as the
 * camera moves. */
float3 sky_stars(float3 cel,
                 float density,
                 float brightness,
                 float variation,
                 float seed,
                 float footprint,
                 float3 color_cool,
                 float3 color_hot,
                 float milky_way,
                 float milky_way_crowding,
                 float3 nebulosity)
{
  float3 a = abs(cel);
  int face;
  float2 uv;
  if (a.x >= a.y && a.x >= a.z) {
    face = (cel.x > 0.0f) ? 0 : 1;
    uv = float2(cel.y, cel.z) / a.x;
  }
  else if (a.y >= a.z) {
    face = (cel.y > 0.0f) ? 2 : 3;
    uv = float2(cel.x, cel.z) / a.y;
  }
  else {
    face = (cel.z > 0.0f) ? 4 : 5;
    uv = float2(cel.x, cel.y) / a.z;
  }

  float2 grid_pos = (uv * 0.5f + 0.5f) * SKY_STAR_GRID;
  int cx = int(clamp(floor(grid_pos.x), 0.0f, SKY_STAR_GRID - 1.0f));
  int cy = int(clamp(floor(grid_pos.y), 0.0f, SKY_STAR_GRID - 1.0f));
  /* One integer identifying the cell's face, with the seed folded in. The node bounds the seed so
   * that this multiply stays inside a signed 32-bit integer in every backend. */
  int base = face + int(seed) * 6;

  /* Same hash used by all three shading backends, in signed integers for OSL compatibility, so
   * the star field matches everywhere. Two calls cover the four values needed here. */
  float3 h0 = hash_int3_to_vec3(int3(cx, cy, base));
  float3 h1 = hash_int3_to_vec3(int3(cx, cy, base + 97));
  float pick = h0.x;
  float jitter_x = h0.y;
  float jitter_y = h0.z;
  float warmth = h1.x;

  /* Only a small share of cells get a star. Count and brightness are chosen separately: one
   * number driving both would give either an empty sky or uniform speckle, not the mix of a few
   * bright stars over many faint ones a real sky has. */
  /* The band is a crowd of stars as much as it is a glow, so it thickens the field here rather
   * than only adding light on top of it. */
  /* Keep the star well inside its own cell. */
  float2 star_grid = float2(float(cx), float(cy)) + 0.5f +
                     (float2(jitter_x, jitter_y) - 0.5f) * 0.6f;
  float2 star_uv = (star_grid / SKY_STAR_GRID) * 2.0f - 1.0f;
  float3 star_dir = sky_star_direction(face, star_uv);

  /* Cells are equal area on the cube face but not on the sphere: one near a face center covers
   * about five times the sky one near a corner does, and without the cube of the largest
   * component undoing exactly that stretch the field clumps into eight knots at the corners of
   * the cube. Both that and the band's crowding are asked at the star's own direction, never at
   * the ray's: they vary continuously across a cell, so asking at the ray would let the same star
   * exist for one sample and not the next, and would put a transcendental between the hash and
   * the decision, which is what the field being identical on every device rests on not doing.
   *
   * The dust is the exception: it is taken at the ray, since the glow needs it there and asking
   * again at the star would double the cost of the whole field. Its lattice spans about ten
   * degrees, so across one pixel it moves by a fraction of a percent - far below the gap between
   * the hash values either side of the decision, save for a cell sitting right on it. */
  float3 star_abs = abs(star_dir);
  float cell_max = max(star_abs.x, max(star_abs.y, star_abs.z));
  /* The same clouds that shape the glow shape the field: a dark cloud loses its stars as well as
   * its light, and a bright knot gains both. */
  float crowding = sky_milky_way_crowding(star_dir, milky_way, milky_way_crowding) * nebulosity.x *
                   nebulosity.y;
  float share = clamp(0.02f * density * crowding, 0.0f, 1.0f) * (cell_max * cell_max * cell_max);
  if (share <= 0.0f || pick < 1.0f - share) {
    return float3(0.0f);
  }
  /* Real star counts rise steeply toward the faint end, so bias survivors toward dim. */
  float flux = pow((pick - (1.0f - share)) / share, 3.0f);
  if (flux < 1.0e-5f) {
    return float3(0.0f);
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
  float radius = max(footprint * 0.7f, 1.0e-5f);
  float dist = length(cel - star_dir);
  float shape = 1.0f - smoothstep(0.0f, radius, dist);
  if (shape <= 0.0f) {
    return float3(0.0f);
  }

  /* Color comes from a temperature, the way the Sun's and the Moon's do. The two ends of the
   * ramp are blackbody colors worked out on the CPU, so all three backends agree even though
   * each has its own blackbody routine; the middle is the Sun's own temperature, which is white
   * by construction. Warmth is spread evenly in reciprocal temperature, where the Planckian
   * locus is nearly straight, so interpolating along it stays close to a real blackbody. */
  float3 tint = (warmth < 0.5f) ? mix(color_cool, float3(1.0f), warmth * 2.0f) :
                                  mix(float3(1.0f), color_hot, (warmth - 0.5f) * 2.0f);
  /* Only the bright ones show their color. Sight is nearly achromatic at these levels, which the
   * night adaptation already applies to the sky as a whole; this is the same effect per star,
   * and without it a high variation reads as confetti. */
  float3 color = mix(float3(1.0f), tint, clamp(variation, 0.0f, 1.0f) * sqrt(flux));

  /* Only partly dimmed by the dust: about half the stars the eye can pick out are in front of
   * the band rather than behind it. */
  float extinction = mix(1.0f, nebulosity.x, 0.6f);
  /* Radiance divides by disc area, so spreading a star over more pixels doesn't change the total
   * light it delivers. Boosted well above the physical level - a star's true brightness is some
   * five decades under a daytime sky, so a night scene would otherwise open black - by the same
   * factor as moonlight, so the two keep their relative brightness. */
  float radiance = 3.0e-6f * flux * brightness * extinction / (radius * radius);
  return color * radiance * shape;
}

/* Direction of a body in the sky, in the frame used by sky_spherical_coordinates(). */
float3 sky_body_direction(float elevation, float rotation)
{
  float theta = M_PI_2 - elevation;
  float phi = -rotation;
  return float3(sin(theta) * sin(phi), sin(theta) * cos(phi), cos(theta));
}

/* True if the ray hits the disc of a body occluding the stars. Negative diameter means off. */
bool sky_behind_body(float3 view, float elevation, float rotation, float angular_diameter)
{
  if (angular_diameter <= 0.0f) {
    return false;
  }
  return dot(view, sky_body_direction(elevation, rotation)) > cos(angular_diameter * 0.5f);
}

/* Hand-placed maria (the Moon's dark plains), in selenographic coordinates. Each entry is a unit
 * vector in the Moon's own frame (x toward the observer, y east, z north) plus an angular radius
 * in radians.
 *
 * The near-side maria are what makes the Moon recognizable, more than any crater, so a handful of
 * overlapping circles is enough - no texture needed. */
#define SKY_MARIA_COUNT 40
const float4 sky_maria[SKY_MARIA_COUNT] = float4[SKY_MARIA_COUNT](
    float4(0.8062f, -0.2312f, 0.5446f, 0.3310f),  /* Mare Imbrium */
    float4(0.5826f, -0.6943f, 0.4226f, 0.2591f),  /* Oceanus Procellarum */
    float4(0.4924f, -0.8529f, 0.1736f, 0.2706f),  /* Oceanus Procellarum */
    float4(0.5736f, -0.8192f, 0.0000f, 0.2303f),  /* Oceanus Procellarum */
    float4(0.7002f, -0.7002f, -0.1392f, 0.2015f), /* Oceanus Procellarum */
    float4(0.8444f, 0.2581f, 0.4695f, 0.2015f),   /* Mare Serenitatis */
    float4(0.8488f, 0.5100f, 0.1392f, 0.2504f),   /* Mare Tranquillitatis */
    float4(0.5821f, 0.8011f, -0.1392f, 0.2533f),  /* Mare Fecunditatis */
    float4(0.8008f, 0.5401f, -0.2588f, 0.0979f),  /* Mare Nectaris */
    float4(0.4925f, 0.8197f, 0.2924f, 0.1595f),   /* Mare Crisium */
    float4(0.7100f, -0.5749f, -0.4067f, 0.1220f), /* Mare Humorum */
    float4(0.8928f, -0.2730f, -0.3584f, 0.2055f), /* Mare Nubium */
    float4(0.9730f, 0.0510f, 0.2250f, 0.0702f),   /* Mare Vaporum */
    float4(0.5591f, 0.0098f, 0.8290f, 0.1382f),   /* Mare Frigoris */
    float4(0.4341f, -0.3039f, 0.8480f, 0.1151f),  /* Mare Frigoris */
    float4(0.5043f, 0.3531f, 0.7880f, 0.1094f),   /* Mare Frigoris */
    float4(0.9065f, -0.3848f, -0.1736f, 0.1007f), /* Mare Cognitum */
    float4(0.6123f, -0.3752f, 0.6959f, 0.0679f),  /* Sinus Iridum */
    float4(0.8507f, -0.5091f, 0.1305f, 0.1474f),  /* Mare Insularum */
    float4(0.9704f, -0.1502f, 0.1891f, 0.0835f),  /* Sinus Aestuum */
    float4(0.9987f, 0.0296f, 0.0419f, 0.0823f),   /* Sinus Medii */
    float4(0.8738f, -0.4549f, -0.1719f, 0.0823f), /* Palus Epidemiarum */
    float4(0.3287f, -0.4873f, 0.8090f, 0.1163f),  /* Sinus Roris */
    float4(0.6879f, 0.3844f, 0.6157f, 0.1203f),   /* Lacus Somniorum */
    float4(0.3503f, 0.8542f, 0.3843f, 0.0864f),   /* Mare Anguis */
    float4(0.3655f, 0.9232f, 0.1184f, 0.0697f),   /* Mare Undarum */
    float4(0.4210f, 0.9069f, 0.0192f, 0.0800f),   /* Mare Spumans */
    float4(0.0436f, 0.9988f, 0.0227f, 0.1077f),   /* Mare Smythii */
    float4(0.0662f, 0.9709f, 0.2300f, 0.1036f),   /* Mare Marginis */
    float4(0.3680f, -0.9249f, -0.0958f, 0.0495f), /* Grimaldi */
    float4(0.2651f, -0.9625f, -0.0576f, 0.0443f), /* Riccioli */
    float4(0.6130f, -0.1004f, 0.7837f, 0.0294f),  /* Plato */
    float4(0.8665f, -0.0606f, 0.4955f, 0.0230f),  /* Archimedes */
    float4(0.9866f, -0.0310f, -0.1599f, 0.0443f), /* Ptolemaeus */
    float4(0.9716f, -0.0475f, -0.2317f, 0.0317f), /* Alphonsus */
    float4(0.9522f, 0.2623f, 0.1564f, 0.0259f),   /* Julius Caesar */
    float4(0.3232f, 0.4977f, 0.8049f, 0.0357f),   /* Endymion */
    float4(0.7696f, 0.4552f, 0.4478f, 0.0173f),   /* Le Monnier */
    float4(0.6229f, -0.7450f, -0.2385f, 0.0132f), /* Billy */
    float4(0.8780f, 0.4359f, -0.1977f, 0.0288f)); /* Theophilus */

/* How dark each plain is. Rows sharing a mare share a tone, so the four Procellarum circles and
 * the three of Frigoris do not show seams against themselves, and the ordering is the one the eye
 * gets from a photograph - Grimaldi, Plato and Billy nearly black, Crisium and Iridum noticeably
 * lighter than Imbrium - rather than measured values.
 *
 * The whole range is compressed against the real geometric albedos of roughly 0.05 to 0.07, for
 * the same reason the highland constant is: at the true spread the disc renders as paper and ink.
 * The mean is what matters downstream, and it is preserved. */
const float sky_maria_tone[SKY_MARIA_COUNT] = float[SKY_MARIA_COUNT](
    0.0795f,  /* Mare Imbrium */
    0.0811f,  /* Oceanus Procellarum */
    0.0811f,  /* Oceanus Procellarum */
    0.0811f,  /* Oceanus Procellarum */
    0.0811f,  /* Oceanus Procellarum */
    0.0875f,  /* Mare Serenitatis */
    0.0827f,  /* Mare Tranquillitatis */
    0.0859f,  /* Mare Fecunditatis */
    0.0843f,  /* Mare Nectaris */
    0.0891f,  /* Mare Crisium */
    0.0811f,  /* Mare Humorum */
    0.0827f,  /* Mare Nubium */
    0.0859f,  /* Mare Vaporum */
    0.0875f,  /* Mare Frigoris */
    0.0875f,  /* Mare Frigoris */
    0.0875f,  /* Mare Frigoris */
    0.0843f,  /* Mare Cognitum */
    0.0907f,  /* Sinus Iridum */
    0.0859f,  /* Mare Insularum */
    0.0875f,  /* Sinus Aestuum */
    0.0891f,  /* Sinus Medii */
    0.0907f,  /* Palus Epidemiarum */
    0.0891f,  /* Sinus Roris */
    0.0859f,  /* Lacus Somniorum */
    0.0907f,  /* Mare Anguis */
    0.0891f,  /* Mare Undarum */
    0.0891f,  /* Mare Spumans */
    0.0859f,  /* Mare Smythii */
    0.0875f,  /* Mare Marginis */
    0.0763f,  /* Grimaldi */
    0.0795f,  /* Riccioli */
    0.0763f,  /* Plato */
    0.0907f,  /* Archimedes */
    0.0923f,  /* Ptolemaeus */
    0.0907f,  /* Alphonsus */
    0.0907f,  /* Julius Caesar */
    0.0827f,  /* Endymion */
    0.0859f,  /* Le Monnier */
    0.0779f,  /* Billy */
    0.0923f); /* Theophilus */

/* Young craters whose ejecta still lies bright on the surface, as a centre in the same
 * selenographic frame as the maria, and the angular reach of the ray system in radians. Reach is
 * the published extent of each system over a 1737 km radius. */
#define SKY_RAY_COUNT 8
const float4 sky_ray_craters[SKY_RAY_COUNT] = float4[SKY_RAY_COUNT](
    float4(0.7134f, -0.1438f, -0.6858f, 1.05f), /* Tycho */
    float4(0.9259f, -0.3388f, 0.1668f, 0.46f),  /* Copernicus */
    float4(0.7801f, -0.6095f, 0.1409f, 0.30f),  /* Kepler */
    float4(0.6198f, -0.6740f, 0.4019f, 0.34f),  /* Aristarchus */
    float4(0.6577f, 0.7004f, 0.2773f, 0.28f),   /* Proclus */
    float4(0.2813f, -0.0501f, 0.9583f, 0.38f),  /* Anaxagoras */
    float4(0.4029f, -0.8151f, -0.4163f, 0.30f), /* Byrgius A */
    float4(0.5279f, 0.6661f, -0.5270f, 0.30f)); /* Stevinus A */

/* How strongly each system shows, and how bright its own core is against the ray albedo. The
 * young craters are not interchangeable: Aristarchus is the brightest point on the Moon by a wide
 * margin, Proclus and Copernicus follow, and Anaxagoras is faint. */
const float sky_ray_strength[SKY_RAY_COUNT] = float[SKY_RAY_COUNT](
    1.00f, 0.75f, 0.55f, 0.65f, 0.50f, 0.40f, 0.45f, 0.45f);
const float sky_ray_bright[SKY_RAY_COUNT] = float[SKY_RAY_COUNT](
    1.15f, 1.10f, 1.05f, 1.55f, 1.30f, 0.95f, 1.00f, 1.05f);

/* Smooth minimum, for unioning the plains below. */
float sky_smin(float a, float b, float k)
{
  float h = max(k - abs(a - b), 0.0f) / k;
  return min(a, b) - h * h * k * 0.25f;
}

/* Derivative of smoothstep, for the crater profile's slope. */
float sky_dsmoothstep(float a, float b, float x)
{
  float t = clamp((x - a) / (b - a), 0.0f, 1.0f);
  return 6.0f * t * (1.0f - t) / (b - a);
}

/* Displace a direction on the Moon's surface by banded noise, so that a shoreline traced out of
 * circles does not read as circles. Amplitude falls with frequency, which is what lets it be
 * applied to every basin at once: the coarse octave carries a small crater bodily instead of
 * tearing it up, and only the fine octaves work the edges. The amplitude is in radians, directly
 * comparable to the radii in the table above. */
float3 sky_moon_warp(float3 body)
{
  float3 d = float3(0.0f);
  float amp = 0.05f;
  float freq = 6.0f;
  for (int i = 0; i < 3; i++) {
    d += amp * (sky_value_noise(body * freq) * 2.0f - 1.0f);
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
float sky_moon_patches(float3 body)
{
  float3 q = body * 3.0f + float3(5.1f, 41.7f, -13.3f);
  float total = 0.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < 6; i++) {
    float n = sky_value_noise(q).x;
    /* Thresholds sit close together so that most of each octave clears one of them and the
     * patches are dense; the amplitude is what keeps that from adding up to more contrast. */
    float bright = smoothstep(0.475f, 0.585f, n);
    /* Written as a forward ramp subtracted from one, never as smoothstep(hi, lo, n). Reversed
     * edges are undefined in GLSL and Cycles resolves them to a step in the wrong direction, so
     * the reversed form silently disagrees between backends. */
    float dark = 1.0f - smoothstep(0.415f, 0.525f, n);
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
float sky_moon_speckle(float3 body)
{
  float3 q = body * 35.0f + float3(-3.9f, 27.1f, 61.4f);
  float total = 0.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < 3; i++) {
    float n = sky_value_noise(q).x;
    float rim = 1.0f - smoothstep(0.0f, 0.08f, abs(n - 0.52f));
    float inside = smoothstep(0.52f, 0.87f, n);
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
float4 sky_moon_craters(float3 body)
{
  float3 q = body * 26.0f + float3(113.0f, -47.0f, 29.0f);
  float3 base = floor(q);
  float best_x = 1.0e6f;
  float3 best_u = float3(0.0f);
  for (int dz = -1; dz <= 1; dz++) {
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        float3 cell = base + float3(float(dx), float(dy), float(dz));
        float3 h = hash_int3_to_vec3(int3(int(cell.x), int(cell.y), int(cell.z)));
        float3 delta = q - (cell + h);
        float dist = length(delta);
        float u = fract(dot(h, float3(7.31f, 3.17f, 11.9f)));
        float radius = 0.12f * pow(1.10f / 0.12f, u);
        float x = dist / radius;
        if (x < best_x) {
          best_x = x;
          best_u = delta / max(dist, 1e-6f);
        }
      }
    }
  }
  float x = best_x;
  float rim = smoothstep(0.78f, 0.94f, x) * (1.0f - smoothstep(1.00f, 1.16f, x));
  float bowl = 1.0f - smoothstep(0.55f, 0.86f, x);
  float albedo = rim - 0.55f * bowl;

  float dbowl = -sky_dsmoothstep(0.35f, 1.00f, x);
  float drim = sky_dsmoothstep(0.78f, 0.94f, x) * (1.0f - smoothstep(1.00f, 1.16f, x)) -
               smoothstep(0.78f, 0.94f, x) * sky_dsmoothstep(1.00f, 1.16f, x);
  float dh_ds = -0.12f * dbowl + 0.06f * drim;

  float3 u = best_u - body * dot(best_u, body);
  u = u / max(length(u), 1e-6f);
  return float4(u * dh_ds, albedo);
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
float sky_moon_relief(float3 body, float3 sun_body)
{
  float total = 0.0f;
  float amp = 1.0f;
  float norm = 0.0f;
  float freq = 60.0f;
  float stretch = 2.2f;
  for (int i = 0; i < 4; i++) {
    float3 p = body - sun_body * dot(body, sun_body) * (1.0f - 1.0f / stretch);
    total += amp * (sky_value_noise(p * freq + float3(-31.7f, 12.9f, 22.4f)).x - 0.5f);
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
float2 sky_moon_rays(float3 p, float cos_phase)
{
  float cover = 0.0f;
  float target = 0.170f;
  for (int i = 0; i < SKY_RAY_COUNT; i++) {
    float3 c = sky_ray_craters[i].xyz;
    float reach = sky_ray_craters[i].w;
    float cd = clamp(dot(p, c), -1.0f, 1.0f);
    float d = acos(cd);
    /* Nothing of this system reaches here, and most points are in range of only one or two. */
    if (d > reach * 1.45f) {
      continue;
    }
    float br = reach * 0.28f;
    float bn = sky_value_noise(p * 30.0f + c * 7.0f).x;
    float blob = 0.62f * smoothstep(0.26f, 0.58f, bn) * (1.0f - smoothstep(br * 0.35f, br, d));

    float3 t = p - c * cd;
    t /= max(length(t), 1e-6f);
    float3 fine = sky_value_noise(t * 18.0f + c * d);
    float tuft = sky_value_noise(t * 5.04f).x;
    float v = fine.x * (0.45f + 0.55f * tuft);
    float end = reach * (0.35f + 1.05f * fine.y);
    float far = clamp(d / max(end, 1e-4f), 0.0f, 1.0f);
    /* Raising the threshold as a filament runs kills the weak ones first, so the system is dense
     * at the crater and sparse at its edge instead of dissolving all at once. */
    float th = 0.34f + 0.54f * pow(far, 0.7f);
    float rays = 0.50f * smoothstep(th - 0.11f, th + 0.11f, v) * pow(1.0f - far, 1.8f);
    rays *= smoothstep(0.0f, br * 0.9f, d);

    float s = sky_ray_strength[i] * max(blob, rays);
    /* The strongest system here sets the tone, not whichever comes last in the table. */
    if (s > cover) {
      cover = s;
      target = 0.170f * sky_ray_bright[i];
    }
  }
  cover *= 0.35f + 0.65f * pow(max(cos_phase, 0.0f), 0.6f);
  return float2(clamp(cover, 0.0f, 1.0f), target);
}

/* Fraction of incoming light reflected back along the view ray, or zero outside the disc.
 * Multiply by the light reaching the Moon to get its radiance. */
float3 sky_moon_reflectance(
    float3 view, float3 moon_dir, float3 sun_dir, float3 pole, float angular_diameter)
{
  float half_angular = angular_diameter * 0.5f;
  float cos_sep = dot(view, moon_dir);
  if (cos_sep < cos(half_angular)) {
    return float3(0.0f);
  }

  /* Frame centered on the Moon; distance is great enough to treat the disc projection as
   * orthographic. Lunar east is the leading limb as the Moon crosses the sky - the right side of
   * the disc when north is up. */
  float3 east = cross(moon_dir, pole);
  /* At the zenith or nadir there's no well-defined east; any perpendicular direction works.
   * Without this fallback the cross product is zero and the whole face comes out NaN. */
  east = (length(east) > 1e-6f) ? normalize(east) : float3(1.0f, 0.0f, 0.0f);
  float3 north = cross(east, moon_dir);

  float3 offset = view - moon_dir * cos_sep;
  float2 uv = float2(dot(offset, east), dot(offset, north)) / sin(half_angular);
  float r2 = dot(uv, uv);
  if (r2 > 1.0f) {
    return float3(0.0f);
  }

  /* Outward normal of the lunar surface, pointing back toward the observer at the center. */
  float3 N = uv.x * east + uv.y * north - sqrt(max(1.0f - r2, 0.0f)) * moon_dir;

  /* Darker plains over brighter highlands, in the Moon's own frame. */
  float3 body = float3(dot(N, -moon_dir), dot(N, east), dot(N, north));
  float3 p = sky_moon_warp(body);
  float patches = sky_moon_patches(body);

  /* Union of the plains, as a smooth minimum over the signed distance to each. Summing soft
   * falloffs cannot merge them: a lone basin has to clear the threshold by itself, so a circle
   * alone already clears it, overlap only tells in a thin annulus, and the joins come out as
   * lens-shaped bulges rather than one plain. */
  float sd = acos(clamp(dot(p, sky_maria[0].xyz), -1.0f, 1.0f)) - sky_maria[0].w;
  float nearest_rim = sd;
  float plain_tone = sky_maria_tone[0];
  for (int i = 1; i < SKY_MARIA_COUNT; i++) {
    float rim = acos(clamp(dot(p, sky_maria[i].xyz), -1.0f, 1.0f)) - sky_maria[i].w;
    sd = sky_smin(sd, rim, 0.045f);
    if (rim < nearest_rim) {
      nearest_rim = rim;
      plain_tone = sky_maria_tone[i];
    }
  }
  /* A wide shoreline, broken by the same patch field the highlands carry. Widening it alone only
   * blurs the boundary; breaking it dissolves the shore into flooded embayments and stranded
   * highland, so that half the disc stops reading as one flat dark shape. */
  float maria = 1.0f - smoothstep(-0.050f, 0.050f, sd + patches * 0.10f);
  /* Maria are young surfaces with almost no craters on them. Holding the craterlets and the
   * speckle out of the plains is most of what makes the disc read as two terrains. */
  float highland = 1.0f - 0.85f * maria;

  /* The phase angle, from the two directions already in hand: the angle at the Moon between the
   * Sun and the viewer. Constant across a half-degree disc, but it costs nothing to take it here
   * rather than thread another value through three backends. */
  float cos_phase = clamp(-dot(moon_dir, sun_dir), -1.0f, 1.0f);
  float phase_angle = acos(cos_phase);
  float phase_deg = degrees(phase_angle);

  /* Plains over highlands. The spread between them is compressed against the measured albedos -
   * maria average 0.046 to 0.060 and highlands about 0.13 - because at the true spread the disc
   * renders as paper and ink. What is given up there is spent on the patch field, which leaves
   * the mean where it was, and the mean is what SKY_moonlight_scale() is fitted against. */
  float albedo = mix(0.130f, plain_tone, maria);
  albedo *= 1.0f + patches * 0.45f * (1.0f - 0.45f * maria);
  albedo *= 1.0f + sky_moon_speckle(body) * 0.34f * (1.0f - 0.75f * maria);
  float4 crater = sky_moon_craters(body);
  albedo *= 1.0f + crater.w * 0.18f * highland;
  /* Bright ejecta is one material lying on two grounds. Mixing toward its own albedo rather than
   * scaling the ground makes it loud over a plain and quiet over highland by itself, which is how
   * the real systems behave. */
  float2 rays = sky_moon_rays(p, cos_phase);
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
  float lunar_lambert = clamp(1.0f - 0.019f * phase_deg + 0.000242f * phase_deg * phase_deg -
                                  0.00000146f * phase_deg * phase_deg * phase_deg,
                              0.0f,
                              1.0f);

  float3 sun_body = float3(dot(sun_dir, -moon_dir), dot(sun_dir, east), dot(sun_dir, north));
  /* Craterlets are shaded by their own slope against the Sun. The gradient lies in the tangent
   * plane, so this dot product picks up only the Sun's tangential component by itself: the
   * shading vanishes where the Sun is overhead and is strongest where it grazes. A full Moon
   * therefore has flat craters at the centre of the disc and shaded ones toward the limb, and a
   * quarter Moon has them shaded along the terminator, with no phase term written here. */
  float shaded = dot(N, sun_dir) +
                 sky_moon_relief(body, sun_body) * 0.040f * (1.0f - 0.6f * maria) -
                 dot(crater.xyz, sun_body) * highland * 0.22f;
  /* Earthshine needs the signed value - it lights the face the Sun does not - so the clamp for
   * the sunlit term is kept separate. */
  float mu0 = max(shaded, 0.0f);
  float mu = dot(N, -view);
  float lit = (mu > 0.0f) ? lunar_lambert * mu0 / max(mu0 + mu, 1e-9f) +
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
  float surge = 1.0f + 0.6f / (1.0f + tan(phase_angle * 0.5f) / 0.06f);

  /* Earthshine: sunlight reflected off Earth faintly lights the Moon's night side, tinted blue.
   * Brightest at new Moon, when the Earth seen from there is full, and gone at full Moon, when it
   * is new. That is what puts the old moon in the new moon's arms. */
  float earth_phase = (1.0f - cos_phase) * 0.5f;
  /* Weighted to blue because Earth is, and small: under a tenth of the sunlit face at its best. */
  float3 earthshine = albedo * float3(0.020f, 0.026f, 0.044f) * earth_phase * max(-shaded, 0.0f);

  /* Fade the very edge to keep the silhouette from aliasing. */
  float edge = 1.0f - smoothstep(0.98f, 1.0f, r2);
  return (float3(albedo * lit * surge) + earthshine) * edge;
}

/* Preetham */
/* lam03+lam4: 5 floats passed as vec4+float */
float sky_perez_function(float4 lam03, float lam4, float theta, float gamma)
{
  float ctheta = cos(theta);
  float cgamma = cos(gamma);

  return (1.0f + lam03[0] * exp(lam03[1] / ctheta)) *
         (1.0f + lam03[2] * exp(lam03[3] * gamma) + lam4 * cgamma * cgamma);
}

float3 xyY_to_xyz(float x, float y, float Y)
{
  float X, Z;

  if (y != 0.0f) {
    X = (x / y) * Y;
  }
  else {
    X = 0.0f;
  }

  if (y != 0.0f && Y != 0.0f) {
    Z = ((1.0f - x - y) / y) * Y;
  }
  else {
    Z = 0.0f;
  }

  return float3(X, Y, Z);
}

[[node]]
void node_tex_sky_preetham(float3 co,
                           float4 config_Y03,
                           float config_Y4,
                           float4 config_x03,
                           float config_x4,
                           float4 config_y03,
                           float config_y4,
                           float2 sun_angles,
                           float3 radiance,
                           float3 xyz_to_r,
                           float3 xyz_to_g,
                           float3 xyz_to_b,
                           float4 &color)
{
  /* convert vector to spherical coordinates */
  float3 spherical = sky_spherical_coordinates(co);
  float theta = spherical[0];
  float phi = spherical[1];

  float suntheta = sun_angles[0];
  float sunphi = sun_angles[1];

  /* angle between sun direction and dir */
  float gamma = sky_angle_between(theta, phi, suntheta, sunphi);

  /* clamp theta to horizon */
  theta = min(theta, M_PI_2 - 0.001f);

  /* compute xyY color space values */
  float Y = radiance[0] * sky_perez_function(config_Y03, config_Y4, theta, gamma);
  float x = radiance[1] * sky_perez_function(config_x03, config_x4, theta, gamma);
  float y = radiance[2] * sky_perez_function(config_y03, config_y4, theta, gamma);

  /* convert to RGB */
  float3 xyz = xyY_to_xyz(x, y, Y);
  color = float4(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz), 1);
}

/* Hosek / Wilkie */
float sky_radiance_hosekwilkie(
    float4 config03, float4 config47, float config8, float theta, float gamma)
{
  float ctheta = cos(theta);
  float cgamma = cos(gamma);

  float expM = exp(config47[0] * gamma);
  float rayM = cgamma * cgamma;
  float mieM = (1.0f + rayM) / pow((1.0f + config8 * config8 - 2.0f * config8 * cgamma), 1.5f);
  float zenith = sqrt(ctheta);

  return (1.0f + config03[0] * exp(config03[1] / (ctheta + 0.01f))) *
         (config03[2] + config03[3] * expM + config47[1] * rayM + config47[2] * mieM +
          config47[3] * zenith);
}

[[node]]
void node_tex_sky_hosekwilkie(float3 co,
                              float4 config_x03,
                              float4 config_x47,
                              float4 config_y03,
                              float4 config_y47,
                              float4 config_z03,
                              float4 config_z47,
                              float3 config_xyz8,
                              float2 sun_angles,
                              float3 radiance,
                              float3 xyz_to_r,
                              float3 xyz_to_g,
                              float3 xyz_to_b,
                              float4 &color)
{
  /* convert vector to spherical coordinates */
  float3 spherical = sky_spherical_coordinates(co);
  float theta = spherical[0];
  float phi = spherical[1];

  float suntheta = sun_angles[0];
  float sunphi = sun_angles[1];

  /* angle between sun direction and dir */
  float gamma = sky_angle_between(theta, phi, suntheta, sunphi);

  /* clamp theta to horizon */
  theta = min(theta, M_PI_2 - 0.001f);

  float3 xyz;
  xyz.x = sky_radiance_hosekwilkie(config_x03, config_x47, config_xyz8[0], theta, gamma) *
          radiance.x;
  xyz.y = sky_radiance_hosekwilkie(config_y03, config_y47, config_xyz8[1], theta, gamma) *
          radiance.y;
  xyz.z = sky_radiance_hosekwilkie(config_z03, config_z47, config_xyz8[2], theta, gamma) *
          radiance.z;

  color = float4(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz), 1);
}

[[node]]
void node_tex_sky_nishita(float3 co,
                          float sky_type,
                          float sun_rotation,
                          float3 xyz_to_r,
                          float3 xyz_to_g,
                          float3 xyz_to_b,
                          float3 sun_pixel_bottom,
                          float3 sun_pixel_top,
                          float4 sun_params,
                          float3 sun_color,
                          float4 moon_params,
                          float3 moon_color,
                          float3 moon_sun_dir,
                          float3 moon_illuminant,
                          float4 star_params,
                          float3 zenith_transmittance,
                          float3 celestial_x,
                          float3 celestial_y,
                          float3 celestial_z,
                          float3 star_color_cool,
                          float3 star_color_hot,
                          float3 milky_way_tint,
                          float milky_way,
                          float milky_way_crowding,
                          float milky_way_structure,
                          float nebulae,
                          float zodiacal_light,
                          float3 zodiacal_tint,
                          sampler2DArray ima,
                          float layer,
                          float4 &color)
{
  float3 spherical = sky_spherical_coordinates(co);
  float3 xyz;
  float dir_elevation = M_PI_2 - spherical.x;
  float x = (spherical.y + M_PI + sun_rotation) / (2.0f * M_PI);
  float fade = 1.0f;
  float y;

  /* Undo the non-linear transformation from the sky LUT. */
  float dir_elevation_abs = (dir_elevation < 0.0f) ? -dir_elevation : dir_elevation;
  y = sqrt(dir_elevation_abs / M_PI_2) * sign(dir_elevation) * 0.5f + 0.5f;

  /* Look up color in the precomputed map and convert to RGB. Sun and Moon colors are already
   * baked into this map, one per body - the two skies are summed here and can't be told apart
   * afterwards. */
  xyz = fade * texture(ima, float3(x, y, layer)).rgb;
  float3 rgb = float3(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz));

  /* The Sun disc covers about half a degree, too small to survive baking into the map, so it's
   * drawn here instead. Every ray sees it, and it reaches the scene through EEVEE's existing
   * mechanism: anything brighter than the World's Sun Threshold is pulled out of the light probe
   * and turned into a sun light, which is what casts shadows. Excluding the disc here would leave
   * the Sun lighting the scene with only its glow, none of its own light. */
  float sun_elevation = sun_params.x;
  float angular_diameter = sun_params.y;
  float sun_intensity = sun_params.z;
  float earth_intersection_angle = sun_params.w;
  /* The Moon occludes the Sun when it's nearer along this ray - an eclipse. What's left of the
   * Sun disc keeps full brightness; the sky dims by the covered fraction, computed from both
   * angular diameters before baking. */
  bool eclipsed = sky_behind_body(normalize(co), moon_params.x, moon_params.y, moon_params.z);
  if (angular_diameter > 0.0f && !eclipsed && dir_elevation > earth_intersection_angle) {
    float sun_angle = sky_angle_between(
        spherical.x, spherical.y, M_PI_2 - sun_elevation, -sun_rotation);
    float half_angular = angular_diameter * 0.5f;
    if (sun_angle < half_angular) {
      /* Limb darkening, coefficient is 0.6. */
      float angle_fraction = sun_angle / half_angular;
      float limb_darkening = 1.0f - 0.6f * (1.0f - sqrt(max(1.0f - angle_fraction * angle_fraction,
                                                            0.0f)));
      float sun_y = ((dir_elevation - sun_elevation) / angular_diameter) + 0.5f;
      float3 disc = mix(sun_pixel_bottom, sun_pixel_top, sun_y) * sun_intensity * limb_darkening;
      /* The disc carries the Sun's tint on top of the atmospheric reddening it already picked up
       * near the horizon. */
      rgb += float3(dot(xyz_to_r, disc), dot(xyz_to_g, disc), dot(xyz_to_b, disc)) * sun_color;
    }
  }

  /* Negative angular diameter means the Moon is off.
   *
   * Visible to camera and glossy rays, not diffuse. The Moon's light already reaches the scene
   * through the moonlit sky around it; gathering a half-degree, far-brighter-than-its-surroundings
   * disc into diffuse light would just add flicker. A Moon reflected in a lake is fine - smeared
   * into ambient light is not. */
  bool visible_to = (g_data.ray_type == RAY_TYPE_CAMERA || g_data.ray_type == RAY_TYPE_GLOSSY);
  if (moon_params.z > 0.0f && visible_to && dir_elevation > earth_intersection_angle) {
    float3 view = normalize(co);
    float3 moon_dir = sky_body_direction(moon_params.x, moon_params.y);
    /* Direction the Moon is lit from - the real Sun direction, or wherever a manual phase
     * implies. Precomputed on the CPU. */
    float3 sun_dir = moon_sun_dir;
    /* Sunlight reaching the Moon travels back down through the air to reach the viewer, dimmed
     * and reddened by whatever atmosphere lies along the line of sight - a Moon near the horizon
     * turns orange for the same reason the Sun does. */
    float3 illuminant = float3(dot(xyz_to_r, moon_illuminant),
                               dot(xyz_to_g, moon_illuminant),
                               dot(xyz_to_b, moon_illuminant));
    /* The third row of the celestial frame is the pole in world coordinates. */
    float3 reflectance = sky_moon_reflectance(view, moon_dir, sun_dir, celestial_z, moon_params.z);
    rgb += reflectance * illuminant * moon_params.w * moon_color;
  }

  /* Stars are never faded by hand - daylight is millions of times brighter, so it drowns them
   * out on its own. */
  if ((star_params.x >= 0.0f || zodiacal_light > 0.0f) && visible_to &&
      dir_elevation > earth_intersection_angle)
  {
    float3 view = normalize(co);
    /* The Sun and Moon occlude stars behind their discs. */
    bool hidden = sky_behind_body(view, moon_params.x, moon_params.y, moon_params.z) ||
                  sky_behind_body(view, sun_elevation, sun_rotation, angular_diameter);
    float footprint = max(length(dFdx(view)), length(dFdy(view)));
    /* Stars and the band are fixed to the celestial sphere, not to the world, so the view
     * direction is carried into that frame before either is looked up. The rotation is rigid, so
     * angles and the pixel footprint mean the same thing on both sides of it. */
    float3 cel = float3(dot(celestial_x, view), dot(celestial_y, view), dot(celestial_z, view));
    float3 stars = float3(0.0f);
    if (star_params.x >= 0.0f) {
      /* Off the band there is nothing to shape, and the fractal is the most expensive thing in
       * this function, so it is only evaluated where it shows. */
      float3 nebulosity = float3(1.0f, 1.0f, 0.0f);
      if ((milky_way_structure > 0.0f || nebulae > 0.0f) && milky_way > 0.0f) {
        float2 g = sky_galactic_coords(cel);
        if (abs(g.x) < 0.45f) {
          nebulosity = sky_nebulosity(cel, milky_way_structure, nebulae);
        }
      }
      stars = sky_stars(cel,
                        star_params.x,
                        star_params.y,
                        star_params.z,
                        star_params.w,
                        footprint,
                        star_color_cool,
                        star_color_hot,
                        milky_way,
                        milky_way_crowding,
                        nebulosity);
      stars += sky_milky_way_glow(cel, milky_way, milky_way_tint, nebulosity);
    }

    /* Sunlight scattered by the dust of the solar system, spread along the ecliptic: a broad
     * wedge reaching up from where the Sun set, and a faint brightening directly opposite it.
     * No date is modeled, so the ecliptic is taken as the plane through the Sun that leans as
     * close to the celestial pole as it can. The Sun lies on the ecliptic by definition, which
     * makes that exactly right whenever the sky's frame and the Sun agree. */
    if (zodiacal_light > 0.0f && !hidden) {
      float3 to_sun = sky_body_direction(sun_elevation, sun_rotation);
      float3 ecliptic = normalize(celestial_z - dot(celestial_z, to_sun) * to_sun);
      float cos_elongation = dot(view, to_sun);
      float sin_latitude = dot(view, ecliptic);
      float wedge = 1.0f / (0.08f + (1.0f - cos_elongation));
      float across = exp(-(sin_latitude / 0.30f) * (sin_latitude / 0.30f));
      float opposition = 1.0f + cos_elongation;
      float gegenschein = 0.55f * exp(-(opposition / 0.09f) * (opposition / 0.09f));
      stars += zodiacal_tint *
               (SKY_ZODIACAL_RADIANCE * zodiacal_light * (wedge + gegenschein) * across);
    }

    /* A star near the horizon is seen through far more air than one overhead - the air mass,
     * approximately 1/sin(elevation) except at the very lowest angles. Transmittance falls off as
     * a power of air mass, so raising the zenith transmittance to this power gives the
     * transmittance along any other line of sight. */
    float sin_elevation = sin(dir_elevation);
    float air_mass = clamp(1.0f / max(sin_elevation, 0.02f), 1.0f, 38.0f);
    float3 transmittance = float3(pow(zenith_transmittance.x, air_mass),
                                  pow(zenith_transmittance.y, air_mass),
                                  pow(zenith_transmittance.z, air_mass));
    rgb += hidden ? float3(0.0f) : stars * transmittance;
  }

  color = float4(rgb, 1.0f);
}
