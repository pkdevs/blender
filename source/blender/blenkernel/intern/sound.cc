/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#ifdef WITH_FFTW3
#  include <fftw3.h>
#endif

#include "MEM_guardedalloc.h"

#include "BLI_build_config.hh"
#include "BLI_enum_flags.hh"
#include "BLI_listbase.hh"
#include "BLI_math_base_c.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.hh"
#include "BLI_threads.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BLT_translation.hh"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"
#include "DNA_speaker_types.h"
#include "DNA_userdef_types.h"

#ifdef WITH_AUDASPACE
#  include <Exception.h>
#  include <IReader.h>
#  include <devices/DeviceManager.h>
#  include <devices/IDeviceFactory.h>
#  include <devices/IHandle.h>
#  include <devices/NULLDevice.h>
#  include <devices/ReadDevice.h>
#  include <file/File.h>
#  include <file/FileManager.h>
#  include <file/FileWriter.h>
#  include <fx/Accumulator.h>
#  include <fx/AnimateableTimeStretchPitchScale.h>
#  include <fx/Envelope.h>
#  include <fx/Highpass.h>
#  include <fx/Limiter.h>
#  include <fx/Lowpass.h>
#  include <fx/Sum.h>
#  include <fx/Threshold.h>
#  include <generator/Silence.h>
#  include <plugin/PluginManager.h>
#  include <respec/ChannelMapper.h>
#  include <respec/LinearResample.h>
#  include <sequence/Sequence.h>
#  include <sequence/SequenceEntry.h>
#  include <util/StreamBuffer.h>

#  include <fmt/format.h>

#endif

#include "BKE_anim_data.hh"
#include "BKE_animsys.hh"
#include "BKE_bpath.hh"
#include "BKE_global.hh"
#include "BKE_idtype.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_packedFile.hh"
#include "BKE_scene.hh"
#include "BKE_scene_runtime.hh"
#include "BKE_sound.hh"
#include "BKE_sound_sample.hh"

#include "sound_reader_cache.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "SEQ_iterator.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_sound.hh"

#include "BLI_concurrent_map.hh"

#include "CLG_log.h"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#endif

namespace blender {

namespace bke {
struct SceneAudioRuntime;

enum class SoundTags {
  None = 0,
  /* Do not free/reset waveform on sound load, only used by undo code. */
  WaveformNoReload = 1 << 0,
  WaveformLoading = 1 << 1,
};
ENUM_OPERATORS(SoundTags);

using bSoundFrequencySamplerMap =
    ConcurrentMap<bSoundFrequencySampler::Key, std::shared_ptr<bSoundFrequencySampler>>;

#ifdef WITH_AUDASPACE
struct SoundRuntimeGeneration {
  AUD_Sound handle;
  bSoundFrequencySamplerMap samplers;
  std::shared_ptr<SoundReaderCache> reader_cache;
};
#endif

struct SoundRuntime {
  AUD_Sound handle;
  AUD_Sound cache;
  /* The audaspace handle that should actually be played back.
   * Should be cache if cache != NULL; otherwise its handle. */
  AUD_Sound playback_handle;
  /* Spin-lock for asynchronous loading of sounds. */
  SpinLock spinlock;

  /* Note: not by-value since #sound_foreach_cache can only
   * save/restore a pointer. */
  Vector<float> *waveform = nullptr;
  SoundTags tags = SoundTags::None;

#ifdef WITH_AUDASPACE
  std::mutex generation_mutex;
  std::shared_ptr<SoundRuntimeGeneration> generation;
#endif
};

}  // namespace bke

static void sound_free_audio(bSound *sound);

static void sound_init_runtime(bSound *sound)
{
  sound->runtime = MEM_new<bke::SoundRuntime>(__func__);
  BLI_spin_init(&sound->runtime->spinlock);
}

#ifdef WITH_AUDASPACE
static void sound_runtime_generation_replace(
    bSound *sound, std::shared_ptr<bke::SoundRuntimeGeneration> generation)
{
  bke::SoundRuntime *runtime = sound->runtime;
  std::shared_ptr<bke::SoundRuntimeGeneration> old_generation;
  {
    std::lock_guard lock{runtime->generation_mutex};
    old_generation = std::move(runtime->generation);
    runtime->generation = std::move(generation);
  }
}

static std::shared_ptr<bke::SoundRuntimeGeneration> sound_runtime_generation_get(
    const bSound &sound)
{
  bke::SoundRuntime *runtime = sound.runtime;
  std::lock_guard lock{runtime->generation_mutex};
  return runtime->generation;
}
#endif

static void sound_free_waveform(bSound *sound)
{
  bke::SoundRuntime *runtime = sound->runtime;
  if (!flag_is_set(runtime->tags, bke::SoundTags::WaveformNoReload)) {
    MEM_SAFE_DELETE(runtime->waveform);
  }
  /* This tag is only valid once. */
  runtime->tags &= ~bke::SoundTags::WaveformNoReload;
}

static void sound_copy_data(Main * /*bmain*/,
                            std::optional<Library *> /*owner_library*/,
                            ID *id_dst,
                            const ID *id_src,
                            const int /*flag*/)
{
  bSound *sound_dst = id_cast<bSound *>(id_dst);
  const bSound *sound_src = id_cast<const bSound *>(id_src);

  /* Just to be sure, should not have any value actually after reading time. */
  sound_dst->newpackedfile = nullptr;

  if (sound_src->packedfile != nullptr) {
    sound_dst->packedfile = BKE_packedfile_duplicate(sound_src->packedfile);
  }

  sound_init_runtime(sound_dst);
}

static void sound_free_data(ID *id)
{
  bSound *sound = id_cast<bSound *>(id);

  if (sound->packedfile) {
    BKE_packedfile_free(sound->packedfile);
    sound->packedfile = nullptr;
  }

  if (sound->runtime) {
    sound_free_audio(sound);
    sound_free_waveform(sound);
    BLI_spin_end(&sound->runtime->spinlock);
    MEM_delete(sound->runtime);
  }
}

static void sound_foreach_cache(ID *id,
                                IDTypeForeachCacheFunctionCallback function_callback,
                                void *user_data)
{
  bSound *sound = id_cast<bSound *>(id);
  IDCacheKey key = {id->session_uid, 1};
  function_callback(id, &key, reinterpret_cast<void **>(&sound->runtime->waveform), 0, user_data);
}

static void sound_foreach_path(ID *id, BPathForeachPathData *bpath_data)
{
  bSound *sound = id_cast<bSound *>(id);
  if (sound->packedfile != nullptr && (bpath_data->flag & BKE_BPATH_FOREACH_PATH_SKIP_PACKED) != 0)
  {
    return;
  }

  /* FIXME: This does not check for empty path... */
  BKE_bpath_foreach_path_fixed_process(bpath_data, sound->filepath, sizeof(sound->filepath));
}

static void sound_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  bSound *sound = id_cast<bSound *>(id);
  const bool is_undo = BLO_write_is_undo(writer);

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  sound->runtime = nullptr;

  /* Do not store packed files in case this is a library override ID. */
  if (ID_IS_OVERRIDE_LIBRARY(sound) && !is_undo) {
    sound->packedfile = nullptr;
  }

  /* write LibData */
  writer->write_id_struct(id_address, sound);
  BKE_id_blend_write(writer, &sound->id);

  BKE_packedfile_blend_write(writer, sound->packedfile);
}

static void sound_blend_read_data(BlendDataReader *reader, ID *id)
{
  bSound *sound = id_cast<bSound *>(id);
  sound_init_runtime(sound);
  if (BLO_read_data_is_undo(reader)) {
    sound->runtime->tags |= bke::SoundTags::WaveformNoReload;
  }

  BKE_packedfile_blend_read(reader, &sound->packedfile, sound->filepath);
  BKE_packedfile_blend_read(reader, &sound->newpackedfile, sound->filepath);
}

IDTypeInfo IDType_ID_SO = {
    .id_code = bSound::id_type,
    .id_filter = FILTER_ID_SO,
    .dependencies_id_types = 0,
    .main_listbase_index = INDEX_ID_SO,
    .struct_size = sizeof(bSound),
    .name = "Sound",
    .name_plural = N_("sounds"),
    .translation_context = BLT_I18NCONTEXT_ID_SOUND,
    .flags = IDTYPE_FLAGS_NO_ANIMDATA | IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = nullptr,

    /* A fuzzy case, think NULLified content is OK here... */
    .init_data = nullptr,
    .copy_data = sound_copy_data,
    .free_data = sound_free_data,
    .make_local = nullptr,
    .foreach_id = nullptr,
    .foreach_cache = sound_foreach_cache,
    .foreach_path = sound_foreach_path,
    .foreach_working_space_color = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = sound_blend_write,
    .blend_read_data = sound_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

#ifdef WITH_AUDASPACE
/* evil globals ;-) */
static char **audio_device_names = nullptr;
#endif

BLI_INLINE void sound_verify_evaluated_id(const ID *id)
{
  UNUSED_VARS_NDEBUG(id);
  /* This is a bit tricky and not quite reliable, but good enough check.
   *
   * We don't want audio system handles to be allocated on an original data-blocks, and only want
   * them to be allocated on a data-blocks which are result of dependency graph evaluation.
   *
   * Data-blocks which are covered by a copy-on-evaluation system of dependency graph will have
   * ID_TAG_COPIED_ON_EVAL tag set on them.
   *
   * Additionally, we also allow data-blocks outside of main database. Those can not be "original"
   * and could be used as a temporary evaluated result during operations like baking.
   *
   * NOTE: We consider ID evaluated if ANY of those flags is set. We do NOT require ALL of them.
   */
  BLI_assert(id->tag & (ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN));
}

bSound *BKE_sound_new_file(Main *bmain, const char *filepath, short stream_index)
{
  bSound *sound;
  const char *blendfile_path = BKE_main_blendfile_path(bmain);
  char filepath_abs[FILE_MAX];

  STRNCPY(filepath_abs, filepath);
  BLI_path_abs(filepath_abs, blendfile_path);

  sound = static_cast<bSound *>(BKE_libblock_alloc(bmain, ID_SO, BLI_path_basename(filepath), 0));
  STRNCPY(sound->filepath, filepath);
  sound_init_runtime(sound);
  sound->stream_index = stream_index;

  /* Extract sound specs for bSound */
  SoundInfo info;
  bool success = BKE_sound_info_get(bmain, sound, &info);
  if (success) {
    sound->samplerate = info.specs.samplerate;
    sound->audio_channels = info.specs.channels;
  }

  return sound;
}

static bSound *sound_new_file_exists_ex(Main *bmain, const char *filepath, short stream_index)
{
  bSound *sound;
  char filepath_abs[FILE_MAX], filepath_test[FILE_MAX];

  STRNCPY(filepath_abs, filepath);
  BLI_path_abs(filepath_abs, BKE_main_blendfile_path(bmain));

  /* Search for an existing sound matching both filepath and stream index. */
  for (sound = static_cast<bSound *>(bmain->sounds.first); sound;
       sound = static_cast<bSound *>(sound->id.next))
  {
    if (sound->stream_index != stream_index) {
      continue;
    }
    STRNCPY(filepath_test, sound->filepath);
    BLI_path_abs(filepath_test, ID_BLEND_PATH(bmain, &sound->id));

    if (BLI_path_cmp(filepath_test, filepath_abs) == 0) {
      id_us_plus(&sound->id); /* officially should not, it doesn't link here! */
      return sound;
    }
  }

  return BKE_sound_new_file(bmain, filepath, stream_index);
}

bSound *BKE_sound_new_file_exists(Main *bmain, const char *filepath, short stream_index)
{
  return sound_new_file_exists_ex(bmain, filepath, stream_index);
}

static Scene *sound_original_scene(const Scene *scene)
{
  return const_cast<Scene *>(DEG_get_original(scene));
}

static void sound_reset_scene_audio_animation(Scene *scene);
static void sound_discard_scene_audio_animation_cache(Depsgraph *depsgraph, Scene *scene);

static bool sound_scene_strip_uses_sequencer(const Strip *strip)
{
  return strip->type == STRIP_TYPE_SCENE && strip->scene != nullptr &&
         (strip->flag & SEQ_SCENE_STRIPS) && strip->scene->ed != nullptr;
}

static bool sound_strip_has_audio(const Strip *strip)
{
  if (strip->type == STRIP_TYPE_SOUND) {
    return strip->sound != nullptr;
  }
  if (strip->type == STRIP_TYPE_SCENE) {
    return strip->scene != nullptr;
  }
  if (strip->type != STRIP_TYPE_META) {
    return false;
  }
  for (const Strip &child : strip->seqbase) {
    if (sound_strip_has_audio(&child)) {
      return true;
    }
  }
  return false;
}

static void sound_collect_scene_audio_animation_write_scenes(const Scene *scene,
                                                             Depsgraph *depsgraph,
                                                             Set<const Scene *> &visited_scenes,
                                                             Vector<Scene *> &r_scenes)
{
  Scene *scene_original = sound_original_scene(scene);
  if (!visited_scenes.add(scene_original)) {
    return;
  }

  Scene *scene_eval = DEG_get_evaluated(depsgraph, scene_original);
  if (DEG_is_evaluated(scene_eval)) {
    r_scenes.append(scene_eval);
  }

  if (scene_original->ed == nullptr) {
    return;
  }
  seq::foreach_strip(&scene_original->ed->seqbase, [&](Strip *strip) {
    if (strip->type == STRIP_TYPE_SCENE && strip->scene != nullptr) {
      sound_collect_scene_audio_animation_write_scenes(
          strip->scene, depsgraph, visited_scenes, r_scenes);
    }
    return true;
  });
}

struct SoundAudioAnimationWriteStateBackup {
  Scene *scene;
  bke::AudioAnimationWriteState state;
};

class SoundAudioAnimationPreserveGuard {
 public:
  explicit SoundAudioAnimationPreserveGuard(Depsgraph *depsgraph)
  {
    Set<const Scene *> visited_scenes;
    Vector<Scene *> scenes;
    sound_collect_scene_audio_animation_write_scenes(
        DEG_get_input_scene(depsgraph), depsgraph, visited_scenes, scenes);

    for (Scene *scene : scenes) {
      backups_.append({scene, scene->runtime->audio.animation_write_state});
      scene->runtime->audio.animation_write_state = bke::AudioAnimationWriteState::Preserve;
    }
  }

  ~SoundAudioAnimationPreserveGuard()
  {
    for (const SoundAudioAnimationWriteStateBackup &backup : backups_) {
      backup.scene->runtime->audio.animation_write_state = backup.state;
    }
  }

  SoundAudioAnimationPreserveGuard(const SoundAudioAnimationPreserveGuard &) = delete;
  SoundAudioAnimationPreserveGuard &operator=(const SoundAudioAnimationPreserveGuard &) = delete;

 private:
  Vector<SoundAudioAnimationWriteStateBackup> backups_;
};

#ifdef WITH_AUDASPACE
static bool sound_audio_animation_is_preserved(const Scene *scene)
{
  const auto state = scene->runtime->audio.animation_write_state;
  return state == bke::AudioAnimationWriteState::Preserve;
}
#endif

void BKE_sound_invalidate_scene_audio_animation_cache(Scene *scene)
{
  BLI_assert(DEG_is_evaluated(scene));
  scene->runtime->audio.animation_cache_dirty = true;
}

static void sound_audio_animation_path_add_property(Set<std::string> &paths,
                                                    PointerRNA &ptr,
                                                    const char *property)
{
  PropertyRNA *prop = RNA_struct_find_property(&ptr, property);
  if (prop == nullptr) {
    return;
  }
  if (const std::optional<std::string> path = RNA_path_from_ID_to_property(&ptr, prop)) {
    paths.add(*path);
  }
}

static Set<std::string> sound_audio_animation_paths(Scene *scene)
{
  Set<std::string> paths;

  PointerRNA scene_ptr = RNA_id_pointer_create(&scene->id);
  sound_audio_animation_path_add_property(paths, scene_ptr, "audio_volume");

  if (scene->ed == nullptr) {
    return paths;
  }

  seq::foreach_strip(&scene->ed->seqbase, [&](Strip *strip) {
    const bool is_audio_strip = ELEM(strip->type, STRIP_TYPE_SOUND, STRIP_TYPE_SCENE) &&
                                sound_strip_has_audio(strip);
    const bool is_audio_meta = strip->type == STRIP_TYPE_META && sound_strip_has_audio(strip);
    if (is_audio_strip || is_audio_meta) {
      PointerRNA strip_ptr = RNA_pointer_create_discrete(&scene->id, RNA_Strip, strip);
      sound_audio_animation_path_add_property(paths, strip_ptr, "volume");
      if (is_audio_strip) {
        sound_audio_animation_path_add_property(paths, strip_ptr, "pan");
      }
    }

    return true;
  });

  return paths;
}

/* Whether the #Scene needs per-frame audio property sampling at all. This is intentionally checked
 * while preparing a cache, rather than on every ordinary frame change. */
static bool sound_scene_has_audio_animation(const Scene *scene, const Set<std::string> &paths)
{
  const Scene *scene_original = sound_original_scene(scene);
  const AnimData *adt = BKE_animdata_from_id(&scene_original->id);
  for (const std::string &path : paths) {
    if (bke::animdata::prop_is_animated(adt, path.c_str(), 0)) {
      return true;
    }
  }
  return false;
}

static bool sound_scene_has_audio_drivers(const Scene *scene, const Set<std::string> &paths)
{
  if (scene->adt == nullptr) {
    return false;
  }
  for (const FCurve &fcurve : scene->adt->drivers) {
    if (paths.contains_as(fcurve.rna_path())) {
      return true;
    }
  }
  return false;
}

enum class SoundAnimationEvaluationMode {
  Direct,
  TargetedDependencies,
  FullScene,
};

enum class SoundAnimationPreparationMode {
  Ensure,
  ExplicitBake,
};

static SoundAnimationEvaluationMode sound_scene_audio_evaluation_mode(
    const Scene *scene,
    const Set<std::string> &paths,
    const std::optional<uint64_t> &frame_change_handlers_hash)
{
  if (frame_change_handlers_hash.has_value()) {
    /* Python handlers can mutate arbitrary data, so their explicit bake keeps the full evaluation
     * semantics. Automatic cache preparation returns before reaching this mode. */
    return SoundAnimationEvaluationMode::FullScene;
  }
  if (sound_scene_has_audio_drivers(scene, paths)) {
    return SoundAnimationEvaluationMode::TargetedDependencies;
  }
  return SoundAnimationEvaluationMode::Direct;
}

struct SoundAnimationBakeRequest {
  Scene *scene;
  ViewLayer *view_layer;
  int frame_start;
  int frame_end;
};

static bool sound_scene_audio_animation_cache_has_data(const bke::SceneAudioRuntime &audio)
{
  return audio.cached_animation_frame_start <= audio.cached_animation_frame_end;
}

static bool sound_scene_audio_animation_cache_is_compatible(
    const bke::SceneAudioRuntime &audio,
    const std::optional<uint64_t> &frame_change_handlers_hash,
    const int frame_start,
    const int frame_end)
{
  return !audio.animation_cache_dirty && audio.cached_animation_frame_start <= frame_start &&
         audio.cached_animation_frame_end >= frame_end &&
         audio.animation_cache_frame_change_handlers_hash == frame_change_handlers_hash;
}

static bool sound_scene_audio_animation_cache_can_extend(
    const bke::SceneAudioRuntime &audio, const std::optional<uint64_t> &frame_change_handlers_hash)
{
  return !audio.animation_cache_dirty &&
         audio.animation_cache_frame_change_handlers_hash == frame_change_handlers_hash &&
         sound_scene_audio_animation_cache_has_data(audio);
}

static void sound_collect_scene_audio_animation_updates(
    Scene *scene,
    ViewLayer *view_layer,
    Depsgraph *depsgraph,
    const SoundAnimationPreparationMode preparation_mode,
    const std::optional<uint64_t> &frame_change_handlers_hash,
    const int frame_start,
    const int frame_end,
    Set<const Scene *> &visited_scenes,
    Vector<SoundAnimationBakeRequest> &r_requests)
{
  if (!visited_scenes.add(scene)) {
    return;
  }

  if (scene->ed != nullptr) {
    seq::foreach_strip(&scene->ed->seqbase, [&](Strip *strip) {
      if (sound_scene_strip_uses_sequencer(strip)) {
        sound_collect_scene_audio_animation_updates(
            sound_original_scene(strip->scene),
            BKE_view_layer_find(strip->scene, strip->scene_view_layer_name),
            depsgraph,
            preparation_mode,
            frame_change_handlers_hash,
            strip->scene->r.sfra,
            strip->scene->r.efra,
            visited_scenes,
            r_requests);
      }
      return true;
    });
  }

  Scene *scene_eval = DEG_get_evaluated(depsgraph, scene);
  if (!DEG_is_evaluated(scene_eval)) {
    r_requests.append({scene, view_layer, frame_start, frame_end});
    return;
  }

  const bke::SceneAudioRuntime &audio = scene_eval->runtime->audio;
  const bool cache_is_compatible = sound_scene_audio_animation_cache_is_compatible(
      audio, frame_change_handlers_hash, frame_start, frame_end);
  if (preparation_mode == SoundAnimationPreparationMode::ExplicitBake || !cache_is_compatible) {
    int bake_frame_start = frame_start;
    int bake_frame_end = frame_end;
    if (preparation_mode == SoundAnimationPreparationMode::Ensure &&
        sound_scene_audio_animation_cache_can_extend(audio, frame_change_handlers_hash))
    {
      bake_frame_start = min_ii(bake_frame_start, audio.cached_animation_frame_start);
      bake_frame_end = max_ii(bake_frame_end, audio.cached_animation_frame_end);
    }
    r_requests.append({scene, view_layer, bake_frame_start, bake_frame_end});
  }
}

static void sound_evaluate_scene_audio_animation_at_frame(Depsgraph *depsgraph,
                                                          Scene *scene_eval,
                                                          const int frame,
                                                          const eAnimData_Recalc recalc)
{
  BKE_scene_frame_set(scene_eval, float(frame));
  const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(
      depsgraph, BKE_scene_frame_get(scene_eval));
  BKE_animsys_evaluate_animdata(
      &scene_eval->id, scene_eval->adt, &anim_eval_context, recalc, false);
}

static void sound_update_scene_audio_at_frame(Depsgraph *depsgraph, Scene *scene_eval)
{
  if (scene_eval->ed != nullptr) {
    seq::eval_strips(depsgraph, scene_eval, &scene_eval->ed->seqbase);
  }
  BKE_sound_set_scene_volume(scene_eval, scene_eval->audio.volume);
}

static void sound_update_scene_audio_animation_at_frame(Depsgraph *depsgraph,
                                                        Scene *scene_eval,
                                                        const int frame,
                                                        const eAnimData_Recalc recalc)
{
  sound_evaluate_scene_audio_animation_at_frame(depsgraph, scene_eval, frame, recalc);
  sound_update_scene_audio_at_frame(depsgraph, scene_eval);
}

static bool sound_object_has_spatial_audio(const Object *object)
{
  if (object->type != OB_SPEAKER || object->adt == nullptr) {
    return false;
  }

  const Speaker *speaker = id_cast<const Speaker *>(object->data);
  if (speaker->sound == nullptr) {
    return false;
  }

  for (const NlaTrack &track : object->adt->nla_tracks) {
    for (const NlaStrip &strip : track.strips) {
      if (strip.type == NLASTRIP_TYPE_SOUND) {
        return true;
      }
    }
  }
  return false;
}

static bool sound_spatial_audio_speaker_output_ids_recursive(
    Scene *scene,
    ViewLayer *view_layer,
    Depsgraph *target_depsgraph,
    const bool include_embedded_child_caches,
    Set<const ViewLayer *> &visited_view_layers,
    Set<ID *> &ids)
{
  if (view_layer == nullptr) {
    view_layer = BKE_view_layer_default_view(scene);
  }
  if (view_layer == nullptr || !visited_view_layers.add(view_layer)) {
    return false;
  }

  bool has_speakers = false;
  /* The target depsgraph builder has already synchronized every view layer used by a #Scene
   * strip.
   * Only read the original bases here: synchronizing Main from a render worker would be unsafe. */
  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    Object *object = base.object;
    if (object->type != OB_SPEAKER) {
      continue;
    }
    Object *target_object = DEG_get_evaluated(target_depsgraph, object);
    if (!DEG_is_evaluated(target_object) || !sound_object_has_spatial_audio(target_object)) {
      continue;
    }
    ids.add(&object->id);
    has_speakers = true;
  }

  if (scene->ed != nullptr) {
    seq::foreach_strip(&scene->ed->seqbase, [&](Strip *strip) {
      if (strip->type == STRIP_TYPE_SCENE && strip->scene != nullptr &&
          (include_embedded_child_caches || !sound_scene_strip_uses_sequencer(strip)))
      {
        has_speakers |= sound_spatial_audio_speaker_output_ids_recursive(
            strip->scene,
            BKE_view_layer_find(strip->scene, strip->scene_view_layer_name),
            target_depsgraph,
            include_embedded_child_caches,
            visited_view_layers,
            ids);
      }
      return true;
    });
  }
  return has_speakers;
}

static bool sound_spatial_audio_speaker_output_ids_from_depsgraph(Depsgraph *depsgraph,
                                                                  Set<ID *> &ids)
{
  if (!DEG_id_type_any_exists(depsgraph, ID_SPK)) {
    return false;
  }

  bool has_speakers = false;
  DEGObjectIterSettings deg_iter_settings = {nullptr};
  deg_iter_settings.depsgraph = depsgraph;
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_INDIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET;
  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, object) {
    if (!sound_object_has_spatial_audio(object)) {
      continue;
    }
    ids.add(&DEG_get_original(object)->id);
    has_speakers = true;
  }
  DEG_OBJECT_ITER_END;
  return has_speakers;
}

static Vector<ID *> sound_spatial_audio_output_ids(Scene *scene,
                                                   ViewLayer *view_layer,
                                                   Depsgraph *target_depsgraph,
                                                   const bool include_listener_without_speakers)
{
  Set<ID *> ids;
  bool has_speakers;
  if (scene == sound_original_scene(DEG_get_input_scene(target_depsgraph))) {
    /* Match #BKE_sound_update_scene(), including nested and set-scene #Speaker objects already
     * present in the target depsgraph. */
    has_speakers = sound_spatial_audio_speaker_output_ids_from_depsgraph(target_depsgraph, ids);
    if (has_speakers && scene->ed != nullptr) {
      /* Embedded child caches already contain their #Speaker objects. Keep the IDs in the
       * depsgraph for the child bake, but do not also add them directly to the parent sound scene.
       */
      Set<ID *> embedded_child_speaker_ids;
      Set<const ViewLayer *> visited_view_layers;
      seq::foreach_strip(&scene->ed->seqbase, [&](Strip *strip) {
        if (sound_scene_strip_uses_sequencer(strip)) {
          sound_spatial_audio_speaker_output_ids_recursive(
              strip->scene,
              BKE_view_layer_find(strip->scene, strip->scene_view_layer_name),
              target_depsgraph,
              true,
              visited_view_layers,
              embedded_child_speaker_ids);
        }
        return true;
      });

      if (!embedded_child_speaker_ids.is_empty()) {
        Set<ID *> scene_speaker_ids;
        visited_view_layers.clear();
        sound_spatial_audio_speaker_output_ids_recursive(
            scene, view_layer, target_depsgraph, false, visited_view_layers, scene_speaker_ids);
        for (ID *id : embedded_child_speaker_ids) {
          if (!scene_speaker_ids.contains(id)) {
            ids.remove(id);
          }
        }
        has_speakers = !ids.is_empty();
      }
    }
  }
  else {
    Set<const ViewLayer *> visited_view_layers;
    has_speakers = sound_spatial_audio_speaker_output_ids_recursive(
        scene, view_layer, target_depsgraph, false, visited_view_layers, ids);
  }

  if (has_speakers || include_listener_without_speakers) {
    if (scene->camera != nullptr) {
      ids.add(&scene->camera->id);
    }
    for (TimeMarker &marker : scene->markers) {
      if (marker.camera != nullptr) {
        ids.add(&marker.camera->id);
      }
    }
  }
  return Vector<ID *>(ids.begin(), ids.end());
}

static Depsgraph *sound_targeted_audio_depsgraph_build(Main *bmain,
                                                       Scene *scene,
                                                       ViewLayer *view_layer,
                                                       const Depsgraph *target_depsgraph,
                                                       const Span<ID *> output_ids)
{
  if (view_layer == nullptr) {
    view_layer = BKE_view_layer_default_view(scene);
  }
  if (view_layer == nullptr) {
    return nullptr;
  }

  Depsgraph *depsgraph = DEG_graph_new(bmain, scene, view_layer, DEG_get_mode(target_depsgraph));
  /* The #Scene ID is an output because callers consume its evaluated scalar audio-driver values.
   * The other output IDs provide the requested spatial audio dependency closure. */
  DEG_graph_build_from_ids(depsgraph, output_ids);
  return depsgraph;
}

static void sound_copy_scene_audio_driver_values(const Scene *scene,
                                                 const Set<std::string> &paths,
                                                 Scene *source_scene_eval,
                                                 Scene *target_scene_eval)
{
  if (scene->adt == nullptr || source_scene_eval == nullptr || target_scene_eval == nullptr) {
    return;
  }

  PointerRNA source_id_ptr = RNA_id_pointer_create(&source_scene_eval->id);
  PointerRNA target_id_ptr = RNA_id_pointer_create(&target_scene_eval->id);
  for (const FCurve &fcurve : scene->adt->drivers) {
    if (!paths.contains_as(fcurve.rna_path())) {
      continue;
    }

    PathResolvedRNA source_anim_rna;
    if (!BKE_animsys_rna_path_resolve(
            &source_id_ptr, fcurve.rna_path().c_str(), fcurve.array_index, &source_anim_rna))
    {
      continue;
    }

    float value;
    if (!BKE_animsys_read_from_rna_path(&source_anim_rna, &value)) {
      continue;
    }

    PathResolvedRNA target_anim_rna;
    if (BKE_animsys_rna_path_resolve(
            &target_id_ptr, fcurve.rna_path().c_str(), fcurve.array_index, &target_anim_rna))
    {
      BKE_animsys_write_to_rna_path(&target_anim_rna, value);
    }
  }
}

static void sound_update_scene_spatial_audio_at_frame(Depsgraph *evaluation_depsgraph,
                                                      Depsgraph *target_depsgraph,
                                                      Scene *target_scene,
                                                      Scene *source_scene,
                                                      Span<ID *> output_ids,
                                                      int frame);

static void sound_evaluate_spatial_audio_at_frame(Depsgraph *evaluation_depsgraph,
                                                  Depsgraph *target_depsgraph,
                                                  Scene *target_scene,
                                                  Scene *source_scene,
                                                  const Span<ID *> output_ids,
                                                  const int frame)
{
  DEG_evaluate_on_framechange_exact(evaluation_depsgraph, frame);
  sound_update_scene_spatial_audio_at_frame(
      evaluation_depsgraph, target_depsgraph, target_scene, source_scene, output_ids, frame);
  /* This graph is not active, so its original IDs are never cleared by the regular scene update.
   * Avoid retaining copy-on-evaluation recalc flags that would copy the Scene on every frame. */
  DEG_ids_clear_recalc(evaluation_depsgraph, false);
}

static void sound_store_scene_audio_animation_cache(
    Scene *scene_eval,
    const int frame_start,
    const int frame_end,
    const std::optional<uint64_t> &frame_change_handlers_hash)
{
  bke::SceneAudioRuntime &audio = scene_eval->runtime->audio;
  audio.animation_cache_dirty = false;
  audio.animation_cache_frame_change_handlers_hash = frame_change_handlers_hash;
  audio.cached_animation_frame_start = frame_start;
  audio.cached_animation_frame_end = frame_end;
}

static void sound_bake_animation_impl(Scene *scene,
                                      Depsgraph *depsgraph,
                                      Depsgraph *targeted_depsgraph,
                                      const Span<ID *> spatial_output_ids,
                                      const Set<std::string> &audio_animation_paths,
                                      const SoundAnimationEvaluationMode evaluation_mode,
                                      const std::optional<uint64_t> &frame_change_handlers_hash,
                                      const int frame_start,
                                      const int frame_end)
{
  BLI_assert(evaluation_mode != SoundAnimationEvaluationMode::TargetedDependencies ||
             targeted_depsgraph != nullptr);
  BLI_assert(evaluation_mode != SoundAnimationEvaluationMode::FullScene ||
             frame_change_handlers_hash.has_value());

  const int oldfra = scene->r.cfra;
  const float oldsubframe = scene->r.subframe;
  const bool is_root_scene = scene == sound_original_scene(DEG_get_input_scene(depsgraph));
  Scene *depsgraph_input_scene = const_cast<Scene *>(DEG_get_input_scene(depsgraph));

  const int oldfra_depsgraph_input = depsgraph_input_scene->r.cfra;
  const float oldsubframe_depsgraph_input = depsgraph_input_scene->r.subframe;

  const auto restore_scene_times = [&]() {
    scene->r.cfra = oldfra;
    scene->r.subframe = oldsubframe;
    depsgraph_input_scene->r.cfra = oldfra_depsgraph_input;
    depsgraph_input_scene->r.subframe = oldsubframe_depsgraph_input;
  };

  Scene *scene_eval = DEG_get_evaluated(depsgraph, scene);
  if (!DEG_is_evaluated(scene_eval)) {
    return;
  }
  scene_eval->runtime->audio.animation_write_state = bke::AudioAnimationWriteState::Bake;
  sound_reset_scene_audio_animation(scene_eval);

  const int bake_frame_start = (frame_start > 0) ? (frame_start - 1) : 0;
  const int bake_frame_end = frame_end + 1;
  if (evaluation_mode != SoundAnimationEvaluationMode::FullScene) {
    for (int cfra = bake_frame_start; cfra <= bake_frame_end; cfra++) {
      if (evaluation_mode == SoundAnimationEvaluationMode::Direct) {
        sound_update_scene_audio_animation_at_frame(depsgraph, scene_eval, cfra, ADT_RECALC_ANIM);
      }
      else {
        DEG_evaluate_on_framechange_exact(targeted_depsgraph, float(cfra));
        Scene *targeted_scene_eval = DEG_get_evaluated_scene(targeted_depsgraph);
        sound_evaluate_scene_audio_animation_at_frame(
            depsgraph, scene_eval, cfra, ADT_RECALC_ANIM);
        sound_copy_scene_audio_driver_values(
            scene, audio_animation_paths, targeted_scene_eval, scene_eval);
        sound_update_scene_audio_at_frame(depsgraph, scene_eval);
        if (!spatial_output_ids.is_empty()) {
          sound_update_scene_spatial_audio_at_frame(
              targeted_depsgraph, depsgraph, scene_eval, scene, spatial_output_ids, cfra);
        }
        /* This graph is not active, so its original IDs are never cleared by the regular scene
         * update. Avoid retaining copy-on-evaluation recalc flags that would copy the #Scene on
         * every frame. */
        DEG_ids_clear_recalc(targeted_depsgraph, false);
      }
      if (evaluation_mode == SoundAnimationEvaluationMode::Direct && targeted_depsgraph != nullptr)
      {
        sound_evaluate_spatial_audio_at_frame(
            targeted_depsgraph, depsgraph, scene_eval, scene, spatial_output_ids, cfra);
      }
    }
  }
  else {
    /* Only explicit handler baking may use full root #Scene evaluation. Automatic preparation
     * returns before reaching this branch when handlers are present. */
    for (int cfra = bake_frame_start; cfra <= bake_frame_end; cfra++) {
      BKE_scene_frame_set(scene, float(cfra));
      BKE_scene_frame_set(depsgraph_input_scene, float(cfra));
      /* Make sure the camera is updated, since it will affect stereo/surround output. */
      BKE_scene_camera_switch_update(scene);

      if (is_root_scene) {
        BKE_scene_graph_update_for_newframe(depsgraph);
      }
      else {
        DEG_evaluate_on_framechange(depsgraph, cfra);
        scene_eval = DEG_get_evaluated(depsgraph, scene);
        if (DEG_is_evaluated(scene_eval)) {
          sound_update_scene_audio_animation_at_frame(depsgraph, scene_eval, cfra, ADT_RECALC_ALL);
          if (targeted_depsgraph != nullptr) {
            sound_evaluate_spatial_audio_at_frame(
                targeted_depsgraph, depsgraph, scene_eval, scene, spatial_output_ids, cfra);
          }
          else if (!spatial_output_ids.is_empty()) {
            sound_update_scene_spatial_audio_at_frame(
                depsgraph, depsgraph, scene_eval, scene, spatial_output_ids, cfra);
          }
        }
      }
    }

    restore_scene_times();
    BKE_scene_camera_switch_update(scene);
  }

  {
    SoundAudioAnimationPreserveGuard preserve_audio_animation(depsgraph);
    if (is_root_scene) {
      BKE_scene_graph_update_for_newframe(depsgraph);
    }
    else {
      DEG_evaluate_on_framechange_exact(depsgraph,
                                        BKE_scene_frame_get(DEG_get_input_scene(depsgraph)));
    }
  }

  scene_eval = DEG_get_evaluated(depsgraph, scene);
  if (DEG_is_evaluated(scene_eval)) {
    scene_eval->runtime->audio.animation_write_state = bke::AudioAnimationWriteState::Normal;
    sound_store_scene_audio_animation_cache(
        scene_eval, frame_start, frame_end, frame_change_handlers_hash);
  }
}

#ifdef WITH_AUDASPACE
class SoundPlaybackPauseGuard {
 public:
  SoundPlaybackPauseGuard(Depsgraph *depsgraph, Scene *scene)
      : depsgraph_(depsgraph), scene_(scene)
  {
    Scene *scene_eval = DEG_get_evaluated(depsgraph_, scene_);
    if (!DEG_is_evaluated(scene_eval) || !scene_eval->runtime->audio.playback_handle) {
      return;
    }
    if (scene_eval->runtime->audio.playback_handle->getStatus() != aud::STATUS_PLAYING) {
      return;
    }

    was_playing_ = true;
    BKE_sound_stop_scene(scene_eval);
  }

  ~SoundPlaybackPauseGuard()
  {
    if (!was_playing_) {
      return;
    }
    Scene *scene_eval = DEG_get_evaluated(depsgraph_, scene_);
    if (DEG_is_evaluated(scene_eval)) {
      BKE_sound_play_scene(scene_eval);
    }
  }

  SoundPlaybackPauseGuard(const SoundPlaybackPauseGuard &) = delete;
  SoundPlaybackPauseGuard &operator=(const SoundPlaybackPauseGuard &) = delete;

 private:
  Depsgraph *depsgraph_;
  Scene *scene_;
  bool was_playing_ = false;
};
#endif

static void sound_bake_scene_audio_animation(Main *bmain,
                                             Scene *scene,
                                             Depsgraph *depsgraph,
                                             const SoundAnimationPreparationMode preparation_mode,
                                             const int frame_start,
                                             const int frame_end)
{
#ifndef WITH_AUDASPACE
  UNUSED_VARS(bmain, scene, depsgraph, preparation_mode, frame_start, frame_end);
  return;
#endif

  std::optional<uint64_t> frame_change_handlers_hash;
#ifdef WITH_PYTHON
  frame_change_handlers_hash = BPY_app_handlers_frame_change_identity();
#endif

  /* Flush pending user edits before inspecting the cache. This also ensures that a render graph's
   * #Scene objects exist. */
  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);

#ifdef WITH_AUDASPACE
  std::optional<SoundPlaybackPauseGuard> pause_playback;
#endif

  if (frame_change_handlers_hash.has_value() &&
      preparation_mode == SoundAnimationPreparationMode::Ensure)
  {
    /* Handler identity is not a dependency-graph input, so a cache made with one handler set
     * cannot be reused safely after the set changes while remaining non-empty. */
    Set<const Scene *> visited_scenes;
    Vector<SoundAnimationBakeRequest> requests;
    sound_collect_scene_audio_animation_updates(scene,
                                                DEG_get_input_view_layer(depsgraph),
                                                depsgraph,
                                                preparation_mode,
                                                frame_change_handlers_hash,
                                                frame_start,
                                                frame_end,
                                                visited_scenes,
                                                requests);
#ifdef WITH_AUDASPACE
    for (const SoundAnimationBakeRequest &request : requests) {
      Scene *scene_eval = DEG_get_evaluated(depsgraph, request.scene);
      if (!DEG_is_evaluated(scene_eval)) {
        continue;
      }

      const bke::SceneAudioRuntime &audio = scene_eval->runtime->audio;
      if (sound_scene_audio_animation_cache_has_data(audio)) {
        if (!pause_playback.has_value()) {
          pause_playback.emplace(depsgraph, scene);
        }
        sound_discard_scene_audio_animation_cache(depsgraph, scene_eval);
      }
      sound_store_scene_audio_animation_cache(scene_eval, 1, 0, frame_change_handlers_hash);
    }
#endif

    /* Implicit preparation must not run arbitrary callbacks across future frames. Playback and
     * rendering evaluate handlers as they advance; explicit cache generation and mixdown opt in.
     */
    return;
  }

  Set<const Scene *> visited_scenes;
  Vector<SoundAnimationBakeRequest> requests;
  sound_collect_scene_audio_animation_updates(scene,
                                              DEG_get_input_view_layer(depsgraph),
                                              depsgraph,
                                              preparation_mode,
                                              frame_change_handlers_hash,
                                              frame_start,
                                              frame_end,
                                              visited_scenes,
                                              requests);
  if (requests.is_empty()) {
    return;
  }

#ifdef WITH_AUDASPACE
  pause_playback.emplace(depsgraph, scene);
#endif

  DEG_id_tag_update_for_side_effect_request(depsgraph, &scene->id, ID_RECALC_SEQUENCER_STRIPS);
  for (const SoundAnimationBakeRequest &request : requests) {
    Scene *scene_update = request.scene;
    if (scene_update != scene) {
      DEG_id_tag_update_for_side_effect_request(
          depsgraph,
          &scene_update->id,
          IDRecalcFlag(ID_RECALC_SEQUENCER_STRIPS | ID_RECALC_ANIMATION));
    }
  }
  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);

  for (const SoundAnimationBakeRequest &request : requests) {
    Scene *scene_update = request.scene;
    Scene *scene_eval = DEG_get_evaluated(depsgraph, scene_update);
    if (!DEG_is_evaluated(scene_eval)) {
      continue;
    }

    bool can_update_audio = true;
    if (scene_eval->ed != nullptr) {
      seq::foreach_strip(&scene_eval->ed->seqbase, [&](Strip *strip) {
        can_update_audio = !sound_scene_strip_uses_sequencer(strip) ||
                           DEG_is_evaluated(strip->scene);
        return can_update_audio;
      });
    }
    if (!can_update_audio) {
      continue;
    }

    const Set<std::string> audio_animation_paths = sound_audio_animation_paths(scene_update);
    const Vector<ID *> spatial_output_ids = sound_spatial_audio_output_ids(
        scene_update, request.view_layer, depsgraph, frame_change_handlers_hash.has_value());
    const bool has_audio_animation = sound_scene_has_audio_animation(scene_update,
                                                                     audio_animation_paths);
    const bool needs_frame_bake = frame_change_handlers_hash.has_value() || has_audio_animation ||
                                  !spatial_output_ids.is_empty();
    const SoundAnimationEvaluationMode evaluation_mode = sound_scene_audio_evaluation_mode(
        scene_update, audio_animation_paths, frame_change_handlers_hash);
    const bool needs_targeted_spatial = !spatial_output_ids.is_empty() &&
                                        (evaluation_mode !=
                                             SoundAnimationEvaluationMode::FullScene ||
                                         scene_update != scene);
    VectorSet<ID *> targeted_output_ids;
    if (needs_frame_bake && evaluation_mode == SoundAnimationEvaluationMode::TargetedDependencies)
    {
      targeted_output_ids.add(&scene_update->id);
    }
    if (needs_targeted_spatial) {
      targeted_output_ids.add_multiple(spatial_output_ids.as_span());
    }
    Depsgraph *targeted_depsgraph = nullptr;
    if (!targeted_output_ids.is_empty()) {
      targeted_depsgraph = sound_targeted_audio_depsgraph_build(
          bmain, scene_update, request.view_layer, depsgraph, targeted_output_ids);
    }
    if (needs_frame_bake) {
      sound_bake_animation_impl(scene_update,
                                depsgraph,
                                targeted_depsgraph,
                                spatial_output_ids,
                                audio_animation_paths,
                                evaluation_mode,
                                frame_change_handlers_hash,
                                request.frame_start,
                                request.frame_end);
    }
    else {
      sound_reset_scene_audio_animation(scene_eval);
      if (scene_eval->ed != nullptr) {
        seq::eval_strips(depsgraph, scene_eval, &scene_eval->ed->seqbase);
      }
      BKE_sound_update_scene(depsgraph, scene_eval);
      BKE_sound_set_scene_volume(scene_eval, scene_eval->audio.volume);
      sound_store_scene_audio_animation_cache(
          scene_eval, request.frame_start, request.frame_end, frame_change_handlers_hash);
    }
    if (targeted_depsgraph != nullptr) {
      DEG_graph_free(targeted_depsgraph);
    }
  }
}

void BKE_sound_bake_animation(
    Main *bmain, Scene *scene, Depsgraph *depsgraph, const int frame_start, const int frame_end)
{
  scene = sound_original_scene(scene);
  sound_bake_scene_audio_animation(bmain,
                                   scene,
                                   depsgraph,
                                   SoundAnimationPreparationMode::ExplicitBake,
                                   frame_start,
                                   frame_end);
}

void BKE_sound_ensure_scene_audio_animation(
    Main *bmain, Scene *scene, Depsgraph *depsgraph, const int frame_start, const int frame_end)
{
  scene = sound_original_scene(scene);
  sound_bake_scene_audio_animation(
      bmain, scene, depsgraph, SoundAnimationPreparationMode::Ensure, frame_start, frame_end);
}

static void sound_free_audio(bSound *sound)
{
#ifdef WITH_AUDASPACE
  bke::SoundRuntime *runtime = sound->runtime;
  sound_runtime_generation_replace(sound, nullptr);
  runtime->handle.reset();
  runtime->playback_handle.reset();
  runtime->cache.reset();
#else
  UNUSED_VARS(sound);
#endif /* WITH_AUDASPACE */
}

#ifdef WITH_AUDASPACE
static CLG_LogRef LOG = {"sound"};

namespace {

struct GlobalState {
  const char *force_device = nullptr;

  /* Parameters of the opened device */
  const char *device_name = nullptr;
  aud::DeviceSpecs initialized_specs;

  /* Device handle and its synchronization mutex. */
  AUD_Device sound_device;
  int buffer_size = 0;
  std::mutex sound_device_mutex;

  bool need_exit = false;
  bool use_delayed_close = true;
  std::thread delayed_close_thread;
  std::condition_variable delayed_close_cv;

  int num_device_users = 0;
  std::chrono::time_point<std::chrono::steady_clock> last_user_disconnect_time_point;

  ~GlobalState()
  {
    /* Ensure that we don't end up in a deadlock if the global state is being cleaned up
     * before BKE_sound_exit_once has been called. (For example if someone called exit()
     * to quickly close the program without cleaning up)
     *
     * If we don't do this, we could end up in a state where this destructor is waiting for
     * other threads to let go of delayed_close_cv forever. See #146640.
     */
    exit_threads();
  }

  void exit_threads()
  {
    {
      std::unique_lock lock(sound_device_mutex);
      need_exit = true;
    }

    if (delayed_close_thread.joinable()) {
      delayed_close_cv.notify_all();
      delayed_close_thread.join();
    }
  }
};

GlobalState g_state;
}  // namespace

static void sound_device_close_no_lock()
{
  if (g_state.sound_device) {
    CLOG_DEBUG(&LOG, "Closing audio device");
    bke::sound_device_exit();
    g_state.sound_device = nullptr;
  }
}

static void sound_device_open_no_lock(const aud::DeviceSpecs &requested_specs)
{
  BLI_assert(!g_state.sound_device);

  CLOG_DEBUG(&LOG, "Opening audio device name:%s", g_state.device_name);

  g_state.sound_device = bke::sound_device_init(
      g_state.device_name, requested_specs, g_state.buffer_size, "Blender");
  if (!g_state.sound_device) {
    g_state.sound_device = bke::sound_device_init(
        "None", requested_specs, g_state.buffer_size, "Blender");
  }

  g_state.initialized_specs = g_state.sound_device->getSpecs();
}

static void sound_device_use_begin()
{
  ++g_state.num_device_users;

  if (g_state.sound_device) {
    return;
  }

  sound_device_open_no_lock(g_state.initialized_specs);
}

static void sound_device_use_end_after(const std::chrono::milliseconds after_ms)
{
  BLI_assert(g_state.num_device_users > 0);
  if (g_state.num_device_users == 0) {
    return;
  }

  --g_state.num_device_users;
  if (g_state.num_device_users == 0) {
    g_state.last_user_disconnect_time_point = std::chrono::steady_clock::now() + after_ms;
    g_state.delayed_close_cv.notify_one();
  }
}

static void sound_device_use_end()
{
  sound_device_use_end_after(std::chrono::milliseconds(0));
}

/* Return true if we need a thread which checks for device usage and closes it when it is inactive.
 * Only runtime-invariant checks are done here, such as possible platform-specific requirements.
 */
static bool sound_use_close_thread()
{
  /* No point starting a thread if sound is disabled and we're running headless. */
  if (g_state.force_device && STREQ(g_state.force_device, "None")) {
#  if defined(WITH_PYTHON_MODULE) || defined(WITH_HEADLESS)
    return false;
#  endif
    if (G.background) {
      return false;
    }
  }

#  if OS_MAC
  /* Closing audio device on macOS prior to 15.2 could lead to interference with other software.
   * See #121911 for details. */
  if (__builtin_available(macOS 15.2, *)) {
    return true;
  }
  return false;
#  else
  return true;
#  endif
}

static void delayed_close_thread_run()
{
  constexpr std::chrono::milliseconds device_close_delay{30000};

  std::unique_lock lock(g_state.sound_device_mutex);

  while (!g_state.need_exit) {
    if (!g_state.use_delayed_close) {
      CLOG_DEBUG(&LOG, "Delayed device close is disabled");
      /* Don't do anything here as delayed close is disabled.
       * Wait so that we don't spin around in the while loop. */
      g_state.delayed_close_cv.wait(lock);
      continue;
    }

    if (g_state.num_device_users == 0) {
      if (g_state.sound_device == nullptr) {
        /* There are no device users, wait until there is device to be waited for to close. */
        g_state.delayed_close_cv.wait(lock);
      }
      else {
        g_state.delayed_close_cv.wait_until(
            lock, g_state.last_user_disconnect_time_point + device_close_delay);
      }
    }
    else {
      /* If there are active device users wait indefinitely, until the system is requested to be
       * closed or the user stops using device.
       * It is not really guaranteed that the CV is notified for every user that stops using
       * device, only the last one is guaranteed to notify the CV. */
      g_state.delayed_close_cv.wait(lock);
    }

    if (g_state.need_exit) {
      CLOG_DEBUG(&LOG, "System exit requested");
      break;
    }

    if (!g_state.use_delayed_close) {
      /* Take into account corner case where you switch from a delayed close device while Blender
       * is running and a delayed close has already been queued up. */
      continue;
    }

    if (!g_state.sound_device) {
      CLOG_DEBUG(&LOG, "Device is not open, nothing to do");
      continue;
    }

    CLOG_DEBUG(&LOG, "Checking last device usage and timestamp");

    if (g_state.num_device_users) {
      CLOG_DEBUG(&LOG, "Device is used by %d user(s)", g_state.num_device_users);
      continue;
    }

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if ((now - g_state.last_user_disconnect_time_point) >= device_close_delay) {
      sound_device_close_no_lock();
    }
  }

  CLOG_DEBUG(&LOG, "Delayed device close thread finished");
}

static SoundJackSyncCallback sound_jack_sync_callback = nullptr;

static void sound_sync_callback(void *data, int mode, float time)
{
  if (sound_jack_sync_callback == nullptr) {
    return;
  }
  Main *bmain = (Main *)data;
  sound_jack_sync_callback(bmain, mode, time);
}

void BKE_sound_force_device(const char *device)
{
  g_state.force_device = device;
}

void BKE_sound_init_once()
{
  bke::sound_system_initialize();
  if (sound_use_close_thread()) {
    CLOG_DEBUG(&LOG, "Using delayed device close thread");
    g_state.delayed_close_thread = std::thread(delayed_close_thread_run);
  }
}

void BKE_sound_exit_once()
{
  g_state.exit_threads();

  std::lock_guard lock(g_state.sound_device_mutex);
  sound_device_close_no_lock();

  if (audio_device_names != nullptr) {
    int i;
    for (i = 0; audio_device_names[i]; i++) {
      free(audio_device_names[i]);
    }
    free(audio_device_names);
    audio_device_names = nullptr;
  }
}

void BKE_sound_init(Main *bmain)
{
  std::lock_guard lock(g_state.sound_device_mutex);
  /* Make sure no instance of the sound system is running, otherwise we get leaks. */
  sound_device_close_no_lock();

  aud::DeviceSpecs requested_specs;
  requested_specs.channels = aud::Channels(U.audiochannels);
  requested_specs.format = aud::SampleFormat(U.audioformat);
  requested_specs.rate = U.audiorate;

  if (g_state.force_device == nullptr) {
    char **names = BKE_sound_get_device_names();
    g_state.device_name = names[0];

    /* make sure device is within the bounds of the array */
    for (int i = 0; names[i]; i++) {
      if (i == U.audiodevice) {
        g_state.device_name = names[i];
      }
    }
  }
  else {
    g_state.device_name = g_state.force_device;
  }

  g_state.buffer_size = U.mixbufsize < 128 ? 1024 : U.mixbufsize;

  if (requested_specs.rate < double(aud::RATE_8000)) {
    requested_specs.rate = aud::RATE_48000;
  }

  if (requested_specs.format <= aud::FORMAT_INVALID) {
    requested_specs.format = aud::FORMAT_S16;
  }

  if (requested_specs.channels <= aud::CHANNELS_INVALID) {
    requested_specs.channels = aud::CHANNELS_STEREO;
  }

  /* Make sure that we have our initalized_specs */
  sound_device_open_no_lock(requested_specs);
  if (STR_ELEM(g_state.device_name, "JACK", "PulseAudio", "PipeWire")) {
    /* JACK:
     * Do not close the device when using JACK. If we close it, we will not be able to
     * respond to JACK audio bus commands.
     *
     * PulseAudio, PipeWire:
     * These APIs are built around the idea that the program using them keeps the device open.
     * Instead it uses audio streams to determine if something is playing back audio or not.
     * These streams are only active when Audaspace is playing back, so we don't need to
     * do anything manually.
     * If we close these devices, it will become very hard and tedious for end users to
     * control the volume or route audio from Blender.
     */
    g_state.use_delayed_close = false;
    aud::DeviceManager::getDevice()->setSyncCallback(sound_sync_callback, bmain);
  }
  else {
    g_state.use_delayed_close = true;
    sound_device_close_no_lock();
  }
}

void BKE_sound_refresh_callback_bmain(Main *bmain)
{
  std::lock_guard lock(g_state.sound_device_mutex);
  if (g_state.sound_device) {
    aud::DeviceManager::getDevice()->setSyncCallback(sound_sync_callback, bmain);
  }
}

static void sound_load_audio(Main *bmain, bSound *sound, bool free_waveform)
{
  bke::SoundRuntime *runtime = sound->runtime;
  sound_runtime_generation_replace(sound, nullptr);
  runtime->cache.reset();
  runtime->handle.reset();
  runtime->playback_handle.reset();
  if (free_waveform) {
    sound_free_waveform(sound);
  }

  {
    char fullpath[FILE_MAX];

    /* load sound */
    PackedFile *pf = sound->packedfile;

    /* Don't modify `sound->filepath`, only change a copy. */
    STRNCPY(fullpath, sound->filepath);
    BLI_path_abs(fullpath, ID_BLEND_PATH(bmain, &sound->id));

    /* but we need a packed file then */
    if (pf) {
      runtime->handle = AUD_Sound(new aud::File((uchar *)pf->data, pf->size, sound->stream_index));
    }
    else {
      /* or else load it from disk */
      runtime->handle = AUD_Sound(new aud::File(fullpath, sound->stream_index));
    }
  }
  if (sound->flags & SOUND_FLAGS_MONO) {
    aud::DeviceSpecs specs;
    specs.channels = aud::CHANNELS_MONO;
    specs.rate = aud::RATE_INVALID;
    specs.format = aud::FORMAT_INVALID;
    runtime->handle = AUD_Sound(new aud::ChannelMapper(runtime->handle, specs));
  }

  if (sound->flags & SOUND_FLAGS_CACHING) {
    try {
      runtime->cache = AUD_Sound(new aud::StreamBuffer(runtime->handle));
    }
    catch (aud::Exception &ex) {
      (void)ex;
    }
  }

  if (runtime->cache) {
    runtime->playback_handle = runtime->cache;
  }
  else {
    runtime->playback_handle = runtime->handle;
  }

  auto generation = std::make_shared<bke::SoundRuntimeGeneration>();
  generation->handle = runtime->handle;
  generation->reader_cache = std::make_shared<bke::SoundReaderCache>(generation->handle);
  sound_runtime_generation_replace(sound, std::move(generation));
}

void BKE_sound_load(Main *bmain, bSound *sound)
{
  sound_verify_evaluated_id(&sound->id);
  sound_load_audio(bmain, sound, true);
}

void BKE_sound_packfile_ensure(Main *bmain, bSound *sound, ReportList *reports)
{
  if (sound->packedfile != nullptr) {
    /* Sound is already packed and considered unmodified, do not attempt to repack it, since its
     * original file may not be available anymore on the current FS.
     *
     * See #152638.
     */
    return;
  }

  sound->packedfile = BKE_packedfile_new(
      reports, sound->filepath, ID_BLEND_PATH(bmain, &sound->id));
}

AUD_Device BKE_sound_mixdown(const Scene *scene,
                             const aud::DeviceSpecs &specs,
                             int start,
                             float volume)
{
  sound_verify_evaluated_id(&scene->id);
  try {
    std::shared_ptr<aud::ReadDevice> device(new aud::ReadDevice(specs));
    device->setQuality(aud::ResampleQuality::MEDIUM);
    device->setVolume(volume);

    aud::Sequence *f = dynamic_cast<aud::Sequence *>(scene->runtime->audio.sound_scene.get());
    f->setSpecs(specs.specs);

    AUD_Handle handle = device->play(f->createQualityReader(aud::ResampleQuality::MEDIUM));
    if (handle.get()) {
      handle->seek(start / scene->frames_per_second());
    }

    return device;
  }
  catch (aud::Exception &) {
    return nullptr;
  }
}

void BKE_sound_create_scene(Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

  /* should be done in version patch, but this gets called before */
  if (scene->r.frs_sec_base == 0) {
    scene->r.frs_sec_base = 1;
  }

  bke::SceneAudioRuntime &audio = scene->runtime->audio;

  aud::Specs specs;
  specs.channels = aud::CHANNELS_STEREO;
  specs.rate = aud::RATE_48000;
  audio.sound_scene = std::make_shared<aud::Sequence>(
      specs, scene->frames_per_second(), scene->audio.flag & AUDIO_MUTE);
  audio.sound_scene->setSpeedOfSound(scene->audio.speed_of_sound);
  audio.sound_scene->setDopplerFactor(scene->audio.doppler_factor);
  audio.sound_scene->setDistanceModel(aud::DistanceModel(scene->audio.distance_model));
  audio.playback_handle = nullptr;
  audio.sound_scrub_handle = nullptr;
  audio.speaker_handles.clear();
  audio.animation_cache_dirty = true;
  audio.animation_write_state = bke::AudioAnimationWriteState::Normal;
  audio.animation_cache_frame_change_handlers_hash.reset();
  audio.cached_animation_frame_start = 1;
  audio.cached_animation_frame_end = 0;
}

void BKE_sound_destroy_scene(Scene *scene)
{
  bke::SceneAudioRuntime &audio = scene->runtime->audio;
  if (audio.playback_handle) {
    audio.playback_handle->stop();
    audio.playback_handle.reset();
  }
  if (audio.sound_scrub_handle) {
    audio.sound_scrub_handle->stop();
    audio.sound_scrub_handle.reset();
  }
  for (AUD_SequenceEntry handle : audio.speaker_handles) {
    audio.sound_scene->remove(handle);
  }
  audio.speaker_handles.clear();
  audio.sound_scene.reset();
  audio.animation_cache_dirty = true;
  audio.animation_write_state = bke::AudioAnimationWriteState::Normal;
  audio.animation_cache_frame_change_handlers_hash.reset();
  audio.cached_animation_frame_start = 1;
  audio.cached_animation_frame_end = 0;
}

void BKE_sound_lock()
{
  g_state.sound_device_mutex.lock();
  if (g_state.sound_device != nullptr) {
    g_state.sound_device->lock();
  }
}

void BKE_sound_unlock()
{
  g_state.sound_device_mutex.unlock();
  if (g_state.sound_device != nullptr) {
    g_state.sound_device->unlock();
  }
}

void BKE_sound_reset_scene_specs(Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

  if (scene->runtime->audio.sound_scene) {
    scene->runtime->audio.sound_scene->setSpecs(g_state.initialized_specs.specs);
  }
}

void BKE_sound_mute_scene(Scene *scene, bool muted)
{
  sound_verify_evaluated_id(&scene->id);
  if (scene->runtime->audio.sound_scene) {
    scene->runtime->audio.sound_scene->mute(muted);
  }
}

void BKE_sound_update_fps(Main *bmain, Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

  if (scene->runtime->audio.sound_scene) {
    scene->runtime->audio.sound_scene->setFPS(scene->frames_per_second());
  }

  seq::sound_update_length(bmain, scene);
}

void BKE_sound_update_scene_listener(Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

  AUD_Sequence sound = scene->runtime->audio.sound_scene;
  sound->setSpeedOfSound(scene->audio.speed_of_sound);
  sound->setDopplerFactor(scene->audio.doppler_factor);
  sound->setDistanceModel(aud::DistanceModel(scene->audio.distance_model));
}

AUD_SequenceEntry BKE_sound_scene_add_scene_sound(Scene *scene, Strip *strip)
{
  sound_verify_evaluated_id(&scene->id);
  if (strip->scene && scene != strip->scene) {
    int startframe = strip->left_handle();
    int endframe = strip->right_handle(scene);
    int frameskip = strip->startofs + strip->anim_startofs;
    const double fps = scene->frames_per_second();
    return AUD_SequenceEntry(
        scene->runtime->audio.sound_scene->add(strip->scene->runtime->audio.sound_scene,
                                               startframe / fps,
                                               endframe / fps,
                                               frameskip / fps));
  }
  return nullptr;
}

AUD_SequenceEntry BKE_sound_add_scene_sound(Scene *scene, Strip *strip)
{
  sound_verify_evaluated_id(&scene->id);
  /* Happens when sequence's sound data-block was removed. */
  if (strip->sound == nullptr) {
    return nullptr;
  }
  sound_verify_evaluated_id(&strip->sound->id);

  int startframe = strip->left_handle();
  int endframe = strip->right_handle(scene);
  int frameskip = strip->startofs + strip->anim_startofs;

  const double fps = scene->frames_per_second();
  const double offset_time = strip->sound->offset_time + strip->sound_offset - frameskip / fps;
  if (offset_time >= 0.0f) {
    return AUD_SequenceEntry(
        scene->runtime->audio.sound_scene->add(strip->sound->runtime->playback_handle,
                                               startframe / fps + offset_time,
                                               endframe / fps,
                                               0.0f));
  }
  return AUD_SequenceEntry(scene->runtime->audio.sound_scene->add(
      strip->sound->runtime->playback_handle, startframe / fps, endframe / fps, -offset_time));
}

void BKE_sound_remove_scene_sound(Scene *scene, AUD_SequenceEntry handle)
{
  scene->runtime->audio.sound_scene->remove(handle);
}

void BKE_sound_mute_scene_sound(AUD_SequenceEntry handle, bool mute)
{
  handle->mute(mute);
}

void BKE_sound_move_scene_sound(const Scene *scene,
                                AUD_SequenceEntry handle,
                                int startframe,
                                int endframe,
                                int frameskip,
                                double audio_offset)
{
  sound_verify_evaluated_id(&scene->id);
  const double fps = scene->frames_per_second();
  const double offset_time = audio_offset - frameskip / fps;
  if (offset_time >= 0.0f) {
    handle->move(startframe / fps + offset_time, endframe / fps, 0.0f);
  }
  else {
    handle->move(startframe / fps, endframe / fps, -offset_time);
  }
}

void BKE_sound_move_scene_sound_defaults(Scene *scene, Strip *strip)
{
  sound_verify_evaluated_id(&scene->id);
  if (strip->runtime->scene_sound) {
    double offset_time = 0.0f;
    if (strip->sound != nullptr) {
      offset_time = strip->sound->offset_time + strip->sound_offset;
    }
    BKE_sound_move_scene_sound(scene,
                               strip->runtime->scene_sound,
                               strip->left_handle(),
                               strip->right_handle(scene),
                               strip->startofs + strip->anim_startofs,
                               offset_time);
  }
}

void BKE_sound_update_scene_sound(AUD_SequenceEntry handle, bSound *sound)
{
  handle->setSound(sound->runtime->playback_handle);
}

#endif /* WITH_AUDASPACE */

void BKE_sound_update_sequence_handle(AUD_SequenceEntry handle, AUD_Sound sound_handle)
{
#ifdef WITH_AUDASPACE
  handle->setSound(sound_handle);
#else
  UNUSED_VARS(handle, sound_handle);
#endif
}

#ifdef WITH_AUDASPACE

template<typename T>
static void set_audaspace_anim_property(const Scene *scene,
                                        const std::shared_ptr<T> &sound,
                                        aud::AnimateablePropertyType type,
                                        const int frame,
                                        float *value,
                                        bool animated)
{
  if (sound_audio_animation_is_preserved(scene)) {
    return;
  }
  aud::AnimateableProperty *prop = sound->getAnimProperty(type);
  prop->writePreserveAnimation(value, frame, animated);
}

void BKE_sound_set_scene_volume(Scene *scene, float volume)
{
  sound_verify_evaluated_id(&scene->id);
  if (scene->runtime->audio.sound_scene == nullptr) {
    return;
  }
  const bool animated = scene->runtime->audio.animation_write_state ==
                        bke::AudioAnimationWriteState::Bake;
  const int frame = scene->r.cfra;
  set_audaspace_anim_property(
      scene, scene->runtime->audio.sound_scene, aud::AP_VOLUME, frame, &volume, animated);
}

void BKE_sound_set_scene_sound_volume_at_frame(
    const Scene *scene, AUD_SequenceEntry handle, const int frame, float volume, bool animated)
{
  set_audaspace_anim_property(scene, handle, aud::AP_VOLUME, frame, &volume, animated);
}

void BKE_sound_set_scene_sound_pitch_at_frame(
    const Scene *scene, AUD_SequenceEntry handle, const int frame, float pitch, bool animated)
{
  set_audaspace_anim_property(scene, handle, aud::AP_PITCH, frame, &pitch, animated);
}

void BKE_sound_set_scene_sound_pitch_constant_range(
    const Scene *scene, AUD_SequenceEntry handle, int frame_start, int frame_end, float pitch)
{
  if (sound_audio_animation_is_preserved(scene)) {
    return;
  }
  frame_start = max_ii(0, frame_start);
  frame_end = max_ii(0, frame_end);
  aud::AnimateableProperty *prop = handle->getAnimProperty(aud::AP_PITCH);
  prop->writeConstantRange(&pitch, frame_start, frame_end);
}

void BKE_sound_set_scene_sound_pan_at_frame(
    const Scene *scene, AUD_SequenceEntry handle, const int frame, float pan, bool animated)
{
  set_audaspace_anim_property(scene, handle, aud::AP_PANNING, frame, &pan, animated);
}

static void sound_reset_scene_audio_animation(Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

  BKE_sound_lock();

  const float default_location[3] = {0.0f, 0.0f, 0.0f};
  const float default_orientation[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float default_value = 1.0f;
  /* #AnimateableProperty::reset() keeps the first value. Restore Audaspace's defaults before
   * rewriting the properties for the current scene. */

  if (scene->runtime->audio.sound_scene != nullptr) {
    aud::AnimateableProperty *volume = scene->runtime->audio.sound_scene->getAnimProperty(
        aud::AP_VOLUME);
    volume->reset();
    aud::AnimateableProperty *location = scene->runtime->audio.sound_scene->getAnimProperty(
        aud::AP_LOCATION);
    location->reset();
    location->write(default_location);
    aud::AnimateableProperty *orientation = scene->runtime->audio.sound_scene->getAnimProperty(
        aud::AP_ORIENTATION);
    orientation->reset();
    orientation->write(default_orientation);
  }

  for (const AUD_SequenceEntry &handle : scene->runtime->audio.speaker_handles) {
    aud::AnimateableProperty *volume = handle->getAnimProperty(aud::AP_VOLUME);
    volume->reset();
    volume->write(&default_value);
    aud::AnimateableProperty *pitch = handle->getAnimProperty(aud::AP_PITCH);
    pitch->reset();
    pitch->write(&default_value);
    aud::AnimateableProperty *location = handle->getAnimProperty(aud::AP_LOCATION);
    location->reset();
    location->write(default_location);
    aud::AnimateableProperty *orientation = handle->getAnimProperty(aud::AP_ORIENTATION);
    orientation->reset();
    orientation->write(default_orientation);
  }

  if (scene->ed != nullptr) {
    seq::foreach_strip(&scene->ed->seqbase, [&](Strip *strip) {
      if (strip->runtime == nullptr || strip->runtime->scene_sound == nullptr) {
        return true;
      }

      AUD_SequenceEntry handle = strip->runtime->scene_sound;
      handle->getAnimProperty(aud::AP_VOLUME)->reset();
      handle->getAnimProperty(aud::AP_PANNING)->reset();
      handle->getAnimProperty(aud::AP_PITCH)->reset();
      return true;
    });
  }

  BKE_sound_unlock();
}

static void sound_discard_scene_audio_animation_cache(Depsgraph *depsgraph, Scene *scene)
{
  sound_reset_scene_audio_animation(scene);
  if (scene->ed != nullptr) {
    seq::eval_strips(depsgraph, scene, &scene->ed->seqbase);
  }
  BKE_sound_update_scene(depsgraph, scene);
  BKE_sound_set_scene_volume(scene, scene->audio.volume);

  bke::SceneAudioRuntime &audio = scene->runtime->audio;
  audio.animation_cache_dirty = true;
  audio.animation_cache_frame_change_handlers_hash.reset();
  audio.cached_animation_frame_start = 1;
  audio.cached_animation_frame_end = 0;
}

void BKE_sound_update_sequencer(Main *main, bSound *sound)
{
  BLI_assert_msg(0, "is not supposed to be used, is weird function.");

  Scene *scene;

  for (scene = static_cast<Scene *>(main->scenes.first); scene;
       scene = static_cast<Scene *>(scene->id.next))
  {
    seq::sound_update(scene, sound);
  }
}

/* This function assumes that you have already held the g_state.sound_device mutex. */
static void sound_start_play_scene(Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

  bke::SceneAudioRuntime &audio = scene->runtime->audio;
  if (audio.playback_handle) {
    audio.playback_handle->stop();
    audio.playback_handle.reset();
  }

  BKE_sound_reset_scene_specs(scene);

  audio.playback_handle = bke::sound_device_play(g_state.sound_device, audio.sound_scene);
  if (audio.playback_handle) {
    audio.playback_handle->setLoopCount(-1);
  }
}

void BKE_sound_play_scene(Scene *scene)
{
  std::lock_guard lock(g_state.sound_device_mutex);
  sound_device_use_begin();
  sound_verify_evaluated_id(&scene->id);

  const double cur_time = FRA2TIME(scene->r.cfra + scene->r.subframe);

  g_state.sound_device->lock();

  bke::SceneAudioRuntime &audio = scene->runtime->audio;
  if (audio.sound_scrub_handle && audio.sound_scrub_handle->getStatus() != aud::STATUS_INVALID) {
    /* If the audio scrub handle is playing back, stop to make sure it is not active.
     * Otherwise, it will trigger a callback that will stop audio playback. */
    audio.sound_scrub_handle->stop();
    audio.sound_scrub_handle = nullptr;
    /* The scrub_handle started playback with playback_handle, stop it so we can
     * properly restart it. */
    audio.playback_handle->pause();
  }

  aud::Status status = audio.playback_handle ? audio.playback_handle->getStatus() :
                                               aud::STATUS_INVALID;

  if (status == aud::STATUS_INVALID) {
    sound_start_play_scene(scene);

    if (!audio.playback_handle) {
      g_state.sound_device->unlock();
      return;
    }
  }

  if (status != aud::STATUS_PLAYING) {
    /* Seeking the synchronizer will also seek the playback handle.
     * Even if we don't have A/V sync on, keep the synchronizer and handle seek time in sync. */
    aud::DeviceManager::getDevice()->seekSynchronizer(cur_time);
    audio.playback_handle->seek(cur_time);
    audio.playback_handle->resume();
  }

  if (scene->audio.flag & AUDIO_SYNC) {
    aud::DeviceManager::getDevice()->playSynchronizer();
  }

  g_state.sound_device->unlock();
}

void BKE_sound_stop_scene(Scene *scene)
{
  std::lock_guard lock(g_state.sound_device_mutex);
  BLI_assert(g_state.sound_device);
  if (scene->runtime->audio.playback_handle) {
    scene->runtime->audio.playback_handle->pause();

    if (scene->audio.flag & AUDIO_SYNC) {
      aud::DeviceManager::getDevice()->stopSynchronizer();
    }
  }
  sound_device_use_end();
}

void BKE_sound_seek_scene(Main *bmain, Scene *scene)
{
  std::lock_guard lock(g_state.sound_device_mutex);
  bool animation_playing = false;
  for (bScreen *screen = static_cast<bScreen *>(bmain->screens.first); screen;
       screen = static_cast<bScreen *>(screen->id.next))
  {
    if (screen->animtimer) {
      animation_playing = true;
      break;
    }
  }

  bool do_audio_scrub = scene->audio.flag & AUDIO_SCRUB && !animation_playing;

  if (do_audio_scrub) {
    /* Make sure the sound device is open for scrubbing. */
    sound_device_use_begin();
  }
  else if (g_state.sound_device == nullptr) {
    /* Nothing to do if there is no sound device and we are not doing audio scrubbing. */
    return;
  }
  sound_verify_evaluated_id(&scene->id);

  g_state.sound_device->lock();

  bke::SceneAudioRuntime &audio = scene->runtime->audio;
  aud::Status status = audio.playback_handle ? audio.playback_handle->getStatus() :
                                               aud::STATUS_INVALID;
  if (status == aud::STATUS_INVALID) {
    sound_start_play_scene(scene);

    if (!audio.playback_handle) {
      g_state.sound_device->unlock();
      if (do_audio_scrub) {
        sound_device_use_end();
      }
      return;
    }

    audio.playback_handle->pause();
  }

  const double one_frame = 1.0 / scene->frames_per_second() +
                           (U.audiorate > 0 ? U.mixbufsize / double(U.audiorate) : 0.0);
  const double cur_time = FRA2TIME(scene->r.cfra);

  if (do_audio_scrub) {
    /* Playback one frame of audio without advancing the timeline. */
    audio.playback_handle->seek(cur_time);
    audio.playback_handle->resume();
    if (audio.sound_scrub_handle && audio.sound_scrub_handle->getStatus() != aud::STATUS_INVALID) {
      audio.sound_scrub_handle->seek(0);
    }
    else {
      if (audio.sound_scrub_handle) {
        audio.sound_scrub_handle->stop();
      }
      audio.sound_scrub_handle = bke::sound_pause_after(audio.playback_handle, one_frame);
    }
    sound_device_use_end_after(std::chrono::milliseconds(int(one_frame * 1000)));
  }
  else if (status == aud::STATUS_PLAYING) {
    /* Seeking the synchronizer will also seek the playback handle.
     * Even if we don't have A/V sync on, keep the synchronizer and handle
     * seek time in sync.
     */
    aud::DeviceManager::getDevice()->seekSynchronizer(cur_time);
    audio.playback_handle->seek(cur_time);
  }

  g_state.sound_device->unlock();
}

double BKE_sound_sync_scene(Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

  /* Ugly: Blender doesn't like it when the animation is played back during rendering */
  if (G.is_rendering) {
    return NAN_FLT;
  }

  if (scene->runtime->audio.playback_handle) {
    if (scene->audio.flag & AUDIO_SYNC) {
      return aud::DeviceManager::getDevice()->getSynchronizerPosition();
    }

    return scene->runtime->audio.playback_handle->getPosition();
  }
  return NAN_FLT;
}

static int sound_read(
    AUD_Sound sound, float *buffer, int length, int samples_per_second, const bool *interrupt)
{
  using namespace aud;
  DeviceSpecs specs;
  float *buf;
  Buffer aBuffer;

  specs.rate = RATE_INVALID;
  specs.channels = CHANNELS_MONO;
  specs.format = FORMAT_INVALID;

  std::shared_ptr<IReader> reader = ChannelMapper(sound, specs).createReader();

  specs.specs = reader->getSpecs();
  int len;
  float samplejump = specs.rate / samples_per_second;
  float min, max, power, overallmax;
  bool eos;

  overallmax = 0;

  for (int i = 0; i < length; i++) {
    len = floor(samplejump * (i + 1)) - floor(samplejump * i);

    if (*interrupt) {
      return 0;
    }

    aBuffer.assureSize(len * AUD_SAMPLE_SIZE(specs));
    buf = aBuffer.getBuffer();

    reader->read(len, eos, buf);

    max = min = *buf;
    power = *buf * *buf;
    for (int j = 1; j < len; j++) {
      if (buf[j] < min) {
        min = buf[j];
      }
      if (buf[j] > max) {
        max = buf[j];
      }
      power += buf[j] * buf[j];
    }

    buffer[i * 3] = min;
    buffer[i * 3 + 1] = max;
    buffer[i * 3 + 2] = std::sqrt(power / len);

    if (overallmax < max) {
      overallmax = max;
    }
    if (overallmax < -min) {
      overallmax = -min;
    }

    if (eos) {
      length = i;
      break;
    }
  }

  if (overallmax > 1.0f) {
    for (int i = 0; i < length * 3; i++) {
      buffer[i] /= overallmax;
    }
  }

  return length;
}

void BKE_sound_read_waveform(Main *bmain, bSound *sound, bool *stop)
{
  if (*stop) {
    BKE_sound_runtime_clear_waveform_loading_tag(sound);
    return;
  }

  bool need_close_audio_handles = false;
  bke::SoundRuntime *runtime = sound->runtime;
  if (runtime->playback_handle == nullptr) {
    /* TODO(sergey): Make it fully independent audio handle. */
    sound_load_audio(bmain, sound, true);
    need_close_audio_handles = true;
  }

  SoundInfo info = bke::sound_info_get(runtime->playback_handle);

  Vector<float> *waveform = MEM_new<Vector<float>>(__func__);
  if (info.length > 0) {
    int length = info.length * SOUND_WAVE_SAMPLES_PER_SECOND;

    waveform->resize(3 * length);
    length = sound_read(
        runtime->playback_handle, waveform->data(), length, SOUND_WAVE_SAMPLES_PER_SECOND, stop);
    waveform->resize(3 * length);
  }

  if (*stop) {
    MEM_delete(waveform);
    MEM_SAFE_DELETE(runtime->waveform);
    BLI_spin_lock(&runtime->spinlock);
    runtime->tags &= ~bke::SoundTags::WaveformLoading;
    BLI_spin_unlock(&runtime->spinlock);
    if (need_close_audio_handles) {
      sound_free_audio(sound);
    }
    return;
  }

  sound_free_waveform(sound);

  BLI_spin_lock(&runtime->spinlock);
  runtime->waveform = waveform;
  runtime->tags &= ~bke::SoundTags::WaveformLoading;
  BLI_spin_unlock(&runtime->spinlock);

  if (need_close_audio_handles) {
    sound_free_audio(sound);
  }
}

static NlaStrip *sound_find_evaluated_nla_strip(ListBaseT<NlaStrip> &strips,
                                                const NlaStrip *strip_original)
{
  for (NlaStrip &strip : strips) {
    if ((strip.orig_strip != nullptr ? strip.orig_strip : &strip) == strip_original) {
      return &strip;
    }
    if (NlaStrip *nested_strip = sound_find_evaluated_nla_strip(strip.strips, strip_original)) {
      return nested_strip;
    }
  }
  return nullptr;
}

static NlaStrip *sound_find_evaluated_nla_strip(Object *object, const NlaStrip &strip)
{
  if (object->adt == nullptr) {
    return nullptr;
  }
  const NlaStrip *strip_original = strip.orig_strip != nullptr ? strip.orig_strip : &strip;
  for (NlaTrack &track : object->adt->nla_tracks) {
    if (NlaStrip *strip_eval = sound_find_evaluated_nla_strip(track.strips, strip_original)) {
      return strip_eval;
    }
  }
  return nullptr;
}

static void sound_update_base_from_evaluation(Scene *scene,
                                              Object *target_object,
                                              Object *evaluation_object,
                                              Set<AUD_SequenceEntry> &new_set)
{
  float quat[4];

  sound_verify_evaluated_id(&scene->id);
  sound_verify_evaluated_id(&target_object->id);
  sound_verify_evaluated_id(&evaluation_object->id);

  if ((evaluation_object->type != OB_SPEAKER) || !evaluation_object->adt) {
    return;
  }

  Speaker *speaker = id_cast<Speaker *>(evaluation_object->data);
  const Speaker *target_speaker = id_cast<const Speaker *>(target_object->data);
  for (NlaTrack &track : evaluation_object->adt->nla_tracks) {
    for (NlaStrip &strip : track.strips) {
      if (strip.type != NLASTRIP_TYPE_SOUND) {
        continue;
      }

      NlaStrip *target_strip = sound_find_evaluated_nla_strip(target_object, strip);
      if (target_strip == nullptr) {
        continue;
      }
      bke::NlaStripRuntime &strip_runtime = target_strip->runtime_get();

      if ((strip_runtime.speaker_handle != nullptr) &&
          scene->runtime->audio.speaker_handles.remove(strip_runtime.speaker_handle))
      {
        if (target_speaker->sound) {
          strip_runtime.speaker_handle->move(
              double(strip.start) / scene->frames_per_second(), FLT_MAX, 0);
        }
        else {
          scene->runtime->audio.sound_scene->remove(strip_runtime.speaker_handle);
          strip_runtime.speaker_handle = nullptr;
        }
      }
      else {
        if (target_speaker->sound) {
          strip_runtime.speaker_handle = AUD_SequenceEntry(scene->runtime->audio.sound_scene->add(
              target_speaker->sound->runtime->playback_handle,
              double(strip.start) / scene->frames_per_second(),
              FLT_MAX,
              0));
          strip_runtime.speaker_handle->setRelative(false);
        }
      }

      if (strip_runtime.speaker_handle) {
        const bool mute = ((strip.flag & NLASTRIP_FLAG_MUTED) || (speaker->flag & SPK_MUTED));
        new_set.add(strip_runtime.speaker_handle);
        strip_runtime.speaker_handle->setVolumeMaximum(speaker->volume_max);
        strip_runtime.speaker_handle->setVolumeMinimum(speaker->volume_min);
        strip_runtime.speaker_handle->setDistanceMaximum(speaker->distance_max);
        strip_runtime.speaker_handle->setDistanceReference(speaker->distance_reference);
        strip_runtime.speaker_handle->setAttenuation(speaker->attenuation);
        strip_runtime.speaker_handle->setConeAngleOuter(speaker->cone_angle_outer);
        strip_runtime.speaker_handle->setConeAngleInner(speaker->cone_angle_inner);
        strip_runtime.speaker_handle->setConeVolumeOuter(speaker->cone_volume_outer);

        mat4_to_quat(quat, evaluation_object->object_to_world().ptr());
        float3 location = evaluation_object->object_to_world().location();
        set_audaspace_anim_property(
            scene, strip_runtime.speaker_handle, aud::AP_LOCATION, scene->r.cfra, location, true);
        set_audaspace_anim_property(
            scene, strip_runtime.speaker_handle, aud::AP_ORIENTATION, scene->r.cfra, quat, true);
        set_audaspace_anim_property(scene,
                                    strip_runtime.speaker_handle,
                                    aud::AP_VOLUME,
                                    scene->r.cfra,
                                    &speaker->volume,
                                    true);
        set_audaspace_anim_property(scene,
                                    strip_runtime.speaker_handle,
                                    aud::AP_PITCH,
                                    scene->r.cfra,
                                    &speaker->pitch,
                                    true);
        strip_runtime.speaker_handle->setSound(target_speaker->sound->runtime->playback_handle);
        strip_runtime.speaker_handle->mute(mute);
      }
    }
  }
}

static void sound_update_listener(Scene *scene, Object *camera)
{
  if (camera == nullptr) {
    return;
  }

  float quat[4];
  mat4_to_quat(quat, camera->object_to_world().ptr());
  float3 location = camera->object_to_world().location();
  set_audaspace_anim_property(
      scene, scene->runtime->audio.sound_scene, aud::AP_LOCATION, scene->r.cfra, location, true);
  set_audaspace_anim_property(
      scene, scene->runtime->audio.sound_scene, aud::AP_ORIENTATION, scene->r.cfra, quat, true);
}

static void sound_finish_spatial_audio_update(Scene *scene, Set<AUD_SequenceEntry> &new_set)
{
  bke::SceneAudioRuntime &audio = scene->runtime->audio;
  for (const AUD_SequenceEntry &handle : audio.speaker_handles) {
    audio.sound_scene->remove(handle);
  }
  audio.speaker_handles = std::move(new_set);
}

static void sound_update_scene_spatial_audio_at_frame(Depsgraph *evaluation_depsgraph,
                                                      Depsgraph *target_depsgraph,
                                                      Scene *target_scene,
                                                      Scene *source_scene,
                                                      const Span<ID *> output_ids,
                                                      const int frame)
{
  BKE_scene_frame_set(target_scene, float(frame));
  Set<AUD_SequenceEntry> new_set;

  DEGObjectIterSettings deg_iter_settings = {nullptr};
  deg_iter_settings.depsgraph = evaluation_depsgraph;
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_INDIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET;
  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, evaluation_object) {
    if (evaluation_object->type != OB_SPEAKER) {
      continue;
    }
    Object *object_original = DEG_get_original(evaluation_object);
    if (!output_ids.contains(&object_original->id)) {
      continue;
    }
    Object *target_object = DEG_get_evaluated(target_depsgraph, object_original);
    if (!DEG_is_evaluated(target_object)) {
      continue;
    }
    sound_update_base_from_evaluation(target_scene, target_object, evaluation_object, new_set);
  }
  DEG_OBJECT_ITER_END;

  Object *camera_original = BKE_scene_camera_switch_find(source_scene, frame);
  if (camera_original == nullptr) {
    camera_original = source_scene->camera;
  }
  if (camera_original != nullptr) {
    Object *camera_eval = DEG_get_evaluated(evaluation_depsgraph, camera_original);
    if (DEG_is_evaluated(camera_eval)) {
      sound_update_listener(target_scene, camera_eval);
    }
  }

  sound_finish_spatial_audio_update(target_scene, new_set);
}

void BKE_sound_update_scene(Depsgraph *depsgraph, Scene *scene)
{
  sound_verify_evaluated_id(&scene->id);

#  ifdef WITH_PYTHON
  if (scene->runtime->audio.animation_write_state == bke::AudioAnimationWriteState::Normal) {
    const std::optional<uint64_t> frame_change_handlers_hash =
        BPY_app_handlers_frame_change_identity();
    if (scene->runtime->audio.animation_cache_frame_change_handlers_hash !=
        frame_change_handlers_hash)
    {
      /* Handler lists are not depsgraph inputs. Detect policy changes at the normal audio update
       * boundary so the next consumer update can discard an incompatible cache. */
      scene->runtime->audio.animation_cache_dirty = true;
    }
  }
#  endif

  Set<AUD_SequenceEntry> new_set;
  Set<ID *> spatial_output_ids;
  Scene *scene_original = sound_original_scene(scene);
  bool filter_spatial_outputs = false;
  if (scene_original == sound_original_scene(DEG_get_input_scene(depsgraph)) &&
      scene_original->ed != nullptr)
  {
    seq::foreach_strip(&scene_original->ed->seqbase, [&](Strip *strip) {
      filter_spatial_outputs = sound_scene_strip_uses_sequencer(strip);
      return !filter_spatial_outputs;
    });
  }
  if (filter_spatial_outputs) {
    /* Keep regular graph updates from re-adding #Speaker objects owned by embedded child caches
     * after the cache bake has removed them from the parent sound scene. */
    for (ID *id : sound_spatial_audio_output_ids(
             scene_original, DEG_get_input_view_layer(depsgraph), depsgraph, false))
    {
      spatial_output_ids.add(id);
    }
  }

  /* cheap test to skip looping over all objects (no speakers is a common case) */
  if (DEG_id_type_any_exists(depsgraph, ID_SPK)) {
    DEGObjectIterSettings deg_iter_settings = {nullptr};
    deg_iter_settings.depsgraph = depsgraph;
    deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                              DEG_ITER_OBJECT_FLAG_LINKED_INDIRECTLY |
                              DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET;
    DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, object) {
      if (filter_spatial_outputs && !spatial_output_ids.contains(&DEG_get_original(object)->id)) {
        continue;
      }
      sound_update_base_from_evaluation(scene, object, object, new_set);
    }
    DEG_OBJECT_ITER_END;
  }

  sound_update_listener(scene, scene->camera);
  sound_finish_spatial_audio_update(scene, new_set);
}

AUD_Sound BKE_sound_get_factory(void *sound)
{
  return ((bSound *)sound)->runtime->playback_handle;
}

float BKE_sound_get_length(Main *bmain, bSound *sound)
{
  if (sound->runtime->playback_handle != nullptr) {
    SoundInfo info = bke::sound_info_get(sound->runtime->playback_handle);
    return info.length;
  }
  SoundInfo info;
  if (!BKE_sound_info_get(bmain, sound, &info)) {
    return 0.0f;
  }
  return info.length;
}

char **BKE_sound_get_device_names()
{
  if (audio_device_names == nullptr) {

    std::vector<std::string> v_names = aud::DeviceManager::getAvailableDeviceNames();
    char **names = (char **)malloc(sizeof(char *) * (v_names.size() + 1));

    for (int i = 0; i < v_names.size(); i++) {
      std::string name = v_names[i];
      const size_t name_size = sizeof(char) * (name.length() + 1);
      names[i] = (char *)malloc(name_size);
      memcpy(names[i], name.c_str(), name_size);
    }
    names[v_names.size()] = nullptr;
    audio_device_names = names;
  }

  return audio_device_names;
}

bool BKE_sound_info_get(Main *main, bSound *sound, SoundInfo *sound_info)
{
  if (sound->runtime->playback_handle != nullptr) {
    *sound_info = bke::sound_info_get(sound->runtime->playback_handle);
    return true;
  }
  /* TODO(sergey): Make it fully independent audio handle. */
  /* Don't free waveforms during non-destructive queries.
   * This causes unnecessary recalculation - see #69921 */
  sound_load_audio(main, sound, false);
  const bool result = sound->runtime->playback_handle != nullptr;
  if (result) {
    *sound_info = bke::sound_info_get(sound->runtime->playback_handle);
  }
  sound_free_audio(sound);
  return result;
}

bool BKE_sound_stream_info_get(Main *main,
                               const char *filepath,
                               int stream,
                               SoundStreamInfo *sound_info)
{
  char filepath_abs[FILE_MAX];
  STRNCPY(filepath_abs, filepath);
  const char *blendfile_path = BKE_main_blendfile_path(main);
  BLI_path_abs(filepath_abs, blendfile_path);

  std::vector<aud::StreamInfo> streams;
  try {
    streams = aud::FileManager::queryStreams(filepath_abs);
  }
  catch (aud::Exception &) {
    return false;
  }

  if (stream < 0 || stream >= streams.size()) {
    return false;
  }

  sound_info->start = streams[stream].start;
  sound_info->duration = streams[stream].duration;
  return true;
}

int BKE_sound_stream_count(Main *main, const char *filepath)
{
  char filepath_abs[FILE_MAX];
  STRNCPY(filepath_abs, filepath);
  BLI_path_abs(filepath_abs, BKE_main_blendfile_path(main));

  std::vector<aud::StreamInfo> streams;
  try {
    streams = aud::FileManager::queryStreams(filepath_abs);
    return streams.size();
  }
  catch (aud::Exception &) {
    return 0;
  }
}

#  ifdef WITH_RUBBERBAND
AUD_Sound BKE_sound_ensure_time_stretch_effect(const Strip *strip, float fps)
{
  seq::StripRuntime &runtime = *strip->runtime;

  /* If we already have a time stretch effect with the same frame-rate, use that. */
  if (runtime.sound_time_stretch != nullptr && runtime.sound_time_stretch_fps == fps) {
    return runtime.sound_time_stretch;
  }

  /* Otherwise create the time stretch effect. */
  runtime.clear_sound_time_stretch();
  runtime.sound_time_stretch = AUD_Sound(
      new aud::AnimateableTimeStretchPitchScale(BKE_sound_playback_handle_get(strip->sound),
                                                fps,
                                                1.0f,
                                                1.0f,
                                                aud::StretcherQuality::HIGH,
                                                false));
  runtime.sound_time_stretch_fps = fps;
  return runtime.sound_time_stretch;
}

void BKE_sound_set_scene_sound_time_stretch_at_frame(
    const Scene *scene, AUD_Sound handle, int frame, float time_stretch, bool animated)
{
  if (sound_audio_animation_is_preserved(scene)) {
    return;
  }
  std::shared_ptr<aud::AnimateableProperty> prop =
      std::dynamic_pointer_cast<aud::AnimateableTimeStretchPitchScale>(handle)->getAnimProperty(
          aud::AP_TIME_STRETCH);
  prop->writePreserveAnimation(&time_stretch, frame, animated);
}

void BKE_sound_set_scene_sound_time_stretch_constant_range(
    const Scene *scene, AUD_Sound handle, int frame_start, int frame_end, float time_stretch)
{
  if (sound_audio_animation_is_preserved(scene)) {
    return;
  }
  frame_start = max_ii(0, frame_start);
  frame_end = max_ii(0, frame_end);
  std::shared_ptr<aud::AnimateableProperty> prop =
      std::dynamic_pointer_cast<aud::AnimateableTimeStretchPitchScale>(handle)->getAnimProperty(
          aud::AP_TIME_STRETCH);
  prop->writeConstantRange(&time_stretch, frame_start, frame_end);
}
#  endif /* WITH_RUBBERBAND */

SoundInfo bke::sound_info_get(AUD_Sound sound)
{
  SoundInfo res;
  res.length = 0.0f;
  res.specs.channels = SOUND_CHANNELS_INVALID;
  res.specs.samplerate = 0;

  try {
    std::shared_ptr<aud::IReader> reader = sound->createReader();
    if (reader) {
      aud::Specs specs = reader->getSpecs();
      res.specs.channels = eSoundChannels(specs.channels);
      res.specs.samplerate = specs.rate;
      res.length = reader->getLength() / float(specs.rate);
    }
  }
  catch (aud::Exception &ex) {
    (void)ex;
  }
  return res;
}

void bke::sound_system_initialize()
{
  aud::PluginManager::loadPlugins();
  aud::NULLDevice::registerPlugin();
}

AUD_Device bke::sound_device_init(const char *device,
                                  const aud::DeviceSpecs &specs,
                                  int buffersize,
                                  const char *name)
{
  using namespace aud;
  try {
    std::shared_ptr<IDeviceFactory> factory = device ? DeviceManager::getDeviceFactory(device) :
                                                       DeviceManager::getDefaultDeviceFactory();

    if (factory) {
      factory->setName(name);
      factory->setBufferSize(buffersize);
      factory->setSpecs(specs);
      auto device = factory->openDevice();
      DeviceManager::setDevice(device);

      return AUD_Device(device);
    }
  }
  catch (Exception &ex) {
    (void)ex;
  }
  return nullptr;
}

void bke::sound_device_exit()
{
  aud::DeviceManager::releaseDevice();
}

AUD_Handle bke::sound_device_play(AUD_Device device, AUD_Sound sound)
{
  if (!device) {
    device = aud::DeviceManager::getDevice();
  }
  try {
    return device->play(sound, true);
  }
  catch (aud::Exception &ex) {
    (void)ex;
  }
  return nullptr;
}

bool bke::sound_device_read(AUD_Device device, uchar *buffer, int length)
{
  BLI_assert(buffer);
  auto read_device = std::dynamic_pointer_cast<aud::ReadDevice>(device);
  if (!read_device) {
    return false;
  }
  try {
    return read_device->read(buffer, length);
  }
  catch (aud::Exception &) {
    return false;
  }
}

AUD_Handle bke::sound_pause_after(AUD_Handle handle, double seconds)
{
  auto device = aud::DeviceManager::getDevice();

  AUD_Sound silence = AUD_Sound(new aud::Silence(device->getSpecs().rate));
  AUD_Sound limiter = AUD_Sound(new aud::Limiter(silence, 0, seconds));

  std::lock_guard<aud::ILockable> lock(*device);

  try {
    AUD_Handle handle2 = device->play(limiter);
    if (handle2.get()) {
      AUD_Handle *data = new AUD_Handle(handle);
      handle2->setStopCallback(
          [](void *data) {
            AUD_Handle *handle = (AUD_Handle *)data;
            (*handle)->pause();
            delete handle;
          },
          data);
      return handle2;
    }
  }
  catch (aud::Exception &ex) {
    (void)ex;
  }
  return nullptr;
}

float *bke::sound_read_file_buffer(const char *filename,
                                   float low,
                                   float high,
                                   float attack,
                                   float release,
                                   float threshold,
                                   bool accumulate,
                                   bool additive,
                                   bool square,
                                   float sthreshold,
                                   double samplerate,
                                   int stream,
                                   int *length)
{
  using namespace aud;

  DeviceSpecs specs;
  specs.channels = CHANNELS_MONO;
  specs.rate = samplerate;

  AUD_Sound file = AUD_Sound(new File(filename, stream));
  int position = 0;

  Buffer buffer;

  try {
    std::shared_ptr<IReader> reader = file->createReader();
    SampleRate rate = reader->getSpecs().rate;

    AUD_Sound sound = AUD_Sound(new ChannelMapper(file, specs));

    if (high < rate) {
      sound = AUD_Sound(new Lowpass(sound, high));
    }
    if (low > 0) {
      sound = AUD_Sound(new Highpass(sound, low));
    }

    sound = AUD_Sound(new Envelope(sound, attack, release, threshold, 0.1f));
    sound = AUD_Sound(new LinearResample(sound, specs));

    if (square) {
      sound = AUD_Sound(new Threshold(sound, sthreshold));
    }

    if (accumulate) {
      sound = AUD_Sound(new Accumulator(sound, additive));
    }
    else if (additive) {
      sound = AUD_Sound(new Sum(sound));
    }

    reader = sound->createReader();

    if (!reader) {
      return nullptr;
    }

    int len;
    bool eos;
    do {
      len = samplerate;
      buffer.resize((position + len) * sizeof(float), true);
      reader->read(len, eos, buffer.getBuffer() + position);
      position += len;
    } while (!eos);
  }
  catch (Exception &) {
    return nullptr;
  }

  float *result = MEM_new_array_uninitialized<float>(position, __func__);
  memcpy(result, buffer.getBuffer(), position * sizeof(float));
  *length = position;
  return result;
}

bool bke::sound_mixdown(AUD_Sequence sequence,
                        uint start,
                        uint length,
                        uint buffersize,
                        const char *filename,
                        const aud::DeviceSpecs &specs,
                        aud::Container format,
                        aud::Codec codec,
                        uint bitrate,
                        bool split_channels,
                        std::string &r_error,
                        bool (*progress_callback)(float, void *),
                        void *progress_data)
{
  using namespace aud;
  try {
    sequence->setSpecs(specs.specs);
    std::shared_ptr<IReader> reader = sequence->createQualityReader(ResampleQuality::MEDIUM);
    reader->seek(start);

    if (!split_channels) {
      std::shared_ptr<IWriter> writer = FileWriter::createWriter(
          filename, specs, format, codec, bitrate);
      FileWriter::writeReader(
          reader, writer, length, buffersize, progress_callback, progress_data);
    }
    else {
      std::vector<std::shared_ptr<IWriter>> writers;
      int channels = specs.channels;

      aud::DeviceSpecs specs_mono = specs;
      specs_mono.channels = CHANNELS_MONO;
      for (int i = 0; i < channels; i++) {
        std::string stream;
        std::string fn = filename;
        size_t index = fn.find_last_of('.');
        size_t index_slash = fn.find_last_of('/');
        size_t index_backslash = fn.find_last_of('\\');

        if ((index == std::string::npos) ||
            ((index < index_slash) && (index_slash != std::string::npos)) ||
            ((index < index_backslash) && (index_backslash != std::string::npos)))
        {
          stream = fmt::format("{}_{}", filename, i + 1);
        }
        else {
          stream = fmt::format("{}_{}{}", fn.substr(0, index), i + 1, fn.substr(index));
        }
        writers.push_back(FileWriter::createWriter(stream, specs_mono, format, codec, bitrate));
      }
      FileWriter::writeReader(
          reader, writers, length, buffersize, progress_callback, progress_data);
    }
    return true;
  }
  catch (Exception &e) {
    r_error = e.getMessage();
    return false;
  }
}

#else /* WITH_AUDASPACE */

#  include "BLI_utildefines.hh"

void BKE_sound_force_device(const char * /*device*/) {}
void BKE_sound_init_once() {}
void BKE_sound_init(Main * /*bmain*/) {}
void BKE_sound_exit_once() {}
void BKE_sound_load(Main * /*bmain*/, bSound * /*sound*/) {}
void BKE_sound_packfile_ensure(Main * /*bmain*/, bSound * /*sound*/, ReportList * /*reports*/) {}
void BKE_sound_create_scene(Scene * /*scene*/) {}
void BKE_sound_destroy_scene(Scene * /*scene*/) {}
void BKE_sound_lock() {}
void BKE_sound_unlock() {}
void BKE_sound_refresh_callback_bmain(Main * /*bmain*/) {}
void BKE_sound_reset_scene_specs(Scene * /*scene*/) {}
void BKE_sound_mute_scene(Scene * /*scene*/, bool /*muted*/) {}
AUD_SequenceEntry BKE_sound_scene_add_scene_sound(Scene * /*scene*/, Strip * /*strip*/)
{
  return nullptr;
}
AUD_SequenceEntry BKE_sound_add_scene_sound(Scene * /*scene*/, Strip * /*strip*/)
{
  return nullptr;
}
void BKE_sound_remove_scene_sound(Scene * /*scene*/, AUD_SequenceEntry /*handle*/) {}
void BKE_sound_mute_scene_sound(AUD_SequenceEntry /*handle*/, bool /*mute*/) {}
void BKE_sound_move_scene_sound(const Scene * /*scene*/,
                                AUD_SequenceEntry /*handle*/,
                                int /*startframe*/,
                                int /*endframe*/,
                                int /*frameskip*/,
                                double /*audio_offset*/)
{
}
void BKE_sound_move_scene_sound_defaults(Scene * /*scene*/, Strip * /*strip*/) {}
void BKE_sound_play_scene(Scene * /*scene*/) {}
void BKE_sound_stop_scene(Scene * /*scene*/) {}
void BKE_sound_seek_scene(Main * /*bmain*/, Scene * /*scene*/) {}
double BKE_sound_sync_scene(Scene * /*scene*/)
{
  return NAN_FLT;
}
void BKE_sound_read_waveform(Main *bmain,
                             bSound *sound,
                             /* NOLINTNEXTLINE: readability-non-const-parameter. */
                             bool *stop)
{
  UNUSED_VARS(sound, stop, bmain);
}

void BKE_sound_update_sequencer(Main * /*main*/, bSound * /*sound*/) {}
void BKE_sound_update_scene(Depsgraph * /*depsgraph*/, Scene * /*scene*/) {}
void BKE_sound_update_scene_sound(AUD_SequenceEntry /*handle*/, bSound * /*sound*/) {}
void BKE_sound_update_scene_listener(Scene * /*scene*/) {}
void BKE_sound_update_fps(Main * /*bmain*/, Scene * /*scene*/) {}
void BKE_sound_set_scene_sound_volume_at_frame(const Scene * /*scene*/,
                                               AUD_SequenceEntry /*handle*/,
                                               int /*frame*/,
                                               float /*volume*/,
                                               bool /*animated*/)
{
}
void BKE_sound_set_scene_sound_pan_at_frame(const Scene * /*scene*/,
                                            AUD_SequenceEntry /*handle*/,
                                            int /*frame*/,
                                            float /*pan*/,
                                            bool /*animated*/)
{
}
void BKE_sound_set_scene_volume(Scene * /*scene*/, float /*volume*/) {}
static void sound_reset_scene_audio_animation(Scene * /*scene*/) {}
static void sound_discard_scene_audio_animation_cache(Depsgraph * /*depsgraph*/, Scene * /*scene*/)
{
}
static void sound_update_scene_spatial_audio_at_frame(Depsgraph * /*evaluation_depsgraph*/,
                                                      Depsgraph * /*target_depsgraph*/,
                                                      Scene * /*target_scene*/,
                                                      Scene * /*source_scene*/,
                                                      Span<ID *> /*output_ids*/,
                                                      const int /*frame*/)
{
}
void BKE_sound_set_scene_sound_pitch_at_frame(const Scene * /*scene*/,
                                              AUD_SequenceEntry /*handle*/,
                                              int /*frame*/,
                                              float /*pitch*/,
                                              bool /*animated*/)
{
}
void BKE_sound_set_scene_sound_pitch_constant_range(const Scene * /*scene*/,
                                                    AUD_SequenceEntry /*handle*/,
                                                    int /*frame_start*/,
                                                    int /*frame_end*/,
                                                    float /*pitch*/)
{
}
float BKE_sound_get_length(Main * /*bmain*/, bSound * /*sound*/)
{
  return 0;
}
char **BKE_sound_get_device_names()
{
  static char *names[1] = {nullptr};
  return names;
}

bool BKE_sound_info_get(Main * /*main*/, bSound * /*sound*/, SoundInfo * /*sound_info*/)
{
  return false;
}

bool BKE_sound_stream_info_get(Main * /*main*/,
                               const char * /*filepath*/,
                               int /*stream*/,
                               SoundStreamInfo * /*sound_info*/)
{
  return false;
}

int BKE_sound_stream_count(Main * /*main*/, const char * /*filepath*/)
{
  return 0;
}

#endif /* WITH_AUDASPACE */

#if !defined(WITH_AUDASPACE) || !defined(WITH_RUBBERBAND)
AUD_Sound BKE_sound_ensure_time_stretch_effect(const Strip * /*strip*/, float /*fps*/)
{
  return nullptr;
}

void BKE_sound_set_scene_sound_time_stretch_at_frame(const Scene * /*scene*/,
                                                     AUD_Sound /*handle*/,
                                                     int /*frame*/,
                                                     float /*time_stretch*/,
                                                     bool /*animated*/)
{
}
void BKE_sound_set_scene_sound_time_stretch_constant_range(const Scene * /*scene*/,
                                                           AUD_Sound /*handle*/,
                                                           int /*frame_start*/,
                                                           int /*frame_end*/,
                                                           float /*time_stretch*/)
{
}
#endif

void BKE_sound_ensure_scene(Scene *scene)
{
  if (scene->runtime->audio.sound_scene != nullptr) {
    return;
  }
  BKE_sound_create_scene(scene);
}

static void sound_ensure_loaded(Main *bmain, bSound *sound)
{
  if (sound->runtime->cache != nullptr) {
    return;
  }
  BKE_sound_load(bmain, sound);
}

void BKE_sound_jack_sync_callback_set(SoundJackSyncCallback callback)
{
#if defined(WITH_AUDASPACE)
  sound_jack_sync_callback = callback;
#else
  UNUSED_VARS(callback);
#endif
}

void BKE_sound_jack_scene_update(Scene *scene, int mode, double time)
{
  sound_verify_evaluated_id(&scene->id);

  /* Ugly: Blender doesn't like it when the animation is played back during rendering. */
  if (G.is_rendering) {
    return;
  }
#ifdef WITH_AUDASPACE
  g_state.sound_device->lock();

  if (mode) {
    BKE_sound_play_scene(scene);
  }
  else {
    BKE_sound_stop_scene(scene);
  }
  if (scene->runtime->audio.playback_handle != nullptr) {
    scene->runtime->audio.playback_handle->seek(time);
  }
  g_state.sound_device->unlock();
#else
  UNUSED_VARS(mode, time);
#endif
}

void BKE_sound_evaluate(Depsgraph *depsgraph, Main *bmain, bSound *sound)
{
  DEG_debug_print_eval(depsgraph, __func__, sound->id.name, sound);
  if (sound->id.recalc & ID_RECALC_SOURCE) {
    /* Sequencer checks this flag to see if the strip sound is to be updated from the Audaspace
     * side. */
    sound->id.recalc |= ID_RECALC_AUDIO;
  }

  if (sound->id.recalc & ID_RECALC_AUDIO) {
    BKE_sound_load(bmain, sound);
    return;
  }
  sound_ensure_loaded(bmain, sound);
}

void BKE_sound_runtime_state_get_and_clear(const bSound *sound,
                                           AUD_Sound *r_cache,
                                           AUD_Sound *r_playback_handle,
                                           Vector<float> **r_waveform)
{
  bke::SoundRuntime *runtime = sound->runtime;
  *r_cache = runtime->cache;
  *r_playback_handle = runtime->playback_handle;
  *r_waveform = runtime->waveform;
  runtime->cache = nullptr;
  runtime->playback_handle = nullptr;
  runtime->waveform = nullptr;
}

void BKE_sound_runtime_state_set(const bSound *sound,
                                 AUD_Sound cache,
                                 AUD_Sound playback_handle,
                                 Vector<float> *waveform)
{
  bke::SoundRuntime *runtime = sound->runtime;
  runtime->cache = cache;
  runtime->playback_handle = playback_handle;
  runtime->waveform = waveform;
}

AUD_Sound BKE_sound_playback_handle_get(const bSound *sound)
{
  if (sound == nullptr) {
    return nullptr;
  }
  return sound->runtime->playback_handle;
}

void BKE_sound_runtime_clear_waveform_loading_tag(bSound *sound)
{
  bke::SoundRuntime *runtime = sound->runtime;
  BLI_spin_lock(&runtime->spinlock);
  runtime->tags &= ~bke::SoundTags::WaveformLoading;
  BLI_spin_unlock(&runtime->spinlock);
}

bool BKE_sound_runtime_start_waveform_loading(bSound *sound)
{
  bke::SoundRuntime *runtime = sound->runtime;
  bool result = false;
  BLI_spin_lock(&runtime->spinlock);
  if (runtime->waveform == nullptr) {
    /* Load the waveform data if it hasn't been loaded and cached already. */
    if (!flag_is_set(runtime->tags, bke::SoundTags::WaveformLoading)) {
      /* Prevent sounds from reloading. */
      runtime->tags |= bke::SoundTags::WaveformLoading;
      result = true;
    }
  }
  BLI_spin_unlock(&runtime->spinlock);
  return result;
}

const Vector<float> *BKE_sound_runtime_get_waveform(const bSound *sound)
{
  return sound->runtime->waveform;
}

namespace bke {

std::shared_ptr<const bSoundFrequencySampler> bSoundFrequencySampler::get_cached(
    const bSound &sound, const Key &key)
{
#ifdef WITH_AUDASPACE
  const std::shared_ptr<SoundRuntimeGeneration> generation = sound_runtime_generation_get(sound);
  if (!generation) {
    return nullptr;
  }
  {
    /* Fast common case when the sampler has been created already. */
    bSoundFrequencySamplerMap::ConstAccessor accessor;
    if (generation->samplers.lookup(accessor, key)) {
      return accessor->second;
    }
  }
  AUD_Sound sound_handle = generation->handle;
  if (!sound_handle) {
    /* Maybe try to load the sound in this case instead of relying on cache. */
    return nullptr;
  }
  /* Slower case when the sampler is newly created. */
  bSoundFrequencySamplerMap::MutableAccessor accessor;
  if (generation->samplers.add(accessor, key)) {
    if (key.channel.has_value()) {
      const SoundInfo info = sound_info_get(sound_handle);
      const int channel = *key.channel;
      if (channel < 0 || channel >= info.specs.channels) {
        return nullptr;
      }
    }
    accessor->second = std::shared_ptr<bSoundFrequencySampler>(
        new bSoundFrequencySampler(sound_handle, generation->reader_cache, key));
  }
  return accessor->second;
#else
  UNUSED_VARS(sound, key);
  return nullptr;
#endif
}

#ifdef WITH_AUDASPACE
static bSoundFrequencySampler::WindowWeights compute_window_function_weights(
    const bSoundFrequencySampler::WindowFunction window, const int size)
{
  Array<float> weights(size);
  switch (window) {
    case bSoundFrequencySampler::WindowFunction::Hann: {
      for (const int i : IndexRange(size)) {
        weights[i] = 0.5f - 0.5f * math::cos((2.0f * std::numbers::pi * i) / (size - 1));
      }
      break;
    }
    case bSoundFrequencySampler::WindowFunction::Hamming: {
      for (const int i : IndexRange(size)) {
        weights[i] = 0.54f - 0.46f * math::cos((2.0f * std::numbers::pi * i) / (size - 1));
      }
      break;
    }
    case bSoundFrequencySampler::WindowFunction::Blackman: {
      for (const int i : IndexRange(size)) {
        weights[i] = 0.42f - 0.5f * math::cos((2.0f * std::numbers::pi * i) / (size - 1)) +
                     0.08f * math::cos((4.0f * std::numbers::pi * i) / (size - 1));
      }
      break;
    }
    case bSoundFrequencySampler::WindowFunction::Rectangular: {
      weights.fill(1.0f);
      break;
    }
  }
  const float sum = std::accumulate(weights.begin(), weights.end(), 0.0f);
  return bSoundFrequencySampler::WindowWeights{std::move(weights), sum};
}

/** Caches the window function weights so that each combination is only computed once. */
static const bSoundFrequencySampler::WindowWeights &get_window_function_weights(
    const bSoundFrequencySampler::WindowFunction window, const int size)
{
  static Mutex mutex;
  static Map<std::pair<bSoundFrequencySampler::WindowFunction, int>,
             std::unique_ptr<bSoundFrequencySampler::WindowWeights>>
      map;
  std::lock_guard lock{mutex};
  return *map.lookup_or_add_cb({window, size}, [&]() {
    return std::make_unique<bSoundFrequencySampler::WindowWeights>(
        compute_window_function_weights(window, size));
  });
}
#endif

#ifdef WITH_AUDASPACE
bSoundFrequencySampler::bSoundFrequencySampler(AUD_Sound sound,
                                               std::shared_ptr<SoundReaderCache> reader_cache,
                                               const Key &key)
    : key_(key),
      reader_cache_(std::move(reader_cache)),
      window_weights_(get_window_function_weights(key.window_function, key.fft_size))
{
  const SoundInfo info = bke::sound_info_get(sound);
  samples_per_second_ = info.specs.samplerate;
  /* This could be a parameter but a single fixed value seems fine for now and makes caching much
   * simpler. */
  window_cache_stride_ = key.fft_size / 8;
  const int window_caches_num = std::ceil(info.length * info.specs.samplerate /
                                          window_cache_stride_);
  window_caches_.reinitialize(window_caches_num);
}
#endif

std::optional<Array<float>> bSoundFrequencySampler::compute_fft(const int start_sample) const
{
  /* Since the result of the dft algorithm is symmetric in this case, only the first half is
   * computed. */
  const int frequencies_num = key_.fft_size / 2;

#if defined(WITH_AUDASPACE) && defined(WITH_FFTW3)
  /* Read some extra samples before the ones we are actually interested in here. This is done
   * because the #read function may sometimes give invalid data for the first samples. */
  const int warmup_samples = std::min(2000, start_sample);

  int channels_num;
  Array<float> read_buffer_extra;
  bool is_end_of_stream = false;
  int length = key_.fft_size + warmup_samples;
  {
    SoundReaderLease reader = reader_cache_->acquire();
    if (!reader) {
      return std::nullopt;
    }
    channels_num = reader->getSpecs().channels;
    read_buffer_extra.reinitialize((key_.fft_size + warmup_samples) * channels_num);
    try {
      reader->seek(std::max(start_sample - warmup_samples, 0));
      reader->read(length, is_end_of_stream, read_buffer_extra.data());
    }
    catch (...) {
      reader.discard();
      throw;
    }
  }
  const Span<float> read_buffer = read_buffer_extra.as_span().drop_front(warmup_samples *
                                                                         channels_num);
  const int read_length = read_buffer.size() / channels_num;

  /* Pull out the samples for the requested channel(s). */
  Array<float> buffer(key_.fft_size, 0.0f);
  if (key_.channel.has_value()) {
    const int channel = *key_.channel;
    if (channel < 0 || channel >= channels_num) {
      return std::nullopt;
    }
    for (const int i : IndexRange(read_length)) {
      buffer[i] = read_buffer[i * channels_num + channel];
    }
  }
  else {
    for (const int i : IndexRange(read_length)) {
      for (const int c : IndexRange(channels_num)) {
        buffer[i] += read_buffer[i * channels_num + c];
      }
      buffer[i] /= channels_num;
    }
  }

  /* Apply window function which avoids spectral leakage (depending on the function). */
  for (const int i : IndexRange(read_length)) {
    buffer[i] *= window_weights_.weights[i];
  }

  /* Set up the fftw plan. */
  fftwf_complex *fftwf_buffer = static_cast<fftwf_complex *>(
      fftwf_malloc(sizeof(fftwf_complex) * (frequencies_num + 1)));
  fftwf_plan plan = fftwf_plan_dft_r2c_1d(
      key_.fft_size, buffer.data(), fftwf_buffer, FFTW_ESTIMATE);
  BLI_SCOPED_DEFER([&]() {
    fftwf_destroy_plan(plan);
    fftwf_free(fftwf_buffer);
  });

  /* Actually perform the fourier transform. */
  fftwf_execute(plan);

  /* Compute the amplitudes of the frequencies. */
  Array<float> frequency_amplitudes(frequencies_num);
  /* The scaling factor is applied so that changing the fft size or window function does not affect
   * the magnitude of the result. */
  const float scaling_factor = 1.0f / window_weights_.weights_sum;
  for (const int i : IndexRange(frequencies_num)) {
    const fftwf_complex &c = fftwf_buffer[i];
    /* Take real and imaginary parts into account which correspond to the sin and cos component of
     * the frequency. */
    frequency_amplitudes[i] = sqrt(pow2f(c[0]) + pow2f(c[1])) * scaling_factor;
  }

  return frequency_amplitudes;
#else
  UNUSED_VARS(start_sample);
  return Array<float>(frequencies_num, 0.0f);
#endif
}

static float catmul_rom_interpolation(
    const float p0, const float p1, const float p2, const float p3, const float t)
{
  const float t2 = t * t;
  const float t3 = t2 * t;

  const float value = 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
                             (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
  return value;
}

static float bspline_interpolation(
    const float p0, const float p1, const float p2, const float p3, const float t)
{
  const float t2 = t * t;
  const float t3 = t2 * t;

  const float b0 = (1.0f - t) * (1.0f - t) * (1.0f - t) / 6.0f;
  const float b1 = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
  const float b2 = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
  const float b3 = t3 / 6.0f;

  return (b0 * p0 + b1 * p1 + b2 * p2 + b3 * p3);
}

float bSoundFrequencySampler::sample(const float time,
                                     const float low,
                                     const float high,
                                     const InterpolationMethod time_interpolation,
                                     const InterpolationMethod frequency_interpolation) const
{
  if (low >= high) {
    return 0.0f;
  }
  const float i_float = std::max(
      0.0f, (time * samples_per_second_ - key_.fft_size / 2) / window_cache_stride_);
  const int i_pre = floorf(i_float);
  const int i_post = i_pre + 1;
  const float t = fractf(i_float);

  switch (time_interpolation) {
    case InterpolationMethod::Linear: {
      const float v_prev = this->sample_frequency_range_in_window(
          i_pre, low, high, frequency_interpolation);
      const float v_next = this->sample_frequency_range_in_window(
          i_post, low, high, frequency_interpolation);

      return math::interpolate(v_prev, v_next, t);
    }
    case InterpolationMethod::CatmullRom: {
      const int i_pre2 = i_pre - 1;
      const int i_post2 = i_post + 1;

      const float p0 = this->sample_frequency_range_in_window(
          i_pre2, low, high, frequency_interpolation);
      const float p1 = this->sample_frequency_range_in_window(
          i_pre, low, high, frequency_interpolation);
      const float p2 = this->sample_frequency_range_in_window(
          i_post, low, high, frequency_interpolation);
      const float p3 = this->sample_frequency_range_in_window(
          i_post2, low, high, frequency_interpolation);

      return catmul_rom_interpolation(p0, p1, p2, p3, t);
    }
    default:
    case InterpolationMethod::BSpline: {
      const int i_pre2 = i_pre - 1;
      const int i_post2 = i_post + 1;

      const float p0 = this->sample_frequency_range_in_window(
          i_pre2, low, high, frequency_interpolation);
      const float p1 = this->sample_frequency_range_in_window(
          i_pre, low, high, frequency_interpolation);
      const float p2 = this->sample_frequency_range_in_window(
          i_post, low, high, frequency_interpolation);
      const float p3 = this->sample_frequency_range_in_window(
          i_post2, low, high, frequency_interpolation);

      return bspline_interpolation(p0, p1, p2, p3, t);
    }
  }
}

float bSoundFrequencySampler::sample_frequency_range_in_window(int window_i,
                                                               float low,
                                                               float high,
                                                               InterpolationMethod method) const
{
  const std::optional<Span<float>> window_opt = this->ensure_window_cache(window_i);
  if (!window_opt.has_value()) {
    return 0.0f;
  }
  const Span<float> window_values = *window_opt;
  const float cumulative_low = this->sample_cumulative_frequency(window_values, low, method);
  const float cumulative_high = this->sample_cumulative_frequency(window_values, high, method);
  return cumulative_high - cumulative_low;
}

float bSoundFrequencySampler::sample_cumulative_frequency(const Span<float> window_values,
                                                          const float frequency,
                                                          const InterpolationMethod method) const
{
  const int max_i = window_values.size() - 1;
  const float i_float = frequency * key_.fft_size / samples_per_second_;
  const int i_pre = std::clamp<int>(std::floor(i_float), 0, max_i);
  const int i_post = std::min(i_pre + 1, max_i);
  const float t = fractf(i_float);

  switch (method) {
    case InterpolationMethod::Linear: {
      return math::interpolate(window_values[i_pre], window_values[i_post], t);
    }
    case InterpolationMethod::CatmullRom: {
      const int i_pre2 = std::max(i_pre - 1, 0);
      const int i_post2 = std::min(i_post + 1, max_i);

      const float p0 = window_values[i_pre2];
      const float p1 = window_values[i_pre];
      const float p2 = window_values[i_post];
      const float p3 = window_values[i_post2];

      return catmul_rom_interpolation(p0, p1, p2, p3, t);
    }
    case InterpolationMethod::BSpline: {
      const int i_pre2 = std::max(i_pre - 1, 0);
      const int i_post2 = std::min(i_post + 1, max_i);

      const float p0 = window_values[i_pre2];
      const float p1 = window_values[i_pre];
      const float p2 = window_values[i_post];
      const float p3 = window_values[i_post2];

      return bspline_interpolation(p0, p1, p2, p3, t);
    }
  }
  return 0.0f;
}

std::optional<Span<float>> bSoundFrequencySampler::ensure_window_cache(int window_i) const
{
  /* Clamp window so that too low or high indices get mapped to the closest valid index. */
  window_i = std::clamp<int>(window_i, 0, window_caches_.size() - 1);

  const WindowCache &window = window_caches_[window_i];
  /* Compute the FFT of that window if that wasn't done already. */
  window.mutex.ensure([&]() {
    std::optional<Array<float>> fft_array = this->compute_fft(window_i * window_cache_stride_);
    if (!fft_array.has_value()) {
      return;
    }
    /* Compute prefix sum for the fft values. */
    const Span<float> fft_values = fft_array->as_span();
    window.cumulative_amplitudes.emplace(fft_values.size() + 1);
    float sum = 0.0f;
    for (const int i : fft_array->index_range()) {
      const float value = std::abs(fft_values[i]);
      (*window.cumulative_amplitudes)[i] = sum;
      sum += value;
    }
    window.cumulative_amplitudes->last() = sum;
  });
  return window.cumulative_amplitudes;
}

}  // namespace bke

}  // namespace blender
