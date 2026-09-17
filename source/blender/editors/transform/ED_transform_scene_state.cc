/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "ED_edit_undo_scene_state.hh"

#include "BLI_listbase.hh"
#include "DNA_scene_types.h"
#include "MEM_guardedalloc.h"
#include <cstring>

namespace blender::ed {

/* Store current scene settings used in edit mode. */
void ED_scene_state_store(EditModeSceneState &dst, const Scene &scene)
{
  for (TransformOrientation &to : scene.transform_spaces) {
    dst.transform_spaces.append(to);
  }

  memcpy(dst.orientation_slots, scene.orientation_slots, sizeof(dst.orientation_slots));

  const ToolSettings *ts = scene.toolsettings;
  dst.proportional_edit = ts->proportional_edit;
  dst.prop_mode = ts->prop_mode;
  dst.proportional_size = ts->proportional_size;
}

/* Restore stored scene settings after undo. */
void ED_scene_state_restore(Scene &scene, const EditModeSceneState &src)
{
  BLI_freelistN(&scene.transform_spaces);

  for (const TransformOrientation &to_src : src.transform_spaces) {
    TransformOrientation *to_dst = MEM_new<TransformOrientation>(__func__);
    *to_dst = to_src;
    to_dst->next = to_dst->prev = nullptr;
    BLI_addtail(&scene.transform_spaces, to_dst);
  }

  memcpy(scene.orientation_slots, src.orientation_slots, sizeof(src.orientation_slots));

  ToolSettings *ts = scene.toolsettings;

  ts->proportional_edit = src.proportional_edit;
  ts->prop_mode = src.prop_mode;
  ts->proportional_size = src.proportional_size;
}

}  // namespace blender::ed
