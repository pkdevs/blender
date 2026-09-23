/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "spz_read_common.hh"

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_spherical_harmonics.hh"

#include "IO_gsplat.hh"

namespace blender::io::spz {

void convert_axis_to_blender(const MutableSpan<float3> positions,
                             const MutableSpan<math::Quaternion> rotations,
                             const Span<MutableSpan<float3>> sh_attrs)
{
  /* By default, SPZ stores data in an RUB (Right, Up, Back) coordinate system following the OpenGL
   * and three.js convention. Blender is RFU (Right, Forward, Up). */

  /* TODO(sergey): Handle extensions that define coordinate system. */
  /* Example: SPZ_ADOBE_coordinate_system. */

  const int sh_degrees = gsplat::degree_for_dimension(sh_attrs.size());

  /* Quarter turn about X, the same as applied to the positions below. */
  const math::SphericalHarmonicsRotation sh_rotation(
      float3x3(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, -1.0f, 0.0f)),
      sh_degrees);

  struct LocalData {
    Array<float3> sh_data;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  threading::parallel_for(IndexRange(positions.size()), 32, [&](const IndexRange range) {
    LocalData &tls = all_tls.local();
    for (const int i : range) {
      positions[i] = {positions[i].x, -positions[i].z, positions[i].y};

      /* Simplified expression for the following:
       * math::to_quaternion(math::AxisAngle(math::AxisSigned::X_POS, M_PI_2)) * rotations[i]. */
      rotations[i] = math::Quaternion(M_SQRT1_2 * (rotations[i].w - rotations[i].x),
                                      M_SQRT1_2 * (rotations[i].x + rotations[i].w),
                                      M_SQRT1_2 * (rotations[i].y - rotations[i].z),
                                      M_SQRT1_2 * (rotations[i].z + rotations[i].y));

      if (sh_degrees == 0) {
        continue;
      }

      if (tls.sh_data.is_empty()) {
        tls.sh_data.reinitialize(sh_attrs.size());
      }

      MutableSpan<float3> sh_data = tls.sh_data;
      gsplat::get_spherical_harmonics(sh_attrs, i, sh_data);
      sh_rotation.rotate(sh_data.take_front(gsplat::dimension_for_degree(sh_degrees)));
      gsplat::set_spherical_harmonics(sh_attrs, i, sh_data);
    }
  });
}

}  // namespace blender::io::spz
