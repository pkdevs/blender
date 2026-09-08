/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

namespace blender {

struct Scene;
struct Strip;

namespace seq {

// todo(habib): doc functions
const Strip *scene_strip_for_time_sync_get(const Scene *sequencer_scene);

bool scene_strip_frame_range_get(const Scene *sequencer_scene, float *r_start, float *r_end);

ScenePlaybackRange scene_playback_range_get(const Scene *sequencer_scene);

}  // namespace seq
}  // namespace blender
