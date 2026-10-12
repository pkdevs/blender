/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edscr
 *
 * Implementation code for the "Synchronize View Across Workspaces" feature of 3D views.
 *
 * This works by copying view related values when switching workspaces from the old to the new
 * active workspace.
 */

#include <optional>
#include <utility>

#include "BLI_listbase.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_math_vector_c.hh"

#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "screen_intern.hh"

namespace blender {

struct Workspace3DViewpointSyncValues {
  /** See #RegionView3D.viewquat. */
  float viewquat[4] = {};
  /** See #RegionView3D.dist. */
  float dist = 0;
  /** See #RegionView3D.ofs. */
  float ofs[3] = {};

  /** See #RegionView3D.persp. */
  eRegionView3D_Persp persp = RV3D_ORTHO;
  /** See #RegionView3D.view. */
  eRegionView3D_View view = RV3D_VIEW_USER;
  /** See #RegionView3D.view_axis_roll. */
  eRegionView3D_ViewAxisRoll view_axis_roll = RV3D_VIEW_AXIS_ROLL_0;

  /** See #View3D.lens. */
  float lens = 50.0f;
  /** See #View3D.clip_start. */
  float clip_start = 0.01f;
  /** See #View3D.clip_end. */
  float clip_end = 1000.0f;
};

static std::pair<View3D *, RegionView3D *> find_synced_rv3d(const bScreen *screen)
{
  for (const ScrArea &area : screen->areabase) {
    if (area.spacetype != SPACE_VIEW3D) {
      continue;
    }
    for (const ARegion &region : area.regionbase) {
      if (region.regiontype != RGN_TYPE_WINDOW) {
        continue;
      }
      View3D *v3d = static_cast<View3D *>(area.spacedata.first_as<View3D>());
      RegionView3D *rv3d = static_cast<RegionView3D *>(region.regiondata);

      if (v3d->flag2 & V3D_SYNC_VIEW_ACCROSS_WORKSPACES) {
        if (rv3d->view == RV3D_VIEW_USER) {
          return {v3d, rv3d};
        }
      }
    }
  }
  return {nullptr, nullptr};
}

static std::optional<Workspace3DViewpointSyncValues> workspace_change_viewport_sync_values_get(
    const bScreen *screen_old)
{
  const auto [synced_v3d_old, synced_rv3d_old] = find_synced_rv3d(screen_old);
  if (!synced_rv3d_old) {
    /* No 3D View to sync from. */
    return {};
  }

  if (synced_rv3d_old->localvd) {
    /* Don't do anything in case one of the 3D views is in local view. */
    return {};
  }

  /* When switching from a camera, the "last" value storage should be used (e.g. `lviewquat`
   * instead of `viewquat`). */
  const bool use_last_view = synced_rv3d_old->persp == RV3D_CAMOB;

  Workspace3DViewpointSyncValues values{};

  copy_qt_qt(values.viewquat,
             use_last_view ? synced_rv3d_old->lviewquat : synced_rv3d_old->viewquat);
  values.dist = synced_rv3d_old->dist;
  copy_v3_v3(values.ofs, synced_rv3d_old->ofs);

  values.view = use_last_view ? synced_rv3d_old->lview : synced_rv3d_old->view;
  values.view_axis_roll = use_last_view ? synced_rv3d_old->lview_axis_roll :
                                          synced_rv3d_old->view_axis_roll;
  values.persp = use_last_view ? synced_rv3d_old->lpersp : synced_rv3d_old->persp;

  values.clip_start = synced_v3d_old->clip_start;
  values.clip_end = synced_v3d_old->clip_end;
  values.lens = synced_v3d_old->lens;

  return values;
}

void workspace_change_sync_3d_views(const bScreen *screen_old, const bScreen *screen_new)
{
  auto [synced_v3d_new, synced_rv3d_new] = find_synced_rv3d(screen_new);
  if (!synced_rv3d_new) {
    /* No 3D View to sync to. */
    return;
  }
  if (synced_rv3d_new->localvd) {
    /* Don't do anything in case one of the 3D views is in local view. */
    return;
  }

  const std::optional<Workspace3DViewpointSyncValues> sync_values =
      workspace_change_viewport_sync_values_get(screen_old);
  if (!sync_values) {
    return;
  }

  synced_rv3d_new->dist = sync_values->dist;
  copy_v3_v3(synced_rv3d_new->ofs, sync_values->ofs);

  synced_v3d_new->clip_start = sync_values->clip_start;
  synced_v3d_new->clip_end = sync_values->clip_end;
  synced_v3d_new->lens = sync_values->lens;

  if (synced_rv3d_new->persp == RV3D_CAMOB) {
    copy_qt_qt(synced_rv3d_new->lviewquat, sync_values->viewquat);

    synced_rv3d_new->lview = sync_values->view;
    synced_rv3d_new->lview_axis_roll = sync_values->view_axis_roll;
    synced_rv3d_new->lpersp = sync_values->persp;
  }
  else {
    copy_qt_qt(synced_rv3d_new->viewquat, sync_values->viewquat);

    synced_rv3d_new->view = sync_values->view;
    synced_rv3d_new->view_axis_roll = sync_values->view_axis_roll;
    synced_rv3d_new->persp = sync_values->persp;
  }
}

}  // namespace blender
