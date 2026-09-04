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
  /* Emission from the upper atmosphere, owing nothing to the Sun. */
  float airglow;
  float3 airglow_tint;
  /* Color of the light the Sun sends toward the atmosphere, applied to the sky it scatters. */
  float3 sun_light_color;

 public:
  SkyLoader(const NodeSkyType sky_type,
            const float sun_elevation,
            const float altitude,
            const float air_density,
            const float aerosol_density,
            const float ozone_density,
            const float aerosol_anisotropy,
            const float airglow,
            const float3 airglow_tint,
            const float3 sun_light_color);
  ~SkyLoader() override;

  bool load_metadata(ImageMetaData &metadata,
                     const ImageLoaderParams &params,
                     Progress &progress) override;

  bool load_pixels(const ImageMetaData &metadata, void *pixels) override;

  string name() const override;

  bool equals(const ImageLoader &other) const override;
};

CCL_NAMESPACE_END
