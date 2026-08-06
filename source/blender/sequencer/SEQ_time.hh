/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include "DNA_listBase.h"

namespace blender {

struct Scene;
struct Strip;
struct rctf;

namespace seq {

/**
 * Initialize given rectangle with the Scene's timeline boundaries.
 *
 * \param scene: the Scene instance whose timeline boundaries are extracted from
 * \param r_rect: output parameter to be filled with timeline boundaries
 */
void timeline_init_boundbox(const Scene *scene, rctf *r_rect);
/**
 * Stretch the given rectangle to include the given strips boundaries
 *
 * \param seqbase: List in which strips are located
 * \param rect: output parameter to be filled with strips' boundaries
 */
void timeline_expand_boundbox(const Scene *scene, const ListBaseT<Strip> *seqbase, rctf *rect);
/**
 * Define boundary rectangle of sequencer timeline and fill in rect data
 *
 * \param scene: Scene in which strips are located
 * \param seqbase: List in which strips are located
 * \param r_rect: data structure describing rectangle, that will be filled in by this function
 */
void timeline_boundbox(const Scene *scene, const ListBaseT<Strip> *seqbase, rctf *r_rect);
/**
 * Find start or end position of next or previous strip.
 * \param scene: Video editing scene
 * \param timeline_frame: reference frame for searching
 * \param side: direction of searching, `SIDE_LEFT`, `SIDE_RIGHT` or `SIDE_BOTH`.
 * \param do_center: find closest strip center if true, otherwise finds closest handle position.
 * \param do_unselected: only find closest position of unselected strip.
 */
int time_find_next_prev_edit(Scene *scene,
                             int timeline_frame,
                             short side,
                             bool do_skip_mute,
                             bool do_center,
                             bool do_unselected);
/* Convert timeline frame to strip frame index. */
float give_frame_index(const Scene *scene, const Strip *strip, float timeline_frame);

/**
 * Return the top-most visible Scene Strip at an arbitrary Sequencer timeline frame.
 */
const Strip *scene_strip_get_for_time_sync_at_frame(const Scene *sequencer_scene,
                                                    int timeline_frame);

/**
 * Return the top-most visible Scene Strip at the Sequencer scene's current frame.
 */
const Strip *scene_strip_get_for_time_sync(const Scene *sequencer_scene);

/**
 * Find the Scene Strip that represents \a source_scene when synchronization originates from an
 * animation editor. The active matching strip is preferred, followed by the matching strip under
 * the Sequencer playhead and then the nearest visible matching strip.
 */
const Strip *scene_strip_get_for_source_scene_time_sync(const Scene *sequencer_scene,
                                                        const Scene *source_scene);

enum class SceneTimeSyncDirection {
  SequencerToScene,
  SceneToSequencer,
};

struct SceneTimeSyncRelation {
  const Scene *sequencer_scene = nullptr;
  const Scene *active_scene = nullptr;
  const Strip *scene_strip = nullptr;
};

/**
 * Resolve the scene and Scene Strip used by both playhead and visible-range synchronization.
 */
bool scene_time_sync_relation_get(const Scene *sequencer_scene,
                                  const Scene *active_scene,
                                  SceneTimeSyncDirection direction,
                                  SceneTimeSyncRelation *r_relation);

/**
 * Map a frame through a resolved synchronization relation.
 *
 * Scene Strip mapping is available for affine forward or reverse playback. Retiming and strobe
 * are supported only in the Sequencer-to-scene playhead path. Same-scene relations always use
 * direct one-to-one mapping.
 */
bool scene_time_sync_frame_map(const SceneTimeSyncRelation &relation,
                               SceneTimeSyncDirection direction,
                               float source_frame,
                               float *r_target_frame);

/**
 * Convert between Sequencer timeline time and a Scene Strip's source-scene time.
 */
bool scene_strip_timeline_frame_to_scene_frame(const Scene *sequencer_scene,
                                               const Strip *scene_strip,
                                               float timeline_frame,
                                               float *r_scene_frame);
bool scene_strip_scene_frame_to_timeline_frame(const Scene *sequencer_scene,
                                               const Strip *scene_strip,
                                               float scene_frame,
                                               float *r_timeline_frame);
/**
 * Update meta strip content start and end, update sound playback range.
 * To be used after any contained strip length or position has changed.
 *
 * \note this function is currently only used internally and in versioning code.
 */
void time_update_meta_strip_range(const Scene *scene, Strip *strip_meta);
/**
 * Move contents of a strip without moving the strip handles.
 */
void time_slip_strip(
    const Scene *scene, Strip *strip, int frame_delta, float subframe_delta, bool slip_keyframes);

}  // namespace seq
}  // namespace blender
