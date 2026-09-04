/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_sky_modal
 */

#pragma once

/* Same as SKY_multiple_scattering_precompute_texture(), but integrates multiple scattering into
 * a LUT instead of approximating it with an analytic fit.
 *
 * The color transform tints the light reaching the top of the atmosphere, and so the whole sky
 * that scatters it. It is a per-channel multiply in the render's RGB space, written as the
 * equivalent 3x3 transform in XYZ, row major. Null leaves the light alone. */
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
                                     const float sun_color_transform[9]);

/* Radiance of the Sun disc at the bottom and the top of its face. Multiple scattering plays no
 * part in light that arrives straight from the Sun, so this differs from the Multiple Scattering
 * model's version of it only in belonging to this model. */
void SKY_hillaire_precompute_sun(float sun_elevation,
                                 float angular_diameter,
                                 float altitude,
                                 float air_density,
                                 float aerosol_density,
                                 float ozone_density,
                                 float r_pixel_bottom[3],
                                 float r_pixel_top[3]);

/* Fraction of light that survives the trip down through the atmosphere from the given elevation.
 * One at the top, falling away toward the horizon and taking the blue with it first. */
void SKY_transmittance(float elevation,
                       float altitude,
                       float air_density,
                       float aerosol_density,
                       float ozone_density,
                       float r_dimmed_xyz[3],
                       float r_clear_xyz[3]);

/* Add the sky lit by a second, much dimmer body to a sky that has already been computed. The
 * azimuth offset is the bearing of that body measured from the first one. */
void SKY_hillaire_add_moon(float *pixels,
                           int stride,
                           int width,
                           int height,
                           float moon_elevation,
                           float azimuth_offset,
                           float altitude,
                           float air_density,
                           float aerosol_density,
                           float ozone_density,
                           float aerosol_anisotropy,
                           float scale,
                           const float moon_color_transform[9]);

/* Color of the airglow emission before the atmosphere or any adaptation touches it, as XYZ.
 * The engines need it to work out how far the night adaptation moves it. */
void SKY_airglow_color(float r_xyz[3]);

/* Add the faint natural glow of the upper atmosphere to an already baked sky. Independent of the
 * Sun, so it is what a moonless night is lit by.
 *
 * Unlike scattered sunlight, this emission is saturated enough to fall outside the render's
 * gamut, so it is cut back into it as it is added rather than left for each shading backend to
 * deal with in its own way. That needs the trip into the render's RGB and back, so the caller
 * passes both halves: color_to_rgb, and its inverse with the color the glow should end up
 * carrying already folded in. Both 3x3, row major, both null to leave the color alone. */
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
                              const float tinted_color_to_xyz[9]);

/* The Sun's own temperature in Kelvin, which the sky model is computed for. Both the Cycles and
 * the Blender side divide the Sun's light through by this when working out its tint, so that the
 * default temperature leaves the light unchanged. */
#define SKY_SUN_TEMPERATURE 5778.0f

/* Luminance at which color is half gone, in the render's own units. Wanat & Mantiuk measured the
 * half point at 0.108 cd/m2; this is not that number, because the night sky here is deliberately
 * lifted far above its true radiance so that it can be seen at all, which breaks any fixed
 * mapping to cd/m2. It is tuned instead against the levels this node actually produces, and
 * happens to land nearby. */
#define SKY_SCOTOPIC_HALF_LUMINANCE 0.1f

/* Roughly what the airglow renders at with its own strength at 1, measured rather than derived.
 * The night adaptation only needs to know how bright a thing is in order to decide how much color
 * the eye would still find in it. */
#define SKY_NOMINAL_LUMINANCE_AIRGLOW 3.5e-4f

/* Drain the color out of something too dim for the eye to find color in, and slide what is left
 * toward blue. `luminance` is how bright the thing actually renders, which is what decides how
 * far to go; `color` only supplies the hue, and its own brightness is handed back untouched. */
void SKY_night_adaptation(const float color[3], float luminance, float strength, float r_color[3]);
