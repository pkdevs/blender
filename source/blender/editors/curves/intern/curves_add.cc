/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 */

#include "BLI_math_base_safe.hh"
#include "BLI_rand.hh"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"

#include "ED_curves.hh"

namespace blender::ed::curves {

bke::CurvesGeometry primitive_random_sphere(const int curves_size, const int points_per_curve)
{
  bke::CurvesGeometry curves(points_per_curve * curves_size, curves_size);

  MutableSpan<int> offsets = curves.offsets_for_write();
  MutableSpan<float3> positions = curves.positions_for_write();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  bke::SpanAttributeWriter<float> radius = attributes.lookup_or_add_for_write_only_span<float>(
      "radius", bke::AttrDomain::Point);

  for (const int i : offsets.index_range()) {
    offsets[i] = points_per_curve * i;
  }

  RandomNumberGenerator rng;

  const OffsetIndices points_by_curve = curves.points_by_curve();
  for (const int i : curves.curves_range()) {
    const IndexRange points = points_by_curve[i];
    MutableSpan<float3> curve_positions = positions.slice(points);
    MutableSpan<float> curve_radii = radius.span.slice(points);

    const float theta = 2.0f * M_PI * rng.get_float();
    const float phi = safe_acosf(2.0f * rng.get_float() - 1.0f);

    float3 no = {std::sin(theta) * std::sin(phi), std::cos(theta) * std::sin(phi), std::cos(phi)};
    no = math::normalize(no);

    float3 co = no;
    for (int key = 0; key < points_per_curve; key++) {
      float t = key / float(points_per_curve - 1);
      curve_positions[key] = co;
      curve_radii[key] = 0.02f * (1.0f - t);

      float3 offset = float3(rng.get_float(), rng.get_float(), rng.get_float()) * 2.0f - 1.0f;
      co += (offset + no) / points_per_curve;
    }
  }

  radius.finish();

  return curves;
}

}  // namespace blender::ed::curves
