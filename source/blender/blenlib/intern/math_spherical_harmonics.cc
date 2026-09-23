/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * The rotation matrices of the bands are built with the recurrence from:
 *
 *   Joseph Ivanic, Klaus Ruedenberg, "Rotation Matrices for Real Spherical Harmonics. Direct
 *   Determination by Recursion", J. Phys. Chem. 1996, 100 (15), with the corrections from the
 *   additions and corrections published in J. Phys. Chem. A 1998, 102 (45).
 *
 * The recurrence uses the standard real form of the basis functions. The renderers' convention
 * differs from it by a factor of (-1)^|m| on every basis function, which is applied at the end.
 */

#include <cmath>

#include "BLI_assert.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_spherical_harmonics.hh"

namespace blender::math {

namespace {

/** Square matrix of a single band, indexed by the orders m, n in [-l, l]. */
struct BandMatrix {
  int l = 0;
  std::array<double, 9 * 9> values{};

  double operator()(const int m, const int n) const
  {
    return values[(m + l) * (2 * l + 1) + (n + l)];
  }
  double &operator()(const int m, const int n)
  {
    return values[(m + l) * (2 * l + 1) + (n + l)];
  }
};

double kronecker(const int a, const int b)
{
  return a == b ? 1.0 : 0.0;
}

double P(const int i, const int a, const int b, const BandMatrix &r1, const BandMatrix &prev)
{
  const int l = prev.l + 1;
  if (b == l) {
    return r1(i, 1) * prev(a, l - 1) - r1(i, -1) * prev(a, -l + 1);
  }
  if (b == -l) {
    return r1(i, 1) * prev(a, -l + 1) + r1(i, -1) * prev(a, l - 1);
  }
  return r1(i, 0) * prev(a, b);
}

double U(const int m, const int n, const BandMatrix &r1, const BandMatrix &prev)
{
  return P(0, m, n, r1, prev);
}

double V(const int m, const int n, const BandMatrix &r1, const BandMatrix &prev)
{
  if (m == 0) {
    return P(1, 1, n, r1, prev) + P(-1, -1, n, r1, prev);
  }
  if (m > 0) {
    return P(1, m - 1, n, r1, prev) * std::sqrt(1.0 + kronecker(m, 1)) -
           P(-1, -m + 1, n, r1, prev) * (1.0 - kronecker(m, 1));
  }
  return P(1, m + 1, n, r1, prev) * (1.0 - kronecker(m, -1)) +
         P(-1, -m - 1, n, r1, prev) * std::sqrt(1.0 + kronecker(m, -1));
}

double W(const int m, const int n, const BandMatrix &r1, const BandMatrix &prev)
{
  BLI_assert(m != 0);
  if (m > 0) {
    return P(1, m + 1, n, r1, prev) + P(-1, -m - 1, n, r1, prev);
  }
  return P(1, m - 1, n, r1, prev) - P(-1, -m + 1, n, r1, prev);
}

/** Compute the matrix of the band `prev.l + 1` from the matrices of band 1 and band `prev.l`. */
BandMatrix next_band(const BandMatrix &r1, const BandMatrix &prev)
{
  BandMatrix result;
  const int l = prev.l + 1;
  result.l = l;
  for (int m = -l; m <= l; m++) {
    const int abs_m = std::abs(m);
    const double d = kronecker(m, 0);
    for (int n = -l; n <= l; n++) {
      const double denominator = (std::abs(n) == l) ? (2 * l) * (2 * l - 1) : (l + n) * (l - n);
      const double u = std::sqrt((l + m) * (l - m) / denominator);
      const double v = 0.5 * std::sqrt((1.0 + d) * (l + abs_m - 1) * (l + abs_m) / denominator) *
                       (1.0 - 2.0 * d);
      const double w = -0.5 * std::sqrt((l - abs_m - 1) * (l - abs_m) / denominator) * (1.0 - d);

      /* The terms with a zero weight would read outside of the previous band. */
      double value = 0.0;
      if (u != 0.0) {
        value += u * U(m, n, r1, prev);
      }
      if (v != 0.0) {
        value += v * V(m, n, r1, prev);
      }
      if (w != 0.0) {
        value += w * W(m, n, r1, prev);
      }
      result(m, n) = value;
    }
  }
  return result;
}

}  // namespace

SphericalHarmonicsRotation::SphericalHarmonicsRotation(const float3x3 &matrix, const int degree)
    : degree_(degree)
{
  BLI_assert(degree >= 0 && degree <= max_degree);

  /* A reflection is a rotation combined with the point reflection, which multiplies the band l
   * by (-1)^l. Build the bands from the rotation part. */
  double3x3 rotation = double3x3(matrix);
  const bool is_reflection = math::determinant(rotation) < 0.0;
  if (is_reflection) {
    rotation = -rotation;
  }

  /* Band 1 of the standard real form is proportional to (y, z, x). Blender matrices are indexed
   * by column first. */
  const int axis_of_order[3] = {1, 2, 0};
  BandMatrix band;
  band.l = 1;
  for (int m = -1; m <= 1; m++) {
    for (int n = -1; n <= 1; n++) {
      band(m, n) = rotation[axis_of_order[n + 1]][axis_of_order[m + 1]];
    }
  }
  const BandMatrix r1 = band;

  int offset = 0;
  for (int l = 1; l <= degree; l++) {
    if (l > 1) {
      band = next_band(r1, band);
    }
    const double parity = (is_reflection && (l % 2 == 1)) ? -1.0 : 1.0;
    const int size = 2 * l + 1;
    for (int m = -l; m <= l; m++) {
      for (int n = -l; n <= l; n++) {
        const double sign = ((m + n) % 2 == 0) ? 1.0 : -1.0;
        band_matrices_[offset + (m + l) * size + (n + l)] = float(parity * sign * band(m, n));
      }
    }
    offset += size * size;
  }
}

void SphericalHarmonicsRotation::rotate(MutableSpan<float3> coefficients) const
{
  BLI_assert(coefficients.size() == (degree_ + 1) * (degree_ + 1) - 1);
  std::array<float3, 9> band_coefficients;
  int matrix_offset = 0;
  int coefficient_offset = 0;
  for (int l = 1; l <= degree_; l++) {
    const int size = 2 * l + 1;
    MutableSpan<float3> band = coefficients.slice(coefficient_offset, size);
    std::copy_n(band.data(), size, band_coefficients.data());
    for (int row = 0; row < size; row++) {
      float3 value(0.0f);
      for (int column = 0; column < size; column++) {
        value += band_matrices_[matrix_offset + row * size + column] * band_coefficients[column];
      }
      band[row] = value;
    }
    matrix_offset += size * size;
    coefficient_offset += size;
  }
}

}  // namespace blender::math
