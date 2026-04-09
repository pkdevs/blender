/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <pxr/usd/usdShade/shader.h>

namespace blender {
struct Image;
}

namespace blender::io::usd {

/** Set the Blender image colorspace from an imported USD texture shader. */
void colorspace_to_image_texture(const pxr::UsdShadeShader &usd_shader,
                                 const pxr::UsdShadeInput &file_input,
                                 bool is_data,
                                 Image *image);

}  // namespace blender::io::usd
