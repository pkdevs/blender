/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

namespace blender {

struct Depsgraph;
struct Main;
struct World;

struct World *BKE_world_add(struct Main *bmain, const char *name);
void BKE_world_eval(struct Depsgraph *depsgraph, struct World *world);

/* Whether any sky in this world is taking its altitude from the camera, and so has to be worked
 * out again whenever the camera moves. */
bool BKE_world_sky_uses_camera_altitude(const struct World *world);

}  // namespace blender
