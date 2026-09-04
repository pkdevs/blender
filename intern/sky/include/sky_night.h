/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_sky_modal
 */

/* Geometry and eye response for the night sky. None of it depends on a sky model, so it is
 * shared by all of them. */

#pragma once

/* Remove whatever an eclipse hides from each body's light, multiplying both in place. A no-op
 * when use_moon is false. */
void SKY_eclipse(bool use_moon,
                 float sun_size,
                 float moon_size,
                 float sun_elevation,
                 float sun_rotation,
                 float moon_elevation,
                 float moon_rotation,
                 float r_sun_light[3],
                 float r_moon_light[3]);

/* Where the Moon is lit from, and how far round from full that leaves it. Driven: lit from
 * wherever the Sun actually is. Manual: the Sun is placed at whatever angle gives the requested
 * lit fraction, swung toward lunar east so the terminator lands somewhere predictable. */
void SKY_moon_illumination(bool manual_phase,
                           float moon_phase,
                           float moon_elevation,
                           float moon_rotation,
                           float sun_elevation,
                           float sun_rotation,
                           float r_direction[3],
                           float *r_phase_angle);

/* Rotation carrying a world direction into the frame the stars and the Milky Way are fixed to.
 * Latitude lifts the celestial pole off the horizon, and longitude and rotation both turn the sky
 * about it. Row major, all angles in radians. */
void SKY_celestial_frame(float latitude, float longitude, float rotation, float r_matrix[9]);

/* Share of the Sun's light the Moon sends down at the given phase, or a negative number when the
 * Moon is off. The fall-off is steeper than the lit fraction alone suggests: a half Moon is under
 * a tenth as bright as a full one, since its rough surface shadows itself. A full Moon is roughly
 * a millionth of the Sun, lifted well above that here so that a night scene does not open black;
 * night_exposure adjusts that lift, in stops. */
float SKY_moonlight_scale(bool use_moon,
                          float phase_angle,
                          float moon_intensity,
                          float night_exposure);

/* The Moon's default color temperature. Cooler than the sunlight it reflects: the Moon itself is
 * very slightly warm, but a dark adapted eye sees moonlight as blue, and that is what a night is
 * expected to look like. Kept apart from the Sun's own temperature so a night can be graded on
 * its own. */
#define SKY_MOON_TEMPERATURE 8000.0f

/* Ends of the range star colors are drawn from. Chosen so that their midpoint in reciprocal
 * temperature, which is where the ramp between them is sampled evenly, lands on the Sun's own
 * temperature - so an average star comes out white. */
#define SKY_STAR_TEMPERATURE_COOL 3378.0f
#define SKY_STAR_TEMPERATURE_HOT 20000.0f

/* Roughly what each of these renders at with its own strength at 1 and Night Exposure at 0,
 * measured rather than derived. The night adaptation only needs to know how bright a thing is in
 * order to decide how much color the eye would still find in it. The airglow's own level lives
 * beside the adaptation itself, in sky_hillaire.h. */
#define SKY_NOMINAL_LUMINANCE_MOONGLOW 1.4e-2f
#define SKY_NOMINAL_LUMINANCE_STARS 5.0e-4f
#define SKY_NOMINAL_LUMINANCE_MILKY_WAY 1.7e-4f
#define SKY_NOMINAL_LUMINANCE_ZODIACAL 1.9e-4f
