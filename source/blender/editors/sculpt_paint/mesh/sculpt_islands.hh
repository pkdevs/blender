/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

namespace blender {

struct Object;
struct SculptSession;

namespace ed::sculpt_paint::islands {

void ensure_vert_cache(Object &object);
void ensure_face_cache(Object &object);

/** Mark cache as invalid. Call when adding or hiding geometry. */
void invalidate(SculptSession &ss);

int vert_id_get(const SculptSession &ss, int vert);
int face_id_get(const SculptSession &ss, int face);

}  // namespace ed::sculpt_paint::islands

}  // namespace blender
