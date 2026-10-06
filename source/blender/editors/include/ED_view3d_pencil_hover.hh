/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>

namespace blender {
struct ARegion;
struct bContext;
struct View3DPencilHover {
  uint32_t object_uid = 0;
  /* None, object, vertex, edge, face. No selection state is changed. */
  int kind = 0;
  int index = -1;
};
void ED_view3d_pencil_hover_update(bContext *C, bool active, const int mval[2]);
void ED_view3d_pencil_hover_clear(ARegion *region);
View3DPencilHover ED_view3d_pencil_hover_get(const ARegion *region);
void ED_view3d_pencil_hover_draw(const bContext *C, ARegion *region, void *customdata);
}  // namespace blender
