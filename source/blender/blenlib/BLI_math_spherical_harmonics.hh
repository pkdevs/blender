/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include <array>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender::math {

/**
 * Rotation of real spherical harmonics coefficients.
 *
 * The coefficients use the same convention as the renderers: the standard real form of the
 * basis functions multiplied by (-1)^|m| [sloan2008], stored band after band starting at band 1,
 * each band ordered from m = -l to m = l. Band 0 is invariant under rotation and not included.
 *
 * Given the coefficients of a function f, the rotated coefficients represent g(d) = f(M^-1 * d).
 * The matrix M has to be orthonormal. Reflections are supported.
 *
 * Building the rotation is relatively expensive, applying it to the coefficients of a single
 * point is cheap. Construct it once and use it for all points.
 *
 * [sloan2008] Peter-Pike Sloan, "Stupid Spherical Harmonics (SH) Tricks", GDC 2008.
 */
class SphericalHarmonicsRotation {
 public:
  static constexpr int max_degree = 4;

 private:
  int degree_;
  /* Row-major (2l + 1) x (2l + 1) matrices of the bands 1 to max_degree, one after another. */
  std::array<float, 3 * 3 + 5 * 5 + 7 * 7 + 9 * 9> band_matrices_;

 public:
  /** \param degree: The number of bands above band 0, at most #max_degree. */
  SphericalHarmonicsRotation(const float3x3 &matrix, int degree);

  /** Rotate the coefficients of one point in place. The span has `(degree + 1)^2 - 1` items. */
  void rotate(MutableSpan<float3> coefficients) const;
};

}  // namespace blender::math
