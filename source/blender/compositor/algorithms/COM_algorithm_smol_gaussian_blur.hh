/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"

namespace blender::compositor {

class Context;
class Result;

/* Approximate a Gaussian blur of a non-single color image, with independent radii in pixels.
 * A radius corresponds to three standard deviations. Sampling extends the image edges; callers
 * that need transparent borders should pad the input first. The output preserves the domain. */
void smol_gaussian_blur(Context &context, const Result &input, Result &output, float2 radius);

}  // namespace blender::compositor
