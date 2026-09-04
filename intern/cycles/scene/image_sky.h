/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 * SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/svm/types.h"

#include "scene/image_loader.h"

CCL_NAMESPACE_BEGIN

class SkyLoader : public ImageLoader {
 private:
  NodeSkyType sky_type;
  float sun_elevation;
  float altitude;
  float air_density;
  float aerosol_density;
  float ozone_density;
  float aerosol_anisotropy;
  /* The Moon lights the sky too. A negative amount of moonlight means it is turned off. */
  float moon_elevation;
  float moon_azimuth_offset;
  float moonlight;
  /* Emission from the upper atmosphere, owing nothing to the Sun or the Moon. */
  float airglow;
  float3 airglow_tint;
  /* Color of the light each body sends toward the atmosphere. The Sun's and the Moon's skies are
   * baked separately and summed, so each is tinted on its own before they are added together. */
  float3 sun_light_color;
  float3 moon_light_color;

 public:
  SkyLoader(const NodeSkyType sky_type,
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
            const float3 moon_light_color);
  ~SkyLoader() override;

  bool load_metadata(ImageMetaData &metadata,
                     const ImageLoaderParams &params,
                     Progress &progress) override;

  bool load_pixels(const ImageMetaData &metadata, void *pixels) override;

  string name() const override;

  bool equals(const ImageLoader &other) const override;
};

CCL_NAMESPACE_END
