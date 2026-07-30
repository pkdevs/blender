/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "spz_read_common.hh"

#include "IO_gsplat.hh"

namespace blender::io::spz {

void convert_axis_to_blender(gsplat::GsplatMutableAttributeAccessor &accessor)
{
  /* By default, SPZ stores data in an RUB (Right, Up, Back) coordinate system following the OpenGL
   * and three.js convention. Blender is RFU (Right, Forward, Up). */

  // TODO(sergey): Handle extensions that define coordinate system.
  // Example: SPZ_ADOBE_coordinate_system.

  MutableSpan<float3> positions = accessor.positions_for_write();
  for (float3 &position : positions) {
    position = {position.x, -position.z, position.y};
  }

  // TODO(sergey): Convert rotation and SH.
}

}  // namespace blender::io::spz
