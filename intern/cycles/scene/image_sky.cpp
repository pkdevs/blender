/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 * SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/image_sky.h"

#include "util/colorspace.h"

#include "util/image_metadata.h"
#include "util/transform.h"

#include "sky_hillaire.h"
#include "sky_nishita.h"

CCL_NAMESPACE_BEGIN

SkyLoader::SkyLoader(const NodeSkyType sky_type,
                     const float sun_elevation,
                     const float altitude,
                     const float air_density,
                     const float aerosol_density,
                     const float ozone_density,
                     const float aerosol_anisotropy,
                     const float moon_elevation,
                     const float moon_azimuth_offset,
                     const float moonlight,
                     const float airglow,
                     const float3 airglow_tint,
                     const float3 sun_light_color,
                     const float3 moon_light_color)
    : sky_type(sky_type),
      sun_elevation(sun_elevation),
      altitude(altitude),
      air_density(air_density),
      aerosol_density(aerosol_density),
      ozone_density(ozone_density),
      aerosol_anisotropy(aerosol_anisotropy),
      moon_elevation(moon_elevation),
      moon_azimuth_offset(moon_azimuth_offset),
      moonlight(moonlight),
      airglow(airglow),
      airglow_tint(airglow_tint),
      sun_light_color(sun_light_color),
      moon_light_color(moon_light_color)
{
}

/* Write a Cycles transform out the way the sky library reads a matrix: 3x3, row major. */
static void sky_matrix(const Transform &t, float r_matrix[9])
{
  for (int col = 0; col < 3; col++) {
    float3 basis = zero_float3();
    basis[col] = 1.0f;
    const float3 mapped = transform_direction(&t, basis);
    for (int row = 0; row < 3; row++) {
      r_matrix[row * 3 + col] = mapped[row];
    }
  }
}

/* Tinting is a per-channel multiply in the render's RGB space, but the sky is baked in XYZ, so the
 * multiply has to be carried across rather than applied to XYZ channels directly. Row major, to
 * match the sky library. */
static void sky_light_color_transform(const float3 light, float r_transform[9])
{
  const Transform to_rgb = ColorSpaceManager::get_xyz_to_scene_linear_rgb();
  const Transform to_xyz = transform_inverse(to_rgb);
  for (int col = 0; col < 3; col++) {
    float3 basis = zero_float3();
    basis[col] = 1.0f;
    const float3 tinted = transform_direction(&to_xyz,
                                              light * transform_direction(&to_rgb, basis));
    for (int row = 0; row < 3; row++) {
      r_transform[row * 3 + col] = tinted[row];
    }
  }
}

SkyLoader::~SkyLoader() = default;

bool SkyLoader::load_metadata(ImageMetaData &metadata,
                              const ImageLoaderParams & /*params*/,
                              Progress & /*progress*/)
{
  metadata.width = 512;
  metadata.height = 256;
  metadata.channels = 3;
  metadata.type = IMAGE_DATA_TYPE_FLOAT4;
  metadata.is_compressible_as_srgb = false;
  return true;
}

bool SkyLoader::load_pixels(const ImageMetaData &metadata, void *pixels)
{
  /* Precompute Sky LUT */
  int width = metadata.width;
  int height = metadata.height;
  float *pixel_data = (float *)pixels;
  if (sky_type == NODE_SKY_SINGLE_SCATTERING) {
    SKY_single_scattering_precompute_texture(pixel_data,
                                             metadata.channels,
                                             width,
                                             height,
                                             sun_elevation,
                                             altitude,
                                             air_density,
                                             aerosol_density,
                                             ozone_density);
  }
  else if (sky_type == NODE_SKY_HILLAIRE) {
    float sun_color_transform[9];
    sky_light_color_transform(sun_light_color, sun_color_transform);
    /* The airglow is cut back into gamut as it is added, which needs the trip into the render's
     * RGB and the trip back out of it as two separate steps. The tint rides on the way back. */
    float airglow_to_rgb[9];
    float airglow_tinted_to_xyz[9];
    const Transform to_rgb = ColorSpaceManager::get_xyz_to_scene_linear_rgb();
    sky_matrix(to_rgb, airglow_to_rgb);
    sky_matrix(transform_inverse(to_rgb) * transform_scale(airglow_tint), airglow_tinted_to_xyz);
    SKY_hillaire_precompute_texture(pixel_data,
                                    metadata.channels,
                                    width,
                                    height,
                                    sun_elevation,
                                    altitude,
                                    air_density,
                                    aerosol_density,
                                    ozone_density,
                                    aerosol_anisotropy,
                                    sun_color_transform);
    /* Scattering is linear in the light that drives it, so the Moon's sky is the same bake run
     * again with the Moon as the illuminant, then turned to the right bearing and added. */
    float moon_color_transform[9];
    sky_light_color_transform(moon_light_color, moon_color_transform);
    SKY_hillaire_add_moon(pixel_data,
                          metadata.channels,
                          width,
                          height,
                          moon_elevation,
                          moon_azimuth_offset,
                          altitude,
                          air_density,
                          aerosol_density,
                          ozone_density,
                          aerosol_anisotropy,
                          moonlight,
                          moon_color_transform);
    /* Owes nothing to the Sun or the Moon, so it goes on last and on its own. */
    SKY_hillaire_add_airglow(pixel_data,
                             metadata.channels,
                             width,
                             height,
                             altitude,
                             air_density,
                             aerosol_density,
                             ozone_density,
                             airglow,
                             airglow_to_rgb,
                             airglow_tinted_to_xyz);
  }
  else {
    SKY_multiple_scattering_precompute_texture(pixel_data,
                                               metadata.channels,
                                               width,
                                               height,
                                               sun_elevation,
                                               altitude,
                                               air_density,
                                               aerosol_density,
                                               ozone_density);
  }

  metadata.conform_pixels(pixels);
  return true;
}

string SkyLoader::name() const
{
  return "sky_multiple_scattering";
}

bool SkyLoader::equals(const ImageLoader &other) const
{
  const SkyLoader &other_sky = (const SkyLoader &)other;
  return sky_type == other_sky.sky_type && sun_elevation == other_sky.sun_elevation &&
         altitude == other_sky.altitude && air_density == other_sky.air_density &&
         aerosol_density == other_sky.aerosol_density &&
         ozone_density == other_sky.ozone_density &&
         aerosol_anisotropy == other_sky.aerosol_anisotropy &&
         moon_elevation == other_sky.moon_elevation &&
         moon_azimuth_offset == other_sky.moon_azimuth_offset &&
         moonlight == other_sky.moonlight && airglow == other_sky.airglow &&
         airglow_tint == other_sky.airglow_tint && sun_light_color == other_sky.sun_light_color &&
         moon_light_color == other_sky.moon_light_color;
}

CCL_NAMESPACE_END
