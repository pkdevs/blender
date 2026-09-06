/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "BKE_compositor.hh"
#include "BKE_sound_types.hh"

#include "BLI_set.hh"
#include "BLI_utility_mixins.hh"

namespace blender {

struct Depsgraph;

namespace nodes::eval_log {
class NodesEvalLog;
}  // namespace nodes::eval_log

namespace bke {

/* Runtime data specific to the compositing trees. */
class CompositorRuntime {
 public:
  /* A nodes log of the last compositor evaluation. */
  std::unique_ptr<nodes::eval_log::NodesEvalLog> nodes_evaluation_log;
  /* Cached data for the interactive compositor. */
  bke::compositor::Cache cache;
  /* A dependency graph used for interactive compositing. This is initialized the first time it is
   * needed, and then kept persistent for the lifetime of the scene. This is done to allow the
   * compositor to track changes to resources its uses as well as reduce the overhead of creating
   * the dependency graph every time it executes. */
  Depsgraph *preview_depsgraph = nullptr;

  ~CompositorRuntime();
};

/* Runtime data specific to the sequencer, e.g. when using scene strips. */
class SequencerRuntime {
 public:
  Depsgraph *depsgraph = nullptr;

  ~SequencerRuntime();
};

/* Audio runtime data. */
enum class AudioAnimationWriteState {
  Normal,
  Bake,
};

enum class AudioAnimationCacheType {
  None,
  Automatic,
  Explicit,
};

struct SceneAudioRuntime {
  AUD_Sequence sound_scene;
  AUD_Handle playback_handle;
  AUD_Handle sound_scrub_handle;
  Set<AUD_SequenceEntry> speaker_handles;
  bool volume_update_needed = false;
  /* Set by the depsgraph when an input to this evaluated #Scene's animation cache changes. */
  bool animation_cache_dirty = true;
  /* An automatic attempt exceeded its budget. Use current-frame updates until an input changes,
   * instead of retrying the same expensive preparation on every playback frame. */
  bool animation_cache_skipped = false;
  /* True when some audible properties still need current-frame evaluation. Also includes nested
   * Scenes after automatic preparation. Movie writers must consume these before advancing time. */
  bool animation_cache_has_live_properties = true;
  /* Controls how evaluated audio updates write to Audaspace. */
  AudioAnimationWriteState animation_write_state = AudioAnimationWriteState::Normal;
  /* Automatic caches only contain directly evaluable animation. Explicit caches can also contain
   * dependency-driven values and can satisfy automatic requests while clean and compatible. */
  AudioAnimationCacheType animation_cache_type = AudioAnimationCacheType::None;
  /* Identity of the frame-change handler lists associated with the current audio cache state.
   * An empty value means that no frame-change handlers were present. */
  std::optional<uint64_t> animation_cache_frame_change_handlers_hash;
  /* Inclusive frame range currently stored in the evaluated Scene's audio cache. */
  int cached_animation_frame_start = 1;
  int cached_animation_frame_end = 0;
};

class SceneRuntime : NonCopyable, NonMovable {
 public:
  CompositorRuntime compositor;
  SequencerRuntime sequencer;
  SceneAudioRuntime audio;
};

}  // namespace bke
}  // namespace blender
