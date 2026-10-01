/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <array>
#include <cmath>
#include <optional>

#include <fmt/format.h>

#include "BLI_assert.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_solvers.hh"
#include "BLI_math_spherical_harmonics.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_gsplat.hh"

#include "DNA_pointcloud_types.h"

namespace blender::bke::gsplat {

int sh_degree_for_coefficients_num(const int coefficients_num)
{
  int degree = 0;
  while (degree < math::SphericalHarmonicsRotation::max_degree &&
         (degree + 2) * (degree + 2) - 1 <= coefficients_num)
  {
    degree++;
  }
  return degree;
}

namespace {

/** A rotation combined with a uniform scale, possibly mirrored. */
struct Similarity {
  float scale;
  math::Quaternion rotation;
};

/* Not using #math::is_uniformly_scaled() because its tolerances are absolute, which accepts small
 * non-uniform scales. A false negative is fine, the general path is exact as well. */
std::optional<Similarity> find_similarity(const float3x3 &linear)
{
  const float determinant = math::determinant(linear);
  const float scale = std::cbrt(std::abs(determinant));
  if (!(scale > 0.0f) || !std::isfinite(scale)) {
    return std::nullopt;
  }
  const float3x3 orthogonal = linear * (1.0f / scale);
  if (!math::is_equal(orthogonal * math::transpose(orthogonal), float3x3::identity(), 1e-5f)) {
    return std::nullopt;
  }
  /* #math::to_quaternion negates a mirroring matrix. That is fine for a gaussian, which is
   * symmetric about its center. */
  return Similarity{scale, math::to_quaternion(orthogonal)};
}

/** The orthogonal factor Q of the polar decomposition `A = Q * P`. */
float3x3 rotation_part(const float3x3 &linear)
{
  float3x3 orthogonal;
  float3x3 positive;
  mat3_polar_decompose(linear.ptr(), orthogonal.ptr(), positive.ptr());
  return orthogonal;
}

/** The number of `radiance:sh_*` coefficients in a complete band #max_degree. */
constexpr int max_sh_coefficients_num = (math::SphericalHarmonicsRotation::max_degree + 1) *
                                            (math::SphericalHarmonicsRotation::max_degree + 1) -
                                        1;

void transform_spherical_harmonics(const float3x3 &linear, const Span<MutableSpan<float3>> sh)
{
  const int degree = sh_degree_for_coefficients_num(sh.size());
  BLI_assert(sh.size() == (degree + 1) * (degree + 1) - 1);
  const math::SphericalHarmonicsRotation rotation(rotation_part(linear), degree);
  threading::parallel_for(sh.first().index_range(), 1024, [&](const IndexRange range) {
    std::array<float3, max_sh_coefficients_num> buffer;
    const MutableSpan<float3> coefficients = MutableSpan(buffer).take_front(sh.size());
    for (const int point : range) {
      for (const int i : sh.index_range()) {
        coefficients[i] = sh[i][point];
      }
      rotation.rotate(coefficients);
      for (const int i : sh.index_range()) {
        sh[i][point] = coefficients[i];
      }
    }
  });
}

bool is_point_attribute(const AttributeAccessor &attributes,
                        const StringRef name,
                        const AttrType type)
{
  const std::optional<AttributeMetaData> meta_data = attributes.lookup_meta_data(name);
  return meta_data && meta_data->domain == AttrDomain::Point && meta_data->data_type == type;
}

}  // namespace

void transform(const float3x3 &linear,
               MutableSpan<math::Quaternion> rotations,
               MutableSpan<float3> scales,
               const Span<MutableSpan<float3>> sh)
{
  BLI_assert(rotations.size() == scales.size());
  if (!sh.is_empty()) {
    for (const MutableSpan<float3> span : sh) {
      BLI_assert(span.size() == sh.first().size());
    }
    BLI_assert(rotations.is_empty() || sh.first().size() == rotations.size());
    transform_spherical_harmonics(linear, sh);
  }
  if (rotations.is_empty()) {
    return;
  }

  if (const std::optional<Similarity> similarity = find_similarity(linear)) {
    threading::parallel_for(rotations.index_range(), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        rotations[i] = similarity->rotation * rotations[i];
        scales[i] *= similarity->scale;
      }
    });
    return;
  }

  /* The covariance of a splat is `F * Fᵀ` with `F = R * S`, so the transformed covariance is
   * `M * Mᵀ` with `M = A * F`. With `M = U * Σ * Vᵀ` that is `U * Σ² * Uᵀ`: U is the new rotation,
   * the singular values are the new scale. Decomposing M rather than `M * Mᵀ` avoids squaring the
   * small scales of flat splats. */
  threading::parallel_for(rotations.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const float3x3 m = linear * math::from_rotation<float3x3>(math::normalize(rotations[i])) *
                         math::from_scale<float3x3>(scales[i]);
      float3x3 u;
      float3x3 v;
      float3 singular_values;
      BLI_svd_m3(m.ptr(), u.ptr(), singular_values, v.ptr());
      /* #math::to_quaternion negates U when it mirrors, see #find_similarity. */
      rotations[i] = math::to_quaternion(u);
      scales[i] = singular_values;
    }
  });
}

AttributesToTransform attributes_to_transform(const AttributeAccessor &attributes)
{
  AttributesToTransform result;
  result.rotation_and_scale = is_point_attribute(attributes, "rotation", AttrType::Quaternion) &&
                              is_point_attribute(attributes, "scale", AttrType::Float3);
  int sh_num = 0;
  while (is_point_attribute(attributes, fmt::format("radiance:sh_{}", sh_num), AttrType::Float3)) {
    sh_num++;
  }
  const int degree = sh_degree_for_coefficients_num(sh_num);
  result.sh_num = (degree + 1) * (degree + 1) - 1;
  return result;
}

void transform_attributes(const float4x4 &object_transform, PointCloud &pointcloud)
{
  if (pointcloud.type != PointCloudType::GSplat) {
    return;
  }
  const float3x3 linear(object_transform);
  if (linear == float3x3::identity()) {
    return;
  }
  MutableAttributeAccessor attributes = pointcloud.attributes_for_write();
  const AttributesToTransform to_transform = attributes_to_transform(attributes);

  SpanAttributeWriter<math::Quaternion> rotations;
  SpanAttributeWriter<float3> scales;
  if (to_transform.rotation_and_scale) {
    rotations = attributes.lookup_for_write_span<math::Quaternion>("rotation");
    scales = attributes.lookup_for_write_span<float3>("scale");
  }

  Vector<SpanAttributeWriter<float3>> sh_writers;
  Vector<MutableSpan<float3>> sh;
  for (const int i : IndexRange(to_transform.sh_num)) {
    sh_writers.append(attributes.lookup_for_write_span<float3>(fmt::format("radiance:sh_{}", i)));
    sh.append(sh_writers.last().span);
  }

  transform(linear, rotations.span, scales.span, sh);

  rotations.finish();
  scales.finish();
  for (SpanAttributeWriter<float3> &writer : sh_writers) {
    writer.finish();
  }
}

}  // namespace blender::bke::gsplat
