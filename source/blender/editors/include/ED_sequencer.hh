/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "BLI_vector_set.hh"
#include "DNA_mask_types.h"
#include "DNA_scene_types.h"

namespace blender {

struct Scene;
struct Strip;
struct SpaceSeq;
struct bContext;
struct View2D;
struct rctf;
struct ARegion;

namespace ed::vse {

enum eStripHandle {
  STRIP_HANDLE_NONE,
  STRIP_HANDLE_LEFT,
  STRIP_HANDLE_RIGHT,
};

struct StripSelection {
  /** Closest strip in the selection to the mouse cursor. */
  Strip *strip1 = nullptr;
  /** Farthest strip in the selection from the mouse cursor. */
  Strip *strip2 = nullptr;
  /** Handle of `strip1`. */
  eStripHandle handle = STRIP_HANDLE_NONE;
};

void select_strip_single(Scene *scene, Strip *strip, bool deselect_all);
/**
 * Iterates over a scene's strips and deselects all of them.
 *
 * \param scene: scene containing strips to be deselected.
 * \return true if any strips were deselected; false otherwise.
 */
bool deselect_all_strips(const Scene *scene);

bool maskedit_mask_poll(bContext *C);
bool check_show_maskedit(SpaceSeq *sseq, Scene *scene);
bool maskedit_poll(bContext *C);
bool maskedit_visible_splines_poll(bContext *C);

void mask_set(bContext *C, Mask *mask);

void mouse_position(Scene *scene, const ARegion *region, const int mval[2], float r_co[2]);
void point_position(Scene *scene, const ARegion *region, float x, float y, float *r_x, float *r_y);
void point_position__reverse(Scene *scene, const ARegion *region, const float *co, float *r_co);
void size_get(const Scene *scene, int *r_width, int *r_height);
void zoom_get(const ARegion *region, float *r_zoomx, float *r_zoomy);
void aspect_get(Scene *scene, float *r_aspx, float *r_aspy);

/**
 * Are we displaying the seq output (not channels or histogram).
 */
bool check_show_imbuf(const SpaceSeq &sseq);

bool check_show_strip(const SpaceSeq &sseq);
/**
 * Check if there is animation shown during playback.
 *
 * - Colors of color strips are displayed on the strip itself.
 * - Backdrop is drawn.
 */
bool has_playback_animation(const Scene *scene);

void ED_operatormacros_sequencer();

Strip *special_preview_get();
void special_preview_set(bContext *C, const int mval[2]);
void special_preview_clear();
/**
 * Returns collection with selected strips presented to user. If operation is done in preview,
 * collection is limited to selected presented strips, that can produce image output at current
 * frame.
 *
 * \param C: context
 * \return collection of strips (`Strip`)
 */
VectorSet<Strip *> selected_strips_from_context(bContext *C);
StripSelection pick_strip_and_handle(const struct Scene *scene,
                                     const View2D *v2d,
                                     float mouse_co[2]);
bool can_select_handle(const Scene *scene, const Strip *strip);
bool handle_is_selected(const Strip *strip, eStripHandle handle);

bool is_scene_time_sync_needed(const bContext &C);
/**
 * Returns the scene strip (if any) that should be used for the scene synchronization feature.
 * This is the top-most visible scene strip at the current time of the \a sequencer_scene.
 */
const Strip *get_scene_strip_for_time_sync(const Scene *sequencer_scene);
void sync_active_scene_and_time_with_scene_strip(bContext &C);

rctf sequencer_clamped_view_bounds_get(const bContext *C, ARegion *region);

}  // namespace ed::vse
}  // namespace blender
